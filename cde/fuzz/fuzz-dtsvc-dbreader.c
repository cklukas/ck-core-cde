/*
 * Fuzz target: the reader of the data-type and action database files
 * (*.dt) of libDtSvc, DtUtil1/DbReader.c.
 *
 * run-fuzz.py cuts the line reader, the variable expansion, the "set"
 * statement parser, the field splitter, the keyword matcher and
 * ReadNextEntry out of DbReader.c verbatim.  The harness runs the loop of
 * _DtDbRead on one file: ClearVariables, then ReadNextEntry until it returns
 * 0, then FreeDbField of each record.  The stand-ins are the Xt allocators
 * (the same as XtMalloc: they abort on failure), the Xrm quark table, the
 * error reporter, the localized message strings and the multibyte switch.
 * The record descriptions are those _DtDtsMMCreateDb passes (DATA_CRITERIA,
 * DATA_ATTRIBUTES and ACTION with 30 fields).
 *
 * Input layout: byte 0, bit 0: use a multibyte (UTF-8) locale and set
 * is_multibyte; bit 1: DtDbVersion may not be set (as after the first record).
 * The rest of the input is the content of the file.  The environment of the
 * process holds a few variables so that ${NAME} references to the environment
 * are expanded as well.
 *
 * Environment FUZZ_AVOID_REPORTED=1: the two bugs already reported for this
 * reader (a DtDbVersion definition longer than the 13 bytes of the buffer the
 * version check expands it into; an invalid multibyte sequence in a "set"
 * name, which loops forever) are not reached: inputs that contain the text
 * DtDbVersion are dropped and the multibyte locale is not used.  The run then
 * goes on to look for other problems.
 */
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/param.h>
#include <sys/types.h>
#include <unistd.h>

#define TRUE 1
#define FALSE 0
#define NLS16
#define DtProgName ((char *)0)
#define DtError 1
#define _DtSvcProcessLock() ((void)0)
#define _DtSvcProcessUnlock() ((void)0)

typedef unsigned char Boolean;
#define True 1
#define False 0
typedef int XrmQuark;
typedef int DtDbPathId;

static int is_multibyte;
static int _DtNl_is_multibyte;
static int use_in_memory_db;

static char *XtMalloc(size_t size) { char *p = malloc(size ? size : 1); if (!p) abort(); return p; }
static char *XtNewString(const char *s) { char *p = strdup(s); if (!p) abort(); return p; }
#define XtNewString(s) XtNewString(s)
static char *XtRealloc(char *old, size_t size) { char *p = realloc(old, size ? size : 1); if (!p) abort(); return p; }
static void XtFree(char *p) { free(p); }

#define DtStrchr(s, c) strchr((s), (c))
#define DtStrrchr(s, c) strrchr((s), (c))
#define DtStrchr_defined 1

static int _is_previous_single(const char *start, const char *p) { (void)start; (void)p; return 1; }

/* Xrm quarks: the strings that were asked for, numbered from 1.  The table of
   the library grows without limit; this one wraps to its last entry. */
#define QUARKS 8192
static char *quarks[QUARKS];
static int quark_count;

static XrmQuark XrmStringToQuark(const char *s)
{
    for (int i = 0; i < quark_count; ++i)
        if (strcmp(quarks[i], s) == 0)
            return i + 1;
    if (quark_count == QUARKS)
        return QUARKS;
    quarks[quark_count] = strdup(s);
    return ++quark_count;
}

static const char *XrmQuarkToString(XrmQuark q)
{
    return (q >= 1 && q <= quark_count) ? quarks[q - 1] : NULL;
}

static void _DtSimpleError(char *program, int severity, char *help, char *format, ...)
{
    char text[2048];
    va_list ap;
    (void)program; (void)severity; (void)help;
    va_start(ap, format);
    vsnprintf(text, sizeof text, format, ap);
    va_end(ap);
}

static char *incompleteDefn = "The definition \"%s\" in the file\n  \"%s\"\n  is incomplete.\n";
static char *multiLineDefn = "multi-line \"%s\" in \"%s\"\n";
static char *tooManyFields = "too many fields in \"%s\" in \"%s\"\n";
static char *noStartSymbol = "no start symbol \"%s\" \"%s\"\n";
static char *invalidStartSymbol = "invalid start symbol \"%s\" \"%s\"\n";
static char *missingStartSymbol = "missing start symbol \"%s\" \"%s\"\n";
static char *cantSetVersion = "cannot set the version in '%s'\n";
static char *invalidVersion = "invalid version in '%s'\n";

#include FUZZ_TYPES_H
#include FUZZ_PRODUCTION_H

#define NUM_RECORDS 3
static DtDbRecordDesc records[NUM_RECORDS] = {
    { "DATA_CRITERIA", 30, NULL }, { "DATA_ATTRIBUTES", 30, NULL }, { "ACTION", 30, NULL } };

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    setenv("HOME", "/home/fuzz", 1);
    setenv("FUZZVAR", "env value with ${FUZZVAR} and \\", 1);
    setenv("EMPTYVAR", "", 1);
    if (!setlocale(LC_ALL, "C.UTF-8") && !setlocale(LC_ALL, "en_US.UTF-8"))
        setlocale(LC_ALL, "");
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0) return 0;
    uint8_t flags = data[0];
    data++; size--;
    static int avoid = -1;
    if (avoid < 0) avoid = getenv("FUZZ_AVOID_REPORTED") != NULL;
    if (avoid) {
        flags &= ~1;
        if (memmem(data, size, "DtDbVersion", 11)) return 0;
    }
    is_multibyte = _DtNl_is_multibyte = flags & 1;
    Boolean version_can_be_set = !(flags & 2);

    if (size == 0) return 0;
    FILE *fd = fmemopen((void *)data, size, "r");
    if (!fd) return 0;

    int allocated = NUMBER_FIELDS_DEFAULT;
    for (int i = 0; i < NUM_RECORDS; i++)
        if (records[i].maxFields > allocated)
            allocated = records[i].maxFields;
    DtDtsDbField *fields = (DtDtsDbField *)XtMalloc((allocated + 1) * sizeof(DtDtsDbField));
    char *tmpLine = XtMalloc(MAX_LINE_LENGTH);
    char *tmpBuffer = XtMalloc(MAX_LINE_LENGTH);
    int index = 0;
    char path[] = "/fuzz/types/file.dt";

    ClearVariables();
    while (ReadNextEntry(&version_can_be_set, fd, &fields, records, NUM_RECORDS, &index, path,
                         &allocated, &tmpBuffer, &tmpLine))
        FreeDbField(fields);
    fclose(fd);
    XtFree((char *)fields);
    XtFree(tmpLine);
    XtFree(tmpBuffer);
    ClearVariables();
    return 0;
}
