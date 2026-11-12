/*
 * dirselect-test.c - tests of dirselect.c, built with the sanitizers.
 *
 * Every buffer is allocated at exactly the size the call is given, so an
 * overrun of any of them is a heap-buffer-overflow report.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "dirselect.h"

#define BUF_SIZE	1024	/* the size of the dialog's directory buffer */
#define MAX_DIR		(BUF_SIZE - DIRSELECT_MASK_LEN - 1)	/* longest stored directory */

static int checks = 0;
static int failures = 0;

#define CHECK(cond, what)						\
    do {								\
	checks++;							\
	if (!(cond)) {							\
	    failures++;							\
	    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (what)); \
	}								\
    } while (0)

static char *
exact(size_t size)
{
    char *p = malloc(size);

    if (p == NULL) {
	fprintf(stderr, "out of memory\n");
	exit(2);
    }
    return p;
}

static void
test_plain_directories(void)
{
    char *buf = exact(BUF_SIZE);
    size_t len;

    CHECK(DirSelectStore(buf, BUF_SIZE, "/usr/local", 10) == 1,
	  "a directory without a trailing slash is stored");
    CHECK(strcmp(buf, "/usr/local/") == 0, "a slash is appended");

    CHECK(DirSelectStore(buf, BUF_SIZE, "/usr/local/", 11) == 1,
	  "a directory with a trailing slash is stored");
    CHECK(strcmp(buf, "/usr/local/") == 0, "a trailing slash is not doubled");

    CHECK(DirSelectStore(buf, BUF_SIZE, "/a/b/c.txt", 5) == 1,
	  "a file name prefix is stored up to its slash");
    CHECK(strcmp(buf, "/a/b/") == 0, "the prefix ends in the slash");

    len = DirSelectAppendMask(buf, BUF_SIZE);
    CHECK(len == 5, "the directory length is returned before the mask");
    CHECK(strcmp(buf, "/a/b/[^.]*") == 0, "the mask follows the directory");

    free(buf);
}

static void
test_empty_and_zero_sizes(void)
{
    char *buf = exact(BUF_SIZE);

    buf[0] = 'X';
    CHECK(DirSelectStore(buf, BUF_SIZE, "", 0) == 0,
	  "an empty text is not a directory");
    CHECK(buf[0] == '\0', "an empty text leaves an empty directory");

    free(buf);
    buf = exact(1);
    buf[0] = 'X';
    CHECK(DirSelectStore(buf, 0, "/tmp", 4) == 0,
	  "a zero-sized buffer stores nothing");
    CHECK(buf[0] == 'X', "a zero-sized buffer is not written");
    free(buf);
}

static void
test_size_limit(void)
{
    char *text = exact(BUF_SIZE + 8);
    char *buf = exact(BUF_SIZE);
    size_t i;

    /*
     * The longest directory that leaves room for the mask is MAX_DIR bytes,
     * the last of them the slash that is appended to this text.
     */
    memset(text, 'a', MAX_DIR - 1);
    CHECK(DirSelectStore(buf, BUF_SIZE, text, MAX_DIR - 1) == 1,
	  "the longest directory that leaves room for the mask is stored");
    CHECK(strlen(buf) == MAX_DIR, "the longest directory is stored whole");
    CHECK(DirSelectAppendMask(buf, BUF_SIZE) == MAX_DIR,
	  "the mask is appended to the longest directory");
    CHECK(strlen(buf) == MAX_DIR + DIRSELECT_MASK_LEN,
	  "the mask fits after the longest directory");

    /* One byte more, with the slash it needs, does not fit. */
    memset(text, 'a', MAX_DIR);
    CHECK(DirSelectStore(buf, BUF_SIZE, text, MAX_DIR) == 0,
	  "a directory that leaves no room for the mask is refused");
    CHECK(buf[0] == '\0', "a refused directory leaves dst empty");

    /* A trailing slash is not added to a text that already has one. */
    memset(text, 'a', MAX_DIR);
    text[MAX_DIR - 1] = '/';
    CHECK(DirSelectStore(buf, BUF_SIZE, text, MAX_DIR) == 1,
	  "a long directory that already ends in a slash is stored");

    /* A text far longer than the buffer is refused, not copied. */
    free(text);
    text = exact(20 * BUF_SIZE);
    memset(text, 'b', 20 * BUF_SIZE);
    CHECK(DirSelectStore(buf, BUF_SIZE, text, 20 * BUF_SIZE) == 0,
	  "a text far longer than the buffer is refused");
    CHECK(buf[0] == '\0', "a far-too-long text leaves dst empty");

    /* Every length from 0 to beyond the limit obeys the same rule. */
    for (i = 0; i < BUF_SIZE + 4; i++) {
	size_t total;
	int ok;

	memset(text, 'c', i);
	ok = DirSelectStore(buf, BUF_SIZE, text, i);
	total = i + ((i > 0 && text[i - 1] != '/') ? 1 : 0);
	if (i == 0 || total > MAX_DIR) {
	    CHECK(ok == 0 && buf[0] == '\0', "a directory over the limit is refused");
	} else {
	    CHECK(ok == 1 && strlen(buf) == total,
		  "a directory within the limit is stored whole");
	    CHECK(buf[strlen(buf) - 1] == '/', "a stored directory ends in a slash");
	    CHECK(DirSelectAppendMask(buf, BUF_SIZE) == total,
		  "the mask fits after every stored directory");
	}
    }

    free(text);
    free(buf);
}

static void
test_small_buffers(void)
{
    char *buf = exact(6);	/* only the mask fits, with its NUL */

    CHECK(DirSelectStore(buf, 6, "a", 1) == 0,
	  "a directory does not fit beside the mask in a six-byte buffer");
    CHECK(buf[0] == '\0', "a refused directory in a small buffer is empty");
    free(buf);

    buf = exact(8);
    CHECK(DirSelectStore(buf, 8, "a", 1) == 1,
	  "a one-byte directory fits in an eight-byte buffer");
    CHECK(strcmp(buf, "a/") == 0, "the slash is appended in a small buffer");
    CHECK(DirSelectAppendMask(buf, 8) == 2, "the mask fits in an eight-byte buffer");
    CHECK(strcmp(buf, "a/[^.]*") == 0, "the mask is complete in a small buffer");
    free(buf);

    buf = exact(8);
    strcpy(buf, "abc");
    CHECK(DirSelectAppendMask(buf, 8) == 3,
	  "the mask is not appended when it does not fit");
    CHECK(strcmp(buf, "abc") == 0, "a directory with no room for the mask is unchanged");
    free(buf);
}

static void
test_default_directory(void)
{
    char *buf = exact(BUF_SIZE);
    char *cwd = exact(BUF_SIZE);
    size_t len;

    CHECK(getcwd(cwd, BUF_SIZE) != NULL, "the test can read its directory");
    DirSelectDefault(buf, BUF_SIZE);
    len = strlen(buf);
    CHECK(len > 0 && buf[len - 1] == '/', "the default directory ends in a slash");
    if (strcmp(cwd, "/") == 0)
	CHECK(strcmp(buf, "/") == 0, "the default directory is the root");
    else
	CHECK(strncmp(buf, cwd, strlen(cwd)) == 0 && len == strlen(cwd) + 1,
	      "the default directory is the current directory");

    free(cwd);
    free(buf);

    buf = exact(6);
    buf[0] = 'X';
    DirSelectDefault(buf, 6);
    CHECK(buf[0] == '\0', "no default is stored when the buffer cannot hold one");
    free(buf);

    buf = exact(1);
    buf[0] = 'X';
    DirSelectDefault(buf, 0);
    CHECK(buf[0] == 'X', "a zero-sized buffer is not written by the default");
    free(buf);
}

int
main(void)
{
    test_plain_directories();
    test_empty_and_zero_sizes();
    test_size_limit();
    test_small_buffers();
    test_default_directory();

    if (failures) {
	fprintf(stderr, "dirselect: %d of %d checks failed\n", failures, checks);
	return 1;
    }
    printf("PASS dirselect: %d checks\n", checks);
    return 0;
}
