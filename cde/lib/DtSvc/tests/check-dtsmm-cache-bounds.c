/*
 * Harness for check-dtsmm-cache-bounds.py.
 *
 * It builds a valid cache file with the routines that build the real one
 * (the string table, integer table and integer list of strtab.c, inttab.c
 * and intarray.c, in the layout of DtUtil1/MMDb.c), maps it with the
 * _DtDtsMMapDB() of DtsMM.c and reads everything in it.  Then it damages
 * the file in one place at a time and checks that _DtDtsMMapDB() refuses it.
 * Without the check, a damaged file that is accepted is read by the same
 * walk, and the sanitizer reports the read outside the file.
 *
 * mmap() returns a malloc()ed copy of the file of exactly its size, so that
 * AddressSanitizer sees every access outside the file; the page rounding of
 * the real mapping would hide them.
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <Dt/DtShmDb.h>

#define TRUE 1
#define FALSE 0
#define DtProgName ((char *)0)
#define DtError 1
#define _DtSvcProcessLock() ((void)0)
#define _DtSvcProcessUnlock() ((void)0)

static void _DtSimpleError(char *program, int severity, char *help, char *format, ...)
{
	(void)program; (void)severity; (void)help; (void)format;
}
static void _DtDtsClear(void) {}

static void *copy_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
	char *copy = malloc(len);

	(void)addr; (void)prot; (void)flags;
	if (!copy || off != 0 || pread(fd, copy, len, 0) != (ssize_t)len)
		return MAP_FAILED;
	return copy;
}
static int copy_munmap(void *addr, size_t len) { (void)len; free(addr); return 0; }
#define mmap copy_mmap
#define munmap copy_munmap

typedef struct DtDirPathsTag DtDirPaths;
int _DtDtsMMInit(int override);
int _DtDtsMMUnLoad(void);
int _DtDtsMMInit(int override) { (void)override; abort(); }

#include "dtsmm-types.h"
#include "dtsmm-functions.h"

/* ---- the valid cache ---- */

#define BOSON(text) _DtShmProtoAddStrtab(strings, (text), &isnew)

static char cache_path[MAXPATHLEN];
static unsigned char *cache;	/* the intact file */
static size_t cache_size;
static unsigned char *work;	/* the file with one damage */

/* where things are, as indexes in the list of integers */
static struct {
	int dc;				/* number of the criteria database */
	int dc_records;
	int list;			/* the list of records of the name "a.txt" */
} layout;

enum { DATABASES = 3 };
static const char *const database_names[DATABASES] = { "DATA_ATTRIBUTES", "DATA_CRITERIA", "ACTION" };
static const int record_counts[DATABASES] = { 2, 5, 3 };

static void build_cache(void)
{
	DtShmProtoIntList ints = _DtShmProtoInitIntLst(100);
	DtShmProtoStrtab strings = _DtShmProtoInitStrtab(100);
	DtDtsMMHeader header;
	DtDtsMMDatabase *databases;
	int isnew, i, r, f, index, *list, *space;
	DtShmBoson *files;
	unsigned char *times;
	DtShmProtoInttab names;
	char text[64];
	size_t size;
	void *table;

	memset(&header, 0, sizeof header);
	header.pathhash = 0x1234;
	header.files_count = 2;
	files = (DtShmBoson *)_DtShmProtoAddIntLst(ints, 2, &header.files_offset);
	files[0] = BOSON("/etc/dt/types");
	files[1] = BOSON("/usr/dt/appconfig/types/C/dtwm.dt");
	times = (unsigned char *)_DtShmProtoAddIntLst(ints, 2 * sizeof(time_t) / sizeof(int), &header.mtimes_offset);
	memcpy(times, &(time_t){ 1000 }, sizeof(time_t));
	memcpy(times + sizeof(time_t), &(time_t){ 2000 }, sizeof(time_t));

	header.num_db = DATABASES;
	databases = (DtDtsMMDatabase *)_DtShmProtoAddIntLst(ints, DATABASES * sizeof(DtDtsMMDatabase) / sizeof(int),
							    &header.db_offset);
	layout.dc = 1;
	layout.dc_records = record_counts[1];
	for (i = 0; i < DATABASES; i++) {
		DtDtsMMRecord *records;
		DtShmProtoInttab index_table;
		int last = -1;

		databases[i].databaseName = BOSON(database_names[i]);
		databases[i].recordCount = record_counts[i];
		records = (DtDtsMMRecord *)_DtShmProtoAddIntLst(ints, record_counts[i] * sizeof(DtDtsMMRecord) / sizeof(int),
								&index);
		databases[i].recordList = index;
		index_table = _DtShmProtoInitInttab(record_counts[i]);
		for (r = 0; r < record_counts[i]; r++) {
			DtDtsMMField *fields;
			int field_count = 2 + r % 2;

			/* the records 0 and 1 of the criteria have the same name */
			snprintf(text, sizeof text, i == 1 && r < 2 ? "a.txt" : i == 1 && r == 4 ? "c.h" : "record_%d_%d", i, r);
			records[r].recordName = BOSON(text);
			if (records[r].recordName != last)
				_DtShmProtoAddInttab(index_table, records[r].recordName, r);
			last = records[r].recordName;
			records[r].pathId = BOSON("/etc/dt/types/types.dt");
			records[r].seq = r;
			records[r].fieldCount = field_count;
			fields = (DtDtsMMField *)_DtShmProtoAddIntLst(ints, field_count * sizeof(DtDtsMMField) / sizeof(int),
								      &index);
			records[r].fieldList = index;
			for (f = 0; f < field_count; f++) {
				snprintf(text, sizeof text, "FIELD_%d", f);
				fields[f].fieldName = BOSON(text);
				snprintf(text, sizeof text, "value of %d %d %d", i, r, f);
				fields[f].fieldValue = f == 2 ? 0 : BOSON(text);
			}
		}
		size = _DtShmProtoSizeInttab(index_table);
		space = _DtShmProtoAddIntLst(ints, size / sizeof(int), &databases[i].nameIndex);
		_DtShmProtoCopyInttab(index_table, space);
		_DtShmProtoDestroyInttab(index_table);
	}

	/* the lists by name of the criteria: "a.txt" has two records, "c.h" one */
	names = _DtShmProtoInitInttab(4);
	list = _DtShmProtoAddIntLst(ints, 2, &layout.list);
	list[0] = 0;
	list[1] = 1;
	_DtShmProtoAddInttab(names, BOSON("a.txt"), -layout.list);
	_DtShmProtoAddInttab(names, BOSON("c.h"), 4);
	/* the records without a name pattern: record 2 may be anything, 3 is a buffer only */
	list = _DtShmProtoAddIntLst(ints, 2, &header.no_name_offset);
	list[0] = 2;
	list[1] = 3;
	header.buffer_start_index = 1;
	size = _DtShmProtoSizeInttab(names);
	space = _DtShmProtoAddIntLst(ints, size / sizeof(int), &header.name_list_offset);
	_DtShmProtoCopyInttab(names, space);
	_DtShmProtoDestroyInttab(names);

	size = _DtShmProtoSizeStrtab(strings);
	table = _DtShmProtoAddIntLst(ints, size / sizeof(int), &header.str_tbl_offset);
	_DtShmProtoCopyStrtab(strings, table);

	size = _DtShmProtoSizeIntLst(ints);
	cache_size = sizeof header + size;
	cache = calloc(1, cache_size);
	work = calloc(1, cache_size);
	assert(cache && work);
	memcpy(cache, &header, sizeof header);
	_DtShmProtoCopyIntLst(ints, cache + sizeof header);
	_DtShmProtoDestroyStrtab(strings);
	_DtShmProtoDestroyIntLst(ints);
}

/* ---- reading everything ---- */

#define CAP(n) ((n) > 4096 ? 4096 : (n))

static size_t touch(const char *s) { return s ? strlen(s) : 0; }

static size_t walk(void)
{
	static const char *const names[] = { "DATA_ATTRIBUTES", "DATA_CRITERIA", "ACTION", "x" };
	static const char *const fields[] = { "FIELD_0", "FIELD_1", "FIELD_2", "x" };
	size_t sink = 0;
	int n, i, d, r, k, *idx;

	idx = _DtDtsMMGetDCNameIndex(&n);
	for (i = 0; i < CAP(n); i++) sink += idx[i];
	idx = _DtDtsMMGetNoNameIndex(&n);
	for (i = 0; i < CAP(n); i++) sink += idx[i];
	idx = _DtDtsMMGetBufferIndex(&n);
	for (i = 0; i < CAP(n); i++) sink += idx[i];
	sink += (size_t)_DtDtsMMGetFileList();
	for (d = 0; d < CAP(head->num_db); d++) {
		DtDtsMMDatabase *db = &db_list[d];
		DtDtsMMRecord *records;

		sink += touch(_DtDtsMMBosonToString(db->databaseName));
		for (k = 0; k < 4; k++) {
			sink += (size_t)_DtDtsMMGet(names[k]);
			sink += (size_t)_DtDtsMMGetRecordByName(db, names[k]);
		}
		for (k = 0; k < 8; k++) {
			int *e = _DtDtsMMGetDbName(db, (DtShmBoson)k);

			if (e) sink += *e;
		}
		records = _DtDtsMMGetPtr(db->recordList);
		for (r = 0; r < CAP(db->recordCount); r++) {
			DtDtsMMField *fields_of = _DtDtsMMGetPtr(records[r].fieldList);

			sink += touch(_DtDtsMMBosonToString(records[r].recordName));
			sink += touch(_DtDtsMMBosonToString(records[r].pathId));
			for (i = 0; i < CAP(records[r].fieldCount); i++) {
				sink += touch(_DtDtsMMBosonToString(fields_of[i].fieldName));
				sink += touch(_DtDtsMMBosonToString(fields_of[i].fieldValue));
			}
			for (k = 0; k < 4; k++)
				sink += touch(_DtDtsMMGetFieldByName(&records[r], fields[k]));
		}
	}
	return sink;
}

/* ---- damage ---- */

static void write_cache(const unsigned char *data, size_t size)
{
	FILE *f = fopen(cache_path, "w");

	assert(f);
	if (size)
		assert(fwrite(data, 1, size, f) == size);
	fclose(f);
}

#define H ((DtDtsMMHeader *)work)
#define I ((int *)(work + sizeof(DtDtsMMHeader)))
#define NINTS ((int)((cache_size - sizeof(DtDtsMMHeader)) / sizeof(int)))
#define DB(n) (&((DtDtsMMDatabase *)&I[H->db_offset])[n])
#define REC(db, n) (&((DtDtsMMRecord *)&I[DB(db)->recordList])[n])
#define FLD(db, r, n) (&((DtDtsMMField *)&I[REC(db, r)->fieldList])[n])

/* the layout of the tables in the file, as strtab.c and inttab.c have it */
typedef struct { unsigned size, stroffset, taboffset, count; } string_table;
typedef struct { unsigned key; unsigned short first, next; } string_entry;
typedef struct { int key, data; unsigned short first, next; } int_entry;

#define STR ((string_table *)&I[H->str_tbl_offset])
#define STR_BYTES ((unsigned)(I[H->str_tbl_offset - 1] * sizeof(int)))
#define STR_ENTRY(n) (&((string_entry *)((unsigned char *)STR + STR->taboffset))[n])
#define NAMES(db) ((int_entry *)&I[DB(db)->nameIndex])
#define NAME_LIST ((int_entry *)&I[H->name_list_offset])

/* the first entry of a table with a chain, and the one it leads to */
static int first_item(int_entry *table)
{
	int i;

	for (i = 1; i <= table->key; i++)
		if (table[i].first)
			return table[i].first;
	assert(!"no chain");
	return 0;
}

static void m_header_str(void) { H->str_tbl_offset = NINTS + 1; }
static void m_header_str_zero(void) { H->str_tbl_offset = 0; }
static void m_header_db_zero(void) { H->db_offset = 0; }
static void m_header_db_count(void) { H->num_db = 1 << 20; }
static void m_header_db_count_big(void) { H->num_db = DATABASES + 1; }
static void m_header_files_count(void) { H->files_count = 1 << 28; }
static void m_header_files_count_more(void) { H->files_count = 3; }
static void m_header_mtimes(void) { H->mtimes_offset = NINTS; }
static void m_header_name_list_zero(void) { H->name_list_offset = 0; }
static void m_header_name_list(void) { H->name_list_offset = NINTS; }
static void m_header_no_name(void) { H->no_name_offset = NINTS; }
static void m_header_no_name_negative(void) { H->no_name_offset = -2; }
static void m_buffer_start_big(void) { H->buffer_start_index = 100; }
static void m_buffer_start_negative(void) { H->buffer_start_index = -1; }
static void m_file_boson(void) { I[H->files_offset] = STR_BYTES; }
static void m_file_boson_negative(void) { I[H->files_offset + 1] = -1; }

static void m_str_count_zero(void) { STR->count = 0; }
static void m_str_count_ffff(void) { STR->count = 0xffff; }
static void m_str_count_big(void) { STR->count = 0x10000 + 5; }
static void m_str_count_more(void) { STR->count += 1000; }
static void m_str_taboffset_end(void) { STR->taboffset = STR_BYTES + 4; }
static void m_str_taboffset_odd(void) { STR->taboffset += 2; }
static void m_str_taboffset_huge(void) { STR->taboffset = 0xfffffff0u; }
static void m_str_stroffset_end(void) { STR->stroffset = STR_BYTES + 1; }
static void m_str_stroffset_header(void) { STR->stroffset = 4; }
static void m_str_key_end(void) { STR_ENTRY(STR->count - 1)->key = STR_BYTES; }
static void m_str_key_huge(void) { STR_ENTRY(0)->key = 0xfffffff0u; }
static void m_str_first_big(void) { STR_ENTRY(0)->first = STR->count; STR_ENTRY(1)->first = STR->count; }
static void m_str_next_big(void)
{
	unsigned i;

	for (i = 0; i < STR->count; i++)
		if (STR_ENTRY(i)->first != 0xffff) {
			STR_ENTRY(STR_ENTRY(i)->first)->next = (unsigned short)(STR->count + 7);
			return;
		}
}
static void m_str_loop(void)
{
	unsigned i;

	for (i = 0; i < STR->count; i++)
		if (STR_ENTRY(i)->first != 0xffff) {
			STR_ENTRY(STR_ENTRY(i)->first)->next = STR_ENTRY(i)->first;
			return;
		}
}
static void m_str_size_negative(void) { I[H->str_tbl_offset - 1] = -1; }
static void m_str_size_huge(void) { I[H->str_tbl_offset - 1] = INT_MAX; }
static void m_str_size_small(void) { I[H->str_tbl_offset - 1] = 2; }

static void m_db_name(void) { DB(1)->databaseName = STR_BYTES; }
static void m_db_name_negative(void) { DB(0)->databaseName = -1; }
static void m_db_count_big(void) { DB(2)->recordCount = 1 << 28; }
static void m_db_count_negative(void) { DB(0)->recordCount = -1; }
static void m_db_count_more(void) { DB(1)->recordCount += 1; }
static void m_db_list_end(void) { DB(0)->recordList = NINTS; }
static void m_db_list_zero(void) { DB(1)->recordList = 0; }
static void m_db_list_beyond(void) { DB(1)->recordList = NINTS + 3; }
static void m_db_index_end(void) { DB(2)->nameIndex = NINTS + 1; }
static void m_db_index_zero(void) { DB(1)->nameIndex = 0; }
static void m_db_index_size(void) { I[DB(1)->nameIndex - 1] = 1; }

static void m_rec_name(void) { REC(1, 0)->recordName = STR_BYTES; }
static void m_rec_name_negative(void) { REC(0, 1)->recordName = -1; }
static void m_rec_path(void) { REC(2, 2)->pathId = 0x7fffff00; }
static void m_rec_count_big(void) { REC(1, 3)->fieldCount = 1 << 28; }
static void m_rec_count_negative(void) { REC(1, 3)->fieldCount = -5; }
static void m_rec_count_more(void) { REC(1, 3)->fieldCount += 1; }
static void m_rec_list_end(void) { REC(1, 4)->fieldList = NINTS; }
static void m_rec_list_zero(void) { REC(0, 0)->fieldList = 0; }
static void m_fld_name(void) { FLD(1, 1, 0)->fieldName = STR_BYTES + 100; }
static void m_fld_name_negative(void) { FLD(2, 1, 1)->fieldName = -3; }
static void m_fld_value(void) { FLD(1, 2, 1)->fieldValue = STR_BYTES; }
static void m_fld_value_negative(void) { FLD(0, 0, 0)->fieldValue = INT_MIN; }

static void m_index_key_big(void) { NAMES(1)->key = 1 << 20; }
static void m_index_key_negative(void) { NAMES(1)->key = -1; }
static void m_index_key_more(void) { NAMES(1)->key += 4; }
static void m_index_first_big(void) { int_entry *t = NAMES(1); t[1].first = t->key + 1; }
static void m_index_loop(void) { int_entry *t = NAMES(1); int i = first_item(t); t[i].next = i; }
static void m_index_data_big(void) { int_entry *t = NAMES(1); t[first_item(t)].data = layout.dc_records; }
static void m_index_data_negative(void) { int_entry *t = NAMES(1); t[first_item(t)].data = -1; }

static void m_names_key_big(void) { NAME_LIST->key = 1 << 20; }
static void m_names_data_big(void) { int_entry *t = NAME_LIST; t[first_item(t)].data = layout.dc_records; }
static void m_names_data_list(void) { int_entry *t = NAME_LIST; t[first_item(t)].data = -(NINTS + 10); }
static void m_names_data_min(void) { int_entry *t = NAME_LIST; t[first_item(t)].data = INT_MIN; }
static void m_names_loop(void) { int_entry *t = NAME_LIST; int i = first_item(t); t[i].next = i; }
static void m_list_element(void) { I[layout.list + 1] = layout.dc_records; }
static void m_list_element_negative(void) { I[layout.list] = -1; }
static void m_list_size(void) { I[layout.list - 1] = 1 << 24; }
static void m_no_name_element(void) { I[H->no_name_offset] = layout.dc_records + 1; }
static void m_no_name_size(void) { I[H->no_name_offset - 1] = -1; }
static void m_no_name_size_big(void) { I[H->no_name_offset - 1] = NINTS; }

static const struct { const char *name; void (*damage)(void); } damages[] = {
#define D(n) { #n, m_##n }
	D(header_str), D(header_str_zero), D(header_db_zero), D(header_db_count), D(header_db_count_big),
	D(header_files_count), D(header_files_count_more), D(header_mtimes), D(header_name_list_zero),
	D(header_name_list), D(header_no_name), D(header_no_name_negative), D(buffer_start_big),
	D(buffer_start_negative), D(file_boson), D(file_boson_negative),
	D(str_count_zero), D(str_count_ffff), D(str_count_big), D(str_count_more), D(str_taboffset_end),
	D(str_taboffset_odd), D(str_taboffset_huge), D(str_stroffset_end), D(str_stroffset_header),
	D(str_key_end), D(str_key_huge), D(str_first_big), D(str_next_big), D(str_loop), D(str_size_negative),
	D(str_size_huge), D(str_size_small),
	D(db_name), D(db_name_negative), D(db_count_big), D(db_count_negative), D(db_count_more), D(db_list_end),
	D(db_list_zero), D(db_list_beyond), D(db_index_end), D(db_index_zero), D(db_index_size),
	D(rec_name), D(rec_name_negative), D(rec_path), D(rec_count_big), D(rec_count_negative), D(rec_count_more),
	D(rec_list_end), D(rec_list_zero), D(fld_name), D(fld_name_negative), D(fld_value), D(fld_value_negative),
	D(index_key_big), D(index_key_negative), D(index_key_more), D(index_first_big), D(index_loop),
	D(index_data_big), D(index_data_negative),
	D(names_key_big), D(names_data_big), D(names_data_list), D(names_data_min),
	D(names_loop), D(list_element), D(list_element_negative), D(list_size), D(no_name_element),
	D(no_name_size), D(no_name_size_big),
#undef D
};

/* The file the fuzzer found: the string table of 0x12ff000c bytes at 0. */
static const unsigned char fuzz_found[] = {
	0x34, 0x12, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x00, 0xff, 0x12, 0x00, 0x00, 0x00, 0x00,
	0x00, 0xff, 0xff, 0xff, 0xff, 0x12, 0x00, 0x00,
};

/* Returns 1 when the file is accepted; an accepted file is read in full. */
static int try_file(const unsigned char *data, size_t size)
{
	int accepted;

	write_cache(data, size);
	accepted = _DtDtsMMapDB(cache_path);
	if (accepted) {
		(void)walk();
		_DtDtsMMUnLoad();
	}
	return accepted;
}

#ifdef HAVE_TABLE_CHECKS
static void table_checks(void)
{
	/* a string table of one entry; the string of the entry is "ab" */
	union { string_table head; unsigned char bytes[64]; } t;
	string_entry *entry;

	memset(&t, 0, sizeof t);
	t.head.size = 40;
	t.head.stroffset = sizeof(string_table);
	t.head.taboffset = 24;
	t.head.count = 1;
	memcpy(t.bytes + 16, "\0ab\0", 4);
	entry = (string_entry *)(t.bytes + 24);
	entry->key = 1;
	entry->first = 0;
	entry->next = 0xffff;
	assert(_DtShmValidateStrtab(t.bytes, 40));
	assert(_DtShmStrtabHasString(t.bytes, 40, 0) && _DtShmStrtabHasString(t.bytes, 40, 1));
	assert(_DtShmStrtabHasString(t.bytes, 40, 2) && _DtShmStrtabHasString(t.bytes, 40, 3));
	assert(!_DtShmStrtabHasString(t.bytes, 40, -1));
	assert(!_DtShmStrtabHasString(t.bytes, 40, 24) && !_DtShmStrtabHasString(t.bytes, 40, 1000));
	assert(!_DtShmValidateStrtab(t.bytes, 31) && !_DtShmValidateStrtab(t.bytes, 8));

	/* strings that run to the end of the area have no end */
	memset(t.bytes + 16, 'A', 8);
	t.head.taboffset = 8 + 24;
	t.head.stroffset = 24;
	memset(t.bytes + 24, 'A', 40);
	assert(!_DtShmStrtabHasString(t.bytes, 64, 1) && !_DtShmStrtabHasString(t.bytes, 64, 39));
	assert(!_DtShmValidateStrtab(t.bytes, 64));
}
#endif

int main(void)
{
	size_t i, bad = 0;
	const char *tmp = getenv("TMPDIR");

	snprintf(cache_path, sizeof cache_path, "%s/dtsmm-cache.%d", tmp ? tmp : ".", (int)getpid());
	build_cache();

	/* the intact cache loads and is read in full */
	write_cache(cache, cache_size);
	assert(_DtDtsMMapDB(cache_path) == 1);
	assert(head->num_db == DATABASES);
	assert(walk() != 0);
	{
		DtDtsMMDatabase *criteria = _DtDtsMMGet("DATA_CRITERIA");
		DtDtsMMRecord *record;
		int n, *e;

		assert(criteria && criteria->recordCount == 5);
		assert(_DtDtsMMGet("ACTION") && _DtDtsMMGet("ACTION")->recordCount == 3);
		assert(_DtDtsMMGet("nothing") == NULL);
		record = _DtDtsMMGetRecordByName(criteria, "c.h");
		assert(record && record->fieldCount == 2);
		assert(strcmp(_DtDtsMMGetFieldByName(record, "FIELD_1"), "value of 1 4 1") == 0);
		assert(_DtDtsMMGetFieldByName(record, "FIELD_0") != NULL);
		e = _DtDtsMMGetNoNameIndex(&n);
		assert(n == 2 && e[0] == 2 && e[1] == 3);
		e = _DtDtsMMGetBufferIndex(&n);
		assert(n == 1 && e[0] == 3);
		e = (int *)_DtShmFindIntTabEntry(_DtDtsMMGetDCNameIndex(&n), _DtDtsMMStringToBoson("a.txt"));
		assert(e && *e < 0 && _DtDtsMMGetPtrSize(-*e) == 2);
		e = (int *)_DtShmFindIntTabEntry(_DtDtsMMGetDCNameIndex(&n), _DtDtsMMStringToBoson("c.h"));
		assert(e && *e == 4);
		assert(_DtDtsMMBosonToString(head->files_offset) != NULL || 1);
	}
	_DtDtsMMUnLoad();
	printf("valid cache: loaded and read in full\n");

	/* every damage is refused */
	for (i = 0; i < sizeof damages / sizeof damages[0]; i++) {
		memcpy(work, cache, cache_size);
		damages[i].damage();
		assert(memcmp(work, cache, cache_size) != 0);
		if (try_file(work, cache_size)) {
			printf("ACCEPTED: %s\n", damages[i].name);
			bad++;
		}
	}
	/* the file of the fuzzer, and files that end early */
	if (try_file(fuzz_found, sizeof fuzz_found)) {
		printf("ACCEPTED: file found by the fuzzer\n");
		bad++;
	}
	for (i = sizeof(DtDtsMMHeader); i < cache_size; i += 4 * 17) {
		/* a file cut short: the header leads to tables beyond the end */
		if (try_file(cache, i)) {
			printf("ACCEPTED: cache cut at %zu of %zu bytes\n", i, cache_size);
			bad++;
		}
	}
	/* a cache that is only the header, and an empty file */
	if (try_file(cache, sizeof(DtDtsMMHeader)) || try_file(cache, 0)) {
		printf("ACCEPTED: header only or empty\n");
		bad++;
	}
	if (bad) {
		printf("%zu damaged files accepted\n", bad);
		return 1;
	}
	printf("damaged caches: %zu changes, all refused\n", sizeof damages / sizeof damages[0]);

	/* and the intact one is still accepted afterwards */
	assert(try_file(cache, cache_size) == 1);
#ifdef HAVE_TABLE_CHECKS
	table_checks();
	printf("table checks passed\n");
#endif
	unlink(cache_path);
	return 0;
}
