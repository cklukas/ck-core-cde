/*
 * Harness for check-name-table-alloc.py: the hash table (hash.c) and the name
 * table built on it (nametbl.c) of libcsa with every allocation failing in
 * turn.  The runner puts this file's prologue in front of the production
 * functions, which it extracts verbatim, and appends the body.  malloc,
 * calloc, strdup, reallocarray and free are redirected to wrappers that keep
 * a table of the live blocks, so that a leak is observable and the Nth
 * allocation can fail.
 */
/* @@PROLOGUE@@ */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define nullptr ((void *) 0)
typedef unsigned int CSA_return_code;
#define CSA_SUCCESS			((CSA_return_code) 0)
#define CSA_E_INSUFFICIENT_MEMORY	((CSA_return_code) 5)
#define CSA_E_INVALID_PARAMETER		((CSA_return_code) 9)
#define MAX_CAL_TBL_SIZE 23
typedef int (*_DtCmHashDestroyProc)(void *data, void *user_arg, unsigned char *key);
typedef struct {
	void	*tbl;
	int	size;
	char	**names;
} _DtCmNameTable;

void _DtCm_free_name_table(_DtCmNameTable *tbl);

#define CK_MAX_BLOCKS 512
static void *ck_blocks[CK_MAX_BLOCKS];
static int ck_live;
static int ck_calls;
static int ck_fail_at;		/* 1-based allocation that fails; 0 = never */

static void *ck_track(void *p)
{
	int i;

	for (i = 0; p && i < CK_MAX_BLOCKS; i++)
		if (ck_blocks[i] == NULL) {
			ck_blocks[i] = p;
			ck_live++;
			return p;
		}
	assert(p == NULL);
	return p;
}

static int ck_fails(void)
{
	return ck_fail_at && ++ck_calls == ck_fail_at;
}

static void *ck_malloc(size_t n)
{
	return ck_fails() ? NULL : ck_track(malloc(n));
}

static void *ck_calloc(size_t n, size_t s)
{
	return ck_fails() ? NULL : ck_track(calloc(n, s));
}

static char *ck_strdup(const char *s)
{
	return ck_fails() ? NULL : ck_track(strdup(s));
}

static void ck_free(void *p)
{
	int i;

	for (i = 0; p && i < CK_MAX_BLOCKS; i++)
		if (ck_blocks[i] == p) {
			ck_blocks[i] = NULL;
			ck_live--;
			break;
		}
	free(p);
}

static void *ck_reallocarray(void *old, size_t n, size_t s)
{
	void *p;
	int i;

	if (ck_fails())
		return NULL;		/* the old block stays valid */
	if (s && n > (size_t) -1 / s)
		return NULL;
	p = realloc(old, n * s);
	for (i = 0; p && old && i < CK_MAX_BLOCKS; i++)
		if (ck_blocks[i] == old) {
			ck_blocks[i] = p;
			return p;
		}
	return ck_track(p);
}

static void arm(int fail_at)
{
	ck_calls = 0;
	ck_fail_at = fail_at;
}

#define malloc ck_malloc
#define calloc ck_calloc
#define strdup ck_strdup
#define free ck_free
#define reallocarray ck_reallocarray

/* @@BODY@@ */
static char *names[] = { NULL, "alpha", "beta", "gamma" };

static void test_hash(void)
{
	void *t;
	void **slot;
	int k;

	/* both allocations of the table fail in turn: no table, no leak */
	for (k = 1; k <= 2; k++) {
		arm(k);
		assert(_DtCmMakeHash(7) == NULL);
		assert(ck_live == 0);
	}
	arm(0);
	t = _DtCmMakeHash(7);
	assert(t != NULL && ck_live == 2);

	/* the entry, then its copy of the key cannot be allocated */
	for (k = 1; k <= 2; k++) {
		arm(k);
		assert(_DtCmGetHash(t, (const unsigned char *) "key") == NULL);
		assert(ck_live == 2);
		assert(_DtCmFindHash(t, (const unsigned char *) "key") == NULL);
	}
	arm(0);
	slot = _DtCmGetHash(t, (const unsigned char *) "key");
	assert(slot != NULL && *slot == NULL && ck_live == 4);
	_DtCmDestroyHash(t, NULL, NULL);
	assert(ck_live == 0);
}

static void test_make_name_table(void)
{
	_DtCmNameTable *t;
	int k, total;

	/* count the allocations of a complete run */
	arm(1000000);
	t = _DtCm_make_name_table(3, names);
	assert(t != NULL);
	total = ck_calls;
	_DtCm_free_name_table(t);
	assert(ck_live == 0);

	/* every one of them fails in turn: no table, no leak */
	for (k = 1; k <= total; k++) {
		arm(k);
		assert(_DtCm_make_name_table(3, names) == NULL);
		assert(ck_live == 0);
	}
}

static void test_add_name(void)
{
	_DtCmNameTable *t;
	int k, total;

	arm(0);
	t = _DtCm_make_name_table(3, names);
	assert(t != NULL);

	/* count the allocations of adding a new name */
	arm(1000000);
	assert(_DtCm_add_name_to_table(t, 0, "delta") == CSA_SUCCESS);
	total = ck_calls;
	assert(t->size == 4 && strcmp(t->names[4], "delta") == 0);

	/* each of them fails in turn: an error, and the table stays usable */
	for (k = 1; k <= total; k++) {
		char name[16];
		int size = t->size;

		snprintf(name, sizeof name, "extra%d", k);
		arm(k);
		assert(_DtCm_add_name_to_table(t, 0, name) ==
		       CSA_E_INSUFFICIENT_MEMORY);
		assert(t->size == size);
		assert(_DtCm_get_index_from_table(t, "alpha") == 1);
	}
	arm(0);
	_DtCm_free_name_table(t);
	assert(ck_live == 0);
}

int main(void)
{
	test_hash();
	test_make_name_table();
	test_add_name();
	puts("name table allocation failures are reported");
	return 0;
}
