/*
 * Harness for check-dbreader-variables.py.
 *
 * It reads database text through ReadNextEntry() of DtUtil1/DbReader.c with
 * "set" lines that the reader used to mishandle: a DtDbVersion of any length
 * and invalid multibyte sequences in the names of a "set" line and of a
 * reference.  Every scenario is a process of its own with a time limit.
 */
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
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

static char *XtMalloc(size_t size)
{
	char *p = malloc(size ? size : 1);

	assert(p);
	return p;
}

static char *XtNewString(const char *s)
{
	char *p = strdup(s);

	assert(p);
	return p;
}
#define XtNewString(s) XtNewString(s)

static char *XtRealloc(char *old, size_t size)
{
	char *p = realloc(old, size ? size : 1);

	assert(p);
	return p;
}

static void XtFree(char *p)
{
	free(p);
}

#define DtStrchr(s, c) strchr((s), (c))
#define DtStrrchr(s, c) strrchr((s), (c))
#define DtStrchr_defined 1

static int _is_previous_single(const char *start, const char *p)
{
	(void)start;
	(void)p;
	return 1;
}

/* quarks: the strings that were asked for, numbered from 1 */
static char *quarks[256];
static int quark_count;

static XrmQuark XrmStringToQuark(const char *s)
{
	int i;

	for (i = 0; i < quark_count; ++i)
		if (strcmp(quarks[i], s) == 0)
			return i + 1;
	assert(quark_count < 256);
	quarks[quark_count] = strdup(s);
	return ++quark_count;
}

static const char *XrmQuarkToString(XrmQuark q)
{
	return (q >= 1 && q <= quark_count) ? quarks[q - 1] : nullptr;
}

/* what the library prints about a bad file */
static char last_error[16384];
static int errors;

static void _DtSimpleError(char *program, int severity, char *help, char *format, ...)
{
	va_list ap;

	(void)program;
	(void)severity;
	(void)help;
	va_start(ap, format);
	vsnprintf(last_error, sizeof last_error, format, ap);
	va_end(ap);
	++errors;
}

/* the messages of the catalog */
static char *incompleteDefn = "The definition \"%s\" in the file\n  \"%s\"\n  is incomplete.\n";
static char *multiLineDefn = "multi-line \"%s\" in \"%s\"\n";
static char *tooManyFields = "too many fields in \"%s\" in \"%s\"\n";
static char *noStartSymbol = "no start symbol \"%s\" \"%s\"\n";
static char *invalidStartSymbol = "invalid start symbol \"%s\" \"%s\"\n";
static char *missingStartSymbol = "missing start symbol \"%s\" \"%s\"\n";
static char *cantSetVersion = "cannot set the version in '%s'\n";
static char *invalidVersion = "invalid version in '%s'\n";

#include "dbreader-types.h"
#include "dbreader-functions.h"

/* ---- helpers ---- */

static char *repeat(int c, size_t count)
{
	char *s = malloc(count + 1);

	assert(s);
	memset(s, c, count);
	s[count] = '\0';
	return s;
}

static DtDbRecordDesc records[] = { { "DATA_ATTRIBUTES", 0, nullptr }, { "DATA_CRITERIA", 0, nullptr } };

struct reader {
	DtDtsDbField *fields;
	int allocated;
	char *buffer;
	char *line;
	Boolean version_can_be_set;
};

static void reader_start(struct reader *r)
{
	memset(r, 0, sizeof *r);
	r->allocated = NUMBER_FIELDS_DEFAULT;
	/* the reader of the library gets malloc()ed memory; the harness starts with zeros so that
	   the scenarios do not depend on what it held */
	r->fields = (DtDtsDbField *)calloc(r->allocated, sizeof(DtDtsDbField));
	r->buffer = calloc(1, MAX_LINE_LENGTH);
	r->line = calloc(1, MAX_LINE_LENGTH);
	assert(r->fields && r->buffer && r->line);
	r->version_can_be_set = True;
	variableCount = 0;
}

static void reader_stop(struct reader *r)
{
	int i;

	for (i = 0; r->fields[i].fieldName || r->fields[i].fieldValue; ++i)
		XtFree(r->fields[i].fieldValue);
	XtFree((char *)r->fields);
	XtFree(r->buffer);
	XtFree(r->line);
	ClearVariables();
}

/* Read the next record of text; returns the value of the field named NAME of
   the record as a new string (NULL when there is none), and sets *got. */
static int next_record(struct reader *r, FILE *f, const char *file_name, int *index)
{
	char *path = strdup(file_name);
	int got = ReadNextEntry(&r->version_can_be_set, f, &r->fields, records, 2, index, path,
				&r->allocated, &r->buffer, &r->line);

	free(path);
	return got;
}

static const char *field_value(struct reader *r, const char *name)
{
	XrmQuark q = XrmStringToQuark(name);
	int i;

	for (i = 0; r->fields[i].fieldName; ++i)
		if (r->fields[i].fieldName == q)
			return r->fields[i].fieldValue;
	return nullptr;
}

static FILE *open_text(const char *text, size_t length)
{
	FILE *f = tmpfile();

	assert(f);
	assert(fwrite(text, 1, length, f) == length);
	rewind(f);
	return f;
}

/* ---- scenarios ---- */

#define TIME_LIMIT 20	/* seconds; the reader used to loop for ever */

/* Read the first record of TEXT; returns whether there was one. */
static int read_first(const char *text, size_t length, int *error_count)
{
	struct reader r;
	int index = -1, got;
	FILE *f;

	reader_start(&r);
	f = open_text(text, length);
	errors = 0;
	got = next_record(&r, f, "file", &index);
	if (error_count)
		*error_count = errors;
	fclose(f);
	reader_stop(&r);
	return got;
}

/* A version of any length is expanded into room that holds it; every value
   except 1.0 (and none at all) rejects the file with a message. */
static void scenario_version_overflow(const char *unused)
{
	static const size_t lengths[] = { 1, 2, 4, 12, 13, 14, 32, 33, 100, 500, 1000, 1010, 1100, 5000 };
	size_t i;

	(void)unused;
	alarm(TIME_LIMIT);
	for (i = 0; i < sizeof lengths / sizeof lengths[0]; ++i) {
		char *value = repeat('a', lengths[i]), *text = malloc(lengths[i] + 128);
		int error_count = -1;

		sprintf(text, "set DtDbVersion=%s\nDATA_ATTRIBUTES r\n{\nICON x\n}\n", value);
		assert(read_first(text, strlen(text), &error_count) == 0);
		assert(error_count == 1);
		assert(strstr(last_error, "invalid version"));
		free(text);
		free(value);
	}
	/* a version that is a variable of its own, and one that is the query itself */
	{
		static const char *const rejected[] = {
			"set V=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
			"set DtDbVersion=${V}${V}${V}\nDATA_ATTRIBUTES r\n{\n}\n",
			"set DtDbVersion=$DtDbVersion$DtDbVersion\nDATA_ATTRIBUTES r\n{\n}\n",
			"set DtDbVersion=2.0\nDATA_ATTRIBUTES r\n{\n}\n",
		};
		int k, error_count;

		for (k = 0; k < 3; ++k) {
			error_count = -1;
			assert(read_first(rejected[k], strlen(rejected[k]), &error_count) == 0);
			assert(error_count == 1);
		}
	}
	alarm(0);
}

/* The result for valid input: 1.0 and a missing version read the record. */
static void scenario_version_valid(const char *unused)
{
	static const char *const accepted[] = {
		"set DtDbVersion=1.0\nset V=value\nDATA_ATTRIBUTES r\n{\nICON <${V}>\n}\n",
		"set DtDbVersion=\nset V=value\nDATA_ATTRIBUTES r\n{\nICON <${V}>\n}\n",
		"set V=value\nDATA_ATTRIBUTES r\n{\nICON <${V}>\n}\n",
		"set DtDbVersion=1.0\nset V=value\nDATA_ATTRIBUTES r\n{\nICON <${V}>\n}\n",
	};
	int k;

	(void)unused;
	alarm(TIME_LIMIT);
	for (k = 0; k < 4; ++k) {
		struct reader r;
		int index = -1, error_count;
		FILE *f;

		reader_start(&r);
		f = open_text(accepted[k], strlen(accepted[k]));
		errors = 0;
		assert(next_record(&r, f, "file", &index));
		error_count = errors;
		assert(error_count == 0);
		assert(field_value(&r, "ICON") && strcmp(field_value(&r, "ICON"), "<value>") == 0);
		fclose(f);
		reader_stop(&r);
	}
	alarm(0);
}

static void use_multibyte(void)
{
	if (!setlocale(LC_ALL, "C.UTF-8") && !setlocale(LC_ALL, "en_US.UTF-8")) {
		fprintf(stderr, "no UTF-8 locale\n");
		exit(1);
	}
	is_multibyte = 1;
	_DtNl_is_multibyte = 1;
}

/* An invalid sequence ends the name of a "set" line and of a reference; the
   scan ends, and the record after the line is read. */
static void scenario_invalid_multibyte(const char *unused)
{
	static const char *const texts[] = {
		"set t\xf4",				/* a cut sequence at the end of a name */
		"set t\xf4\nDATA_ATTRIBUTES r\n{\nICON ok\n}\n",
		"set \xff\xfe=1\nDATA_ATTRIBUTES r\n{\nICON ok\n}\n",
		"set a\xc3\nDATA_ATTRIBUTES r\n{\nICON ok\n}\n",
		"set ab\x80\x80 =1\nDATA_ATTRIBUTES r\n{\nICON ok\n}\n",
		"set A=1\nDATA_ATTRIBUTES r\n{\nICON ok ${A\xff} $A\xf4 ${\xfe\n}\n",
		"DATA_ATTRIBUTES r\n{\nICON ok $\xff${\xff}${a\xc3\n}\n",
	};
	size_t i;

	(void)unused;
	use_multibyte();
	alarm(TIME_LIMIT);
	for (i = 0; i < sizeof texts / sizeof texts[0]; ++i) {
		struct reader r;
		int index = -1, got;
		FILE *f;

		reader_start(&r);
		f = open_text(texts[i], strlen(texts[i]));
		got = next_record(&r, f, "file", &index);
		if (i >= 1) {
			assert(got);
			assert(field_value(&r, "ICON") && strncmp(field_value(&r, "ICON"), "ok", 2) == 0);
		}
		fclose(f);
		reader_stop(&r);
	}
	alarm(0);
}

/* Valid multibyte text gives what it gave before. */
static void scenario_valid_multibyte(const char *unused)
{
	static const char text[] = "set caf\xc3\xa9=1\nset x\xe3\x81\x82y=two\n"
		"DATA_ATTRIBUTES r\n{\nICON ${caf\xc3\xa9}-${x\xe3\x81\x82y}-\xc3\xa9${caf\xc3\xa9}\n}\n";
	struct reader r;
	int index = -1;
	FILE *f;

	(void)unused;
	use_multibyte();
	alarm(TIME_LIMIT);
	reader_start(&r);
	f = open_text(text, sizeof text - 1);
	assert(next_record(&r, f, "file", &index));
	assert(field_value(&r, "ICON") && strcmp(field_value(&r, "ICON"), "1-two-\xc3\xa9" "1") == 0);
	fclose(f);
	reader_stop(&r);
	alarm(0);
}

int main(int argc, char **argv)
{
	static const struct {
		const char *name;
		void (*run)(const char *);
	} scenarios[] = {
		{ "version_overflow", scenario_version_overflow },
		{ "version_valid", scenario_version_valid },
		{ "invalid_multibyte", scenario_invalid_multibyte },
		{ "valid_multibyte", scenario_valid_multibyte },
	};
	size_t i;

	assert(argc == 2);
	for (i = 0; i < sizeof scenarios / sizeof scenarios[0]; ++i) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			scenarios[i].run(argv[1]);
			return 0;
		}
	}
	fprintf(stderr, "unknown scenario %s\n", argv[1]);
	return 2;
}
