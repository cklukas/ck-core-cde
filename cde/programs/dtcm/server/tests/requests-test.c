/*
 * Regression tests for the rpc.cmsd request path: the XDR decoding of
 * requests, the validation of the names and attributes they carry, the
 * writers of the calendar log, and the request handlers that store or remove
 * calendars and entries.  The requests are built by hand, with strings of
 * lengths the real clients never send.
 *
 * The program includes the production log.c and cmsfunc.c of rpc.cmsd and
 * is linked with the rest of the built rpc.cmsd (check-requests.py compiles
 * the other sources it exercises next to it), so it exercises the shipped
 * code.  The system calls that change privileges, owners and modes, and the
 * ones that open and remove calendar files, go through the test_*() functions
 * below so that tests can make them fail or watch them.  Each case runs in
 * its own process (requests-test CASE) so that a crash in one case is
 * reported for that case alone; check-requests.py runs them all under the
 * sanitizers.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <rpc/rpc.h>

static int test_open(const char *path, int flags, ...);
static int test_seteuid(uid_t uid);
static int test_chmod(const char *path, mode_t mode);
static int test_chown(const char *path, uid_t uid, gid_t gid);
static int test_unlink(const char *path);
static void test_exit(int status);

#define open		test_open
#define seteuid		test_seteuid
#define chmod		test_chmod
#define chown		test_chown
#define unlink		test_unlink
#define exit		test_exit

#include "../log.c"
#include "../cmsfunc.c"

#undef open
#undef seteuid
#undef chmod
#undef chown
#undef unlink
#undef exit

#include "cmxdr.h"
#include "attr.h"
#include "match.h"
#include "iso8601.h"
#include "lutil.h"
#include "reparser.h"
#include "garbage.h"
#include "access.h"

extern boolean_t RulesMatch(char *rule1, char *rule2);

/* what the production code expects the program to define (svcmain.c) */
int debug;
char *pgname = "requests-test";
uid_t daemon_uid;
gid_t daemon_gid;

extern char *_DtCm_rule_buf;
extern int _DtCm_yylex(void);

static int failures;

#define CHECK(cond) \
  do { \
    if(!(cond)) { \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++; \
    } \
  } while(0)

/* a result code that is not the expected one is shown */
#define CHECK_CODE(actual, expected) \
  do { \
    unsigned long a_ = (unsigned long)(actual); \
    unsigned long e_ = (unsigned long)(expected); \
    if(a_ != e_) { \
      fprintf(stderr, "FAIL %s:%d: %s is %lu, expected %s (%lu)\n", __FILE__, \
	      __LINE__, #actual, a_, #expected, e_); \
      failures++; \
    } \
  } while(0)

/* ---- hooks for the system calls of the log writers ----------------------- */

#define MAX_EUID_CALLS 8

static int fail_append_open;		/* fail opens for appending */
static int fail_chmod, fail_chown, fail_restore;
static uid_t euid_calls[MAX_EUID_CALLS];
static int neuid_calls;
static int exit_called, exit_status;
static jmp_buf exit_jump;

static int test_open(const char *path, int flags, ...)
{
  mode_t mode = 0;

  if((flags & O_APPEND) && fail_append_open) {
    errno = EACCES;
    return(-1);
  }
  if(flags & O_CREAT) {
    va_list ap;

    va_start(ap, flags);
    mode = va_arg(ap, int);
    va_end(ap);
  }
  return(open(path, flags, mode));
}

static int test_seteuid(uid_t uid)
{
  if(neuid_calls < MAX_EUID_CALLS)
    euid_calls[neuid_calls++] = uid;
  if(fail_restore && uid != 0) {
    errno = EPERM;
    return(-1);
  }
  return(0);
}

/* The daemon reaches its files through the group of the file; the user running
   the test owns them and needs the owner's write permission instead. */
static int test_chmod(const char *path, mode_t mode)
{
  if(fail_chmod) {
    errno = EPERM;
    return(-1);
  }
  return(chmod(path, mode | S_IWUSR));
}

static int test_chown(const char *path, uid_t uid, gid_t gid)
{
  if(fail_chown) {
    errno = EPERM;
    return(-1);
  }
  return(chown(path, uid, gid));
}

/* The path must be a live string: strlen() is checked by AddressSanitizer. */
static int test_unlink(const char *path)
{
  if(strlen(path) == 0)
    return(-1);
  return(unlink(path));
}

static void test_exit(int status)
{
  exit_called = 1;
  exit_status = status;
  longjmp(exit_jump, 1);
}

/* ---- fixtures ------------------------------------------------------------ */

static char spool[PATH_MAX];
static char user[256];			/* login name of the test user */

static char *repeat(char c, size_t n)
{
  char *s = malloc(n + 1);

  if(s == NULL)
    abort();
  memset(s, c, n);
  s[n] = '\0';
  return(s);
}

/* A private spool directory for the calendar files of the case. */
static void setup(void)
{
  struct passwd *pw = getpwuid(getuid());
  const char *base = getenv("CMSD_TEST_DIR");

  if(pw == NULL) {
    fprintf(stderr, "no passwd entry for the test user\n");
    exit(2);
  }
  snprintf(user, sizeof(user), "%s", pw->pw_name);
  snprintf(spool, sizeof(spool), "%s/spool-XXXXXX", base ? base : ".");
  if(mkdtemp(spool) == NULL) {
    perror("mkdtemp");
    exit(2);
  }
  spool_dir = spool;
  daemon_uid = geteuid();
  daemon_gid = getegid();
  _DtCm_init_hash();
  init_time();
}

static struct authunix_parms credentials;

static struct svc_req request(void)
{
  struct svc_req rq;

  memset(&rq, 0, sizeof(rq));
  credentials.aup_uid = getuid();
  credentials.aup_gid = getgid();
  credentials.aup_machname = "testhost";
  rq.rq_cred.oa_flavor = AUTH_UNIX;
  rq.rq_clntcred = (void *)&credentials;
  return(rq);
}

static char *slurp(const char *path)
{
  FILE *f = fopen(path, "r");
  long size;
  char *text;

  if(f == NULL)
    return(NULL);
  fseek(f, 0, SEEK_END);
  size = ftell(f);
  rewind(f);
  text = malloc(size + 1);
  if(text == NULL || fread(text, 1, size, f) != (size_t)size)
    abort();
  text[size] = '\0';
  fclose(f);
  return(text);
}

/* An empty file in the spool directory, open for appending. */
static int open_log(const char *name, char *path, size_t size)
{
  snprintf(path, size, "%s/%s", spool, name);
  return(open(path, O_CREAT | O_WRONLY | O_APPEND, 0600));
}

static cms_attribute_value *value(int type)
{
  cms_attribute_value *v = calloc(1, sizeof(*v));

  v->type = type;
  return(v);
}

static cms_attribute_value *string_value(int type, const char *text)
{
  cms_attribute_value *v = value(type);

  v->item.string_value = strdup(text);
  return(v);
}

static void set_attribute(cms_attribute *attr, const char *name, int num,
			  cms_attribute_value *v)
{
  attr->name.name = strdup(name);
  attr->name.num = num;
  attr->value = v;
}

/* ---- XDR: the operator array of a request -------------------------------- */

enum { LOOKUP_ENTRIES, ARCHIVE, RESTORE };

/* A request with NUM_ATTRS attributes that carries NUM_OPS operators. */
static size_t encode_request(char *buf, size_t size, int kind,
			     u_int num_attrs, u_int num_ops)
{
  XDR x;
  char *empty = "";
  char *cal = "cal@testhost";
  char *name = "name";
  bool_t flag = FALSE;
  short num = 0;
  long op = 0;
  u_int i;

  xdrmem_create(&x, buf, size, XDR_ENCODE);
  xdr_string(&x, &cal, ~0);
  if(kind == ARCHIVE)
    xdr_bool(&x, &flag);
  else if(kind == RESTORE)
    xdr_string(&x, &empty, ~0);
  xdr_string(&x, &empty, ~0);
  xdr_u_int(&x, &num_attrs);
  for(i = 0; i < num_attrs; i++) {
    xdr_short(&x, &num);
    xdr_string(&x, &name, ~0);
    xdr_bool(&x, &flag);		/* no value */
  }
  xdr_u_int(&x, &num_ops);
  for(i = 0; i < num_ops; i++)
    xdr_long(&x, &op);
  return(xdr_getpos(&x));
}

/* Decode and free a request; returns whether it was accepted. */
static bool_t decode_request(int kind, char *buf, size_t size)
{
  XDR x;
  bool_t ok;
  union {
    cms_lookup_entries_args lookup;
    cms_archive_args archive;
    cms_restore_args restore;
  } args;

  memset(&args, 0, sizeof(args));
  xdrmem_create(&x, buf, size, XDR_DECODE);
  switch(kind) {
  case LOOKUP_ENTRIES:
    ok = xdr_cms_lookup_entries_args(&x, &args.lookup);
    xdr_free(DTCM_XDR_PROC(xdr_cms_lookup_entries_args), (char *)&args.lookup);
    break;
  case ARCHIVE:
    ok = xdr_cms_archive_args(&x, &args.archive);
    xdr_free(DTCM_XDR_PROC(xdr_cms_archive_args), (char *)&args.archive);
    break;
  default:
    ok = xdr_cms_restore_args(&x, &args.restore);
    xdr_free(DTCM_XDR_PROC(xdr_cms_restore_args), (char *)&args.restore);
    break;
  }
  return(ok);
}

static void check_operator_count(int kind)
{
  char buf[1024];
  size_t size;

  /* operators and attributes agree: accepted */
  size = encode_request(buf, sizeof(buf), kind, 2, 2);
  CHECK(decode_request(kind, buf, size));

  /* more operators than attributes must not become the attribute count */
  size = encode_request(buf, sizeof(buf), kind, 1, 12);
  CHECK(!decode_request(kind, buf, size));

  /* neither may fewer */
  size = encode_request(buf, sizeof(buf), kind, 3, 1);
  CHECK(!decode_request(kind, buf, size));
}

static void case_xdr_lookup_operators(void)
{
  check_operator_count(LOOKUP_ENTRIES);
}

static void case_xdr_archive_operators(void)
{
  check_operator_count(ARCHIVE);
}

static void case_xdr_restore_operators(void)
{
  check_operator_count(RESTORE);
}

/* ---- names and attributes from the network -------------------------------- */

static void case_prefix_length(void)
{
  char *huge = repeat('A', 20000);
  char *prefix = _DtCmGetPrefix(huge, '@');

  CHECK(prefix != NULL && strlen(prefix) == 20000);
  free(prefix);
  free(huge);

  prefix = _DtCmGetPrefix("name@host", '@');
  CHECK(prefix != NULL && strcmp(prefix, "name") == 0);
  free(prefix);
  prefix = _DtCmGetPrefix("name", '@');
  CHECK(prefix != NULL && strcmp(prefix, "name") == 0);
  free(prefix);
  CHECK(_DtCmGetPrefix("@host", '@') == NULL);
  CHECK(_DtCmGetPrefix("", '@') == NULL);
  CHECK(_DtCmGetPrefix(NULL, '@') == NULL);
}

/* A calendar name that is a user name, with a host part of any length. */
static void case_calendar_name_attribute(void)
{
  char owner[300], *host, *name;
  cms_attribute attr;
  cms_attribute_value val;
  CSA_return_code stat;

  setup();
  host = repeat('h', 6000);
  name = malloc(strlen(user) + strlen(host) + 2);
  sprintf(name, "%s@%s", user, host);
  snprintf(owner, sizeof(owner), "%s@testhost", user);

  attr.name.name = CSA_CAL_ATTR_CALENDAR_NAME;
  attr.name.num = 0;
  attr.value = &val;
  val.type = CSA_VALUE_STRING;
  val.item.string_value = name;

  stat = _DtCm_check_cal_cms_attributes(_DtCMS_VERSION4, 1, &attr, owner,
					name, B_TRUE, B_TRUE, B_FALSE);
  CHECK(stat == CSA_E_INVALID_ATTRIBUTE_VALUE);
  free(name);
  free(host);
}

static void case_empty_classification(void)
{
  cms_attribute attrs[3];
  CSA_return_code stat;

  memset(attrs, 0, sizeof(attrs));
  set_attribute(&attrs[0], CSA_ENTRY_ATTR_START_DATE, 0,
		string_value(CSA_VALUE_DATE_TIME, "20300101T120000Z"));
  set_attribute(&attrs[1], CSA_ENTRY_ATTR_TYPE, 0, value(CSA_VALUE_UINT32));
  set_attribute(&attrs[2], CSA_ENTRY_ATTR_CLASSIFICATION, 0, NULL);

  setup();
  stat = _DtCm_check_entry_cms_attributes(_DtCMS_VERSION4, 3, attrs,
					  CSA_CB_ENTRY_ADDED, B_TRUE);
  CHECK_CODE(stat, CSA_SUCCESS);
}

static void case_criteria_types(void)
{
  cms_attribute attr;
  cms_attribute_value val;
  CSA_return_code stat;
  boolean_t no_match, no_start, no_end;
  time_t s1, s2, e1, e2;
  long id;
  CSA_uint32 hnum;
  cms_attribute *hattrs;
  CSA_enum *hops;
  CSA_opaque_data opaque;
  char *digits = malloc(3);

  setup();

  /* a start date that is not a date time must not be read as a string */
  attr.name.name = CSA_ENTRY_ATTR_START_DATE;
  attr.name.num = 0;
  attr.value = &val;
  memset(&val, 0, sizeof(val));
  val.type = CSA_VALUE_SINT32;
  val.item.sint32_value = 5;
  stat = _DtCmHashCriteria(_DtCm_entry_name_tbl, 1, NULL, &attr, NULL,
			   &no_match, &no_start, &no_end, &s1, &s2, &e1, &e2,
			   &id, &hnum, &hattrs, &hops);
  CHECK_CODE(stat, CSA_E_INVALID_ATTRIBUTE_VALUE);

  attr.name.name = CSA_ENTRY_ATTR_END_DATE;
  stat = _DtCmHashCriteria(_DtCm_entry_name_tbl, 1, NULL, &attr, NULL,
			   &no_match, &no_start, &no_end, &s1, &s2, &e1, &e2,
			   &id, &hnum, &hattrs, &hops);
  CHECK_CODE(stat, CSA_E_INVALID_ATTRIBUTE_VALUE);

  /* the identifier is opaque data, not a terminated string */
  memcpy(digits, "123", 3);
  opaque.size = 3;
  opaque.data = (unsigned char *)digits;
  attr.name.name = CSA_ENTRY_ATTR_REFERENCE_IDENTIFIER;
  val.type = CSA_VALUE_OPAQUE_DATA;
  val.item.opaque_data_value = &opaque;
  stat = _DtCmHashCriteria(_DtCm_entry_name_tbl, 1, NULL, &attr, NULL,
			   &no_match, &no_start, &no_end, &s1, &s2, &e1, &e2,
			   &id, &hnum, &hattrs, &hops);
  CHECK_CODE(stat, CSA_SUCCESS);
  CHECK(id == 123);
  free(digits);
}

static void case_tick_out_of_range(void)
{
  char buf[32];

  if(sizeof(time_t) >= 8)
    CHECK(_csa_tick_to_iso8601((time_t)1 << 60, buf) != 0);
  CHECK(_csa_tick_to_iso8601(1893456000, buf) == 0);
  CHECK(strcmp(buf, "20300101T000000Z") == 0);
}

static void case_rule_date_token(void)
{
  char *rule = malloc(200);
  int token;

  memset(rule, 'a', 199);
  rule[0] = '1';
  rule[199] = '\0';
  _DtCm_rule_buf = rule;
  token = _DtCm_yylex();
  CHECK(token == ERROR);

  _DtCm_rule_buf = "20300101T000000Z";
  token = _DtCm_yylex();
  CHECK(token == DATE);
  CHECK(strcmp(_DtCm_yylval.date, "20300101T000000Z") == 0);
  free(rule);
}

/* ---- log writers ---------------------------------------------------------- */

static Tag_4 appointment_tag = { appointment_4, 1, NULL };

/* Strings with the characters that need escaping in a log. */
static char *awkward(size_t n)
{
  char *s = repeat('x', n);

  memcpy(s + 10, "\"q\\b\nl", 6);
  return(s);
}

static void case_log_appointment_fields(void)
{
  char path[PATH_MAX + 64], *text;
  int fd;
  Appt_4 appt;
  Attribute_4 attr;
  Except_4 exception;
  char *what = awkward(20000), *details = awkward(20000);
  char *author = awkward(20000), *aname = awkward(9000);
  char *avalue = awkward(9000), *adata = awkward(9000);
  size_t size;

  setup();
  fd = open_log("appt", path, sizeof(path));
  memset(&appt, 0, sizeof(appt));
  memset(&attr, 0, sizeof(attr));
  memset(&exception, 0, sizeof(exception));
  appt.appt_id.tick = 1893456000;
  appt.appt_id.key = 7;
  appt.tag = &appointment_tag;
  appt.what = what;
  appt.client_data = details;
  appt.author = author;
  appt.duration = 3600;
  appt.ntimes = 4;
  appt.period.period = daily_4;
  appt.period.enddate = 1893459600;
  exception.ordinal = 2;
  appt.exception = &exception;
  attr.attr = aname;
  attr.value = avalue;
  attr.clientdata = adata;
  appt.attr = &attr;

  CHECK(_DtCmsAppendAppt4ByFD(fd, &appt, _DtCmsLogAdd) == CSA_SUCCESS);
  CHECK(_DtCmsAppendAppt4ByFD(fd, &appt, _DtCmsLogRemove) == CSA_SUCCESS);
  close(fd);

  text = slurp(path);
  CHECK(text != NULL);
  if(text) {
    char *end = strchr(text, '\n');

    /* the add record is one line that carries all the strings in full, and
       the remove record that follows is the second line */
    CHECK(strncmp(text, "(add \"", 6) == 0);
    CHECK(end != NULL && strncmp(end + 1, "(remove \"", 9) == 0);
    size = end ? (size_t)(end - text) : 0;
    CHECK(size > 20000 * 3 + 9000 * 3);
    CHECK(strstr(text, "xxxxxxxxxx\\\"q\\\\b\\nlxxxx") != NULL);
    CHECK(strstr(text, "details: \"xxxxxxxxxx\\\"q") != NULL);
    CHECK(strstr(text, "author: \"xxxxxxxxxx\\\"q") != NULL);
    CHECK(strstr(text, "exceptions: (2 ) ") != NULL);
    CHECK(strstr(text, "apptstat: active privacy: public )") != NULL);
    free(text);
  }
  free(what);
  free(details);
  free(author);
  free(aname);
  free(avalue);
  free(adata);
}

static void case_log_access_list(void)
{
  char path[PATH_MAX + 64], *text, *who = repeat('w', 6000), *expected;
  int fd;
  Access_Entry_4 second = { "second", access_read_4, NULL };
  Access_Entry_4 first;

  setup();
  fd = open_log("access", path, sizeof(path));
  first.who = who;
  first.access_type = access_read_4;
  first.next = &second;
  CHECK(_DtCmsAppendAccessByFD(fd, access_read_4, &first) == CSA_SUCCESS);
  close(fd);

  text = slurp(path);
  expected = malloc(6100);
  sprintf(expected, "(access read \"%s\" \"second\" )\n", who);
  CHECK(text != NULL && strcmp(text, expected) == 0);
  free(text);
  free(expected);
  free(who);
}

static void case_log_entry_table(void)
{
  char path[PATH_MAX + 64], *text, *name = repeat('n', 6000), *expected;
  char *names[3];
  int types[3], fd;

  setup();
  fd = open_log("table", path, sizeof(path));
  names[0] = NULL;
  names[1] = name;
  names[2] = "short";
  types[0] = 0;
  types[1] = CSA_VALUE_STRING;
  types[2] = CSA_VALUE_UINT32;
  CHECK(_DtCmsAppendHTableByFD(fd, 2, names, types) == CSA_SUCCESS);
  close(fd);

  text = slurp(path);
  expected = malloc(6200);
  sprintf(expected,
	  "(entrytable (1 \"%s\" \"5:string\")\n(2 \"short\" \"4:uint32\")\n)\n",
	  name);
  CHECK(text != NULL && strcmp(text, expected) == 0);
  free(text);
  free(expected);
  free(name);
}

static cms_attribute_value *access_list_value(const char *user1, const char *user2)
{
  cms_attribute_value *v = value(CSA_VALUE_ACCESS_LIST);
  cms_access_entry *first = calloc(1, sizeof(*first));
  cms_access_entry *second = calloc(1, sizeof(*second));

  first->user = strdup(user1);
  first->rights = 3;
  first->next = second;
  second->user = strdup(user2);
  second->rights = 5;
  v->item.access_list_value = first;
  return(v);
}

static cms_attribute_value *date_time_list_value(const char *long_date)
{
  cms_attribute_value *v = value(CSA_VALUE_DATE_TIME_LIST);
  CSA_date_time_entry *first = calloc(1, sizeof(*first));
  CSA_date_time_entry *second = calloc(1, sizeof(*second));

  first->date_time = strdup("20300101T000000Z");
  first->next = second;
  second->date_time = strdup(long_date);
  v->item.date_time_list_value = first;
  return(v);
}

static cms_attribute_value *reminder_value(const char *snooze, size_t size)
{
  cms_attribute_value *v = value(CSA_VALUE_REMINDER);
  CSA_reminder *r = calloc(1, sizeof(*r));

  r->lead_time = strdup("+PT10S");
  r->snooze_time = strdup(snooze);
  r->repeat_count = 3;
  r->reminder_data.size = size;
  r->reminder_data.data = (unsigned char *)repeat('r', size);
  v->item.reminder_value = r;
  return(v);
}

static cms_attribute_value *opaque_value(size_t size)
{
  cms_attribute_value *v = value(CSA_VALUE_OPAQUE_DATA);
  CSA_opaque_data *d = calloc(1, sizeof(*d));

  d->size = size;
  d->data = (unsigned char *)awkward(size);
  v->item.opaque_data_value = d;
  return(v);
}

/* Calendar attributes: custom ones with long names and long values of every
   type that has a variable length. */
static void case_log_calendar_attributes(void)
{
  char path[PATH_MAX + 64], *text, *name = repeat('c', 6000);
  char *big = awkward(30000), *user1 = repeat('u', 5000);
  char *date = repeat('d', 200), *snooze = repeat('z', 6000);
  cms_attribute attrs[6];
  int fd;

  setup();
  fd = open_log("calattrs", path, sizeof(path));
  memset(attrs, 0, sizeof(attrs));
  set_attribute(&attrs[1], name, 20, string_value(CSA_VALUE_STRING, big));
  set_attribute(&attrs[2], "access", 21, access_list_value(user1, "other"));
  set_attribute(&attrs[3], "dates", 22, date_time_list_value(date));
  set_attribute(&attrs[4], "reminder", 23, reminder_value(snooze, 5000));
  set_attribute(&attrs[5], "opaque", 24, opaque_value(7000));

  CHECK(_DtCmsAppendCalAttrsByFD(fd, 5, attrs) == CSA_SUCCESS);
  close(fd);

  text = slurp(path);
  CHECK(text != NULL);
  if(text) {
    size_t len = strlen(text);

    CHECK(strncmp(text, "(calendarattributes (\"cccc", 26) == 0);
    CHECK(len > 6000 + 30000 + 5000 + 6000 + 5000 + 7000);
    CHECK(strcmp(text + len - 5, "\")\n)\n") == 0);
    CHECK(strstr(text, "\",\"5:string\",\"xxxxxxxxxx\\\"q\\\\b\\nl") != NULL);
    CHECK(strstr(text, ":3 other:5\")") != NULL);
    CHECK(strstr(text, "20300101T000000Z ddd") != NULL);
    CHECK(strstr(text, "\",\"14:opaque_data\",\"7000:xxxxxxxxxx") != NULL);
    free(text);
  }
}

/* The hashed form of an entry, as it is logged: numbers instead of names. */
static void case_log_entry(void)
{
  char path[PATH_MAX + 64], *text, *big = awkward(40000);
  char *user1 = repeat('u', 5000), *date = repeat('d', 200);
  cms_entry entry;
  cms_attribute attrs[4];
  int fd;

  setup();
  fd = open_log("entry", path, sizeof(path));
  memset(&entry, 0, sizeof(entry));
  memset(attrs, 0, sizeof(attrs));
  set_attribute(&attrs[1], "a", 1, string_value(CSA_VALUE_STRING, big));
  set_attribute(&attrs[2], "b", 2, access_list_value(user1, "other"));
  set_attribute(&attrs[3], "c", 3, date_time_list_value(date));
  entry.key.time = 1893456000;
  entry.key.id = 5;
  entry.num_attrs = 3;
  entry.attrs = attrs;

  CHECK(_DtCmsAppendEntryByFD(fd, &entry, _DtCmsLogAdd) == CSA_SUCCESS);
  entry.key.time = -1;
  CHECK(_DtCmsAppendEntryByFD(fd, &entry, _DtCmsLogAdd) != CSA_SUCCESS);
  close(fd);

  text = slurp(path);
  CHECK(text != NULL);
  if(text) {
    const char *start = "(add \"20300101T000000Z\" key: 5\nhashedattributes: "
			"((1 \"xxxxxxxxxx\\\"q";
    size_t len = strlen(text);

    CHECK(strncmp(text, start, strlen(start)) == 0);
    CHECK(len > 40000 + 5000 + 200);
    CHECK(len > 6 && strcmp(text + len - 6, "\")\n))\n") == 0);
    free(text);
  }
}

/* ---- file names, privileges ------------------------------------------------ */

static void case_calendar_file_names(void)
{
  char *log, expected[PATH_MAX + 64];

  setup();
  CHECK(_DtCmsGetLogFN("../escape") == NULL);
  CHECK(_DtCmsGetLogFN("a/b") == NULL);
  CHECK(_DtCmsGetLogFN("/etc/passwd") == NULL);
  CHECK(_DtCmsGetBakFN("x/y") == NULL);
  CHECK(_DtCmsGetTmpFN("x/y") == NULL);
  CHECK(_DtCmsGetDelFN("x/y") == NULL);
  CHECK(_DtCmsGetLogFN(NULL) == NULL);

  log = _DtCmsGetLogFN("plain.name");
  snprintf(expected, sizeof(expected), "%s/callog.plain.name", spool);
  CHECK(log != NULL && strcmp(log, expected) == 0);
  free(log);
}

static void case_file_mode_privileges(void)
{
  int r;

  setup();

  /* a change that fails on a missing file goes to root and back */
  neuid_calls = 0;
  r = _DtCmsSetFileMode("/nonexistent", 100, 100, 0640, B_TRUE, B_FALSE);
  CHECK(r == -1);
  CHECK(neuid_calls == 2 && euid_calls[0] == 0 &&
	euid_calls[1] == daemon_uid);

  /* so does a failing chmod: it must not leave the process root */
  fail_chmod = 1;
  neuid_calls = 0;
  r = _DtCmsSetFileMode("any", 100, 100, 0640, B_TRUE, B_FALSE);
  CHECK(r == -1);
  CHECK(neuid_calls == 2 && euid_calls[0] == 0 &&
	euid_calls[1] == daemon_uid);
  fail_chmod = 0;

  /* nor a failing chown */
  fail_chown = 1;
  neuid_calls = 0;
  r = _DtCmsSetFileMode("any", 100, 100, 0640, B_TRUE, B_FALSE);
  CHECK(r == -1);
  CHECK(neuid_calls == 2 && euid_calls[0] == 0 &&
	euid_calls[1] == daemon_uid);
  fail_chown = 0;

  /* a process that cannot give up root stops serving */
  fail_restore = 1;
  neuid_calls = 0;
  exit_called = 0;
  if(setjmp(exit_jump) == 0)
    _DtCmsSetFileMode("any", 100, 100, 0640, B_TRUE, B_FALSE);
  CHECK(exit_called && exit_status != 0);
  fail_restore = 0;
}

/* ---- request handlers: calendars --------------------------------------------- */

static cms_create_args create_args(char *cal, unsigned num_attrs,
				   cms_attribute *attrs)
{
  cms_create_args args;

  memset(&args, 0, sizeof(args));
  args.cal = cal;
  args.char_set = "";
  args.num_attrs = num_attrs;
  args.attrs = attrs;
  return(args);
}

/* A calendar file that cannot be completed is removed, by its name. */
static void case_create_removes_failed_file(void)
{
  struct svc_req rq;
  cms_create_args args;
  CSA_return_code *res;
  char path[PATH_MAX + 64], *cal = strdup("failing@testhost");

  setup();
  rq = request();
  args = create_args(cal, 0, NULL);
  snprintf(path, sizeof(path), "%s/callog.failing", spool);

  fail_append_open = 1;
  res = cms_create_calendar_5_svc(&args, &rq);
  fail_append_open = 0;
  CHECK(*res == CSA_X_DT_E_BACKING_STORE_PROBLEM);
  CHECK(access(path, F_OK) != 0);
  free(cal);
}

static void case_create_names(void)
{
  struct svc_req rq;
  cms_create_args args;
  CSA_return_code *res;
  char *cal, *huge, path[PATH_MAX + 64];

  setup();
  rq = request();

  /* no calendar name */
  cal = strdup("");
  args = create_args(cal, 0, NULL);
  res = cms_create_calendar_5_svc(&args, &rq);
  CHECK(*res == CSA_E_INVALID_PARAMETER);
  free(cal);
  cal = strdup("@testhost");
  args = create_args(cal, 0, NULL);
  res = cms_create_calendar_5_svc(&args, &rq);
  CHECK(*res == CSA_E_INVALID_PARAMETER);
  free(cal);

  /* a name that is a path into a directory of the spool directory */
  snprintf(path, sizeof(path), "%s/callog.some", spool);
  CHECK(mkdir(path, 0700) == 0);
  cal = strdup("some/where@testhost");
  args = create_args(cal, 0, NULL);
  res = cms_create_calendar_5_svc(&args, &rq);
  CHECK(*res != CSA_SUCCESS);
  snprintf(path, sizeof(path), "%s/callog.some/where", spool);
  CHECK(access(path, F_OK) != 0);
  free(cal);

  /* a name without a limit */
  huge = repeat('A', 20000);
  cal = malloc(strlen(huge) + 16);
  sprintf(cal, "%s@testhost", huge);
  args = create_args(cal, 0, NULL);
  res = cms_create_calendar_5_svc(&args, &rq);
  CHECK(*res != CSA_SUCCESS);
  free(cal);
  free(huge);
}

/* The attributes that the server ignores are given back to the arguments. */
static void case_create_keeps_argument_names(void)
{
  struct svc_req rq;
  cms_create_args args;
  cms_attribute attrs[2];
  CSA_return_code *res;
  char *cal = strdup("kept@testhost"), *name_ptr, *owner_ptr;
  char path[PATH_MAX + 64], owner[300];

  setup();
  rq = request();
  snprintf(owner, sizeof(owner), "%s@testhost", user);
  memset(attrs, 0, sizeof(attrs));
  set_attribute(&attrs[0], CSA_CAL_ATTR_CALENDAR_NAME, 0,
		string_value(CSA_VALUE_STRING, "kept@testhost"));
  set_attribute(&attrs[1], CSA_CAL_ATTR_CALENDAR_OWNER, 0,
		string_value(CSA_VALUE_CALENDAR_USER, owner));
  name_ptr = attrs[0].name.name;
  owner_ptr = attrs[1].name.name;
  args = create_args(cal, 2, attrs);

  res = cms_create_calendar_5_svc(&args, &rq);
  CHECK(*res == CSA_SUCCESS);
  CHECK(attrs[0].name.name == name_ptr);
  CHECK(attrs[1].name.name == owner_ptr);
  snprintf(path, sizeof(path), "%s/callog.kept", spool);
  CHECK(access(path, F_OK) == 0);
  free(cal);
}

/* An entry with strings of any length survives being stored and loaded. */
static void case_large_entry_round_trip(void)
{
  struct svc_req rq;
  cms_create_args cargs;
  cms_insert_args iargs;
  cms_lookup_entries_args largs;
  cms_attribute attrs[4];
  cms_entry_res *ires;
  cms_entries_res *lres;
  CSA_return_code *res;
  _DtCmsCalendar *cal;
  char *name = strdup("big@testhost");
  char *summary = awkward(40000), *custom_name = repeat('n', 3000);
  char *custom_value = awkward(9000);
  cms_attribute *found;
  unsigned i, seen = 0;

  setup();
  rq = request();
  cargs = create_args(name, 0, NULL);
  res = cms_create_calendar_5_svc(&cargs, &rq);
  CHECK_CODE(*res, CSA_SUCCESS);

  memset(attrs, 0, sizeof(attrs));
  set_attribute(&attrs[0], CSA_ENTRY_ATTR_START_DATE, 0,
		string_value(CSA_VALUE_DATE_TIME, "20300101T120000Z"));
  set_attribute(&attrs[1], CSA_ENTRY_ATTR_TYPE, 0, value(CSA_VALUE_UINT32));
  set_attribute(&attrs[2], CSA_ENTRY_ATTR_SUMMARY, 0,
		string_value(CSA_VALUE_STRING, summary));
  set_attribute(&attrs[3], custom_name, 0,
		string_value(CSA_VALUE_STRING, custom_value));
  memset(&iargs, 0, sizeof(iargs));
  iargs.cal = name;
  iargs.num_attrs = 4;
  iargs.attrs = attrs;
  ires = cms_insert_entry_5_svc(&iargs, &rq);
  CHECK_CODE(ires->stat, CSA_SUCCESS);

  /* forget the calendar in memory and read it back from its file */
  CHECK(_DtCmsGetCalendarByName(name, B_FALSE, &cal) == CSA_SUCCESS);
  CHECK(cal != NULL);
  _DtCmsFreeCalendar(cal);

  memset(&largs, 0, sizeof(largs));
  largs.cal = name;
  largs.char_set = "";
  lres = cms_lookup_entries_5_svc(&largs, &rq);
  CHECK_CODE(lres->stat, CSA_SUCCESS);
  CHECK(lres->entries != NULL);
  if(lres->entries) {
    found = lres->entries->attrs;
    for(i = 0; i < lres->entries->num_attrs; i++) {
      if(found[i].value && found[i].value->type == CSA_VALUE_STRING &&
	 found[i].value->item.string_value) {
	if(strcmp(found[i].value->item.string_value, summary) == 0)
	  seen |= 1;
	if(strcmp(found[i].value->item.string_value, custom_value) == 0 &&
	   strcmp(found[i].name.name, custom_name) == 0)
	  seen |= 2;
      }
    }
    CHECK(seen == 3);
  }
  free(name);
}

/* ---- request handlers: appointments of the old format ----------------------- */

/* An old format calendar with NUM appointments that the test user owns. */
static void make_v1_calendar(const char *name, int num)
{
  char *path = _DtCmsGetLogFN((char *)name);
  Access_Entry_4 world = { WORLD, access_read_4, NULL };
  Appt_4 appt;
  char author[300];
  int fd, i;

  fd = open(path, O_CREAT | O_WRONLY, 0600);
  close(fd);
  CHECK(_DtCmsWriteVersionString(path, _DtCMS_VERSION1) == CSA_SUCCESS);
  CHECK(_DtCmsAppendAccessByFN(path, access_read_4, &world) == CSA_SUCCESS);
  snprintf(author, sizeof(author), "%s@testhost", user);
  for(i = 1; i <= num; i++) {
    memset(&appt, 0, sizeof(appt));
    appt.appt_id.tick = 1893456000 + 3600 * i;
    appt.appt_id.key = i;
    appt.tag = &appointment_tag;
    appt.what = "appointment";
    appt.author = author;
    appt.duration = 1800;
    appt.period.period = single_4;
    CHECK(_DtCmsAppendAppt4ByFN(path, &appt, _DtCmsLogAdd) == CSA_SUCCESS);
  }
  free(path);
}

static void case_update_old_format_entry(void)
{
  struct svc_req rq;
  cms_update_args args;
  cms_attribute attr;
  cms_entry_res *res;
  char *cal = strdup("old@testhost");

  setup();
  rq = request();
  make_v1_calendar("old", 1);

  memset(&attr, 0, sizeof(attr));
  set_attribute(&attr, CSA_ENTRY_ATTR_SUMMARY, 0,
		string_value(CSA_VALUE_STRING, "changed"));
  memset(&args, 0, sizeof(args));
  args.cal = cal;
  args.entry.time = 1893456000 + 3600;
  args.entry.id = 1;
  args.scope = CSA_SCOPE_ALL;
  args.num_attrs = 1;
  args.attrs = &attr;

  res = cms_update_entry_5_svc(&args, &rq);
  CHECK_CODE(res->stat, CSA_SUCCESS);
  CHECK(res->entry != NULL);
  free(cal);
}

/* ---- request handlers: version 4 of the protocol ---------------------------------- */

static void case_delete_several_appointments(void)
{
  struct svc_req rq;
  Table_Args_4 args;
  Table_Res_4 *res;
  Uidopt_4 first, second, third;

  setup();
  rq = request();
  make_v1_calendar("many", 3);

  memset(&args, 0, sizeof(args));
  args.target = strdup("many@testhost");
  args.args.tag = UIDOPT_4;
  args.args.Args_4_u.uidopt = &first;
  memset(&first, 0, sizeof(first));
  memset(&second, 0, sizeof(second));
  memset(&third, 0, sizeof(third));
  first.appt_id.tick = 1893456000 + 3600;
  first.appt_id.key = 1;
  first.option = do_all_4;
  first.next = &second;
  second.appt_id.tick = 1893456000 + 7200;
  second.appt_id.key = 2;
  second.option = do_all_4;
  second.next = &third;
  third.appt_id.tick = 1893456000 + 10800;
  third.appt_id.key = 3;
  third.option = do_all_4;

  res = _DtCm_rtable_delete_4_svc(&args, &rq);
  CHECK(res->status == access_ok_4);
  CHECK(res->res.Table_Res_List_4_u.a != NULL);
}

static void case_create_existing_calendar(void)
{
  struct svc_req rq;
  Table_Op_Args_4 args;
  Table_Status_4 *res;
  char path[PATH_MAX + 64], *before, *after;
  int fd;

  setup();
  rq = request();
  fd = open_log("callog.victim", path, sizeof(path));
  close(fd);
  CHECK(_DtCmsWriteVersionString(path, _DtCMS_VERSION1) == CSA_SUCCESS);
  before = slurp(path);

  memset(&args, 0, sizeof(args));
  args.target = strdup("victim@testhost");
  res = _DtCm_rtable_create_4_svc(&args, &rq);
  CHECK(*res == tbl_exist_4);

  /* the calendar that was there keeps its content and its file */
  after = slurp(path);
  CHECK(before != NULL && after != NULL && strcmp(before, after) == 0);
  free(before);
  free(after);
}

static void case_create_new_calendar(void)
{
  struct svc_req rq;
  Table_Op_Args_4 args;
  Table_Status_4 *res;
  char path[PATH_MAX + 64], *text;

  setup();
  rq = request();
  memset(&args, 0, sizeof(args));
  args.target = strdup("fresh@testhost");
  res = _DtCm_rtable_create_4_svc(&args, &rq);
  CHECK(*res == ok_4);

  snprintf(path, sizeof(path), "%s/callog.fresh", spool);
  text = slurp(path);
  CHECK(text != NULL && strstr(text, "(access read \"world\" )") != NULL);
  free(text);
}

/* ---- reading calendar files, reminders ----------------------------------------- */

static size_t what_length;

static boolean_t note_what(caddr_t node, caddr_t data)
{
  Appt_4 *appt = (Appt_4 *)data;

  (void)node;
  if(appt->what)
    what_length = strlen(appt->what);
  return(B_FALSE);
}

/* A string of a calendar file is as long as the file is. */
static void case_read_long_string(void)
{
  char *path, *what = repeat('W', 100000), *name = strdup("longstring@testhost");
  _DtCmsCalendar *cal;
  FILE *f;

  setup();
  path = _DtCmsGetLogFN("longstring");
  f = fopen(path, "w");
  fprintf(f, "Version: 1\n(access read \"world\" )\n");
  fprintf(f, "(add \"Tue Jan  1 00:00:00 2030\" key: 1 what: \"%s\" "
	  "duration: 0 period: single ntimes: 0 author: \"%s@testhost\" "
	  "tags: ((appointment , 1)) apptstat: active privacy: public )\n",
	  what, user);
  fclose(f);

  CHECK_CODE(_DtCmsLoadCalendar(name, &cal), CSA_SUCCESS);
  CHECK(cal != NULL);
  if(cal) {
    _DtCmsEnumerateUp(cal, note_what);
    CHECK(what_length == 100000);
  }
  free(path);
  free(what);
  free(name);
}

/* A string that ends with the file is not a string: reading stops there. */
static void case_read_unterminated_string(void)
{
  char *path, *name = strdup("unterminated@testhost");
  _DtCmsCalendar *cal;
  FILE *f;

  setup();
  path = _DtCmsGetLogFN("unterminated");
  f = fopen(path, "w");
  fprintf(f, "Version: 1\n(add \"Tue Jan  1 00:00:00 2030\" key: 1 what: \"abc");
  fclose(f);

  (void)_DtCmsLoadCalendar(name, &cal);
  free(path);
  free(name);
}

/* The identifier of an entry in a reminder reply is a terminated string. */
static void case_reminder_identifier(void)
{
  struct svc_req rq;
  cms_create_args cargs;
  cms_insert_args iargs;
  cms_reminder_args rargs;
  cms_reminder_res *rres;
  cms_attribute attrs[4];
  cms_attribute_value *reminder;
  CSA_reminder *r;
  char *name = strdup("remind@testhost");
  CSA_return_code *res;
  cms_entry_res *ires;

  setup();
  rq = request();
  cargs = create_args(name, 0, NULL);
  res = cms_create_calendar_5_svc(&cargs, &rq);
  CHECK_CODE(*res, CSA_SUCCESS);

  memset(attrs, 0, sizeof(attrs));
  reminder = value(CSA_VALUE_REMINDER);
  r = calloc(1, sizeof(*r));
  r->lead_time = strdup("+PT60S");
  reminder->item.reminder_value = r;
  set_attribute(&attrs[0], CSA_ENTRY_ATTR_START_DATE, 0,
		string_value(CSA_VALUE_DATE_TIME, "20300101T120000Z"));
  set_attribute(&attrs[1], CSA_ENTRY_ATTR_TYPE, 0, value(CSA_VALUE_UINT32));
  set_attribute(&attrs[2], CSA_ENTRY_ATTR_AUDIO_REMINDER, 0, reminder);
  memset(&iargs, 0, sizeof(iargs));
  iargs.cal = name;
  iargs.num_attrs = 3;
  iargs.attrs = attrs;
  ires = cms_insert_entry_5_svc(&iargs, &rq);
  CHECK_CODE(ires->stat, CSA_SUCCESS);

  memset(&rargs, 0, sizeof(rargs));
  rargs.cal = name;
  rargs.tick = 1893456000 - 86400;
  rres = cms_lookup_reminder_5_svc(&rargs, &rq);
  CHECK_CODE(rres->stat, CSA_SUCCESS);
  CHECK(rres->rems != NULL);
  if(rres->rems && rres->rems->entryid) {
    char expected[300];

    snprintf(expected, sizeof(expected), "%ld:remind@", rres->rems->key.id);
    CHECK(strncmp(rres->rems->entryid, expected, strlen(expected)) == 0);
    CHECK(strlen(rres->rems->entryid) > strlen(expected));
  }
  free(name);
}

/* ---- attribute numbers sent by clients ------------------------------------------- */

/* The number a client sends with the name of an attribute is no index of ours. */
static void case_calendar_attribute_numbers(void)
{
  struct svc_req rq;
  cms_create_args cargs;
  cms_set_cal_attr_args sargs;
  cms_attribute attrs[1];
  cms_get_cal_attr_args gargs;
  cms_attr_name names[1];
  cms_get_cal_attr_res *gres;
  CSA_return_code *res;
  _DtCmsCalendar *cal;
  char *name = strdup("numbers@testhost"), owner[300];

  setup();
  rq = request();
  cargs = create_args(name, 0, NULL);
  res = cms_create_calendar_5_svc(&cargs, &rq);
  CHECK_CODE(*res, CSA_SUCCESS);
  CHECK_CODE(_DtCmsGetCalendarByName(name, B_FALSE, &cal), CSA_SUCCESS);
  CHECK(cal != NULL);
  if(cal == NULL)
    return;
  snprintf(owner, sizeof(owner), "%s",
	   cal->attrs[CSA_CAL_ATTR_CALENDAR_OWNER_I].value->\
	   item.calendar_user_value);

  /* a number beyond the attributes of the calendar */
  memset(attrs, 0, sizeof(attrs));
  set_attribute(&attrs[0], "far-away", 30000,
		string_value(CSA_VALUE_STRING, "x"));
  memset(&sargs, 0, sizeof(sargs));
  sargs.cal = name;
  sargs.num_attrs = 1;
  sargs.attrs = attrs;
  res = cms_set_calendar_attr_5_svc(&sargs, &rq);
  CHECK_CODE(*res, CSA_SUCCESS);
  CHECK(_DtCm_get_index_from_table(cal->cal_tbl, "far-away") ==
	(int)cal->num_attrs);

  /* the number of the owner attribute, which cannot be set */
  set_attribute(&attrs[0], "not-the-owner", CSA_CAL_ATTR_CALENDAR_OWNER_I,
		string_value(CSA_VALUE_CALENDAR_USER, "someone@elsewhere"));
  res = cms_set_calendar_attr_5_svc(&sargs, &rq);
  CHECK_CODE(*res, CSA_SUCCESS);
  CHECK(strcmp(cal->attrs[CSA_CAL_ATTR_CALENDAR_OWNER_I].value->\
	       item.calendar_user_value, owner) == 0);

  /* and asking for attributes by such numbers */
  memset(names, 0, sizeof(names));
  names[0].name = "far-away";
  names[0].num = 31000;
  memset(&gargs, 0, sizeof(gargs));
  gargs.cal = name;
  gargs.num_names = 1;
  gargs.names = names;
  gres = cms_get_calendar_attr_5_svc(&gargs, &rq);
  CHECK_CODE(gres->stat, CSA_SUCCESS);
  CHECK(gres->num_attrs == 1 && gres->attrs != NULL &&
	strcmp(gres->attrs[0].name.name, "far-away") == 0);
  free(name);
}

static cms_key insert_entry(struct svc_req *rq, char *cal, const char *summary,
			    const char *custom_name)
{
  cms_insert_args iargs;
  cms_attribute attrs[4];
  cms_entry_res *ires;
  cms_key key;
  unsigned n = 0;

  memset(attrs, 0, sizeof(attrs));
  set_attribute(&attrs[n++], CSA_ENTRY_ATTR_START_DATE, 0,
		string_value(CSA_VALUE_DATE_TIME, "20300101T120000Z"));
  set_attribute(&attrs[n++], CSA_ENTRY_ATTR_TYPE, 0, value(CSA_VALUE_UINT32));
  set_attribute(&attrs[n++], CSA_ENTRY_ATTR_SUMMARY, 0,
		string_value(CSA_VALUE_STRING, summary));
  if(custom_name)
    set_attribute(&attrs[n++], custom_name, 0,
		  string_value(CSA_VALUE_STRING, "custom"));
  memset(&iargs, 0, sizeof(iargs));
  iargs.cal = cal;
  iargs.num_attrs = n;
  iargs.attrs = attrs;
  ires = cms_insert_entry_5_svc(&iargs, rq);
  CHECK_CODE(ires->stat, CSA_SUCCESS);
  memset(&key, 0, sizeof(key));
  if(ires->entry)
    key = ires->entry->key;
  return(key);
}

/* An entry has no attribute for a name that was added after it, whatever
   number the client has for the name. */
static void case_entry_attributes_by_name(void)
{
  struct svc_req rq;
  cms_create_args cargs;
  cms_get_entry_attr_args gargs;
  cms_get_entry_attr_res *gres;
  cms_attr_name names[2];
  cms_key key;
  CSA_return_code *res;
  char *name = strdup("late@testhost");

  setup();
  rq = request();
  cargs = create_args(name, 0, NULL);
  res = cms_create_calendar_5_svc(&cargs, &rq);
  CHECK_CODE(*res, CSA_SUCCESS);
  key = insert_entry(&rq, name, "entry a", NULL);
  insert_entry(&rq, name, "entry b", "late-attribute");

  memset(names, 0, sizeof(names));
  names[0].name = "late-attribute";
  names[1].name = CSA_ENTRY_ATTR_SUMMARY;
  names[1].num = 30000;
  memset(&gargs, 0, sizeof(gargs));
  gargs.cal = name;
  gargs.num_keys = 1;
  gargs.keys = &key;
  gargs.num_names = 2;
  gargs.names = names;
  gres = cms_get_entry_attr_5_svc(&gargs, &rq);
  CHECK_CODE(gres->stat, CSA_SUCCESS);
  CHECK(gres->entries != NULL);
  if(gres->entries) {
    cms_get_entry_attr_res_item *item = gres->entries;

    CHECK_CODE(item->stat, CSA_SUCCESS);
    CHECK(item->num_attrs == 2);
    if(item->num_attrs == 2) {
      CHECK(item->attrs[0].value == NULL);
      CHECK(item->attrs[1].value != NULL &&
	    strcmp(item->attrs[1].value->item.string_value, "entry a") == 0);
    }
  }
  free(name);
}


/* ---- recurrence rules ----------------------------------------------------------- */

/* A recurrence rule of days of the year with N entries and the duration. */
static char *long_rule(unsigned n, unsigned duration)
{
  size_t size = (size_t)n * 5 + 64;
  char *rule = malloc(size), *p = rule;
  unsigned i;

  p += sprintf(p, "YD1");
  for(i = 0; i < n; i++)
    p += sprintf(p, " %u", 1 + i % 365);
  sprintf(p, " #%u", duration);
  return(rule);
}

/* The text built for a rule is as long as the rule needs; rules that differ
   in their duration only are the same rule. */
static void case_rule_text_long(void)
{
  char *a = long_rule(2000, 1), *b = long_rule(2000, 2);
  char *c = long_rule(1999, 2);

  CHECK(RulesMatch(a, b));
  CHECK(!RulesMatch(a, c));
  free(a);
  free(b);
  free(c);
}

/* ---- the state of a calendar is for those who may look at it ---------------------- */

/* An old format calendar the test user owns, without rights for others. */
static void make_private_v1_calendar(const char *name, int num)
{
  char *path = _DtCmsGetLogFN((char *)name);
  Appt_4 appt;
  char author[300];
  int fd, i;

  fd = open(path, O_CREAT | O_WRONLY, 0600);
  close(fd);
  CHECK(_DtCmsWriteVersionString(path, _DtCMS_VERSION1) == CSA_SUCCESS);
  snprintf(author, sizeof(author), "%s@testhost", user);
  for(i = 1; i <= num; i++) {
    memset(&appt, 0, sizeof(appt));
    appt.appt_id.tick = 1893456000 + 3600 * i;
    appt.appt_id.key = i;
    appt.tag = &appointment_tag;
    appt.what = "appointment";
    appt.author = author;
    appt.duration = 1800;
    appt.period.period = single_4;
    CHECK(_DtCmsAppendAppt4ByFN(path, &appt, _DtCmsLogAdd) == CSA_SUCCESS);
  }
  free(path);
}

/* A request of another user of the host. */
static struct svc_req stranger_request(void)
{
  struct svc_req rq = request();
  struct passwd *pw;

  setpwent();
  while((pw = getpwent()) != NULL)
    if(pw->pw_uid != getuid() && pw->pw_uid != 0)
      break;
  if(pw == NULL) {
    fprintf(stderr, "no second user in the passwd database\n");
    exit(2);
  }
  credentials.aup_uid = pw->pw_uid;
  endpwent();
  return(rq);
}

static void case_stranger_calendar_state(void)
{
  struct svc_req owner, other;
  Table_Args_4 args;
  Table_Status_4 *status;
  int *size;
  uid_t owner_uid;

  setup();
  owner = request();
  owner_uid = credentials.aup_uid;
  make_private_v1_calendar("secret", 3);
  memset(&args, 0, sizeof(args));
  args.target = strdup("secret@testhost");
  args.args.tag = TICK_4;

  size = _DtCm_rtable_size_4_svc(&args, &owner);
  CHECK(*size == 3);
  status = _DtCm_rtable_check_4_svc(&args, &owner);
  CHECK(*status != denied_4 && *status != notable_4);

  other = stranger_request();
  CHECK(credentials.aup_uid != owner_uid);
  size = _DtCm_rtable_size_4_svc(&args, &other);
  CHECK(*size == 0);
  status = _DtCm_rtable_check_4_svc(&args, &other);
  CHECK(*status == denied_4);

  /* and the debug mode of the server does not let anybody discard the
     appointments of another user */
  debug = 1;
  args.args.Args_4_u.tick = 0;
  status = _DtCm_rtable_flush_table_4_svc(&args, &other);
  CHECK(*status == ok_4);
  status = _DtCm_rtable_flush_table_4_svc(&args, &owner);
  debug = 0;
  credentials.aup_uid = owner_uid;
  size = _DtCm_rtable_size_4_svc(&args, &owner);
  CHECK(*size == 3);
  free(args.target);
}

static void case_stranger_registration(void)
{
  struct svc_req owner, other;
  Registration_4 reg;
  Registration_Status_4 *status;
  cms_register_args rargs;
  CSA_return_code *res;
  uid_t owner_uid;

  setup();
  owner = request();
  owner_uid = credentials.aup_uid;
  make_private_v1_calendar("quiet", 1);

  memset(&reg, 0, sizeof(reg));
  reg.target = strdup("quiet@testhost");
  reg.prognum = 0x40000001;
  reg.versnum = 2;
  reg.procnum = 1;
  reg.pid = 4242;
  memset(&rargs, 0, sizeof(rargs));
  rargs.cal = reg.target;
  rargs.update_type = CSA_CB_ENTRY_ADDED;
  rargs.prognum = reg.prognum;
  rargs.versnum = reg.versnum;
  rargs.procnum = reg.procnum;
  rargs.pid = reg.pid;

  other = stranger_request();
  status = _DtCm_register_callback_4_svc(&reg, &other);
  CHECK(*status == failed_4);
  res = cms_register_5_svc(&rargs, &other);
  CHECK_CODE(*res, CSA_E_NO_AUTHORITY);

  credentials.aup_uid = owner_uid;
  status = _DtCm_register_callback_4_svc(&reg, &owner);
  CHECK(*status == registered_4);
  res = cms_register_5_svc(&rargs, &owner);
  CHECK_CODE(*res, CSA_SUCCESS);
  free(reg.target);
}

/* ---- garbage collection ------------------------------------------------------------ */

static int count_descriptors(void)
{
  int n = 0, fd;

  for(fd = 0; fd < 256; fd++)
    if(fcntl(fd, F_GETFD) != -1)
      n++;
  return(n);
}

/* A dump that fails gives the temporary log back. */
static void case_dump_closes_descriptor(void)
{
  _DtCmsCalendar *cal;
  char *dump_name;
  int before, i;

  setup();
  dump_name = strdup("dump@testhost");
  cal = _DtCmsMakeCalendar(user, dump_name);
  CHECK(cal != NULL);
  if(cal == NULL)
    return;
  before = count_descriptors();
  for(i = 0; i < 4; i++) {
    CHECK(_DtCmsDumpDataV1("/dev/full", cal) != CSA_SUCCESS);
    CHECK(_DtCmsDumpDataV2("/dev/full", cal) != CSA_SUCCESS);
  }
  CHECK(count_descriptors() == before);
  _DtCmsFreeCalendar(cal);
  free(dump_name);
}


/* ---- attributes without a value ---------------------------------------------------- */

/* An attribute of a request may have no value, also when its name is new. */
static void case_null_attribute_values(void)
{
  struct svc_req rq;
  cms_create_args cargs;
  cms_set_cal_attr_args sargs;
  cms_insert_args iargs;
  cms_update_args uargs;
  cms_attribute attrs[3];
  cms_entry_res *ires;
  CSA_return_code *res;
  cms_key key;
  char *name = strdup("empty@testhost");

  setup();
  rq = request();
  cargs = create_args(name, 0, NULL);
  res = cms_create_calendar_5_svc(&cargs, &rq);
  CHECK_CODE(*res, CSA_SUCCESS);
  key = insert_entry(&rq, name, "an entry", NULL);

  memset(attrs, 0, sizeof(attrs));
  set_attribute(&attrs[0], "no-value-one", 0, NULL);
  set_attribute(&attrs[1], "no-value-two", 40000, NULL);
  set_attribute(&attrs[2], CSA_CAL_ATTR_CALENDAR_NAME, 0, NULL);
  memset(&sargs, 0, sizeof(sargs));
  sargs.cal = name;
  sargs.num_attrs = 3;
  sargs.attrs = attrs;
  res = cms_set_calendar_attr_5_svc(&sargs, &rq);
  CHECK(*res != CSA_SUCCESS);

  /* in a new calendar */
  {
    char *other = strdup("empty2@testhost");

    cargs = create_args(other, 3, attrs);
    res = cms_create_calendar_5_svc(&cargs, &rq);
    CHECK(*res != CSA_SUCCESS);
    free(other);
  }

  memset(attrs, 0, sizeof(attrs));
  set_attribute(&attrs[0], "entry-no-value", 0, NULL);
  set_attribute(&attrs[1], CSA_ENTRY_ATTR_SUMMARY, 0, NULL);
  memset(&iargs, 0, sizeof(iargs));
  iargs.cal = name;
  iargs.num_attrs = 2;
  iargs.attrs = attrs;
  ires = cms_insert_entry_5_svc(&iargs, &rq);
  CHECK(ires->stat != CSA_SUCCESS);

  memset(&uargs, 0, sizeof(uargs));
  uargs.cal = name;
  uargs.entry = key;
  uargs.scope = CSA_SCOPE_ALL;
  uargs.num_attrs = 2;
  uargs.attrs = attrs;
  /* a value that is not there removes the attribute; one that does not
     exist has nothing to remove */
  ires = cms_update_entry_5_svc(&uargs, &rq);
  CHECK_CODE(ires->stat, CSA_SUCCESS);
  free(name);
}

/* ---- the cases ------------------------------------------------------------ */

static const struct {
  const char *name;
  void (*run)(void);
} cases[] = {
  { "xdr-lookup-operators", case_xdr_lookup_operators },
  { "xdr-archive-operators", case_xdr_archive_operators },
  { "xdr-restore-operators", case_xdr_restore_operators },
  { "prefix-length", case_prefix_length },
  { "calendar-name-attribute", case_calendar_name_attribute },
  { "empty-classification", case_empty_classification },
  { "criteria-types", case_criteria_types },
  { "tick-out-of-range", case_tick_out_of_range },
  { "rule-date-token", case_rule_date_token },
  { "log-appointment-fields", case_log_appointment_fields },
  { "log-access-list", case_log_access_list },
  { "log-entry-table", case_log_entry_table },
  { "log-calendar-attributes", case_log_calendar_attributes },
  { "log-entry", case_log_entry },
  { "calendar-file-names", case_calendar_file_names },
  { "file-mode-privileges", case_file_mode_privileges },
  { "create-removes-failed-file", case_create_removes_failed_file },
  { "create-names", case_create_names },
  { "create-keeps-argument-names", case_create_keeps_argument_names },
  { "large-entry-round-trip", case_large_entry_round_trip },
  { "update-old-format-entry", case_update_old_format_entry },
  { "delete-several-appointments", case_delete_several_appointments },
  { "create-existing-calendar", case_create_existing_calendar },
  { "create-new-calendar", case_create_new_calendar },
  { "read-long-string", case_read_long_string },
  { "read-unterminated-string", case_read_unterminated_string },
  { "reminder-identifier", case_reminder_identifier },
  { "calendar-attribute-numbers", case_calendar_attribute_numbers },
  { "entry-attributes-by-name", case_entry_attributes_by_name },
  { "rule-text-long", case_rule_text_long },
  { "stranger-calendar-state", case_stranger_calendar_state },
  { "stranger-registration", case_stranger_registration },
  { "dump-closes-descriptor", case_dump_closes_descriptor },
  { "null-attribute-values", case_null_attribute_values },
};

int main(int argc, char **argv)
{
  size_t i;

  if(argc == 2 && strcmp(argv[1], "--list") == 0) {
    for(i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
      puts(cases[i].name);
    return(0);
  }
  if(argc != 2) {
    fprintf(stderr, "usage: %s --list | CASE\n", argv[0]);
    return(2);
  }
  for(i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    if(strcmp(cases[i].name, argv[1]) == 0) {
      cases[i].run();
      return(failures ? 1 : 0);
    }
  }
  fprintf(stderr, "unknown case %s\n", argv[1]);
  return(2);
}
