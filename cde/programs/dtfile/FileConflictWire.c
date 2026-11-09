/*
 * CDE - Common Desktop Environment
 *
 * These libraries and programs are free software; you can redistribute them
 * and/or modify them under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * These libraries and programs are distributed without any warranty; without
 * even the implied warranty of merchantability or fitness for a particular
 * purpose. See the GNU Lesser General Public License for more details.
 */

#include "FileConflictWire.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CONFLICT_REQUEST_MAGIC UINT32_C(0x44434631)
#define CONFLICT_REPLY_MAGIC UINT32_C(0x44435231)
#define CONFLICT_END_MAGIC UINT32_C(0x454e4431)
#define CONFLICT_PARTIAL_IDLE_SECONDS 30

typedef struct {
    sigset_t previous;
    sigset_t pipe_signal;
    int was_pending;
} ConflictWriteScope;

static int
ConflictBeginWrite(ConflictWriteScope *scope)
{
    sigset_t pending;
    int error;
    sigemptyset(&scope->pipe_signal);
    sigaddset(&scope->pipe_signal, SIGPIPE);
    error = pthread_sigmask(SIG_BLOCK, &scope->pipe_signal, &scope->previous);
    if (error != 0) { errno = error; return -1; }
    if (sigpending(&pending) != 0)
    {
        int saved = errno;
        (void)pthread_sigmask(SIG_SETMASK, &scope->previous, NULL);
        errno = saved;
        return -1;
    }
    scope->was_pending = sigismember(&pending, SIGPIPE);
    return 0;
}

static int
ConflictEndWrite(ConflictWriteScope *scope, int result)
{
    int saved = errno, error;
    if (result != 0 && saved == EPIPE && !scope->was_pending)
    {
        sigset_t pending;
        /* A pipe write generates SIGPIPE for this calling thread. Its mask
         * stays blocked until we consume that newly generated signal. Do not
         * consume a signal that was pending before this operation.
         */
        if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE))
        {
            int signal_number;
            do { error = sigwait(&scope->pipe_signal, &signal_number); }
            while (error == EINTR);
        }
    }
    error = pthread_sigmask(SIG_SETMASK, &scope->previous, NULL);
    if (error != 0 && result == 0) { errno = error; return -1; }
    errno = saved;
    return result;
}

static int
ConflictWriteAll(int fd, const void *bytes, size_t length)
{
    const unsigned char *next = bytes;
    while (length != 0)
    {
        ssize_t written = write(fd, next, length);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0 || (size_t)written > length)
        { if (written >= 0) errno = EIO; return -1; }
        next += (size_t)written;
        length -= (size_t)written;
    }
    return 0;
}

static int
ConflictWriteWords(int fd, const uint32_t *words, size_t count)
{
    uint32_t encoded[5];
    size_t i;
    if (count > sizeof(encoded) / sizeof(encoded[0]))
    { errno = EINVAL; return -1; }
    for (i = 0; i < count; ++i) encoded[i] = htonl(words[i]);
    return ConflictWriteAll(fd, encoded, count * sizeof(encoded[0]));
}

static int
ConflictWriteRequestPacket(int fd, const DtfileConflictBatch *batch)
{
    uint32_t words[5];
    size_t i;
    int result;
    words[0] = CONFLICT_REQUEST_MAGIC;
    words[1] = (uint32_t)batch->operation;
    words[2] = (uint32_t)batch->selected_count;
    words[3] = (uint32_t)batch->count;
    words[4] = (uint32_t)batch->native_bytes;
    result = ConflictWriteWords(fd, words, 5);
    for (i = 0; result == 0 && i < batch->count; ++i)
    {
        const DtfileConflictItem *item = &batch->items[i];
        words[0] = item->request_index;
        words[1] = item->flags;
        words[2] = item->source == NULL ? 0 : (uint32_t)strlen(item->source);
        words[3] = (uint32_t)strlen(item->target);
        result = ConflictWriteWords(fd, words, 4);
        if (result == 0 && words[2] != 0)
            result = ConflictWriteAll(fd, item->source, words[2]);
        if (result == 0) result = ConflictWriteAll(fd, item->target, words[3]);
    }
    if (result == 0)
    {
        words[0] = CONFLICT_END_MAGIC;
        result = ConflictWriteWords(fd, words, 1);
    }
    return result;
}

int
DtfileConflictWriteRequest(int fd, const DtfileConflictBatch *batch)
{
    ConflictWriteScope scope;
    int result;
    if (DtfileConflictBatchValidate(batch) != 0) return -1;
    if (ConflictBeginWrite(&scope) != 0) return -1;
    result = ConflictWriteRequestPacket(fd, batch);
    return ConflictEndWrite(&scope, result);
}

int
DtfileConflictWriteTaggedRequest(int fd, unsigned short tag,
                                 const DtfileConflictBatch *batch)
{
    ConflictWriteScope scope;
    int result;
    if (DtfileConflictBatchValidate(batch) != 0) return -1;
    if (ConflictBeginWrite(&scope) != 0) return -1;
    result = ConflictWriteAll(fd, &tag, sizeof(tag));
    if (result == 0) result = ConflictWriteRequestPacket(fd, batch);
    return ConflictEndWrite(&scope, result);
}

int
DtfileConflictWriteDecisions(int fd, const DtfileConflictBatch *batch,
                            const DtfileConflictDecision *decisions)
{
    ConflictWriteScope scope;
    uint32_t words[3];
    size_t i, bytes = 0;
    int result;
    if (decisions == NULL) { errno = EINVAL; return -1; }
    if (DtfileConflictBatchValidate(batch) != 0) return -1;
    /* Finish all validation before writing the first byte. The worker likewise
     * waits for every indexed decision and the trailer before changing files.
     */
    for (i = 0; i < batch->count; ++i)
    {
        size_t length;
        if (DtfileConflictDecisionValidate(batch, i, &decisions[i]) != 0) return -1;
        length = decisions[i].name == NULL ? 0 : strlen(decisions[i].name);
        if (length > DTFILE_CONFLICT_BYTES_LIMIT - bytes)
        { errno = E2BIG; return -1; }
        bytes += length;
    }
    if (ConflictBeginWrite(&scope) != 0) return -1;
    words[0] = CONFLICT_REPLY_MAGIC;
    words[1] = (uint32_t)batch->count;
    words[2] = (uint32_t)bytes;
    result = ConflictWriteWords(fd, words, 3);
    for (i = 0; result == 0 && i < batch->count; ++i)
    {
        words[0] = decisions[i].request_index;
        words[1] = (uint32_t)decisions[i].action;
        words[2] = decisions[i].name == NULL ? 0 : (uint32_t)strlen(decisions[i].name);
        result = ConflictWriteWords(fd, words, 3);
        if (result == 0 && words[2] != 0)
            result = ConflictWriteAll(fd, decisions[i].name, words[2]);
    }
    if (result == 0)
    {
        words[0] = CONFLICT_END_MAGIC;
        result = ConflictWriteWords(fd, words, 1);
    }
    return ConflictEndWrite(&scope, result);
}

int
DtfileConflictWriteCancel(int fd)
{
    ConflictWriteScope scope;
    uint32_t words[4] = {CONFLICT_REPLY_MAGIC, 0, 0, CONFLICT_END_MAGIC};
    int result;
    if (ConflictBeginWrite(&scope) != 0) return -1;
    result = ConflictWriteWords(fd, words, 4);
    return ConflictEndWrite(&scope, result);
}

typedef struct {
    pid_t owner;
    int started;
    struct timespec last_progress;
} ConflictReadWatch;

static int
ConflictReadAll(int fd, void *bytes, size_t length, ConflictReadWatch *watch)
{
    unsigned char *next = bytes;
    while (length != 0)
    {
        struct pollfd descriptor = {fd, POLLIN, 0};
        int ready;
        if (getppid() != watch->owner) { errno = ECANCELED; return -1; }
        if (watch->started)
        {
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
            if (now.tv_sec - watch->last_progress.tv_sec >= CONFLICT_PARTIAL_IDLE_SECONDS)
            { errno = ETIMEDOUT; return -1; }
        }
        ready = poll(&descriptor, 1, 1000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) return -1;
        if (ready == 0) continue;
        if (descriptor.revents & POLLNVAL) { errno = EBADF; return -1; }
        if (descriptor.revents & (POLLIN | POLLHUP | POLLERR))
        {
            ssize_t received = read(fd, next, length);
            if (received < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (received <= 0 || (size_t)received > length)
            { if (received >= 0) errno = EIO; return -1; }
            next += (size_t)received;
            length -= (size_t)received;
            if (clock_gettime(CLOCK_MONOTONIC, &watch->last_progress) != 0) return -1;
            watch->started = 1;
        }
    }
    return 0;
}

static int
ConflictReadWords(int fd, uint32_t *words, size_t count, ConflictReadWatch *watch)
{
    size_t i;
    if (ConflictReadAll(fd, words, count * sizeof(*words), watch) != 0) return -1;
    for (i = 0; i < count; ++i) words[i] = ntohl(words[i]);
    return 0;
}

int
DtfileConflictReadDecisions(int fd, pid_t owner, const DtfileConflictBatch *batch,
                           DtfileConflictDecision **decisions)
{
    ConflictReadWatch watch = {0};
    DtfileConflictDecision *items = NULL;
    uint32_t words[3], declared_bytes;
    size_t i, bytes = 0;
    int saved;
    if (decisions == NULL) { errno = EINVAL; return -1; }
    *decisions = NULL;
    if (owner <= 0) { errno = EINVAL; return -1; }
    if (DtfileConflictBatchValidate(batch) != 0) return -1;
    watch.owner = owner;
    if (ConflictReadWords(fd, words, 3, &watch) != 0) return -1;
    if (words[0] != CONFLICT_REPLY_MAGIC ||
        (words[1] != 0 && words[1] != batch->count) ||
        words[2] > DTFILE_CONFLICT_BYTES_LIMIT)
    { errno = EINVAL; return -1; }
    if (words[1] == 0)
    {
        if (words[2] != 0 || ConflictReadWords(fd, words, 1, &watch) != 0)
        { if (words[2] != 0) errno = EINVAL; return -1; }
        if (words[0] != CONFLICT_END_MAGIC) { errno = EINVAL; return -1; }
        return 0;
    }
    declared_bytes = words[2];
    items = calloc(batch->count, sizeof(*items));
    if (items == NULL) return -1;
    for (i = 0; i < batch->count; ++i)
    {
        if (ConflictReadWords(fd, words, 3, &watch) != 0) goto failed;
        if (words[2] > DTFILE_CONFLICT_PATH_LIMIT || words[2] > declared_bytes - bytes)
        { errno = E2BIG; goto failed; }
        items[i].request_index = words[0];
        items[i].action = (DtfileConflictAction)words[1];
        if (words[2] != 0)
        {
            items[i].name = malloc((size_t)words[2] + 1);
            if (items[i].name == NULL) goto failed;
            if (ConflictReadAll(fd, items[i].name, words[2], &watch) != 0) goto failed;
            if (memchr(items[i].name, 0, words[2]) != NULL) { errno = EINVAL; goto failed; }
            items[i].name[words[2]] = 0;
        }
        bytes += words[2];
        if (DtfileConflictDecisionValidate(batch, i, &items[i]) != 0) goto failed;
    }
    if (bytes != declared_bytes) { errno = EINVAL; goto failed; }
    if (ConflictReadWords(fd, words, 1, &watch) != 0) goto failed;
    if (words[0] != CONFLICT_END_MAGIC) { errno = EINVAL; goto failed; }
    *decisions = items;
    return 1;
failed:
    saved = errno;
    DtfileConflictDecisionsFree(items, batch->count);
    errno = saved;
    return -1;
}

enum ConflictReadStage {
    CONFLICT_HEADER, CONFLICT_ITEM_HEADER, CONFLICT_SOURCE,
    CONFLICT_TARGET, CONFLICT_TRAILER, CONFLICT_READY, CONFLICT_TAKEN,
    CONFLICT_FAILED
};

struct DtfileConflictReader {
    DtfileConflictBatch batch;
    enum ConflictReadStage stage;
    uint32_t words[5];
    uint32_t expected_bytes, index, flags, source_length, target_length;
    size_t used, needed, row;
    char *source, *target;
    int error;
};

DtfileConflictReader *
DtfileConflictReaderNew(void)
{
    DtfileConflictReader *reader = calloc(1, sizeof(*reader));
    if (reader != NULL) reader->needed = 5 * sizeof(uint32_t);
    return reader;
}

void
DtfileConflictReaderFree(DtfileConflictReader *reader)
{
    if (reader == NULL) return;
    free(reader->source);
    free(reader->target);
    DtfileConflictBatchFree(&reader->batch);
    free(reader);
}

static int
ConflictReaderAdvance(DtfileConflictReader *reader)
{
    if (reader->stage == CONFLICT_HEADER)
    {
        uint32_t *words = reader->words;
        size_t i;
        for (i = 0; i < 5; ++i) words[i] = ntohl(words[i]);
        if (words[0] != CONFLICT_REQUEST_MAGIC || words[4] > DTFILE_CONFLICT_BYTES_LIMIT)
        { errno = EINVAL; return -1; }
        if (DtfileConflictBatchInit(&reader->batch, (DtfileConflictOperation)words[1],
                                   words[2], words[3]) != 0) return -1;
        reader->expected_bytes = words[4];
        reader->stage = CONFLICT_ITEM_HEADER;
        reader->needed = 4 * sizeof(uint32_t);
    }
    else if (reader->stage == CONFLICT_ITEM_HEADER)
    {
        size_t i, bytes;
        for (i = 0; i < 4; ++i) reader->words[i] = ntohl(reader->words[i]);
        reader->index = reader->words[0];
        reader->flags = reader->words[1];
        reader->source_length = reader->words[2];
        reader->target_length = reader->words[3];
        if (reader->index >= reader->batch.selected_count ||
            reader->source_length > DTFILE_CONFLICT_PATH_LIMIT ||
            reader->target_length == 0 || reader->target_length > DTFILE_CONFLICT_PATH_LIMIT ||
            (reader->batch.operation == DTFILE_CONFLICT_BUFFER
              ? reader->source_length != 0 || reader->flags != 0
              : reader->source_length == 0))
        { errno = EINVAL; return -1; }
        bytes = (size_t)reader->source_length + reader->target_length + 2;
        if (reader->batch.native_bytes > reader->expected_bytes ||
            bytes > reader->expected_bytes - reader->batch.native_bytes)
        { errno = E2BIG; return -1; }
        if (reader->source_length != 0)
        {
            reader->source = malloc((size_t)reader->source_length + 1);
            if (reader->source == NULL) return -1;
            reader->source[reader->source_length] = 0;
        }
        reader->target = malloc((size_t)reader->target_length + 1);
        if (reader->target == NULL) return -1;
        reader->target[reader->target_length] = 0;
        reader->stage = reader->source_length == 0 ? CONFLICT_TARGET : CONFLICT_SOURCE;
        reader->needed = reader->source_length == 0 ? reader->target_length : reader->source_length;
    }
    else if (reader->stage == CONFLICT_SOURCE)
    {
        reader->stage = CONFLICT_TARGET;
        reader->needed = reader->target_length;
    }
    else if (reader->stage == CONFLICT_TARGET)
    {
        if (DtfileConflictSetItem(&reader->batch, reader->row, reader->index,
                                  reader->flags, reader->source, reader->source_length,
                                  reader->target, reader->target_length) != 0) return -1;
        free(reader->source); reader->source = NULL;
        free(reader->target); reader->target = NULL;
        ++reader->row;
        reader->stage = reader->row == reader->batch.count ? CONFLICT_TRAILER : CONFLICT_ITEM_HEADER;
        reader->needed = reader->stage == CONFLICT_TRAILER ? sizeof(uint32_t) : 4 * sizeof(uint32_t);
    }
    else if (reader->stage == CONFLICT_TRAILER)
    {
        if (ntohl(reader->words[0]) != CONFLICT_END_MAGIC ||
            reader->batch.native_bytes != reader->expected_bytes)
        { errno = EINVAL; return -1; }
        if (DtfileConflictBatchValidate(&reader->batch) != 0) return -1;
        reader->stage = CONFLICT_READY;
        reader->needed = 0;
    }
    reader->used = 0;
    return 0;
}

int
DtfileConflictReadAvailable(DtfileConflictReader *reader, int fd, size_t byte_budget)
{
    size_t attempts = 128;
    if (reader == NULL || byte_budget == 0 || reader->stage == CONFLICT_TAKEN)
    { errno = EINVAL; return -1; }
    if (reader->stage == CONFLICT_FAILED) { errno = reader->error; return -1; }
    if (reader->stage == CONFLICT_READY) return 1;
    /* Bound one dispatch even if a caller supplies a larger budget. */
    if (byte_budget > 64u * 1024u) byte_budget = 64u * 1024u;
    while (byte_budget != 0 && attempts-- != 0)
    {
        struct pollfd descriptor = {fd, POLLIN, 0};
        unsigned char *destination;
        size_t amount = reader->needed - reader->used;
        ssize_t received;
        int ready = poll(&descriptor, 1, 0);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) goto failed;
        if (ready == 0) return 0;
        if (descriptor.revents & POLLNVAL) { errno = EBADF; goto failed; }
        if (!(descriptor.revents & (POLLIN | POLLHUP | POLLERR))) return 0;
        destination = reader->stage == CONFLICT_SOURCE ? (unsigned char *)reader->source :
                      reader->stage == CONFLICT_TARGET ? (unsigned char *)reader->target :
                      (unsigned char *)reader->words;
        if (amount > byte_budget) amount = byte_budget;
        received = read(fd, destination + reader->used, amount);
        if (received < 0 && errno == EINTR) continue;
        if (received < 0 && errno == EAGAIN) return 0;
        if (received <= 0 || (size_t)received > amount)
        { if (received >= 0) errno = EIO; goto failed; }
        reader->used += (size_t)received;
        byte_budget -= (size_t)received;
        if (reader->used == reader->needed && ConflictReaderAdvance(reader) != 0)
            goto failed;
        if (reader->stage == CONFLICT_READY) return 1;
    }
    return 0;
failed:
    reader->error = errno;
    reader->stage = CONFLICT_FAILED;
    return -1;
}

int
DtfileConflictReaderTake(DtfileConflictReader *reader, DtfileConflictBatch *batch)
{
    if (reader == NULL || batch == NULL || reader->stage != CONFLICT_READY ||
        batch->operation != 0 || batch->selected_count != 0 ||
        batch->count != 0 || batch->native_bytes != 0 || batch->items != NULL)
    { errno = EINVAL; return -1; }
    *batch = reader->batch;
    memset(&reader->batch, 0, sizeof(reader->batch));
    reader->stage = CONFLICT_TAKEN;
    return 0;
}
