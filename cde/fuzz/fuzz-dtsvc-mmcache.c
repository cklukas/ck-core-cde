/*
 * Fuzz target: the memory-mapped type database cache of libDtSvc
 * (the ~/.dt/.dt_db_cache.* or /tmp/dtdbcache_* file).
 *
 * run-fuzz.py cuts the cache reader out of DtUtil1/DtsMM.c (the header check
 * cache_header_valid/index_inside, _DtDtsMMapDB, _DtDtsMMUnLoad and the
 * accessors _DtDtsMMGetPtr, GetDCNameIndex, GetNoNameIndex, GetBufferIndex,
 * GetFileList, GetDbName, Get, GetRecordByName, GetField, GetFieldByName,
 * String/BosonToString) and the table readers _DtShmStringToBoson,
 * _DtShmBosonToString and _DtShmFindIntTabEntry out of strtab.c and
 * inttab.c, verbatim.
 *
 * Stand-ins:
 *   - the input is written to a file below TMPDIR, so that _DtDtsMMapDB opens
 *     and fstat()s a real file owned by the user, as in the library;
 *   - mmap() returns a malloc()ed copy of the file of exactly the file size
 *     and munmap() frees it.  The real mmap rounds up to a page and reads
 *     past the end of the file give zeros there; the copy lets AddressSanitizer
 *     see every access outside the file;
 *   - _DtDtsMMInit (the loader that rebuilds a missing cache) is never needed
 *     after a successful map, so it only aborts; _DtDtsClear and the locks and
 *     the error reporter do nothing.
 *
 * Input: the content of the cache file.  After a successful map the harness
 * does what the consumers of the cache do: it walks every database, every
 * record and every field, resolves their names and values through the string
 * table, looks records and fields up by name, and reads the name index, the
 * no-name index, the buffer index and the file list.  The walk is capped at
 * 4096 items per list so that an invalid count ends in an out-of-bounds
 * read, not in a long loop.
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define TRUE 1
#define FALSE 0
#define DtProgName ((char *)0)
#define DtError 1
#define _DtSvcProcessLock() ((void)0)
#define _DtSvcProcessUnlock() ((void)0)
typedef char *caddr_t_;

static void _DtSimpleError(char *program, int severity, char *help, char *format, ...)
{
    (void)program; (void)severity; (void)help; (void)format;
}
static void _DtDtsClear(void) {}

/* A copy of the file in place of the mapping. */
static void *fuzz_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    (void)addr; (void)prot; (void)flags;
    char *copy = malloc(len);
    if (!copy || off != 0 || pread(fd, copy, len, 0) != (ssize_t)len)
        return MAP_FAILED;
    return copy;
}
static int fuzz_munmap(void *addr, size_t len) { (void)len; free(addr); return 0; }
#define mmap fuzz_mmap
#define munmap fuzz_munmap

typedef struct DtDirPathsTag DtDirPaths;
int _DtDtsMMInit(int override);
int _DtDtsMMUnLoad(void);

#include FUZZ_TYPES_H
#include FUZZ_PRODUCTION_H

int _DtDtsMMInit(int override) { (void)override; abort(); }

static char cache_path[MAXPATHLEN];

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    const char *tmp = getenv("TMPDIR");
    snprintf(cache_path, sizeof cache_path, "%s/dt_db_cache.%d", tmp ? tmp : ".", (int)getpid());
    return 0;
}

#define CAP(n) ((n) > 4096 ? 4096 : (n))

static size_t touch(const char *s) { return s ? strlen(s) : 0; }

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FILE *f = fopen(cache_path, "w");
    if (!f) abort();
    if (size) fwrite(data, 1, size, f);
    fclose(f);

    if (!_DtDtsMMapDB(cache_path))
        return 0;

    size_t sink = 0;
    int n;
    int *idx;
    sink += (size_t)_DtDtsMMGetDCNameIndex(&n);
    idx = _DtDtsMMGetDCNameIndex(&n);
    for (int i = 0; i < CAP(n); i++) sink += idx[i];
    idx = _DtDtsMMGetNoNameIndex(&n);
    for (int i = 0; i < CAP(n); i++) sink += idx[i];
    idx = _DtDtsMMGetBufferIndex(&n);
    for (int i = 0; i < CAP(n); i++) sink += idx[i];
    sink += (size_t)_DtDtsMMGetFileList();
    static const char *names[] = { "DATA_ATTRIBUTES", "DATA_CRITERIA", "ACTION", "x" };
    static const char *fields[] = { "ICON", "DESCRIPTION", "NAME_PATTERN", "ACTIONS", "x" };

    for (int d = 0; d < CAP(head->num_db); d++) {
        DtDtsMMDatabase *db = &db_list[d];
        sink += touch(_DtDtsMMBosonToString(db->databaseName));
        for (int k = 0; k < 4; k++) {
            sink += (size_t)_DtDtsMMGet(names[k]);
            sink += (size_t)_DtDtsMMGetRecordByName(db, names[k]);
        }
        for (int k = 0; k < 8; k++) {
            int *e = _DtDtsMMGetDbName(db, (DtShmBoson)k);
            if (e) sink += *e;
        }
        DtDtsMMRecord *recs = _DtDtsMMGetPtr(db->recordList);
        for (int r = 0; r < CAP(db->recordCount); r++) {
            DtDtsMMRecord *rec = &recs[r];
            sink += touch(_DtDtsMMBosonToString(rec->recordName));
            sink += touch(_DtDtsMMBosonToString(rec->pathId));
            DtDtsMMField *flds = _DtDtsMMGetPtr(rec->fieldList);
            for (int i = 0; i < CAP(rec->fieldCount); i++) {
                sink += touch(_DtDtsMMBosonToString(flds[i].fieldName));
                sink += touch(_DtDtsMMBosonToString(flds[i].fieldValue));
            }
            for (int k = 0; k < 5; k++)
                sink += touch(_DtDtsMMGetFieldByName(rec, fields[k]));
        }
    }
    _DtDtsMMUnLoad();
    return (int)(sink & 0) ;
}
