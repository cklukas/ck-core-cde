/*
 * Sanitizer regression harness for DtSearch hardening fixes:
 *  - msgutil.c: split_llist / merge_llist / sort_llist (broken base case
 *    and null-dereference) and clean_wrap (1-byte heap buffer underflow
 *    on empty string)
 *  - dtsrjoint.c: ditto_split / merge_by_prox / merge_by_date / ditto_sort
 *  - dtsrapi.c: both_valid_dates 32-bit DtSrObjdate -1 check on 64-bit hosts
 *  - fileman.c: fread_d99_header clamping corrupted hole_count bounds
 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <arpa/inet.h>

typedef unsigned char UCHAR;
typedef int32_t DtSrINT32;
typedef uint32_t DtSrUINT32;
typedef int16_t DtSrINT16;
typedef uint16_t DtSrUINT16;
typedef DtSrINT32 DtSrObjdate;
typedef DtSrINT32 DB_ADDR;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#define WHITESPACE 0x01
#define DtSrSORT_PROX 1
#define DtSrSORT_DATE 2
#define NUM_HOLES 20
#define PROGNAME "TEST"

typedef struct _llist_struct {
    struct _llist_struct *link;
    void *data;
} LLIST;

typedef struct _DtSrResult {
    struct _DtSrResult *link;
    DtSrINT32 proximity;
    DtSrObjdate objdate;
    int id;
} DtSrResult;

typedef struct {
    DtSrINT32 hole_size;
    DtSrINT32 offset;
} FREE_SPACE_STR;

typedef struct {
    DtSrINT32 hole_count[2];
    FREE_SPACE_STR hole_array[2][NUM_HOLES];
} FILE_HEADER;

typedef struct {
    DtSrObjdate objdate1;
    DtSrObjdate objdate2;
} USRBLK;

#ifndef NTOHL
#define NTOHL(x) ((x) = (DtSrINT32)ntohl((uint32_t)(x)))
#endif
#define CATGETS(catd, set, num, dflt) (dflt)

static int (*compare_llist)(LLIST *, LLIST *) = nullptr;
static unsigned char ascii_charmap[256];
static int ditsort_type = DtSrSORT_PROX;
static FILE *aa_stderr = nullptr;
static USRBLK usrblk;
static char sprintbuf[512];
static int dtsearch_catd = 0;
#define MS_ausapi 1

#define VOWEL 0x02
#define IS_VOWEL(c) ((paice_charmap[(UCHAR)(c)] & VOWEL) != 0)
typedef struct prule_t {
    struct prule_t *link;
    UCHAR *suffix;
    UCHAR suflen;
    char must_be_intact;
    UCHAR remove_count;
    UCHAR aplen;
    UCHAR *apndstr;
    char is_last_rule;
} PRULE;
static int debugging_paice = FALSE;
static int paice_charmap_buf[256];
static int *paice_charmap = paice_charmap_buf;
static char paicebuf[136];
static int paicelen = 0;
static int paicewcsl = 0;
static int word_is_intact = TRUE;
static int
euro_mbtowc(wchar_t *pwc, const char *p, const char *s)
{
    if (p < s)
        return -1;
    *pwc = (unsigned char)*p;
    return 1;
}

static void
DtSearchExit(int code)
{
    abort();
}

static void
DtSearchAddMessage(char *msg)
{
    (void)msg;
}

static DtSrObjdate
DtSearchValidDateString(char *datestr)
{
    if (datestr == nullptr || *datestr == '\0')
        return 0;
    if (strcmp(datestr, "invalid") == 0)
        return (DtSrObjdate)-1;
    if (strcmp(datestr, "2020-01-01") == 0)
        return (120 << 20) | (0 << 16) | (1 << 11);
    if (strcmp(datestr, "2025-01-01") == 0)
        return (125 << 20) | (0 << 16) | (1 << 11);
    return (DtSrObjdate)-1;
}

static LLIST *
pop_llist(LLIST **lst)
{
    LLIST *first_node = *lst;
    if (first_node != nullptr)
        *lst = first_node->link;
    return first_node;
}

static int
cmp_int_llist(LLIST *a, LLIST *b)
{
    int va = *(int *)a->data;
    int vb = *(int *)b->data;
    return (va > vb) - (va < vb);
}

/* @@EXTRACTED_FUNCTIONS@@ */

static void
test_clean_wrap_empty_and_normal(void)
{
    ascii_charmap[' '] = WHITESPACE;
    ascii_charmap['\t'] = WHITESPACE;

    /* Heap-allocate a 1-byte empty string so ASan detects *(string - 1) underflow */
    char *empty = (char *)malloc(1);
    assert(empty != nullptr);
    empty[0] = '\0';
    int lines = clean_wrap(empty, 72);
    assert(lines == 1);
    free(empty);

    char *text = strdup("hello world from dtsearch clean wrap");
    assert(text != nullptr);
    lines = clean_wrap(text, 12);
    assert(lines >= 3);
    free(text);
}

static void
test_sort_llist(void)
{
    compare_llist = cmp_int_llist;
    int vals[5] = { 42, 7, 19, 3, 25 };
    LLIST nodes[5];
    for (int i = 0; i < 5; i++) {
        nodes[i].data = &vals[i];
        nodes[i].link = (i + 1 < 5) ? &nodes[i + 1] : nullptr;
    }
    LLIST *sorted = sort_llist(&nodes[0]);
    int expected[5] = { 3, 7, 19, 25, 42 };
    for (int i = 0; i < 5; i++) {
        assert(sorted != nullptr);
        assert(*(int *)sorted->data == expected[i]);
        sorted = sorted->link;
    }
    assert(sorted == nullptr);
}

static void
test_ditto_sort(void)
{
    DtSrResult nodes[4];
    int prox[4] = { 30, 10, 40, 20 };
    for (int i = 0; i < 4; i++) {
        nodes[i].proximity = prox[i];
        nodes[i].objdate = 100 + i;
        nodes[i].id = i;
        nodes[i].link = (i + 1 < 4) ? &nodes[i + 1] : nullptr;
    }
    ditsort_type = DtSrSORT_PROX;
    DtSrResult *sorted = ditto_sort(&nodes[0]);
    int expected_prox[4] = { 10, 20, 30, 40 };
    for (int i = 0; i < 4; i++) {
        assert(sorted != nullptr);
        assert(sorted->proximity == expected_prox[i]);
        sorted = sorted->link;
    }
    assert(sorted == nullptr);
}

static void
test_both_valid_dates(void)
{
    assert(both_valid_dates("invalid", "2025-01-01") == FALSE);
    assert(both_valid_dates("2020-01-01", "invalid") == FALSE);
    assert(both_valid_dates("2020-01-01", "2025-01-01") == TRUE);
}

static void
test_fread_d99_header_bounds(void)
{
    FILE_HEADER raw;
    memset(&raw, 0, sizeof(raw));
    raw.hole_count[0] = (DtSrINT32)htonl((uint32_t)9999);
    raw.hole_count[1] = (DtSrINT32)htonl((uint32_t)-50);

    FILE *fp = tmpfile();
    assert(fp != nullptr);
    assert(fwrite(&raw, sizeof(raw), 1, fp) == 1);
    fflush(fp);

    FILE_HEADER out;
    memset(&out, 0, sizeof(out));
    assert(fread_d99_header(&out, fp) == TRUE);
    fclose(fp);

    assert(out.hole_count[0] == NUM_HOLES);
    assert(out.hole_count[1] == 0);
}

static void
test_paice_is_matching_rule_underflow(void)
{
    paice_charmap['A'] = VOWEL;
    paice_charmap['E'] = VOWEL;
    strcpy(paicebuf, "ABE");
    paicelen = 3;
    paicewcsl = 3;
    word_is_intact = TRUE;

    /* Reversed suffix "EBAX" has length 4 > paicelen (3), with first 3 chars matching "ABE" backwards */
    PRULE rule;
    memset(&rule, 0, sizeof(rule));
    rule.suffix = (UCHAR *)"EBAX";
    rule.suflen = 4;
    rule.remove_count = 2;
    assert(is_matching_rule(&rule) == FALSE);
}

int
main(void)
{
    aa_stderr = stderr;
    test_clean_wrap_empty_and_normal();
    test_sort_llist();
    test_ditto_sort();
    test_both_valid_dates();
    test_fread_d99_header_bounds();
    test_paice_is_matching_rule_underflow();
    printf("check-dtsearch-hardening: all tests passed\n");
    return 0;
}
