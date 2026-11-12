/*
 * Behavioural test for build_file_list() and build_name_list() in
 * DtUtil1/MMDb.c.
 *
 * The runner (check-mmdb-alloc.py) prepends this file's prologue to the
 * production code, which it extracts verbatim from MMDb.c, and appends the
 * test body (after the body marker).  The shared-memory table routines the
 * code builds on are replaced by simple models, and realloc is redirected to
 * a wrapper that can be told to fail on the Nth call.  A revision whose
 * build_file_list() still returns nothing is run without STATUS_RETURNED.
 *
 * The runner splits this file at its two @@ marker comments.
 */

/* @@PROLOGUE@@ */
#include <assert.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

typedef int DtShmBoson;
typedef struct { char **dirs; char **paths; } DtDirPaths;
typedef struct { int files_offset, files_count, mtimes_offset,
                 buffer_start_index, no_name_offset, name_list_offset; } DtDtsMMHeader;
typedef struct { char *name; } DtDtsDbRecord;
typedef struct { int recordCount; DtDtsDbRecord **recordList; } DtDtsDbDatabase;
typedef int _Xreaddirparams;
#define _XReaddir(dirp, buf) readdir(dirp)
#define DtDTS_DT_UNKNOWN "unknown"
#define DtDTS_DT_RECURSIVE_LINK "recursive"
#define DtDTS_DT_BROKEN_LINK "broken"
#define DtDTS_NAME_PATTERN 1
#define DtDTS_PATH_PATTERN 2

/* Models of the string table, the integer list and the integer table. */
typedef int *DtShmProtoIntList;
typedef int DtShmProtoStrtab;
typedef struct { int boson, value; } Pair;
typedef struct { Pair pairs[64]; int n; } Table;
typedef Table *DtShmProtoInttab;

static char *strings[256];
static int nstrings;
static DtShmBoson _DtShmProtoAddStrtab(DtShmProtoStrtab h, const char *s, int *isnew)
{
    int i;
    (void)h;
    for (i = 0; i < nstrings; i++)
        if (strcmp(strings[i], s) == 0) { *isnew = 0; return i + 1; }
    strings[nstrings] = strdup(s);
    *isnew = 1;
    return ++nstrings;
}

static int int_area[4096];
static int int_used;
static void *_DtShmProtoAddIntLst(DtShmProtoIntList h, int n, int *offset)
{
    void *p = &int_area[int_used];
    (void)h;
    *offset = int_used;
    int_used += n;
    assert(int_used <= 4096);
    return p;
}

static DtShmProtoInttab _DtShmProtoInitInttab(int n)
{
    (void)n;
    return calloc(1, sizeof(Table));
}
static void _DtShmProtoAddInttab(DtShmProtoInttab t, int boson, int value)
{
    assert(t->n < 64);
    t->pairs[t->n].boson = boson;
    t->pairs[t->n++].value = value;
}
static int _DtShmProtoSizeInttab(DtShmProtoInttab t) { return t->n * 2 * sizeof(int); }
static void _DtShmProtoCopyInttab(DtShmProtoInttab t, void *space) { memcpy(space, t->pairs, t->n * sizeof(Pair)); }
static int inttab_destroyed;
static void _DtShmProtoDestroyInttab(DtShmProtoInttab t) { ++inttab_destroyed; free(t); }

static char *_DtDtsDbGetFieldByName(DtDtsDbRecord *r, int field)
{
    return field == DtDTS_NAME_PATTERN ? r->name : NULL;
}

static int realloc_calls;
static int realloc_fail_at;	/* 1-based call number that fails; 0 = never */

static void *ck_realloc(void *p, size_t n)
{
    ++realloc_calls;
    if (realloc_fail_at && realloc_calls == realloc_fail_at)
        return NULL;		/* the old block stays valid, as in libc */
    return realloc(p, n);
}
#define realloc ck_realloc

static DtShmProtoStrtab shm_handle;
static DtShmProtoIntList int_handle;

/* @@BODY@@ */
static void arm(int fail_at)
{
    realloc_calls = 0;
    realloc_fail_at = fail_at;
}

static void reset_models(void)
{
    int i;
    for (i = 0; i < nstrings; i++)
        free(strings[i]);
    nstrings = 0;
    int_used = 0;
    memset(int_area, 0, sizeof int_area);
}

/* 1 if the scan succeeded, 0 if it failed. */
static int scan(DtDirPaths *dirs, DtDtsMMHeader *header)
{
    memset(header, 0, sizeof *header);
#ifdef STATUS_RETURNED
    return build_file_list(int_handle, dirs, header, ".dt");
#else
    build_file_list(int_handle, dirs, header, ".dt");
    return 1;
#endif
}

static void test_file_list(const char *base)
{
    char d1[2 * MAXPATHLEN], d2[2 * MAXPATHLEN], cwd[MAXPATHLEN], cwd2[MAXPATHLEN], f[3 * MAXPATHLEN];
    char *paths[3];
    DtDirPaths dirs;
    DtDtsMMHeader header;
    int i;

    snprintf(d1, sizeof d1, "%s/dir1", base);
    snprintf(d2, sizeof d2, "%s/dir2", base);
    mkdir(base, 0700);
    mkdir(d1, 0700);
    mkdir(d2, 0700);
    for (i = 0; i < 3; i++) {
        FILE *fp;
        snprintf(f, sizeof f, "%s/f%d.dt", i < 2 ? d1 : d2, i);
        fp = fopen(f, "w");
        assert(fp);
        fclose(fp);
    }
    paths[0] = d1; paths[1] = d2; paths[2] = NULL;
    dirs.dirs = dirs.paths = paths;
    getcwd(cwd, sizeof cwd);

    /* Control: two directories and three files make five entries. */
    arm(0);
    assert(scan(&dirs, &header) == 1);
    assert(header.files_count == 5);
    assert(realloc_calls == 10);	/* two lists, five entries */
    getcwd(cwd2, sizeof cwd2);
    assert(strcmp(cwd, cwd2) == 0);

    /* Every one of those ten reallocs may fail in turn. */
    for (i = 1; i <= 10; i++) {
        reset_models();
        arm(i);
        assert(scan(&dirs, &header) == 0);
        assert(realloc_calls == i);
        getcwd(cwd2, sizeof cwd2);
        assert(strcmp(cwd, cwd2) == 0);
    }
}

static DtDtsDbRecord recs[8];
static DtDtsDbRecord *rec_ptrs[8];

static void test_name_list(void)
{
    /* the names: a b b c c c d e -> lists of 1, 2, 3, 1, 1 records */
    static const char *names[8] = { "a", "b", "b", "c", "c", "c", "d", "e" };
    DtDtsDbDatabase db;
    DtDtsMMHeader head;
    int i, calls, r, ref_tbl[64], ref_n;

    for (i = 0; i < 8; i++) {
        recs[i].name = (char *)names[i];
        rec_ptrs[i] = &recs[i];
    }
    db.recordCount = 8;
    db.recordList = rec_ptrs;

    /* Control: the result and the number of reallocs without injection. */
    reset_models();
    memset(&head, 0, sizeof head);
    arm(0);
    r = build_name_list(&db, int_handle, &head);
    assert(r >= 0);
    calls = realloc_calls;
    assert(calls == 6);	/* three growths, two shrinks, one first block */
    assert(head.name_list_offset >= 0);
    ref_n = int_used;
    assert(ref_n > 0 && ref_n <= 64);
    memcpy(ref_tbl, int_area, ref_n * sizeof(int));

    /*
     * A failure of a realloc that makes room for another record fails the
     * build; a failure of one that only shrinks the list to a single record
     * keeps the larger block and does not change the result.  The first
     * and the third growth and the shrinks are told apart by the outcome.
     */
    for (i = 1; i <= calls; i++) {
        int tables = inttab_destroyed, status;

        reset_models();
        memset(&head, 0, sizeof head);
        arm(i);
        status = build_name_list(&db, int_handle, &head);
        if (status < 0) {
            assert(realloc_calls == i);
            assert(inttab_destroyed == tables + 1);
        } else {
            assert(realloc_calls == calls);
            assert(int_used == ref_n);
            assert(memcmp(int_area, ref_tbl, ref_n * sizeof(int)) == 0);
        }
    }
}

int main(int argc, char **argv)
{
    /* argv[2] runs one part only: "file-list" or "name-list" */
    assert(argc >= 2);
    if (argc < 3 || strcmp(argv[2], "file-list") == 0)
        test_file_list(argv[1]);
    if (argc < 3 || strcmp(argv[2], "name-list") == 0)
        test_name_list();
    puts("MMDb: allocation failures end the scan and the index build cleanly");
    return 0;
}
