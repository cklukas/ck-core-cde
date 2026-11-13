/*
 * Harness for check-tree-list-alloc.py: the creation and insertion functions
 * of the calendar daemon's tree and list (tree.c, list.c) with calloc
 * failing.  The runner puts this file's prologue in front of the production
 * functions, which it extracts verbatim, and appends the body.  calloc and
 * free are redirected to wrappers that keep a table of the live blocks, so
 * that the Nth allocation can fail and a leak is observable.
 */
/* @@PROLOGUE@@ */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

typedef int boolean_t;
#define nullptr ((void *) 0)

typedef enum {
	rb_ok		= 0,
	rb_duplicate	= 1,
	rb_badtable	= 2,
	rb_notable	= 3,
	rb_failed	= 4,
	rb_other	= 5,
	rb_nomem	= 6
} Rb_Status;

typedef enum { _DtCmsIsLess, _DtCmsIsEqual, _DtCmsIsGreater } _DtCmsComparisonResult;
typedef caddr_t (*_DtCmsGetKeyProc)(caddr_t data);
typedef _DtCmsComparisonResult (*_DtCmsCompareProc)(caddr_t key, caddr_t data);
typedef boolean_t (*_DtCmsEnumerateProc)(caddr_t node, caddr_t data);
typedef struct RepeatEvent RepeatEvent;

typedef enum {red=0, black=1} Color;
typedef struct node {
	struct node *llink;
	struct node *rlink;
	Color	color;
	caddr_t	data;
} Tree_node;
typedef struct {
	Tree_node *root;
	caddr_t private;
} Rb_tree;

typedef struct lnode {
	struct lnode	*llink;
	struct lnode	*rlink;
	caddr_t		data;
	time_t		lasttick;
	int		duration;
	RepeatEvent	*re;
} List_node;
typedef struct {
	List_node	*root;
	caddr_t	private;
} Hc_list;
#define	hc_lookup_next(p_node)		(p_node)->rlink

#define CK_MAX_BLOCKS 256
static void *ck_blocks[CK_MAX_BLOCKS];
static int ck_live;
static int ck_calls;
static int ck_fail_at;		/* 1-based call that fails; 0 = never */

static void *ck_calloc(size_t n, size_t size)
{
	void *p;
	int i;

	if (ck_fail_at && ++ck_calls == ck_fail_at)
		return NULL;
	p = calloc(n, size);
	for (i = 0; p && i < CK_MAX_BLOCKS; i++)
		if (ck_blocks[i] == NULL) {
			ck_blocks[i] = p;
			ck_live++;
			return p;
		}
	assert(p == NULL);
	return p;
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

static void arm(int fail_at)
{
	ck_calls = 0;
	ck_fail_at = fail_at;
}

#define calloc ck_calloc
#define free ck_free

/* The insertion into a tree is not under test: only the node's allocation. */
static Rb_Status rb_insert_node(Rb_tree *tree, Tree_node *node, caddr_t key)
{
	return rb_ok;
}

/* @@BODY@@ */
static _DtCmsComparisonResult cmp(caddr_t key, caddr_t data)
{
	return key == data ? _DtCmsIsEqual : (key < data ? _DtCmsIsLess : _DtCmsIsGreater);
}

static void test_list(void)
{
	Hc_list *l;
	List_node *node = nullptr;
	int k;

	/* every allocation of hc_create fails in turn: no result, no leak */
	for (k = 1; k <= 2; k++) {
		arm(k);
		assert(hc_create(NULL, cmp) == NULL);
		assert(ck_live == 0);
	}
	arm(0);
	l = hc_create(NULL, cmp);
	assert(l != NULL && ck_live == 2);

	/* the node cannot be allocated: nothing is inserted */
	arm(1);
	assert(hc_insert(l, (caddr_t) 1, (caddr_t) 1, NULL, &node) == rb_nomem);
	assert(node == NULL && l->root == NULL && ck_live == 2);

	arm(0);
	assert(hc_insert(l, (caddr_t) 1, (caddr_t) 1, NULL, &node) == rb_ok);
	assert(node != NULL && l->root == node && ck_live == 3);
}

static void test_tree(void)
{
	Rb_tree *t;
	int k, base = ck_live;

	/* the five allocations of rb_create fail in turn: no result, no leak */
	for (k = 1; k <= 5; k++) {
		arm(k);
		assert(rb_create(NULL, cmp) == NULL);
		assert(ck_live == base);
	}
	arm(0);
	t = rb_create(NULL, cmp);
	assert(t != NULL && ck_live == base + 5);

	/* the node of an insertion cannot be allocated */
	arm(1);
	assert(rb_insert(t, (caddr_t) 1, (caddr_t) 1) == rb_nomem);
	assert(ck_live == base + 5);
	arm(0);
	assert(rb_insert(t, (caddr_t) 1, (caddr_t) 1) == rb_ok);
}

int main(void)
{
	test_list();
	test_tree();
	puts("tree and list allocation failures are reported");
	return 0;
}
