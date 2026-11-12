/*
 * Behavioural test for _DtAddEntry() in DtUtil2/addToRes.c.
 *
 * The runner (check-addtores-alloc.py) prepends this file's prologue to the
 * type definitions and the production functions _DtAllocEntries(),
 * _DtFreeEntries() and _DtAddEntry(), which it extracts verbatim from
 * addToRes.c, and appends the test body (after the body marker).  realloc is
 * redirected to a wrapper that can be told to fail on the Nth call, and free
 * counts the blocks released.
 *
 * The runner splits this file at its two @@ marker comments.
 */

/* @@PROLOGUE@@ */
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int Bool;
#define True 1
#define False 0

static int realloc_calls;
static int realloc_fail_at;	/* 1-based call number that fails; 0 = never */
static int frees;

static void *ck_realloc(void *p, size_t n)
{
    ++realloc_calls;
    if (realloc_fail_at && realloc_calls == realloc_fail_at)
        return NULL;		/* the old block stays valid, as in libc */
    return realloc(p, n);
}

static void ck_free(void *p)
{
    if (p)
        ++frees;
    free(p);
}
#define realloc ck_realloc
#define free ck_free

/* @@BODY@@ */
static Entry make(const char *tag, const char *value)
{
    Entry e = { NULL, NULL, 0, False };
    e.tag = malloc(strlen(tag) + 1);
    e.value = malloc(strlen(value) + 1);
    assert(e.tag && e.value);
    strcpy(e.tag, tag);
    strcpy(e.value, value);
    return e;
}

int main(void)
{
    Entries *e = _DtAllocEntries();
    char tag[32];
    int i, before;

    assert(e->room == INIT_ENTRY_SIZE && e->used == 0);
    for (i = 0; i < INIT_ENTRY_SIZE; i++) {
        snprintf(tag, sizeof tag, "tag%04d", i);
        _DtAddEntry(e, make(tag, "v"));
    }
    assert(e->used == e->room);

    /* The table is full and the realloc fails: the table stays intact. */
    realloc_calls = 0;
    realloc_fail_at = 1;
    before = frees;
    _DtAddEntry(e, make("overflow", "lost"));
    assert(realloc_calls == 1);
    assert(e->room == INIT_ENTRY_SIZE && e->used == INIT_ENTRY_SIZE);
    assert(frees == before + 2);	/* the dropped entry was released */
    for (i = 0; i < INIT_ENTRY_SIZE; i++) {
        snprintf(tag, sizeof tag, "tag%04d", i);
        assert(strcmp(e->entry[i].tag, tag) == 0);
        assert(strcmp(e->entry[i].value, "v") == 0);
    }

    /* An entry for a tag already present still replaces it. */
    realloc_calls = 0;
    realloc_fail_at = 1;
    _DtAddEntry(e, make("tag0007", "new"));
    assert(realloc_calls == 0);
    assert(strcmp(e->entry[7].value, "new") == 0);

    /* Without injection the table doubles and takes the entry. */
    realloc_calls = 0;
    realloc_fail_at = 0;
    _DtAddEntry(e, make("overflow", "kept"));
    assert(realloc_calls == 1);
    assert(e->room == 2 * INIT_ENTRY_SIZE && e->used == INIT_ENTRY_SIZE + 1);
    assert(strcmp(e->entry[INIT_ENTRY_SIZE].tag, "overflow") == 0);
    assert(e->entry[INIT_ENTRY_SIZE].usable == True);

    _DtFreeEntries(e);
    puts("addToRes entry table: a failed growth keeps the table intact");
    return 0;
}
