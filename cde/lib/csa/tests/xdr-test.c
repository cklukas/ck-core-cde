/*
 * Regression tests for the XDR routines of the calendar protocols
 * (cmxdr.c, cmcbxdr.c, rtable2_xdr.c, rtable3_xdr.c, rtable4_xdr.c).
 *
 * The messages are built by hand, with lengths, counts and list lengths the
 * real clients and servers never send.  Each case runs in its own process
 * (xdr-test CASE) so that a crash is reported for that case alone;
 * check-xdr.py runs them under AddressSanitizer and UndefinedBehaviorSanitizer
 * with leak detection on.
 *
 * The program uses only the public headers of the library and the limits of
 * cmlimits.h, so it can be linked with other versions of the XDR sources.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <rpc/rpc.h>

#include "csa.h"
#include "cm.h"
#include "cmcb.h"
#include "cmxdr.h"
#include "rtable2.h"
#include "rtable3.h"
#include "rtable4.h"
#include "cmlimits.h"

/* defined in cmxdr.c; no header declares it */
extern bool_t xdr_cms_attribute_value(XDR *xdrs, cms_attribute_value *objp);

static int failures;

#define CHECK(cond) \
  do { \
    if(!(cond)) { \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++; \
    } \
  } while(0)

/* ---- messages ------------------------------------------------------------ */

typedef struct {
  char *data;
  size_t length, size;
} Message;

static void put_bytes(Message *m, const void *bytes, size_t n)
{
  if(m->length + n > m->size) {
    m->size = (m->length + n) * 2 + 64;
    m->data = realloc(m->data, m->size);
    if(m->data == NULL)
      abort();
  }
  memcpy(m->data + m->length, bytes, n);
  m->length += n;
}

static void put_u32(Message *m, uint32_t v)
{
  uint32_t be = htonl(v);

  put_bytes(m, &be, 4);
}

/* an XDR string of N characters 'x' */
static void put_string(Message *m, size_t n)
{
  static const char zeros[4];
  char *text = malloc(n + 1);

  put_u32(m, (uint32_t)n);
  memset(text, 'x', n);
  put_bytes(m, text, n);
  put_bytes(m, zeros, (4 - n % 4) % 4);
  free(text);
}

/* Decode the message with PROC into OBJ, which is zeroed; returns whether
   it decoded. */
static bool_t decode(Message *m, xdrproc_t proc, void *obj)
{
  XDR xdrs;
  bool_t ok;

  xdrmem_create(&xdrs, m->data, (u_int)m->length, XDR_DECODE);
  ok = proc(&xdrs, obj);
  xdr_destroy(&xdrs);
  return(ok);
}

static void release(xdrproc_t proc, void *obj)
{
  XDR xdrs;

  xdrmem_create(&xdrs, NULL, 0, XDR_FREE);
  proc(&xdrs, obj);
}

/* Encode OBJ into M; returns whether it encoded. */
static bool_t encode(Message *m, xdrproc_t proc, void *obj, size_t room)
{
  XDR xdrs;
  bool_t ok;

  m->data = malloc(room);
  m->size = room;
  xdrmem_create(&xdrs, m->data, (u_int)room, XDR_ENCODE);
  ok = proc(&xdrs, obj);
  m->length = xdr_getpos(&xdrs);
  xdr_destroy(&xdrs);
  return(ok);
}

/* ---- strings ------------------------------------------------------------ */

typedef struct {
  const char *name;
  xdrproc_t proc;
  size_t object_size;
  size_t limit;
  /* the message of the object with a string of N characters */
  void (*build)(Message *m, size_t n);
} StringCase;

static void build_open_args(Message *m, size_t n) { put_string(m, n); put_u32(m, 1); }
static void build_create_charset(Message *m, size_t n)
{
  put_string(m, 1); put_string(m, n); put_u32(m, 1); put_u32(m, 0);
}
static void build_archive_res(Message *m, size_t n) { put_u32(m, 0); put_string(m, n); }
static void build_reminder_ref(Message *m, size_t n)
{
  put_string(m, n); put_string(m, 1); put_u32(m, 1); put_u32(m, 1);
  put_u32(m, 1); put_u32(m, 0);
}
static void build_callback_user(Message *m, size_t n)
{
  put_string(m, 1); put_string(m, n); put_u32(m, 0);
}
static void build_table_args4(Message *m, size_t n)
{
  put_string(m, n); put_u32(m, 0); put_u32(m, 1); put_u32(m, 1);
}
static void build_buffer4(Message *m, size_t n) { put_string(m, n); }
static void build_table_op_args(Message *m, size_t n)
{
  put_string(m, 1); put_string(m, n);
}
static void build_table_args3(Message *m, size_t n)
{
  put_string(m, n); put_u32(m, 0); put_u32(m, 1); put_u32(m, 1);
}
static void build_table_args2(Message *m, size_t n)
{
  put_string(m, n); put_u32(m, 0); put_u32(m, 1);
}

static const StringCase string_cases[] = {
  { "calendar name of a v5 request", (xdrproc_t)xdr_cms_open_args,
    sizeof(cms_open_args), _DtCM_LIMIT_NAME, build_open_args },
  { "character set of a v5 request", (xdrproc_t)xdr_cms_create_args,
    sizeof(cms_create_args), _DtCM_LIMIT_CHARSET, build_create_charset },
  { "archive data of a v5 reply", (xdrproc_t)xdr_cms_archive_res,
    sizeof(cms_archive_res), _DtCM_LIMIT_ARCHIVE, build_archive_res },
  { "attribute name of a reminder reference", (xdrproc_t)xdr_cms_reminder_ref,
    sizeof(cms_reminder_ref), _DtCM_LIMIT_ATTR_NAME, build_reminder_ref },
  { "user of a callback", (xdrproc_t)xdr_cmcb_update_callback_args,
    sizeof(cmcb_update_callback_args), _DtCM_LIMIT_NAME, build_callback_user },
  { "target of a v4 request", (xdrproc_t)_DtCm_xdr_Table_Args_4,
    sizeof(Table_Args_4), _DtCM_LIMIT_NAME, build_table_args4 },
  { "buffer of v4", (xdrproc_t)_DtCm_xdr_Buffer_4, sizeof(Buffer_4),
    _DtCM_LIMIT_TEXT, build_buffer4 },
  { "new target of a v4 rename", (xdrproc_t)_DtCm_xdr_Table_Op_Args_4,
    sizeof(Table_Op_Args_4), _DtCM_LIMIT_NAME, build_table_op_args },
  { "target of a v3 request", (xdrproc_t)_DtCm_xdr_Table_Args_3,
    sizeof(Table_Args_3), _DtCM_LIMIT_NAME, build_table_args3 },
  { "target of a v2 request", (xdrproc_t)_DtCm_xdr_Table_Args_2,
    sizeof(Table_Args_2), _DtCM_LIMIT_NAME, build_table_args2 },
};

/* A string at the limit decodes, one more character does not. */
static void case_string_limits(void)
{
  size_t i;

  for(i = 0; i < sizeof(string_cases) / sizeof(string_cases[0]); i++) {
    const StringCase *c = &string_cases[i];
    void *obj;
    Message m;
    bool_t ok;
    size_t n = c->limit;

    memset(&m, 0, sizeof(m));
    obj = calloc(1, c->object_size);
    c->build(&m, n);
    ok = decode(&m, c->proc, obj);
    if(!ok)
      fprintf(stderr, "%s: a string of the limit is refused\n", c->name);
    CHECK(ok);
    release(c->proc, obj);
    free(m.data);

    memset(&m, 0, sizeof(m));
    memset(obj, 0, c->object_size);
    c->build(&m, n + 1);
    ok = decode(&m, c->proc, obj);
    if(ok)
      fprintf(stderr, "%s: a string of the limit + 1 is accepted\n", c->name);
    CHECK(!ok);
    release(c->proc, obj);
    free(m.data);
    free(obj);
  }
}

/* The length of a string is read before the string and must not make the
   decoder allocate. */
static void case_hostile_lengths(void)
{
  Message m;
  cms_open_args args;
  cms_archive_res res;
  uint32_t lengths[] = { 0x7fffffffu, 0xffffffffu, 0x80000000u };
  size_t i;

  for(i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
    memset(&m, 0, sizeof(m));
    put_u32(&m, lengths[i]);
    memset(&args, 0, sizeof(args));
    CHECK(!decode(&m, (xdrproc_t)xdr_cms_open_args, &args));
    release((xdrproc_t)xdr_cms_open_args, &args);
    free(m.data);

    memset(&m, 0, sizeof(m));
    put_u32(&m, 0);
    put_u32(&m, lengths[i]);
    memset(&res, 0, sizeof(res));
    CHECK(!decode(&m, (xdrproc_t)xdr_cms_archive_res, &res));
    release((xdrproc_t)xdr_cms_archive_res, &res);
    free(m.data);
  }
}

/* ---- arrays -------------------------------------------------------------- */

/* A get_cal_attr request with N attribute names. */
static void build_names_request(Message *m, size_t n, int with_names)
{
  size_t i;

  put_string(m, 1);
  put_u32(m, (uint32_t)n);
  if(with_names)
    for(i = 0; i < n; i++) {
      put_u32(m, 0);		/* number, as an int */
      put_string(m, 0);
    }
}

static void case_array_limits(void)
{
  Message m;
  cms_get_cal_attr_args args;

  memset(&m, 0, sizeof(m));
  build_names_request(&m, _DtCM_LIMIT_ATTRIBUTES, 1);
  memset(&args, 0, sizeof(args));
  CHECK(decode(&m, (xdrproc_t)xdr_cms_get_cal_attr_args, &args));
  CHECK(args.num_names == _DtCM_LIMIT_ATTRIBUTES);
  release((xdrproc_t)xdr_cms_get_cal_attr_args, &args);
  free(m.data);

  /* one more name, all of them in the message */
  memset(&m, 0, sizeof(m));
  build_names_request(&m, _DtCM_LIMIT_ATTRIBUTES + 1, 1);
  memset(&args, 0, sizeof(args));
  CHECK(!decode(&m, (xdrproc_t)xdr_cms_get_cal_attr_args, &args));
  release((xdrproc_t)xdr_cms_get_cal_attr_args, &args);
  free(m.data);
}

/* The count is read before the elements and must not make the decoder
   allocate. */
static void case_hostile_counts(void)
{
  Message m;
  cms_get_cal_attr_args args;
  cms_list_calendars_res list;
  uint32_t counts[] = { 0x7fffffffu, 0xffffffffu, 0x10000000u };
  size_t i;

  for(i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
    memset(&m, 0, sizeof(m));
    build_names_request(&m, counts[i], 0);
    memset(&args, 0, sizeof(args));
    CHECK(!decode(&m, (xdrproc_t)xdr_cms_get_cal_attr_args, &args));
    release((xdrproc_t)xdr_cms_get_cal_attr_args, &args);
    free(m.data);

    memset(&m, 0, sizeof(m));
    put_u32(&m, 0);
    put_u32(&m, counts[i]);
    memset(&list, 0, sizeof(list));
    CHECK(!decode(&m, (xdrproc_t)xdr_cms_list_calendars_res, &list));
    release((xdrproc_t)xdr_cms_list_calendars_res, &list);
    free(m.data);
  }
}

/* The opaque data of a value is bounded as well. */
static void case_opaque_data(void)
{
  Message m;
  cms_attribute_value value;
  size_t sizes[] = { _DtCM_LIMIT_TEXT, _DtCM_LIMIT_TEXT + 1, 0x7fffffffu };
  size_t i;

  for(i = 0; i < 3; i++) {
    memset(&m, 0, sizeof(m));
    put_u32(&m, CSA_VALUE_OPAQUE_DATA);
    put_u32(&m, 1);		/* the pointer is there */
    put_u32(&m, (uint32_t)sizes[i]);
    if(sizes[i] <= _DtCM_LIMIT_TEXT + 1) {
      char *data = calloc(1, sizes[i]);

      put_bytes(&m, data, sizes[i]);
      free(data);
    }
    memset(&value, 0, sizeof(value));
    if(i == 0)
      CHECK(decode(&m, (xdrproc_t)xdr_cms_attribute_value, &value));
    else
      CHECK(!decode(&m, (xdrproc_t)xdr_cms_attribute_value, &value));
    release((xdrproc_t)xdr_cms_attribute_value, &value);
    free(m.data);
  }
}

/* ---- attribute values, enumerations ---------------------------------------- */

static void case_value_types(void)
{
  Message m;
  cms_attribute_value value;
  int types[] = { -1, 15, 99, CSA_VALUE_ATTENDEE_LIST };
  size_t i;

  for(i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
    memset(&m, 0, sizeof(m));
    put_u32(&m, (uint32_t)types[i]);
    put_u32(&m, 0);
    memset(&value, 0, sizeof(value));
    if(decode(&m, (xdrproc_t)xdr_cms_attribute_value, &value))
      fprintf(stderr, "type %d is accepted\n", types[i]);
    CHECK(!decode(&m, (xdrproc_t)xdr_cms_attribute_value, &value));
    free(m.data);
  }
}

static void case_enumerations(void)
{
  Message m;
  Table_Args_4 args4;
  Table_Res_4 res4;
  Tag_4 tag;
  Table_Status_4 status;

  /* a request of version 4 whose argument type is not one of the types */
  memset(&m, 0, sizeof(m));
  put_string(&m, 1);
  put_u32(&m, 99);
  put_u32(&m, 1);
  memset(&args4, 0, sizeof(args4));
  CHECK(!decode(&m, (xdrproc_t)_DtCm_xdr_Table_Args_4, &args4));
  release((xdrproc_t)_DtCm_xdr_Table_Args_4, &args4);
  free(m.data);

  /* a reply with a status that no server sends */
  memset(&m, 0, sizeof(m));
  put_u32(&m, 99);
  put_u32(&m, 0);
  memset(&res4, 0, sizeof(res4));
  CHECK(!decode(&m, (xdrproc_t)_DtCm_xdr_Table_Res_4, &res4));
  release((xdrproc_t)_DtCm_xdr_Table_Res_4, &res4);
  free(m.data);

  /* the type of a list of results */
  memset(&m, 0, sizeof(m));
  put_u32(&m, 0);
  put_u32(&m, 77);
  memset(&res4, 0, sizeof(res4));
  CHECK(!decode(&m, (xdrproc_t)_DtCm_xdr_Table_Res_4, &res4));
  release((xdrproc_t)_DtCm_xdr_Table_Res_4, &res4);
  free(m.data);

  /* an event type */
  memset(&m, 0, sizeof(m));
  put_u32(&m, 5);
  put_u32(&m, 0);
  put_u32(&m, 0);
  memset(&tag, 0, sizeof(tag));
  CHECK(!decode(&m, (xdrproc_t)_DtCm_xdr_Tag_4, &tag));
  release((xdrproc_t)_DtCm_xdr_Tag_4, &tag);
  free(m.data);

  memset(&m, 0, sizeof(m));
  put_u32(&m, 9);
  memset(&status, 0, sizeof(status));
  CHECK(!decode(&m, (xdrproc_t)_DtCm_xdr_Table_Status_4, &status));
  free(m.data);

  /* the largest value of each is accepted */
  memset(&m, 0, sizeof(m));
  put_u32(&m, 8);
  CHECK(decode(&m, (xdrproc_t)_DtCm_xdr_Table_Status_4, &status));
  free(m.data);
}

/* ---- lists --------------------------------------------------------------- */

#define LONG_LIST 300000

/* A list of N nodes in a result of version 4, built by hand. */
static Uid_4 *make_uids(size_t n)
{
  Uid_4 *head = NULL;
  size_t i;

  for(i = 0; i < n; i++) {
    Uid_4 *node = calloc(1, sizeof(*node));

    node->appt_id.tick = (time_t)i;
    node->appt_id.key = (long)i;
    node->next = head;
    head = node;
  }
  return(head);
}

static void free_uids(Uid_4 *head)
{
  while(head != NULL) {
    Uid_4 *next = head->next;

    free(head);
    head = next;
  }
}

static size_t count_uids(Uid_4 *head)
{
  size_t n = 0;

  for(; head != NULL; head = head->next)
    n++;
  return(n);
}

/* A reply with a very long list is sent, read and freed; the routines for a
   list nest as deep as the list is long if they recurse. */
static void case_long_list(void)
{
  Table_Res_4 res, copy;
  Message m;

  memset(&res, 0, sizeof(res));
  res.status = access_ok_4;
  res.res.tag = ID_4;
  res.res.Table_Res_List_4_u.i = make_uids(LONG_LIST);

  CHECK(encode(&m, (xdrproc_t)_DtCm_xdr_Table_Res_4, &res, LONG_LIST * 40));
  CHECK(m.length > (size_t)LONG_LIST * 12);

  memset(&copy, 0, sizeof(copy));
  CHECK(decode(&m, (xdrproc_t)_DtCm_xdr_Table_Res_4, &copy));
  CHECK(copy.res.tag == ID_4);
  CHECK(count_uids(copy.res.Table_Res_List_4_u.i) == LONG_LIST);
  if(count_uids(copy.res.Table_Res_List_4_u.i) == LONG_LIST) {
    Uid_4 *node = copy.res.Table_Res_List_4_u.i;

    CHECK(node->appt_id.key == LONG_LIST - 1);
  }
  release((xdrproc_t)_DtCm_xdr_Table_Res_4, &copy);
  CHECK(copy.res.Table_Res_List_4_u.i == NULL);
  free(m.data);
  free_uids(res.res.Table_Res_List_4_u.i);
}

/* Freeing a list of the extensible protocol. */
static void case_free_long_list(void)
{
  cms_entry *head = NULL, *e;
  size_t i;
  cms_entries_res res;

  for(i = 0; i < LONG_LIST; i++) {
    e = calloc(1, sizeof(*e));
    e->key.id = (long)i;
    e->next = head;
    head = e;
  }
  memset(&res, 0, sizeof(res));
  res.entries = head;
  release((xdrproc_t)xdr_cms_entries_res, &res);
  CHECK(res.entries == NULL);
}

/* Lists have a longest length, and a longer one is refused on both sides. */
static void case_list_limits(void)
{
  Table_Res_4 res, copy;
  Message m;
  size_t i;
  Tag_4 *tags = NULL, *t;
  Appt_4 appt, back;
  size_t counts[] = { _DtCM_LIMIT_TAGS, _DtCM_LIMIT_TAGS + 1 };

  for(i = 0; i < 2; i++) {
    size_t n;

    tags = NULL;
    for(n = 0; n < counts[i]; n++) {
      t = calloc(1, sizeof(*t));
      t->tag = appointment_4;
      t->next = tags;
      tags = t;
    }
    memset(&appt, 0, sizeof(appt));
    appt.tag = tags;
    appt.what = "x";
    appt.author = "x";
    appt.client_data = "x";
    memset(&res, 0, sizeof(res));
    res.res.tag = AP_4;
    res.res.Table_Res_List_4_u.a = &appt;

    memset(&m, 0, sizeof(m));
    if(i == 0) {
      CHECK(encode(&m, (xdrproc_t)_DtCm_xdr_Table_Res_4, &res, 4096));
      memset(&copy, 0, sizeof(copy));
      CHECK(decode(&m, (xdrproc_t)_DtCm_xdr_Table_Res_4, &copy));
      back = *copy.res.Table_Res_List_4_u.a;
      CHECK(back.tag != NULL);
      release((xdrproc_t)_DtCm_xdr_Table_Res_4, &copy);
    } else {
      /* too long to send */
      CHECK(!encode(&m, (xdrproc_t)_DtCm_xdr_Table_Res_4, &res, 4096));
      /* and to read: the same reply made by a server without the limit */
      free(m.data);
      memset(&m, 0, sizeof(m));
      put_u32(&m, 0);			/* status */
      put_u32(&m, AP_4);
      put_u32(&m, 1);			/* an appointment */
      put_u32(&m, 0); put_u32(&m, 0);	/* id */
      put_u32(&m, 1);			/* tags */
      for(n = 0; n < counts[i]; n++) {
	put_u32(&m, 0); put_u32(&m, 0);	/* tag, showtime */
	put_u32(&m, n + 1 < counts[i]);
      }
      /* the rest of the appointment need not be there: the tags are what
         makes the message wrong */
      memset(&copy, 0, sizeof(copy));
      CHECK(!decode(&m, (xdrproc_t)_DtCm_xdr_Table_Res_4, &copy));
      release((xdrproc_t)_DtCm_xdr_Table_Res_4, &copy);
    }
    free(m.data);
    while(tags != NULL) {
      t = tags->next;
      free(tags);
      tags = t;
    }
  }
}

/* A list of version 4 and the same list written by the recursive routines
   that rpcgen generates have the same bytes; these are written out below
   for one list of each shape: `next' last, in the middle and first. */
static void put_u32_at_end(Message *m, uint32_t v) { put_u32(m, v); }

static void case_wire_format(void)
{
  Message got, want;
  Tag_4 t3 = { appointment_4, 33, NULL }, t2 = { reminder_4, 22, &t3 },
	t1 = { otherTag_4, 11, &t2 };
  Abb_Appt_4 b2, b1;
  Attribute_4 a3, a2, a1;
  Registration_4 r2, r1;

  /* next last: tag showtime more tag showtime more tag showtime nomore */
  memset(&want, 0, sizeof(want));
  put_u32(&want, 2); put_u32(&want, 11); put_u32(&want, 1);
  put_u32(&want, 1); put_u32(&want, 22); put_u32(&want, 1);
  put_u32(&want, 0); put_u32(&want, 33); put_u32(&want, 0);
  CHECK(encode(&got, (xdrproc_t)_DtCm_xdr_Tag_4, &t1, 256));
  CHECK(got.length == want.length && memcmp(got.data, want.data, want.length) == 0);
  free(got.data);
  free(want.data);

  /* next first: more, more, no more, then the strings of the last node
     first */
  memset(&a1, 0, sizeof(a1));
  memset(&a2, 0, sizeof(a2));
  memset(&a3, 0, sizeof(a3));
  a1.next = &a2; a2.next = &a3;
  a1.attr = "a1"; a1.value = "v1"; a1.clientdata = "c1";
  a2.attr = "a2"; a2.value = "v2"; a2.clientdata = "c2";
  a3.attr = "a3"; a3.value = "v3"; a3.clientdata = "c3";
  memset(&want, 0, sizeof(want));
  put_u32(&want, 1); put_u32(&want, 1); put_u32(&want, 0);
  {
    const char *order[] = { "3", "2", "1" };
    int i;

    for(i = 0; i < 3; i++) {
      char s[3];
      size_t k;

      for(k = 0; k < 3; k++) {
	s[0] = "avc"[k]; s[1] = order[i][0]; s[2] = 0;
	put_u32(&want, 2);
	put_bytes(&want, s, 2);
	put_bytes(&want, "\0\0", 2);
      }
    }
  }
  CHECK(encode(&got, (xdrproc_t)_DtCm_xdr_Attribute_4, &a1, 512));
  CHECK(got.length == want.length && memcmp(got.data, want.data, want.length) == 0);
  free(got.data);
  free(want.data);

  /* next in the middle: the last two fields of the second node come
     before those of the first */
  memset(&b1, 0, sizeof(b1));
  memset(&b2, 0, sizeof(b2));
  b1.next = &b2;
  b1.what = "1"; b2.what = "2";
  b1.appt_status = cancelled_4; b2.appt_status = completed_4;
  b1.privacy = private_4; b2.privacy = semiprivate_4;
  b1.period.period = daily_4; b2.period.period = yearly_4;
  b1.duration = 10; b2.duration = 20;
  memset(&want, 0, sizeof(want));
  {
    int i;
    Abb_Appt_4 *nodes[2] = { &b1, &b2 };

    for(i = 0; i < 2; i++) {
      put_u32(&want, 0); put_u32(&want, 0);	/* id */
      put_u32(&want, 0);			/* no tag */
      put_u32(&want, 1);			/* what */
      put_bytes(&want, i ? "2\0\0\0" : "1\0\0\0", 4);
      put_u32(&want, nodes[i]->duration);
      put_u32(&want, nodes[i]->period.period);
      put_u32(&want, nodes[i]->period.nth);
      put_u32(&want, 0);			/* enddate */
      put_u32(&want, i == 0);			/* more */
    }
    for(i = 1; i >= 0; i--) {
      put_u32(&want, nodes[i]->appt_status);
      put_u32(&want, nodes[i]->privacy);
    }
  }
  CHECK(encode(&got, (xdrproc_t)_DtCm_xdr_Abb_Appt_4, &b1, 512));
  CHECK(got.length == want.length && memcmp(got.data, want.data, want.length) == 0);
  free(got.data);
  free(want.data);

  /* the same for a registration: the process id follows the next node */
  memset(&r1, 0, sizeof(r1));
  memset(&r2, 0, sizeof(r2));
  r1.next = &r2;
  r1.target = "a"; r2.target = "b";
  r1.prognum = 1; r1.versnum = 2; r1.procnum = 3; r1.pid = 101;
  r2.prognum = 4; r2.versnum = 5; r2.procnum = 6; r2.pid = 202;
  memset(&want, 0, sizeof(want));
  put_u32(&want, 1); put_bytes(&want, "a\0\0\0", 4);
  put_u32(&want, 1); put_u32(&want, 2); put_u32(&want, 3);
  put_u32(&want, 1);
  put_u32(&want, 1); put_bytes(&want, "b\0\0\0", 4);
  put_u32(&want, 4); put_u32(&want, 5); put_u32(&want, 6);
  put_u32(&want, 0);
  put_u32(&want, 202);
  put_u32(&want, 101);
  CHECK(encode(&got, (xdrproc_t)_DtCm_xdr_Registration_4, &r1, 512));
  CHECK(got.length == want.length && memcmp(got.data, want.data, want.length) == 0);
  free(got.data);
  free(want.data);
  (void)put_u32_at_end;
}

/* A message that ends in the middle of a list leaves what was read in a
   state that can be freed, with nothing lost. */
static void case_truncated_list(void)
{
  Table_Res_4 res;
  Message m;
  size_t cut;

  memset(&res, 0, sizeof(res));
  res.res.tag = ID_4;
  res.res.Table_Res_List_4_u.i = make_uids(50);
  CHECK(encode(&m, (xdrproc_t)_DtCm_xdr_Table_Res_4, &res, 4096));
  free_uids(res.res.Table_Res_List_4_u.i);

  for(cut = 8; cut < m.length; cut += 4) {
    Table_Res_4 copy;
    Message part = m;

    part.length = cut;
    memset(&copy, 0, sizeof(copy));
    CHECK(!decode(&part, (xdrproc_t)_DtCm_xdr_Table_Res_4, &copy));
    release((xdrproc_t)_DtCm_xdr_Table_Res_4, &copy);
  }
  free(m.data);
}

/* An attribute list in front of a failing string frees every node. */
static void case_truncated_attributes(void)
{
  Appt_4 appt, copy;
  Attribute_4 a3, a2, a1;
  Message m;
  size_t cut;

  memset(&a1, 0, sizeof(a1));
  memset(&a2, 0, sizeof(a2));
  memset(&a3, 0, sizeof(a3));
  a1.next = &a2; a2.next = &a3;
  a1.attr = a2.attr = a3.attr = "name";
  a1.value = a2.value = a3.value = "value";
  a1.clientdata = a2.clientdata = a3.clientdata = "data";
  memset(&appt, 0, sizeof(appt));
  appt.what = appt.author = appt.client_data = "text";
  appt.attr = &a1;
  CHECK(encode(&m, (xdrproc_t)_DtCm_xdr_Appt_4, &appt, 4096));

  for(cut = 0; cut < m.length; cut += 4) {
    Message part = m;

    part.length = cut;
    memset(&copy, 0, sizeof(copy));
    CHECK(!decode(&part, (xdrproc_t)_DtCm_xdr_Appt_4, &copy));
    release((xdrproc_t)_DtCm_xdr_Appt_4, &copy);
  }
  free(m.data);
}

/* A string that is not there is sent as the empty string. */
static void case_null_strings(void)
{
  Appt_4 appt, copy;
  Message m;
  cms_name name = NULL;
  Table_Args_4 args;

  memset(&appt, 0, sizeof(appt));
  CHECK(encode(&m, (xdrproc_t)_DtCm_xdr_Appt_4, &appt, 4096));
  memset(&copy, 0, sizeof(copy));
  CHECK(decode(&m, (xdrproc_t)_DtCm_xdr_Appt_4, &copy));
  CHECK(copy.what != NULL && copy.what[0] == '\0');
  release((xdrproc_t)_DtCm_xdr_Appt_4, &copy);
  free(m.data);

  CHECK(encode(&m, (xdrproc_t)xdr_cms_name, &name, 64));
  free(m.data);

  memset(&args, 0, sizeof(args));
  args.args.tag = TICK_4;
  CHECK(encode(&m, (xdrproc_t)_DtCm_xdr_Table_Args_4, &args, 64));
  free(m.data);
}

/* An array that is said to have elements and is not there is not sent. */
static void case_missing_array(void)
{
  cms_get_cal_attr_args args;
  Message m;

  memset(&args, 0, sizeof(args));
  args.cal = "calendar";
  args.num_names = 3;
  CHECK(!encode(&m, (xdrproc_t)xdr_cms_get_cal_attr_args, &args, 256));
  free(m.data);
}

/* The lists of the extensible protocol: limits, and the order of the bytes
   against the form of the recursive routines (the next node follows the
   members of a node). */
static void case_access_list(void)
{
  cms_access_entry first, second;
  cms_entry_res dummy;
  cms_attribute_value value;
  Message m;
  size_t i;

  (void)dummy;
  memset(&first, 0, sizeof(first));
  memset(&second, 0, sizeof(second));
  first.user = "alice@host";
  first.rights = 3;
  first.next = &second;
  second.user = "bob@host";
  second.rights = 4;
  memset(&value, 0, sizeof(value));
  value.type = CSA_VALUE_ACCESS_LIST;
  value.item.access_list_value = &first;
  CHECK(encode(&m, (xdrproc_t)xdr_cms_attribute_value, &value, 512));
  {
    Message want;

    memset(&want, 0, sizeof(want));
    put_u32(&want, CSA_VALUE_ACCESS_LIST);
    put_u32(&want, 1);
    put_u32(&want, 10); put_bytes(&want, "alice@host\0\0", 12);
    put_u32(&want, 3);
    put_u32(&want, 1);
    put_u32(&want, 8); put_bytes(&want, "bob@host", 8);
    put_u32(&want, 4);
    put_u32(&want, 0);
    CHECK(m.length == want.length &&
	  memcmp(m.data, want.data, want.length) == 0);
    free(want.data);
  }
  free(m.data);

  /* the longest list and one more */
  for(i = 0; i < 2; i++) {
    size_t n, count = _DtCM_LIMIT_ACCESS_ENTRIES + i;
    Message long_message;
    cms_attribute_value back;

    memset(&long_message, 0, sizeof(long_message));
    put_u32(&long_message, CSA_VALUE_ACCESS_LIST);
    put_u32(&long_message, 1);
    for(n = 0; n < count; n++) {
      put_u32(&long_message, 0);	/* the user */
      put_u32(&long_message, 1);	/* the rights */
      put_u32(&long_message, n + 1 < count);
    }
    memset(&back, 0, sizeof(back));
    if(i == 0)
      CHECK(decode(&long_message, (xdrproc_t)xdr_cms_attribute_value, &back));
    else
      CHECK(!decode(&long_message, (xdrproc_t)xdr_cms_attribute_value, &back));
    release((xdrproc_t)xdr_cms_attribute_value, &back);
    free(long_message.data);
  }
}

/* The callback protocol. */
static void case_callback_limits(void)
{
  Message m;
  cmcb_update_callback_args args;
  size_t counts[] = { _DtCM_LIMIT_ATTRIBUTES, _DtCM_LIMIT_ATTRIBUTES + 1 };
  size_t i;

  for(i = 0; i < 2; i++) {
    size_t n;

    memset(&m, 0, sizeof(m));
    put_string(&m, 1);
    put_string(&m, 1);
    put_u32(&m, CSA_CB_CALENDAR_ATTRIBUTE_UPDATED);
    put_u32(&m, 1);		/* the data are there */
    put_u32(&m, (uint32_t)counts[i]);
    for(n = 0; n < counts[i]; n++)
      put_string(&m, 0);
    memset(&args, 0, sizeof(args));
    if(i == 0)
      CHECK(decode(&m, (xdrproc_t)xdr_cmcb_update_callback_args, &args));
    else
      CHECK(!decode(&m, (xdrproc_t)xdr_cmcb_update_callback_args, &args));
    release((xdrproc_t)xdr_cmcb_update_callback_args, &args);
    free(m.data);
  }
}

/* ---- the cases ------------------------------------------------------------ */

static const struct {
  const char *name;
  void (*run)(void);
} cases[] = {
  { "string-limits", case_string_limits },
  { "hostile-lengths", case_hostile_lengths },
  { "array-limits", case_array_limits },
  { "hostile-counts", case_hostile_counts },
  { "opaque-data", case_opaque_data },
  { "value-types", case_value_types },
  { "enumerations", case_enumerations },
  { "long-list", case_long_list },
  { "free-long-list", case_free_long_list },
  { "list-limits", case_list_limits },
  { "wire-format", case_wire_format },
  { "truncated-list", case_truncated_list },
  { "truncated-attributes", case_truncated_attributes },
  { "null-strings", case_null_strings },
  { "missing-array", case_missing_array },
  { "access-list", case_access_list },
  { "callback-limits", case_callback_limits },
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
