/*
 * Harness for check-dts-buffers.py.
 *
 * It runs the functions of DtUtil1/Dts.c that carry data-type database text
 * (attribute values, patterns, content tests, keyword and shell expansion,
 * directory type files) with the database, the file system probes and the
 * memory allocator under the control of the test.  dts-types.h and
 * dts-functions.h are cut out of Dts.c, Dts.h and DtsMM.h by the driver.
 */
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MAXINT INT_MAX
#define DtProgName ((char *)0)
#define DtError 1
#define _DtSvcAppLockDefault() ((void)0)
#define _DtSvcAppUnlockDefault() ((void)0)
#define _DtSvcProcessLock() ((void)0)
#define _DtSvcProcessUnlock() ((void)0)

typedef char *_Xstrtokparams;
#define _XStrtok(s1, s2, p) strtok_r((s1), (s2), &(p))

/* ---- allocation and pipe accounting with failure injection ---- */

static unsigned allocations;	/* allocation calls since arm() */
static unsigned fail_at;	/* the allocation call that fails, 0 for none */
static long live;		/* blocks handed out and not yet freed */
static long open_pipes;		/* popen() results not yet pclose()d */

static void arm(unsigned n)
{
	allocations = 0;
	fail_at = n;
}

static unsigned disarm(void)
{
	unsigned used = allocations;

	allocations = 0;
	fail_at = 0;
	return used;
}

static int fail_now(void)
{
	return ++allocations == fail_at;
}

static void *t_malloc(size_t size)
{
	void *p;

	if (fail_now()) {
		errno = ENOMEM;
		return NULL;
	}
	p = malloc(size);
	if (p)
		++live;
	return p;
}

static void *t_calloc(size_t count, size_t size)
{
	void *p;

	if (fail_now()) {
		errno = ENOMEM;
		return NULL;
	}
	p = calloc(count, size);
	if (p)
		++live;
	return p;
}

static void *t_realloc(void *old, size_t size)
{
	void *p;

	if (fail_now()) {
		errno = ENOMEM;
		return NULL;
	}
	p = realloc(old, size);
	if (p && !old)
		++live;
	return p;
}

static void t_free(void *p)
{
	if (p) {
		--live;
		free(p);
	}
}

static char *t_strdup(const char *s)
{
	size_t size = strlen(s) + 1;
	char *p = t_malloc(size);

	if (p)
		memcpy(p, s, size);
	return p;
}

static FILE *t_popen(const char *command, const char *mode)
{
	FILE *f;

	if (strstr(command, "popen-fails")) {
		errno = EAGAIN;
		return NULL;
	}
	f = popen(command, mode);
	if (f)
		++open_pipes;
	return f;
}

static int t_pclose(FILE *f)
{
	--open_pipes;
	return pclose(f);
}

/* what the stubbed database layer allocates is outside the injection */
static char *plain_strdup(const char *s)
{
	return strdup(s);
}

static void plain_free(void *p)
{
	free(p);
}

/* reads and mappings of the file contents under the control of the test */
static int mapping_fails;	/* mmap() fails */
static int read_mode;		/* 0: as the file is, 1: the first 40 bytes and then the end, 2: EIO */

static void *t_mmap(void *address, size_t length, int protection, int flags, int fd, off_t offset)
{
	if (mapping_fails) {
		errno = ENODEV;
		return MAP_FAILED;
	}
	return mmap(address, length, protection, flags, fd, offset);
}

static ssize_t t_read(int fd, void *buffer, size_t count)
{
	if (read_mode == 2) {
		errno = EIO;
		return -1;
	}
	if (read_mode == 1) {
		static int served;

		if (served) {
			served = 0;
			return 0;
		}
		served = 1;
		count = count > 40 ? 40 : count;
	}
	return read(fd, buffer, count);
}

/* a byte that is not a character leaves the character as it finds it; the
   test sets it to a value no byte of a pattern has */
static int t_mbtowc(wchar_t *wc, const char *s, size_t n)
{
	int length = mbtowc(wc, s, n);

	if (length < 0 && wc)
		*wc = (wchar_t)0x7ffffff0;
	return length;
}

#define malloc t_malloc
#define calloc t_calloc
#define realloc t_realloc
#define free t_free
#define strdup t_strdup
#define popen t_popen
#define pclose t_pclose
#define mmap t_mmap
#define read t_read
#define mbtowc t_mbtowc

/* ---- stubs for what Dts.c takes from the rest of the library ---- */

typedef int DtShmBoson;
#include "dts-types.h"

static char reported_format[512];
static char reported_argument[512];

static void _DtSimpleError(char *program, int severity, char *help, char *format, ...)
{
	va_list ap;
	char *argument;

	(void)program;
	(void)severity;
	(void)help;
	snprintf(reported_format, sizeof reported_format, "%s", format);
	va_start(ap, format);
	argument = va_arg(ap, char *);
	snprintf(reported_argument, sizeof reported_argument, "%s", argument ? argument : "(null)");
	va_end(ap);
}

static char *DtStrchr(const char *s, int c)
{
	return strchr(s, c);
}

static int _is_previous_single(const char *start, const char *p)
{
	(void)start;
	(void)p;
	return 1;
}

static char *_dt_strpbrk(const char *s, const char *set)
{
	return strpbrk(s, set);
}

/* A database of a few records, built by the scenarios. */
#define MAX_BOSONS 64
#define MAX_RECORDS 4
#define MAX_FIELDS 4
static char boson_text[MAX_BOSONS][1024];
static int boson_count;
static DtDtsMMField field_table[MAX_RECORDS][MAX_FIELDS];
static DtDtsMMRecord record_table[MAX_RECORDS];
static int record_count;
static DtDtsMMDatabase fake_db;

static DtShmBoson _DtDtsMMStringToBoson(const char *s)
{
	int i;

	if (!s || !*s)
		return -1;
	for (i = 0; i < boson_count; ++i)
		if (strcmp(boson_text[i], s) == 0)
			return i + 1;
	assert(boson_count < MAX_BOSONS && strlen(s) < sizeof boson_text[0]);
	strcpy(boson_text[boson_count], s);
	return ++boson_count;
}

static const char *_DtDtsMMBosonToString(DtShmBoson boson)
{
	return (boson > 0 && boson <= boson_count) ? boson_text[boson - 1] : NULL;
}

/* Not the library's version: that one copies into a fixed 1024 byte block. */
static char *_DtDtsMMExpandValue(const char *value)
{
	return value ? plain_strdup(value) : NULL;
}

static void _DtDtsMMSafeFree(char *value)
{
	plain_free(value);
}

#define RECORD_LIST 1000
static void *_DtDtsMMGetPtr(int index)
{
	return index == RECORD_LIST ? (void *)record_table : (void *)field_table[index];
}

static DtDtsMMRecord *_DtDtsMMGetRecordByName(DtDtsMMDatabase *db, const char *name)
{
	DtShmBoson boson = _DtDtsMMStringToBoson(name);
	int i;

	for (i = 0; i < db->recordCount; ++i)
		if (record_table[i].recordName == boson)
			return &record_table[i];
	return NULL;
}

static const char *_DtDtsMMGetFieldByName(DtDtsMMRecord *record, const char *name)
{
	DtShmBoson boson = _DtDtsMMStringToBoson(name);
	DtDtsMMField *fields = _DtDtsMMGetPtr(record->fieldList);
	int i;

	for (i = 0; i < record->fieldCount; ++i)
		if (fields[i].fieldName == boson)
			return _DtDtsMMBosonToString(fields[i].fieldValue);
	return NULL;
}

static int da_database_loaded = 1;

static DtDtsMMDatabase *get_dc_db(void)
{
	return &fake_db;
}

static DtDtsMMDatabase *get_da_db(void)
{
	return da_database_loaded ? &fake_db : NULL;
}

static DtDtsMMRecord *name_list(type_info_t *linfo, DtDtsMMDatabase *db, DtDtsMMRecord *rec_ptr)
{
	(void)linfo;
	(void)db;
	if (!rec_ptr)
		return record_count ? &record_table[0] : NULL;
	return (rec_ptr + 1 < record_table + record_count) ? rec_ptr + 1 : NULL;
}

#ifndef REAL_PROBES
/* The probes below look at the file system; the scenarios supply the answers. */
static const struct stat *get_stat(type_info_t *info)
{
	return info->file_stat;
}

static const char *get_buff(type_info_t *info)
{
	return info->buffer;
}

static const char *get_link_name(type_info_t *info)
{
	(void)info;
	return NULL;
}

static const char *get_link_path(type_info_t *info)
{
	(void)info;
	return NULL;
}
#endif

static int type_mode(char *attr, type_info_t *info)
{
	(void)attr;
	(void)info;
	return 0;
}

#include "dts-functions.h"

/* ---- helpers ---- */

static void reset_database(void)
{
	memset(field_table, 0, sizeof field_table);
	memset(record_table, 0, sizeof record_table);
	record_count = 0;
	fake_db.recordCount = 0;
	fake_db.recordList = RECORD_LIST;
	da_database_loaded = 1;
}

static DtDtsMMRecord *add_record(const char *name)
{
	DtDtsMMRecord *record = &record_table[record_count];

	assert(record_count < MAX_RECORDS);
	record->recordName = _DtDtsMMStringToBoson(name);
	record->fieldList = record_count;
	++record_count;
	fake_db.recordCount = record_count;
	return record;
}

static void add_field(DtDtsMMRecord *record, const char *name, const char *value)
{
	DtDtsMMField *field = &field_table[record - record_table][record->fieldCount++];

	assert(record->fieldCount <= MAX_FIELDS);
	field->fieldName = _DtDtsMMStringToBoson(name);
	field->fieldValue = _DtDtsMMStringToBoson(value);
}

static char *repeat(int c, size_t count)
{
	char *s = malloc(count + 1);

	assert(s);
	memset(s, c, count);
	s[count] = '\0';
	return s;
}

static char *concat(const char *a, const char *b, const char *c)
{
	char *s = malloc(strlen(a) + strlen(b) + strlen(c) + 1);

	assert(s);
	strcpy(s, a);
	strcat(s, b);
	strcat(s, c);
	return s;
}

static void write_file(const char *path, const char *content)
{
	size_t length = strlen(content);
	ssize_t written;
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

	assert(fd >= 0);
	written = write(fd, content, length);
	assert(written == (ssize_t)length);
	close(fd);
}

/* ---- max_buf ---- */

static void scenario_max_buf(const char *arena)
{
	static const size_t lengths[] = { 0, 1, 98, 99, 100, 101, 150, 500, 1000, 3000, 7, 0, 3000 };
	static const size_t steps[] = { MB_SIZE + 1, 2 * MB_SIZE, 10 * MB_SIZE, 1000 * MB_SIZE };
	type_info_t info;
	char segment[3001];
	char *b, *old;
	size_t old_size, i, n;

	(void)arena;
	memset(segment, 'a', sizeof segment - 1);
	segment[sizeof segment - 1] = '\0';

	/* The copy DtDtsDataToDataType makes of one attribute segment: lengths
	 * of MB_SIZE and more did not fit the buffer, later ones did not even
	 * have the recorded size. */
	memset(&info, 0, sizeof info);
	for (i = 0; i < sizeof lengths / sizeof lengths[0]; ++i) {
		n = lengths[i];
		b = max_buf(n + 1, &info);
		assert(b && b == info.mb && info.mb_size >= n + 1);
		strncpy(b, segment, n);
		b[n] = '\0';
		assert(strlen(b) == n);
	}
	free(info.mb);

	/* a request of 0 on a new info yields the default buffer, zeroed */
	memset(&info, 0, sizeof info);
	b = max_buf(0, &info);
	assert(b && b == info.mb && info.mb_size == MB_SIZE);
	for (i = 0; i < MB_SIZE; ++i)
		assert(b[i] == '\0');
	memset(b, 'z', MB_SIZE);
	assert(max_buf(MB_SIZE, &info) == b && max_buf(0, &info) == b);

	/* repeated growth keeps what was stored and zeroes what was added */
	strcpy(b, "keep");
	for (i = 0; i < sizeof steps / sizeof steps[0]; ++i) {
		old_size = info.mb_size;
		b = max_buf(steps[i], &info);
		assert(b && b == info.mb && info.mb_size >= steps[i]);
		assert(strcmp(b, "keep") == 0);
		for (n = old_size; n < info.mb_size; ++n)
			assert(b[n] == '\0');
		memset(b + 5, 'x', info.mb_size - 5);
	}
	/* a smaller request neither moves nor shrinks the buffer */
	old = info.mb;
	old_size = info.mb_size;
	assert(max_buf(1, &info) == old && max_buf(MB_SIZE, &info) == old && max_buf(0, &info) == old);
	assert(info.mb_size == old_size);
	free(info.mb);

	/* the first request may already exceed the default size */
	memset(&info, 0, sizeof info);
	b = max_buf(5000, &info);
	assert(b && info.mb_size >= 5000);
	memset(b, 'x', 5000);
	free(info.mb);

	/* growth that cannot be satisfied leaves the buffer as it was */
	memset(&info, 0, sizeof info);
	b = max_buf(0, &info);
	strcpy(b, "keep");
	old = info.mb;
	old_size = info.mb_size;
	arm(1);
	assert(max_buf(10 * MB_SIZE, &info) == NULL);
	assert(disarm() == 1);
	assert(info.mb == old && info.mb_size == old_size && strcmp(info.mb, "keep") == 0);
	memset(info.mb, 'k', info.mb_size);
	b = max_buf(10 * MB_SIZE, &info);
	assert(b && info.mb_size >= 10 * MB_SIZE);
	memset(b, 'x', 10 * MB_SIZE);
	free(info.mb);

	/* a buffer that cannot be created records nothing */
	memset(&info, 0, sizeof info);
	for (i = 0; i < 2; ++i) {
		arm(1);
		assert(max_buf(i ? 0 : 5 * MB_SIZE, &info) == NULL);
		assert(disarm() == 1);
		assert(info.mb == NULL && info.mb_size == 0);
	}
	b = max_buf(5 * MB_SIZE, &info);
	assert(b && info.mb_size >= 5 * MB_SIZE);
	free(info.mb);

	/* sizes no object can have are refused without touching the allocator */
	memset(&info, 0, sizeof info);
	arm(0);
	assert(max_buf(SIZE_MAX, &info) == NULL);
	assert(max_buf((size_t)PTRDIFF_MAX + 1, &info) == NULL);
	assert(disarm() == 0);
	assert(info.mb == NULL && info.mb_size == 0);
	b = max_buf(0, &info);
	old = info.mb;
	old_size = info.mb_size;
	assert(b && b == old);
	arm(0);
	assert(max_buf(SIZE_MAX, &info) == NULL);
	assert(disarm() == 0);
	assert(info.mb == old && info.mb_size == old_size);
	free(info.mb);

	assert(live == 0);
}

/* ---- DtDtsDataToDataType, the caller of max_buf ---- */

static void scenario_data_to_data_type(const char *arena)
{
	DtDtsMMRecord *record;
	char buffer[200];
	char *name, *unmatched, *pattern, *text, *content, *type;
	unsigned k, used;
	long before;
	int pass;

	(void)arena;
	name = repeat('a', 200);
	unmatched = repeat('b', 120);
	pattern = concat(unmatched, "|", name);
	memset(buffer, 'c', sizeof buffer);
	text = repeat('c', sizeof buffer);
	content = concat("0 string ", text, "");

	/* Records with attributes far longer than the 100 byte default: a name
	 * pattern of two long alternatives, a long content test, and a field
	 * that has no value at all. */
	reset_database();
	record = add_record("LONGNAME");
	add_field(record, DtDTS_NAME_PATTERN, pattern);
	add_field(record, DtDTS_DATA_ATTRIBUTES_NAME, "LONGNAME");
	record = add_record("LONGCONTENT");
	add_field(record, DtDTS_CONTENT, content);
	add_field(record, DtDTS_DATA_ATTRIBUTES_NAME, "LONGCONTENT");
	record = add_record("NOVALUE");
	add_field(record, DtDTS_NAME_PATTERN, NULL);
	add_field(record, DtDTS_DATA_ATTRIBUTES_NAME, "NOVALUE");

	type = DtDtsDataToDataType(name, NULL, -1, NULL, NULL, NULL, NULL);
	assert(type && strcmp(type, "LONGNAME") == 0);
	free(type);
	type = DtDtsDataToDataType(NULL, buffer, sizeof buffer, NULL, NULL, NULL, "plain");
	assert(type && strcmp(type, "LONGCONTENT") == 0);
	free(type);
	type = DtDtsDataToDataType("short-name", NULL, -1, NULL, NULL, NULL, NULL);
	assert(type && strcmp(type, DtDTS_DT_UNKNOWN) == 0);
	free(type);

	/* the same lookups with each allocation failing in turn */
	for (pass = 0; pass < 2; ++pass) {
		for (k = 1; k < 100; ++k) {
			before = live;
			arm(k);
			if (pass)
				type = DtDtsDataToDataType(NULL, buffer, sizeof buffer, NULL, NULL, NULL, "plain");
			else
				type = DtDtsDataToDataType(name, NULL, -1, NULL, NULL, NULL, NULL);
			used = disarm();
			if (type) {
				if (strcmp(type, pass ? "LONGCONTENT" : "LONGNAME") != 0 &&
				    strcmp(type, DtDTS_DT_UNKNOWN) != 0)
					fprintf(stderr, "allocation %u failing gave '%s'\n", k, type);
				assert(strcmp(type, pass ? "LONGCONTENT" : "LONGNAME") == 0 ||
				       strcmp(type, DtDTS_DT_UNKNOWN) == 0);
				free(type);
			}
			assert(live == before);
			if (used < k)
				break;
		}
		assert(k < 100);
	}

	free(name);
	free(unmatched);
	free(pattern);
	free(text);
	free(content);
	assert(live == 0);
}

/* ---- type_content ---- */

struct fixture {
	type_info_t info;
	struct stat st;
	char *data;
};

static unsigned last_used;

/* Match attr against data, a file of the given mode, failing allocation FAIL. */
static int run_content(const char *data, size_t length, mode_t mode, const char *path,
		       const char *attr, unsigned fail)
{
	struct fixture f;
	char *copy;
	int match;

	memset(&f, 0, sizeof f);
	f.data = malloc(length ? length : 1);
	assert(f.data);
	memcpy(f.data, data, length);
	f.st.st_mode = mode;
	f.st.st_size = length;
	f.info.file_stat = &f.st;
	f.info.buffer = f.data;
	f.info.buff_size = (int)length;
	f.info.file_fd = -1;
	f.info.mmap_size_to_free = -1;
	f.info.size_to_free = -1;
	f.info.file_path = (char *)path;
	copy = repeat('x', strlen(attr));	/* exactly sized, like the text max_buf hands over */
	memcpy(copy, attr, strlen(attr));
	arm(fail);
	match = type_content(copy, &f.info);
	last_used = disarm();
	free(copy);
	free(f.data);
	return match;
}

static int content(const char *attr)
{
	static const char data[] = "HELLO world";

	return run_content(data, sizeof data - 1, S_IFREG | 0644, NULL, attr, 0);
}

static void scenario_type_content(const char *arena)
{
	static const char *const nothing[] = {
		"", "   ", "\t\n", "0", "0 ", "-", "-5", "0 string", "0 string ", "0 byte", "0 short", "0 long",
		"0 string nope", "6 string worlds", "12 string x",
		/* offsets no buffer can be indexed with */
		"4294967295 byte 0x48", "4294967295 string H", "4294967295 short 0", "4294967295 long 0",
		"2147483648 string H", "99999999999999999999 byte 0", "--3 byte 0x48", "--3 string H",
	};
	static const char *const something[] = {
		"0 string HELLO", "6 string world", "-5 string world", "0 byte 0x48", "0 byte 0x48 0x45",
		"1 short 0x454c", "0 long 0x48454c4c", "10 byte 0x64",
	};
	char present[MAXPATHLEN + 32];
	size_t i;
	unsigned k;
	long before;
	int match;

	for (i = 0; i < sizeof nothing / sizeof nothing[0]; ++i)
		assert(content(nothing[i]) == 0);
	for (i = 0; i < sizeof something / sizeof something[0]; ++i)
		assert(content(something[i]) == 1);

	/* a directory is matched by the name of a file inside it */
	snprintf(present, sizeof present, "%s/present", arena);
	write_file(present, "x");
	assert(run_content("", 0, S_IFDIR | 0755, arena, "filename present", 0) == 1);
	assert(run_content("", 0, S_IFDIR | 0755, arena, "filename absent", 0) == 0);
	assert(run_content("", 0, S_IFDIR | 0755, arena, "filename", 0) == 0);
	assert(run_content("", 0, S_IFDIR | 0755, NULL, "filename present", 0) == 0);

	/* every allocation of a match failing in turn is a mismatch, not a crash */
	for (i = 0; i < 2; ++i) {
		for (k = 1; k < 20; ++k) {
			before = live;
			if (i)
				match = run_content("", 0, S_IFDIR | 0755, arena, "filename present", k);
			else
				match = run_content("HELLO", 5, S_IFREG | 0644, NULL, "0 string HELLO", k);
			assert(match == 0 || match == 1);
			assert(live == before);
			if (last_used < k) {
				assert(match == 1);
				break;
			}
			assert(match == 0);
		}
		assert(k < 20);
	}
	unlink(present);
	assert(live == 0);
}

/* ---- the directory type file ---- */

static void scenario_directory_type(const char *arena)
{
	static const struct {
		const char *text;
		const char *type;
	} files[] = {
		{ "DATA_ATTRIBUTES_NAME mytype\n", "mytype" },
		{ "DIR_INFO\n{\n\tDATA_ATTRIBUTES_NAME\tfoo bar\n}\n", "foo" },
		{ "DATA_ATTRIBUTES_NAME mytype", "mytype" },
		{ "DATA_ATTRIBUTES_NAME\ttwo\twords", "two" },
		{ "DATA_ATTRIBUTES_NAME", NULL },
		{ "DATA_ATTRIBUTES_NAME  \n", NULL },
		{ "no keyword here\n", NULL },
		{ "", NULL },
	};
	char dir[MAXPATHLEN + 32], file[MAXPATHLEN + 64], *type, *longer;
	size_t i;
	unsigned k, used;
	long before;

	snprintf(dir, sizeof dir, "%s/dirtype", arena);
	assert(mkdir(dir, 0700) == 0);
	snprintf(file, sizeof file, "%s/%s", dir, DtDTS_DT_DIR);

	assert(_DtDtsGetDataType(dir) == NULL);		/* no type file */
	for (i = 0; i < sizeof files / sizeof files[0]; ++i) {
		write_file(file, files[i].text);
		type = _DtDtsGetDataType(dir);
		assert(files[i].type ? (type && strcmp(type, files[i].type) == 0) : type == NULL);
		free(type);
	}

	/* a directory name longer than any path */
	longer = repeat('a', 2 * MAXPATHLEN);
	assert(_DtDtsGetDataType(longer) == NULL);
	free(longer);

	write_file(file, "DATA_ATTRIBUTES_NAME mytype");
	for (k = 1; k < 20; ++k) {
		before = live;
		arm(k);
		type = _DtDtsGetDataType(dir);
		used = disarm();
		assert(type == NULL || strcmp(type, "mytype") == 0);
		free(type);
		assert(live == before);
		if (used < k)
			break;
	}
	assert(k < 20);

	/* setting the type: new file, kept file, replaced word, damaged file */
	unlink(file);
	type = DtDtsSetDataType(dir, "first", 0);
	assert(type && strcmp(type, "first") == 0);
	free(type);
	type = DtDtsSetDataType(dir, "second", 0);
	assert(type && strcmp(type, "first") == 0);
	free(type);
	type = DtDtsSetDataType(dir, "second", 1);
	assert(type && strcmp(type, "second") == 0);
	free(type);
	type = _DtDtsGetDataType(dir);
	assert(type && strcmp(type, "second") == 0);
	free(type);
	write_file(file, "DATA_ATTRIBUTES_NAME");
	type = DtDtsSetDataType(dir, "third", 1);
	assert(type && strcmp(type, "third") == 0);
	free(type);

	for (k = 1; k < 20; ++k) {
		write_file(file, "DATA_ATTRIBUTES_NAME old\n");
		before = live;
		arm(k);
		type = DtDtsSetDataType(dir, "swept", 1);
		used = disarm();
		assert(type == NULL || strcmp(type, "swept") == 0);
		free(type);
		assert(live == before);
		if (used < k)
			break;
	}
	assert(k < 20);

	unlink(file);
	rmdir(dir);
	assert(live == 0);
}

/* ---- keyword expansion ---- */

static void expect_expansion(const char *attr, const char *path, const char *expected)
{
	char *result = expand_keyword(attr, path);

	assert(result && strcmp(result, expected) == 0);
	free(result);
}

static void scenario_expand_keyword(const char *arena)
{
	static const char all[] = "<%dir%|%name%|%base%|%suffix%|%file%>";
	char *result;
	unsigned k, used;
	long before;

	(void)arena;
	expect_expansion("%file%", "/a/b.txt", "/a/b.txt");
	expect_expansion("[%dir%]", "/a/b.txt", "[/a]");
	expect_expansion("%name%", "/a/b.txt", "b.txt");
	expect_expansion("%base%", "/a/b.txt", "b");
	expect_expansion("%suffix%", "/a/b.txt", "txt");
	expect_expansion("%base%.%suffix%", "/a/b.txt", "b.txt");
	expect_expansion("no keyword, 100%", "/a/b.txt", "no keyword, 100%");
	expect_expansion("%unknown% %file%", "/a/b.txt", "%unknown% /a/b.txt");
	expect_expansion("%file%%file%%file%", "x", "xxx");
	expect_expansion("%file%", "", "");
	expect_expansion("%file%", NULL, "%file%");
	assert(expand_keyword(NULL, "/a/b.txt") == NULL);

	/* the suffix and the base are those of the name, not of the whole path */
	expect_expansion("%suffix%", "/a.d/README", "");
	expect_expansion("%base%", "/a.d/README", "README");
	expect_expansion("%suffix%", "/a.d/archive.tar.gz", "gz");
	expect_expansion("%base%", "/a.d/archive.tar.gz", "archive.tar");
	expect_expansion("%dir%", "/top", "/");
	expect_expansion("%dir%", "/a/b/c.d", "/a/b");

	/* a name without a directory part */
	expect_expansion("%dir%", "plain.c", ".");
	expect_expansion("%name%", "plain.c", "plain.c");
	expect_expansion("%base%", "plain.c", "plain");
	expect_expansion("%suffix%", "plain.c", "c");
	expect_expansion(all, "plain.c", "<.|plain.c|plain|c|plain.c>");

	/* every allocation failing in turn */
	for (k = 1; k < 20; ++k) {
		before = live;
		arm(k);
		result = expand_keyword(all, "/a/b.txt");
		used = disarm();
		assert(result == NULL || strcmp(result, "</a|b.txt|b|txt|/a/b.txt>") == 0);
		free(result);
		assert(live == before);
		if (used < k)
			break;
	}
	assert(k < 20);
	assert(live == 0);
}

/* ---- shell expansion ---- */

/* expand_shell() took the text only before commands were run with the keywords quoted */
static char *shell_expand(const char *attr, const char *path)
{
#ifdef OLD_EXPAND_SHELL
	(void)path;
	return expand_shell(attr);
#else
	return expand_shell(attr, path);
#endif
}

static void scenario_expand_shell(const char *arena)
{
	char *result, *zeros;
	unsigned k, used;
	long before;

	(void)arena;
	result = append(NULL, "ab");
	assert(result && strcmp(result, "ab") == 0);
	result = append(result, "cd");
	assert(result && strcmp(result, "abcd") == 0);
	free(result);

	/* append consumes its first argument, also when it fails */
	result = strdup("old");
	before = live;
	arm(1);
	assert(append(result, "more") == NULL);
	assert(disarm() == 1);
	assert(live == before - 1);
	arm(1);
	assert(append(NULL, "more") == NULL);
	assert(disarm() == 1);

	result = shell_expand("plain", NULL);
	assert(result && strcmp(result, "plain") == 0);
	free(result);
	result = shell_expand("a`printf XY`b", NULL);
	assert(result && strcmp(result, "aXYb") == 0);
	free(result);
	result = shell_expand("`printf 1`+`printf 2`", NULL);
	assert(result && strcmp(result, "1+2") == 0);
	free(result);
	zeros = repeat('0', 300);
	result = shell_expand("<`printf '%0300d' 0`>", NULL);
	assert(result && strlen(result) == 302 && strncmp(result + 1, zeros, 300) == 0);
	free(result);
	free(zeros);
	assert(shell_expand(NULL, NULL) == NULL);
	assert(open_pipes == 0);		/* every pipe was closed */

	/* a command that cannot be started is reported as text, not as a format */
	assert(shell_expand("x`popen-fails %s%n`y", NULL) == NULL);
	assert(strcmp(reported_format, "%s") == 0);
	assert(strcmp(reported_argument, "popen-fails %s%n") == 0);

	for (k = 1; k < 30; ++k) {
		before = live;
		arm(k);
		result = shell_expand("x`printf ab`y`printf cd`z", NULL);
		used = disarm();
		assert(result == NULL || strcmp(result, "xabycdz") == 0);
		free(result);
		assert(live == before && open_pipes == 0);
		if (used < k)
			break;
	}
	assert(k < 30);
	assert(live == 0);
}

/* ---- lists of attributes and data types ---- */

static void scenario_attribute_lists(const char *arena)
{
	static const char *const names[] = { "T1", "T2", "T3" };
	DtDtsMMRecord *record;
	DtDtsAttribute **attributes;
	char **list;
	unsigned k, used;
	long before;
	size_t i;
	int pass;

	(void)arena;

	/* Three types that all carry the attribute searched for: the list of
	 * names needs room for each of them and the terminating null. */
	reset_database();
	for (i = 0; i < 3; ++i) {
		record = add_record(names[i]);
		add_field(record, "MIME", "text/plain");
		add_field(record, "ICON", "%name%.pm");
	}
	list = DtDtsFindAttribute("MIME", "text/plain");
	assert(list && list[0] && list[1] && list[2] && !list[3]);
	assert(strcmp(list[0], "T1") == 0 && strcmp(list[2], "T3") == 0);
	DtDtsFreeDataTypeNames(list);
	list = DtDtsFindAttribute("MIME", "image/png");
	assert(list && !list[0]);
	DtDtsFreeDataTypeNames(list);
	list = DtDtsDataTypeNames();
	assert(list && list[0] && list[1] && list[2] && !list[3]);
	DtDtsFreeDataTypeNames(list);

	reset_database();
	record = add_record("ONLY");
	add_field(record, "MIME", "text/plain");
	list = DtDtsFindAttribute("MIME", "text/plain");
	assert(list && list[0] && strcmp(list[0], "ONLY") == 0 && !list[1]);
	DtDtsFreeDataTypeNames(list);

	reset_database();
	for (i = 0; i < 3; ++i) {
		record = add_record(names[i]);
		add_field(record, "MIME", "text/plain");
		add_field(record, "ICON", "%name%.pm");
	}
	attributes = DtDtsDataTypeToAttributeList("T2", "/x/y.txt");
	assert(attributes && attributes[0] && attributes[1] && !attributes[2]);
	assert(strcmp(attributes[0]->name, "MIME") == 0 && strcmp(attributes[0]->value, "text/plain") == 0);
	assert(strcmp(attributes[1]->name, "ICON") == 0 && strcmp(attributes[1]->value, "y.txt.pm") == 0);
	DtDtsFreeAttributeList(attributes);
	assert(DtDtsDataTypeToAttributeList("nope", NULL) == NULL);
	assert(DtDtsDataTypeToAttributeList(NULL, NULL) == NULL);

	/* each allocation failing in turn */
	for (pass = 0; pass < 3; ++pass) {
		for (k = 1; k < 40; ++k) {
			before = live;
			arm(k);
			if (pass == 0) {
				list = DtDtsFindAttribute("MIME", "text/plain");
				used = disarm();
				assert(!list || (list[0] && list[1] && list[2] && !list[3]));
				if (list)
					DtDtsFreeDataTypeNames(list);
			} else if (pass == 1) {
				list = DtDtsDataTypeNames();
				used = disarm();
				assert(!list || (list[0] && list[1] && list[2] && !list[3]));
				if (list)
					DtDtsFreeDataTypeNames(list);
			} else {
				attributes = DtDtsDataTypeToAttributeList("T2", "/x/y.txt");
				used = disarm();
				if (attributes)
					DtDtsFreeAttributeList(attributes);
			}
			assert(live == before);
			if (used < k)
				break;
		}
		assert(k < 40);
	}

	/* without a loaded database there is nothing to list */
	da_database_loaded = 0;
	assert(DtDtsDataTypeToAttributeList("T1", NULL) == NULL);
	assert(DtDtsDataTypeNames() == NULL);
	assert(DtDtsFindAttribute("MIME", "text/plain") == NULL);
	assert(live == 0);
}

/* ---- the per-lookup state ---- */

static void scenario_set_vals(const char *arena)
{
	struct stat st;
	type_info_t *info;
	char here[MAXPATHLEN], cwd[MAXPATHLEN + 64], component[256], *name, *expected;
	char *components[64];
	size_t length, depth, i, last;
	unsigned k, used;
	long before;
	int form;

	memset(&st, 0, sizeof st);
	st.st_mode = S_IFREG | 0644;

	/* every allocation failing in turn releases what was set up */
	for (form = 0; form < 2; ++form) {
		for (k = 1; k < 30; ++k) {
			before = live;
			arm(k);
			if (form)
				info = set_vals(NULL, "data", 4, NULL, NULL, NULL, "opt");
			else
				info = set_vals("rel.txt", NULL, -1, &st, "/l/target", &st, "opt");
			used = disarm();
			if (info)
				free(cleanup(0, info));
			else
				assert(used >= k);
			assert(live == before);
			if (used < k)
				break;
		}
		assert(k < 30);
	}

	/* a relative name is qualified with the working directory, and its
	 * name part is the name itself even if the directory repeats it */
	assert(getcwd(here, sizeof here));
	snprintf(cwd, sizeof cwd, "%s/dts-name-test", arena);
	assert(mkdir(cwd, 0700) == 0 && chdir(cwd) == 0);
	info = set_vals("dts-name-test", NULL, -1, NULL, NULL, NULL, NULL);
	assert(info);
	length = strlen(info->file_path);
	assert(strcmp(info->name, "dts-name-test") == 0);
	assert(info->name == info->file_path + length - strlen("dts-name-test"));
	assert(strcmp(info->file_path + length - 2 * strlen("dts-name-test") - 1,
		      "dts-name-test/dts-name-test") == 0);
	free(cleanup(0, info));
	assert(chdir(here) == 0 && rmdir(cwd) == 0);

	/* a working directory that fills the path buffer leaves no room for a
	 * relative name, which then has to be put together elsewhere */
	assert(chdir(arena) == 0);
	depth = 0;
	for (;;) {
		size_t room;

		assert(getcwd(cwd, sizeof cwd));
		room = (MAXPATHLEN - 12) - strlen(cwd) - 1;
		assert(room >= 1 && depth < 64);
		last = room > 150 ? 100 : room;
		memset(component, depth % 2 ? 'e' : 'd', last);
		component[last] = '\0';
		components[depth] = strdup(component);
		assert(components[depth]);
		assert(mkdir(component, 0700) == 0 && chdir(component) == 0);
		++depth;
		if (last == room)
			break;
	}
	assert(getcwd(cwd, sizeof cwd) && strlen(cwd) == MAXPATHLEN - 12);
	name = repeat('n', 40);
	info = set_vals(name, NULL, -1, NULL, NULL, NULL, NULL);
	assert(info);
	expected = concat(cwd, "/", name);
	assert(strcmp(info->file_path, expected) == 0);
	assert(strcmp(info->name, name) == 0);
	free(expected);
	free(name);
	free(cleanup(0, info));
	for (i = depth; i-- > 0;) {
		assert(chdir("..") == 0 && rmdir(components[i]) == 0);
		free(components[i]);
	}
	assert(chdir(here) == 0);
	assert(live == 0);
}

/* ---- commands in values ---- */

static void scenario_shell_text(const char *arena)
{
	char *result;

	(void)arena;
	/* a backquote without a partner is text */
	result = shell_expand("a`b", NULL);
	assert(result && strcmp(result, "a`b") == 0);
	free(result);
	result = shell_expand("x`printf 1`y`z", NULL);
	assert(result && strcmp(result, "x1y`z") == 0);
	free(result);
	result = shell_expand("`", NULL);
	assert(result && strcmp(result, "`") == 0);
	free(result);

	/* the keywords of a command are one word each */
	result = shell_expand("`printf '[%s]' %name%`", "/d/a b;c.txt");
	assert(result && strcmp(result, "[a b;c.txt]") == 0);
	free(result);
	result = shell_expand("`printf '[%s]' %file%`", "/d/it's.txt");
	assert(result && strcmp(result, "[/d/it's.txt]") == 0);
	free(result);
	result = shell_expand("`printf '[%s]' %suffix%`", "/d/noext");
	assert(result && strcmp(result, "[]") == 0);
	free(result);
	assert(live == 0);
}

/* A file name with a command in backquotes is a name, not a command. */
static void scenario_value_injection(const char *arena)
{
	char here[MAXPATHLEN], marker[MAXPATHLEN + 16], *path, *result;
	DtShmBoson plain = _DtDtsMMStringToBoson("icon-%name%-%file%");
	DtShmBoson quoted = _DtDtsMMStringToBoson("`printf '<%s>' %name%`");
	struct stat st;

	assert(getcwd(here, sizeof here));
	assert(chdir(arena) == 0);
	snprintf(marker, sizeof marker, "%s/pwned", arena);
	unlink(marker);
	path = concat(arena, "/x`touch pwned`.txt", "");

	result = expand_value(plain, path);
	assert(result);
	assert(strstr(result, "icon-x`touch pwned`.txt-") == result);
	free(result);
	assert(stat(marker, &st) == -1);

	result = expand_value(quoted, path);
	assert(result && strcmp(result, "<x`touch pwned`.txt>") == 0);
	free(result);
	assert(stat(marker, &st) == -1);

	free(path);
	assert(chdir(here) == 0);
	assert(live == 0);
}

/* A byte that is not a character is that byte for the comparison. */
static void scenario_invalid_characters(const char *arena)
{
	(void)arena;
	if (!setlocale(LC_ALL, "C.UTF-8"))
		return;				/* no locale with invalid sequences */
	assert(gmatch("\xff", "\xfe") == 0);
	assert(gmatch("\xff", "\xff") == 1);
	assert(gmatch("a\xff" "b", "a?b") == 1);
	assert(gmatch("\xfe", "[\xfe\xff]") == 1);
	assert(gmatch("\xff\xfe", "\xff\xff") == 0);
	assert(gmatch("na\xc3\xafve", "na?ve") == 1);
	setlocale(LC_ALL, "C");
}

#ifdef REAL_PROBES
/* ---- the file probes ---- */

static char *make_path(const char *arena, const char *name)
{
	return concat(arena, "/", name);
}

static const char *link_of(const char *path, type_info_t **info)
{
	*info = set_vals(path, NULL, -1, NULL, NULL, NULL, NULL);
	assert(*info);
	get_link_info(*info);
	return (*info)->link_path;
}

static void scenario_links(const char *arena)
{
	char *dir = make_path(arena, "links"), *target, *rel, *chain, *dangling, *a, *b;
	char *up, *path, *expected;
	type_info_t *info;
	const char *found;
	int i;

	assert(mkdir(dir, 0700) == 0);
	target = make_path(dir, "target");
	write_file(target, "x");
	rel = make_path(dir, "rel");
	chain = make_path(dir, "chain");
	dangling = make_path(dir, "dangling");
	a = make_path(dir, "a");
	b = make_path(dir, "b");
	assert(symlink("target", rel) == 0);
	assert(symlink("rel", chain) == 0);
	assert(symlink("missing", dangling) == 0);
	assert(symlink("b", a) == 0);
	assert(symlink("a", b) == 0);

	/* a file that is no link leads to itself */
	found = link_of(target, &info);
	assert(found && strcmp(found, target) == 0);
	assert(strcmp(info->link_name, "target") == 0);
	free(cleanup(0, info));

	/* the target of a link is whole, and a relative one starts in the
	   directory of the link */
	found = link_of(rel, &info);
	assert(found && strcmp(found, target) == 0);
	assert(strcmp(info->link_name, "target") == 0);
	free(cleanup(0, info));
	found = link_of(chain, &info);
	assert(found && strcmp(found, target) == 0);
	free(cleanup(0, info));

	/* an absolute target, and a target in another directory */
	up = make_path(arena, "up");
	assert(symlink(target, up) == 0);
	found = link_of(up, &info);
	assert(found && strcmp(found, target) == 0);
	free(cleanup(0, info));
	path = make_path(dir, "sub");
	assert(mkdir(path, 0700) == 0);
	free(path);
	path = make_path(dir, "sub/outward");
	assert(symlink("../target", path) == 0);
	found = link_of(path, &info);
	expected = make_path(dir, "sub/../target");
	assert(found && strcmp(found, expected) == 0);
	free(cleanup(0, info));
	free(expected);
	unlink(path);
	free(path);
	path = make_path(dir, "sub");
	rmdir(path);
	free(path);

	/* a link to nothing is the name that is missing */
	found = link_of(dangling, &info);
	assert(found && found != (char *)-1);
	assert(strcmp(info->link_name, "missing") == 0);
	free(cleanup(0, info));

	/* links that lead back to themselves end with an error */
	alarm(20);
	found = link_of(a, &info);
	alarm(0);
	assert(found == (char *)-1);
	assert(info->error == ELOOP);
	free(cleanup(0, info));

	/* a long chain that ends */
	for (i = 0; i < 30; ++i) {
		char from[32], to[32];

		snprintf(from, sizeof from, "c%d", i);
		snprintf(to, sizeof to, i ? "c%d" : "target", i - 1);
		path = make_path(dir, from);
		assert(symlink(to, path) == 0);
		free(path);
	}
	path = make_path(dir, "c29");
	found = link_of(path, &info);
	assert(found && strcmp(found, target) == 0);
	free(cleanup(0, info));
	free(path);
	for (i = 0; i < 30; ++i) {
		char from[32];

		snprintf(from, sizeof from, "c%d", i);
		path = make_path(dir, from);
		unlink(path);
		free(path);
	}

	unlink(a);
	unlink(b);
	unlink(dangling);
	unlink(chain);
	unlink(rel);
	unlink(up);
	unlink(target);
	rmdir(dir);
	free(a);
	free(b);
	free(dangling);
	free(chain);
	free(rel);
	free(up);
	free(target);
	free(dir);
	assert(live == 0);
}

static void scenario_contents(const char *arena)
{
	char *path = make_path(arena, "contents"), *text = repeat('A', 100);
	type_info_t *info;
	const char *buffer;
	unsigned k, used;
	size_t i;

	write_file(path, text);

	/* mapped */
	info = set_vals(path, NULL, -1, NULL, NULL, NULL, NULL);
	buffer = get_buff(info);
	assert(buffer && get_buff_size(info) == 100 && memcmp(buffer, text, 100) == 0);
	free(cleanup(0, info));

	/* read, and the file ends early: what was not read is zeros */
	mapping_fails = 1;
	read_mode = 1;
	info = set_vals(path, NULL, -1, NULL, NULL, NULL, NULL);
	buffer = get_buff(info);
	assert(buffer);
	assert(get_buff_size(info) == 40);
	for (i = 0; i < 101; ++i)
		assert(i < 40 ? buffer[i] == 'A' : buffer[i] == 0);
	free(cleanup(0, info));
	read_mode = 0;

	/* read completely */
	info = set_vals(path, NULL, -1, NULL, NULL, NULL, NULL);
	buffer = get_buff(info);
	assert(buffer && get_buff_size(info) == 100 && memcmp(buffer, text, 100) == 0);
	free(cleanup(0, info));

	/* a read that fails gives nothing, now and when asked again */
	read_mode = 2;
	info = set_vals(path, NULL, -1, NULL, NULL, NULL, NULL);
	assert(get_buff(info) == NULL);
	assert(info->error == EIO);
	assert(info->buffer == NULL);
	info->error = 0;
	assert(get_buff(info) == NULL);
	free(cleanup(0, info));
	read_mode = 0;

	/* memory that runs out while the contents are read */
	for (k = 1; k < 20; ++k) {
		long before = live;

		info = set_vals(path, NULL, -1, NULL, NULL, NULL, NULL);
		assert(info);
		arm(k);
		buffer = get_buff(info);
		used = disarm();
		assert(buffer == NULL || get_buff_size(info) == 100);
		free(cleanup(0, info));
		assert(live == before);
		if (used < k)
			break;
	}
	assert(k < 20);
	mapping_fails = 0;

	/* an empty file has nothing */
	write_file(path, "");
	info = set_vals(path, NULL, -1, NULL, NULL, NULL, NULL);
	assert(get_buff(info) == NULL);
	free(cleanup(0, info));

	unlink(path);
	free(path);
	free(text);
	assert(live == 0);
}

/* The size of a buffer given by the caller is what the matching trusts. */
static void scenario_buffer_sizes(const char *arena)
{
	static const struct {
		const char *attr;
		int size;
		int match;
	} tests[] = {
		/* a size that says there is nothing to look at */
		{ "0 byte 0x48", -1, 0 }, { "0 byte 0x48", 0, 0 }, { "0 byte 0x48", -5, 0 },
		{ "-5 string world", -5, 0 }, { "0 string HELLO", 0, 0 },
		{ "5 byte 0x48", -5, 0 }, { "5 string x", -1, 0 }, { "5 byte 0x48", 0, 0 },
		/* positions beyond the end */
		{ "100 byte 0x48", 11, 0 }, { "12 string x", 11, 0 }, { "11 byte 0", 11, 0 },
		{ "10 short 0", 11, 0 }, { "8 long 0x6f", 11, 0 },
		/* positions in it, also counted from the end */
		{ "0 string HELLO", 11, 1 }, { "6 string world", 11, 1 }, { "-5 string world", 11, 1 },
		{ "-11 string HELLO", 11, 1 }, { "-12 string HELLO", 11, 0 }, { "-0 string x", 11, 0 },
		{ "0 byte 0x48", 4, 1 }, { "3 string LO", 5, 1 }, { "3 string LO ", 4, 0 },
	};
	size_t i;

	(void)arena;
	for (i = 0; i < sizeof tests / sizeof tests[0]; ++i) {
		/* the buffer is exactly as long as it is said to be, or shorter */
		size_t length = tests[i].size > 0 ? (size_t)tests[i].size : 1;
		char *buffer = malloc(length), *attr = strdup(tests[i].attr);
		type_info_t *info;

		assert(buffer && attr);
		memcpy(buffer, "HELLO world", length);
		info = set_vals(NULL, buffer, tests[i].size, NULL, NULL, NULL, NULL);
		assert(info);
		assert(type_content(attr, info) == tests[i].match);
		free(cleanup(0, info));
		free(attr);
		free(buffer);
	}
	assert(live == 0);
}
#endif

/* ---- driver ---- */

int main(int argc, char **argv)
{
	static const struct {
		const char *name;
		void (*run)(const char *);
	} scenarios[] = {
		{ "max_buf", scenario_max_buf },
		{ "data_to_data_type", scenario_data_to_data_type },
		{ "type_content", scenario_type_content },
		{ "directory_type", scenario_directory_type },
		{ "expand_keyword", scenario_expand_keyword },
		{ "expand_shell", scenario_expand_shell },
		{ "attribute_lists", scenario_attribute_lists },
		{ "set_vals", scenario_set_vals },
		{ "shell_text", scenario_shell_text },
		{ "value_injection", scenario_value_injection },
		{ "invalid_characters", scenario_invalid_characters },
#ifdef REAL_PROBES
		{ "links", scenario_links },
		{ "contents", scenario_contents },
		{ "buffer_sizes", scenario_buffer_sizes },
#endif
	};
	size_t i;

	assert(argc == 3);
	for (i = 0; i < sizeof scenarios / sizeof scenarios[0]; ++i) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			scenarios[i].run(argv[2]);
			return 0;
		}
	}
	fprintf(stderr, "unknown scenario %s\n", argv[1]);
	return 2;
}
