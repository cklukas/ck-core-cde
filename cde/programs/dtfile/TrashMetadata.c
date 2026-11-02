/* Read the length-delimited Trash format without trusting header arithmetic. */
#include "TrashMetadata.h"
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void DtfileFreeTrashMetadata(DtfileTrashMetadata *record)
{
    free(record->external);
    free(record->filename);
    free(record->deletion_date);
    memset(record, 0, sizeof(*record));
}

static int ReadLength(FILE *stream, size_t *length, int first)
{
    int ch = fgetc(stream);
    size_t value = 0;
    if (ch == EOF)
        return ferror(stream) ? DTFILE_TRASH_IO_ERROR :
               first ? DTFILE_TRASH_END : DTFILE_TRASH_INVALID;
    if (ch < '0' || ch > '9')
        return DTFILE_TRASH_INVALID;
    do {
        unsigned int digit = (unsigned int)(ch - '0');
        if (value > (SIZE_MAX - digit) / 10)
            return DTFILE_TRASH_INVALID;
        value = value * 10 + digit;
        ch = fgetc(stream);
    } while (ch >= '0' && ch <= '9');
    if (ch == EOF && ferror(stream))
        return DTFILE_TRASH_IO_ERROR;
    if (ch != ' ' || value == 0 || value == SIZE_MAX)
        return DTFILE_TRASH_INVALID;
    *length = value;
    return DTFILE_TRASH_RECORD;
}

static int ReadField(FILE *stream, size_t length, char **field)
{
    struct stat info;
    off_t position;
    char *data;

    /* A regular file cannot supply more bytes than it has left. Reject the
     * common corrupt huge-length header before attempting its allocation.
     */
    if (fstat(fileno(stream), &info) == 0 && S_ISREG(info.st_mode)) {
        position = ftello(stream);
        if (position < 0)
            return DTFILE_TRASH_IO_ERROR;
        if (position > info.st_size ||
            (uintmax_t)length > (uintmax_t)(info.st_size - position))
            return DTFILE_TRASH_INVALID;
    }
    data = malloc(length + 1);
    if (!data)
        return DTFILE_TRASH_NO_MEMORY;
    if (fread(data, 1, length, stream) != length) {
        int status = ferror(stream) ? DTFILE_TRASH_IO_ERROR : DTFILE_TRASH_INVALID;
        free(data);
        return status;
    }
    if (memchr(data, '\0', length)) {
        free(data);
        return DTFILE_TRASH_INVALID;
    }
    data[length] = '\0';
    *field = data;
    return DTFILE_TRASH_RECORD;
}

static int ReadAttributes(FILE *stream, DtfileTrashMetadata *record)
{
    char *line = NULL, *token, *state;
    size_t capacity = 0;
    ssize_t length;
    int status = DTFILE_TRASH_RECORD;

    errno = 0;
    length = getline(&line, &capacity, stream);
    if (length < 0) {
        free(line);
        return errno == ENOMEM ? DTFILE_TRASH_NO_MEMORY :
               ferror(stream) ? DTFILE_TRASH_IO_ERROR : DTFILE_TRASH_INVALID;
    }
    if (memchr(line, '\0', (size_t)length)) {
        free(line);
        return DTFILE_TRASH_INVALID;
    }
    for (token = strtok_r(line, " \t\r\n", &state); token;
         token = strtok_r(NULL, " \t\r\n", &state)) {
        if (strncmp(token, "DeletionDate=", 13) == 0) {
            if (record->deletion_date || token[13] == '\0') {
                status = DTFILE_TRASH_INVALID;
                break;
            }
            record->deletion_date = strdup(token + 13);
            if (!record->deletion_date) {
                status = DTFILE_TRASH_NO_MEMORY;
                break;
            }
        } else if (strncmp(token, "RecSize=", 8) == 0) {
            unsigned long long value = 0;
            char *digit = token + 8;
            if (record->rec_size_valid || !*digit) {
                status = DTFILE_TRASH_INVALID;
                break;
            }
            for (; *digit; ++digit) {
                if (*digit < '0' || *digit > '9' ||
                    value > (ULLONG_MAX - (unsigned int)(*digit - '0')) / 10) {
                    status = DTFILE_TRASH_INVALID;
                    break;
                }
                value = value * 10 + (unsigned int)(*digit - '0');
            }
            if (status != DTFILE_TRASH_RECORD)
                break;
            record->rec_size = value;
            record->rec_size_valid = 1;
        }
    }
    free(line);
    return status;
}

int DtfileReadTrashMetadata(FILE *stream, DtfileTrashMetadata *result)
{
    size_t external_length, filename_length;
    int status, delimiter;
    memset(result, 0, sizeof(*result));
    status = ReadLength(stream, &external_length, 1);
    if (status != DTFILE_TRASH_RECORD)
        return status;
    status = ReadLength(stream, &filename_length, 0);
    if (status != DTFILE_TRASH_RECORD)
        return status;
    status = ReadField(stream, external_length, &result->external);
    if (status != DTFILE_TRASH_RECORD)
        goto failed;
    delimiter = fgetc(stream);
    if (delimiter != ' ')
        goto invalid;
    status = ReadField(stream, filename_length, &result->filename);
    if (status != DTFILE_TRASH_RECORD)
        goto failed;
    /* Internal names are basenames, never paths that can escape Trash. */
    if (strchr(result->filename, '/') || strcmp(result->filename, ".") == 0 ||
        strcmp(result->filename, "..") == 0)
        goto invalid;
    delimiter = fgetc(stream);
    if (delimiter == ' ')
        status = ReadAttributes(stream, result);
    else if (delimiter == '\n' || (delimiter == EOF && !ferror(stream)))
        status = DTFILE_TRASH_RECORD;
    else
        goto invalid;
    if (status == DTFILE_TRASH_RECORD)
        return status;
    goto failed;

invalid:
    status = ferror(stream) ? DTFILE_TRASH_IO_ERROR : DTFILE_TRASH_INVALID;
failed:
    DtfileFreeTrashMetadata(result);
    return status;
}
