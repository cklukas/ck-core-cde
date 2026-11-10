/*
 * Harness for check-dbreader.py.
 *
 * It runs the reader of the data-type and action database files
 * (DtUtil1/DbReader.c) on text that real files do not have: very long
 * names and paths in the messages about a bad definition, lines without a
 * newline, many variables, a variable that refers to itself.  The functions
 * are cut out of DbReader.c by the driver (dbreader-functions.h) and the
 * Xt and Xrm functions they use are replaced by small ones.
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
	return (q >= 1 && q <= quark_count) ? quarks[q - 1] : NULL;
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

static DtDbRecordDesc records[] = { { "DATA_ATTRIBUTES", 0, NULL }, { "DATA_CRITERIA", 0, NULL } };

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
	return NULL;
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

/* The text of a message about a bad definition is as long as the names. */
static void scenario_messages(const char *unused)
{
	struct reader r;
	char *name = repeat('n', 1000), *path = repeat('p', 3500), text[2048];
	int index = -1, i;
	FILE *f;

	(void)unused;
	reader_start(&r);
	/* a definition that ends before its end symbol */
	snprintf(text, sizeof text, "DATA_ATTRIBUTES %s\n{\n\tICON x\n", name);
	f = open_text(text, strlen(text));
	errors = 0;
	(void)next_record(&r, f, path, &index);
	assert(errors == 1);
	assert(strstr(last_error, name) && strstr(last_error, path));
	fclose(f);

	/* the same for each of the problems */
	{
		const char *bad[] = {
			"DATA_ATTRIBUTES %s\nICON x\n}\n",		/* no start symbol */
			"DATA_ATTRIBUTES %s\n{\n{\n}\n",		/* a second one */
			"DATA_ATTRIBUTES %s\n{\nICON x \\\n}\n",	/* continued field */
			"DATA_ATTRIBUTES %s\n}\n",			/* end without start */
		};

		for (i = 0; i < 4; ++i) {
			snprintf(text, sizeof text, bad[i], name);
			f = open_text(text, strlen(text));
			errors = 0;
			index = -1;
			(void)next_record(&r, f, path, &index);
			assert(errors >= 1);
			assert(strstr(last_error, path));
			fclose(f);
		}
	}
	/* a version that cannot be set in the middle of a file */
	{
		char *version_text = malloc(8192);

		snprintf(version_text, 8192, "DATA_ATTRIBUTES a\n{\n}\nset DtDbVersion=2.0\nDATA_ATTRIBUTES b\n{\n}\n");
		f = open_text(version_text, strlen(version_text));
		errors = 0;
		(void)next_record(&r, f, path, &index);
		(void)next_record(&r, f, path, &index);
		assert(errors == 1 && strstr(last_error, path));
		fclose(f);
		free(version_text);
	}
	reader_stop(&r);
	free(name);
	free(path);
}

/* A newline is the end of a line, and nothing else is cut off. */
static void scenario_lines(const char *unused)
{
	struct reader r;
	int index = -1;
	FILE *f;
	static const char with_nul[] = "\0 ignored\nDATA_ATTRIBUTES x\n{\nICON last\n}\n";
	char *long_line = malloc(5000), *text = malloc(6000);

	(void)unused;
	reader_start(&r);

	/* a last line without a newline is whole */
	{
		static const char two[] = "DATA_ATTRIBUTES a\n{\nICON abc\n}\nDATA_ATTRIBUTES b\n{\nICON xyz\n}";

		f = open_text(two, sizeof two - 1);
		assert(next_record(&r, f, "file", &index));
		assert(field_value(&r, "ICON") && strcmp(field_value(&r, "ICON"), "abc") == 0);
		assert(next_record(&r, f, "file", &index));
		assert(field_value(&r, "ICON") && strcmp(field_value(&r, "ICON"), "xyz") == 0);
		fclose(f);
		reader_stop(&r);
	}

	/* a line that starts with a NUL byte */
	reader_start(&r);
	f = open_text(with_nul, sizeof with_nul - 1);
	(void)next_record(&r, f, "file", &index);
	fclose(f);
	reader_stop(&r);

	/* a line longer than the buffer is one line, cut at the buffer */
	reader_start(&r);
	memset(long_line, 'v', 4999);
	long_line[4999] = '\0';
	snprintf(text, 6000, "DATA_ATTRIBUTES a\n{\nICON %s\nDESCRIPTION second\n}\n", long_line);
	f = open_text(text, strlen(text));
	assert(next_record(&r, f, "file", &index));
	assert(field_value(&r, "DESCRIPTION") && strcmp(field_value(&r, "DESCRIPTION"), "second") == 0);
	fclose(f);
	reader_stop(&r);
	free(long_line);
	free(text);
}

/* More variables than the table has room for at first. */
static void scenario_many_variables(const char *unused)
{
	struct reader r;
	char *text = malloc(100000), *p = text;
	int index = -1, i, got = 0;
	FILE *f;
	char name[32], expected[32];

	(void)unused;
	reader_start(&r);
	for (i = 0; i < 40; ++i)
		p += sprintf(p, "set V%d=value%d\nDATA_ATTRIBUTES r%d\n{\nICON ${V%d}\n}\n", i, i, i, i);
	f = open_text(text, strlen(text));
	for (i = 0; i < 40; ++i) {
		if (!next_record(&r, f, "file", &index))
			break;
		snprintf(expected, sizeof expected, "value%d", i);
		assert(field_value(&r, "ICON") && strcmp(field_value(&r, "ICON"), expected) == 0);
		++got;
	}
	assert(got == 40);
	assert(variableCount == 40);
	fclose(f);
	reader_stop(&r);
	free(text);
	(void)name;
}

/* Variables that refer to themselves, and to each other, end. */
static void scenario_cycles(const char *unused)
{
	struct reader r;
	int index = -1;
	FILE *f;
	const char text[] = "set A=x${A}\nset B=${C}\nset C=${B}\nDATA_ATTRIBUTES r\n{\nICON ${A}\nDESCRIPTION ${B}\n}\n";

	(void)unused;
	reader_start(&r);
	f = open_text(text, sizeof text - 1);
	alarm(30);
	assert(next_record(&r, f, "file", &index));
	alarm(0);
	assert(field_value(&r, "ICON") != NULL);
	fclose(f);
	reader_stop(&r);
}

/* Lines with variables: shorter, longer and no value; a name without a value
   is not defined; the environment. */
static void scenario_variables(const char *unused)
{
	struct reader r;
	char *line;
	int index = -1;
	FILE *f;
	const char text[] = "set NOVALUE\nset SHORT=a\nset LONG=a-much-longer-value-than-the-reference\nset EMPTY=\n"
		"DATA_ATTRIBUTES r\n{\nICON <${SHORT}><${LONG}><${EMPTY}><$SHORT><${MISSING}>\nDESCRIPTION ${FROMENV}\n}\n";

	(void)unused;
	setenv("FROMENV", "k=v=w", 1);
	reader_start(&r);
	f = open_text(text, sizeof text - 1);
	assert(next_record(&r, f, "file", &index));
	assert(field_value(&r, "ICON"));
	assert(strcmp(field_value(&r, "ICON"),
		      "<a><a-much-longer-value-than-the-reference><><a><>") == 0);
	assert(field_value(&r, "DESCRIPTION") && strcmp(field_value(&r, "DESCRIPTION"), "k=v=w") == 0);
	fclose(f);
	reader_stop(&r);

	/* a reference at the very end of a line of the length of the buffer */
	line = XtMalloc(MAX_LINE_LENGTH);
	memset(line, 'x', MAX_LINE_LENGTH - 8);
	strcpy(line + MAX_LINE_LENGTH - 8, "${ENV2}");
	setenv("ENV2", "0123456789abcdef0123456789abcdef", 1);
	_DtDbFillVariables(&line);
	assert(strlen(line) == MAX_LINE_LENGTH - 8 + 32);
	XtFree(line);
}

/* After a character of more than one byte the line is still searched. */
static void scenario_multibyte(const char *unused)
{
	char *line;

	(void)unused;
	if (!setlocale(LC_ALL, "C.UTF-8"))
		return;
	is_multibyte = 1;
	setenv("AFTER", "found", 1);
	line = XtMalloc(MAX_LINE_LENGTH);
	strcpy(line, "caf\xc3\xa9 ${AFTER} and \xc3\xa9${AFTER}");
	_DtDbFillVariables(&line);
	assert(strstr(line, "found and") != NULL);
	assert(strstr(line, "\xc3\xa9" "found") != NULL);
	XtFree(line);
	is_multibyte = 0;
	setlocale(LC_ALL, "C");
}

int main(int argc, char **argv)
{
	static const struct {
		const char *name;
		void (*run)(const char *);
	} scenarios[] = {
		{ "messages", scenario_messages },
		{ "lines", scenario_lines },
		{ "many_variables", scenario_many_variables },
		{ "cycles", scenario_cycles },
		{ "variables", scenario_variables },
		{ "multibyte", scenario_multibyte },
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
