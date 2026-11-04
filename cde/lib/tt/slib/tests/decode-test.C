/*
 * Regression tests for the decoding of what a ToolTalk client sends to
 * ttsession: the strings and objects of the XDR encodings, the session
 * addresses that come with them, the authentication of a request and the
 * argument handling of the RPC procedures.  The encodings are built by hand,
 * with lengths and null markers the libtt of a real client never produces.
 *
 * check-decode.py compiles the production sources these tests exercise next
 * to this program, with the sanitizers, and links them with the built
 * libtt and libstt.a, so the code that ships is what is tested.  The RPC
 * requests go through the production _tt_service_rpc(), with a transport of
 * this program's making that serves the request bodies from memory and
 * records the replies.  Each case runs in its own process (decode-test CASE),
 * so a crash is reported for its case alone.
 *
 * /dev/urandom is the one file the production code opens that a test has to
 * control; the program's __wrap_open() (see the link line in
 * check-decode.py) lets a case replace it.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <rpc/rpc.h>

#include <string>
#include <vector>

#include "tt_options.h"
#include "util/tt_global_env.h"
#include "util/tt_string.h"
#include "util/tt_xdr_version.h"
#include "mp/mp_global.h"
#include "mp/mp_auth.h"
#include "mp/mp_session.h"
#include "mp/mp_rpc_interface.h"
#include "mp/mp_xdr_functions.h"
#include "api/c/api_xdr.h"
#include "mp_s_global.h"
#include "mp_s_mp.h"
#include "mp_s_session.h"
#include "mp_otype.h"
#include "mp_rpc_implement.h"
#include "mp_typedb.h"

#if defined(__SANITIZE_ADDRESS__)
/* of the address sanitizer's runtime, whose headers do not all come with gcc */
extern "C" size_t __sanitizer_get_current_allocated_bytes(void);
#endif

static int failures;

#define CHECK(cond) \
  do { \
    if(!(cond)) { \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
      failures++; \
    } \
  } while(0)

/* what the session of the case wants to see in a request */
static std::string session_cookie;

/*
 * The memory in use, to see that what a failed decoding allocated has been
 * freed.  Only the address sanitizer can tell; without it nothing is
 * checked.  Headroom stands for what libraries keep for themselves.
 */
static size_t
allocated(void)
{
#if defined(__SANITIZE_ADDRESS__)
  return __sanitizer_get_current_allocated_bytes();
#else
  return 0;
#endif
}

#define NO_LEAK_SINCE(before) (allocated() < (before) + (1 << 16))

/* ---- /dev/urandom, under the control of the cases -------------------- */

extern "C" int __real_open(const char *path, int flags, ...);

static enum { RANDOM_REAL, RANDOM_FAIL, RANDOM_FEED } random_mode = RANDOM_REAL;
static std::string random_feed;
static int random_opens;

extern "C" int
__wrap_open(const char *path, int flags, ...)
{
  mode_t mode = 0;
  if(flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, int);
    va_end(ap);
  }
  if(random_mode != RANDOM_REAL && path && !strcmp(path, "/dev/urandom")) {
    random_opens++;
    if(random_mode == RANDOM_FAIL) {
      errno = ENOENT;
      return -1;
    }
    /* the generator is a pipe that holds the bytes of the case and ends */
    int fds[2];
    if(pipe(fds))
      return -1;
    if(write(fds[1], random_feed.data(), random_feed.size()) !=
       (ssize_t)random_feed.size()) {
      close(fds[0]);
      close(fds[1]);
      return -1;
    }
    close(fds[1]);
    return fds[0];
  }
  return __real_open(path, flags, mode);
}

/* ---- setup -------------------------------------------------------------- */

static void
init_globals(void)
{
  _tt_global = new _Tt_global();
  _tt_global->silent = 1;
  _tt_s_mp = new _Tt_s_mp;
  _tt_mp = (_Tt_mp *)_tt_s_mp;
}

/* A growing XDR encoding, for building the bodies of requests. */
class Body {
public:
  Body() : buf(1 << 20) {
    xdrmem_create(&xdrs, &buf[0], (u_int)buf.size(), XDR_ENCODE);
  }
  Body &i(int v) { xdr_int(&xdrs, &v); return *this; }
  Body &l(long v) { xdr_long(&xdrs, &v); return *this; }
  /* a _Tt_string in the encoding of xdr version 2 and later */
  Body &str(const char *s, int len = -2) {
    if(!s) {
      i(-1);
      return *this;
    }
    if(len == -2)
      len = (int)strlen(s);
    i(len);
    xdr_opaque(&xdrs, (caddr_t)s, len);
    return *this;
  }
  Body &cookie();
  Body &raw(const void *p, int n) {
    xdr_opaque(&xdrs, (caddr_t)p, n);
    return *this;
  }
  /* a procid: not null, id, pid, host address */
  Body &procid(const char *id, const char *host) {
    i(0);
    str(id);
    l(1234);
    str(host);
    return *this;
  }
  const char *data() { return &buf[0]; }
  u_int size() { return xdr_getpos(&xdrs); }

private:
  XDR xdrs;
  std::vector<char> buf;
};

Body &
Body::cookie()
{
  return str(session_cookie.data(), (int)session_cookie.size());
}

/* ---- strings ---------------------------------------------------------- */

/*
 * Decodes a string from a record stream, the way ttsession reads what a
 * client sends: the stream holds the given bytes and ends there.
 */
struct Stream {
  const char *data;
  int size;
  int pos;
};

#if defined(OPT_TIRPC)
typedef int (*record_io)(void *, void *, int);
#else
typedef int (*record_io)();
#endif

static int
stream_read(void *handle, void *buf, int len)
{
  Stream *s = (Stream *)handle;
  int n = s->size - s->pos;
  if(n <= 0)
    return -1;
  if(n > len)
    n = len;
  memcpy(buf, s->data + s->pos, n);
  s->pos += n;
  return n;
}

static int
stream_write(void *, void *, int len)
{
  return len;
}

static bool_t
decode_from_record(const std::string &bytes, _Tt_string &s)
{
  /* one record, one last fragment */
  std::string rec;
  uint32_t mark = htonl(0x80000000u | (uint32_t)bytes.size());
  rec.append((const char *)&mark, 4);
  rec += bytes;
  Stream st = { rec.data(), (int)rec.size(), 0 };
  XDR xdrs;
  xdrrec_create(&xdrs, 4000, 4000, (char *)&st,
		(record_io)stream_read, (record_io)stream_write);
  xdrs.x_op = XDR_DECODE;
  xdrrec_skiprecord(&xdrs);
  bool_t ok = tt_xdr_string(&xdrs, &s);
  xdr_destroy(&xdrs);
  return ok;
}

static std::string
be32(uint32_t v)
{
  uint32_t n = htonl(v);
  return std::string((const char *)&n, 4);
}

static bool_t
decode_mem(const std::string &bytes, _Tt_string &s)
{
  XDR xdrs;
  xdrmem_create(&xdrs, (char *)bytes.data(), (u_int)bytes.size(), XDR_DECODE);
  return tt_xdr_string(&xdrs, &s);
}

/* A length no allocation can satisfy: the string cannot be decoded. */
static void
test_string_v2_huge_length(void)
{
  init_globals();
  _Tt_string s;
  std::string wire = be32(INT_MAX) + std::string(64, 'A');
  CHECK(!decode_from_record(wire, s));
}

static void
test_string_v2_length_beyond_data_leaves_nothing(void)
{
  init_globals();
  size_t before = allocated();
  {
    _Tt_string s;
    std::string wire = be32(1 << 20) + std::string(10, 'A');
    CHECK(!decode_mem(wire, s));
  }
  /* the buffer made for the content must not be left behind */
  CHECK(NO_LEAK_SINCE(before));
}

static void
test_string_v2_negative_length(void)
{
  init_globals();
  _Tt_string s;
  CHECK(!decode_mem(be32((uint32_t)-5) + std::string(16, 'A'), s));
  _Tt_string t;
  CHECK(!decode_mem(be32((uint32_t)INT_MIN), t));
}

static void
test_string_v2_roundtrip(void)
{
  init_globals();
  _Tt_string empty = "";
  _Tt_string text = "hello, world";
  _Tt_string null_string;
  char out[256];
  XDR enc;
  xdrmem_create(&enc, out, sizeof out, XDR_ENCODE);
  CHECK(tt_xdr_string(&enc, &text));
  CHECK(tt_xdr_string(&enc, &empty));
  CHECK(tt_xdr_string(&enc, &null_string));
  u_int used = xdr_getpos(&enc);

  XDR dec;
  xdrmem_create(&dec, out, used, XDR_DECODE);
  _Tt_string a, b, c;
  CHECK(tt_xdr_string(&dec, &a));
  CHECK(tt_xdr_string(&dec, &b));
  CHECK(tt_xdr_string(&dec, &c));
  CHECK(a.len() == 12 && !strcmp((char *)a, "hello, world"));
  CHECK(b.len() == 0 && (char *)b != 0);
  CHECK(c.len() == 0 && (char *)c == 0);
}

/* version 1: the length is sent twice, once by the string, once by xdr_bytes */
static std::string
v1_wire(uint32_t len, uint32_t len2, const std::string &data)
{
  return be32(len) + be32(len2) + data;
}

static void
test_string_v1_huge_length(void)
{
  init_globals();
  _Tt_xdr_version v1(1);
  _Tt_string s;
  /* the second length is small and agrees with the data that follows */
  CHECK(!decode_mem(v1_wire(INT_MAX, 4, "AAAA"), s));
  CHECK(s.len() == 0);
}

static void
test_string_v1_lengths_disagree(void)
{
  init_globals();
  _Tt_xdr_version v1(1);
  _Tt_string s;
  CHECK(!decode_mem(v1_wire(100, 3, std::string("abc\0", 4)), s));
  CHECK(s.len() == 0);
}

static void
test_string_v1_negative_length(void)
{
  init_globals();
  _Tt_xdr_version v1(1);
  _Tt_string s;
  CHECK(!decode_mem(be32((uint32_t)-7), s));
  CHECK(s.len() >= 0);
}

static void
test_string_v1_roundtrip(void)
{
  init_globals();
  _Tt_xdr_version v1(1);
  _Tt_string text = "version one";
  _Tt_string null_string;
  char out[128];
  XDR enc;
  xdrmem_create(&enc, out, sizeof out, XDR_ENCODE);
  CHECK(tt_xdr_string(&enc, &text));
  CHECK(tt_xdr_string(&enc, &null_string));
  u_int used = xdr_getpos(&enc);
  XDR dec;
  xdrmem_create(&dec, out, used, XDR_DECODE);
  _Tt_string a, b;
  CHECK(tt_xdr_string(&dec, &a));
  CHECK(tt_xdr_string(&dec, &b));
  CHECK(a.len() == 11 && !strcmp((char *)a, "version one"));
  CHECK(b.len() == 0);
}

/* ---- the lists of strings of the API -------------------------------- */

static void
test_cstring_list_bad_counts(void)
{
  init_globals();
  size_t before = allocated();
  char **list = 0;
  XDR neg;
  std::string a = be32((uint32_t)-1);
  xdrmem_create(&neg, (char *)a.data(), (u_int)a.size(), XDR_DECODE);
  CHECK(!_tt_xdr_cstring_list(&neg, &list));

  /* a count the data cannot back, with the first string cut short */
  std::string b = be32(1000000) + be32(40) + std::string(8, 'x');
  XDR trunc;
  xdrmem_create(&trunc, (char *)b.data(), (u_int)b.size(), XDR_DECODE);
  CHECK(!_tt_xdr_cstring_list(&trunc, &list));
  CHECK(list == 0);
  CHECK(NO_LEAK_SINCE(before));
}

static void
test_bstring_negative_length(void)
{
  init_globals();
  char *s = 0;
  std::string a = be32((uint32_t)-1) + be32(4000) + std::string(4000, 'A');
  XDR x;
  xdrmem_create(&x, (char *)a.data(), (u_int)a.size(), XDR_DECODE);
  CHECK(!tt_xdr_bstring(&x, &s));
}

/* ---- session addresses ---------------------------------------------- */

class TestSession : public _Tt_session {
public:
  using _Tt_session::parsed_address;
  const _Tt_string &hostaddr() const { return _hostaddr; }
};

static void
test_session_address_long_host(void)
{
  init_globals();
  /* the host field of the address is 16 characters at most: a longer one
     must not be stored past its buffer */
  const char *hosts[] = { "1234567890123456", "12345678901234567890",
			  "1234567890123456789012345678901234567890123456789"
			  "0123456789012345678901234567890123456789012345" };
  for(const char *h : hosts) {
    TestSession s;
    _Tt_string addr = (std::string("01 1234 100001 1 1 1000 ") + h +
		       " 4").c_str();
    s.parsed_address(addr);
    CHECK(s.hostaddr().len() <= 16);
  }
}

static void
test_session_address_short(void)
{
  init_globals();
  const char *shorts[] = { "1", "01", "0" };
  for(const char *a : shorts) {
    TestSession s;
    _Tt_string addr = a;
    Tt_status st = s.parsed_address(addr);
    CHECK(st == TT_ERR_INVALID || st == TT_ERR_NO_MATCH);
  }
}

static void
test_session_address_valid_and_level(void)
{
  init_globals();
  {
    TestSession s;
    _Tt_string addr = "01 1234 100001 1 1 1000 127.0.0.1 4";
    CHECK(s.parsed_address(addr) == TT_OK);
    CHECK(!strcmp((char *)s.hostaddr(), "127.0.0.1"));
    CHECK(s.auth_level() == _TT_AUTH_ICEAUTH);
    CHECK(s.rpc_program() == 100001);
    CHECK(s.rpc_version() == 4);
  }
  /* an authentication level that is none of the levels */
  const char *bad[] = { "01 1234 100001 1 7 1000 127.0.0.1 4",
			"01 1234 100001 1 4 1000 127.0.0.1 4",
			"01 1234 100001 1 -1 1000 127.0.0.1 4" };
  for(const char *b : bad) {
    TestSession s;
    _Tt_string addr = b;
    CHECK(s.parsed_address(addr) == TT_ERR_INVALID);
  }
}

static void
test_session_set_id_long_host(void)
{
  init_globals();
  /* "X <host> <number>": the host is not kept, so its length is free */
  for(int n : { 10, 63, 64, 65, 200, 100000 }) {
    TestSession s;
    std::string id = "X " + std::string(n, 'h') + " 7";
    CHECK(s.set_id((char *)id.c_str()) == TT_OK);
  }
  TestSession bad;
  CHECK(bad.set_id((char *)"X onlyhost") == TT_ERR_SESSION);
  CHECK(bad.set_id((char *)"X") == TT_ERR_SESSION);
  CHECK(bad.set_id((char *)"Y 1 2") == TT_ERR_SESSION);
}

/* ---- the cookie ----------------------------------------------------- */

static std::string tmp_dir;

static std::string
make_tmp_dir(void)
{
  const char *base = getenv("TT_DECODE_TEST_DIR");
  std::string dir = std::string(base ? base : ".") + "/case-" +
		    std::to_string((long)getpid());
  mkdir(dir.c_str(), 0700);
  return dir;
}

static void
test_cookie_from_random_generator(void)
{
  init_globals();
  /* 16 bytes, three of them zero, and the bytes that replace those */
  std::string first("ab\0cd\0efghijkl\0mn", 16);
  random_feed = first + "XYZ";
  random_mode = RANDOM_FEED;
  char *cookie = _tt_GenerateMagicCookie(16);
  random_mode = RANDOM_REAL;
  CHECK(random_opens == 1);
  CHECK(cookie != 0);
  if(cookie) {
    CHECK(strlen(cookie) == 16);
    CHECK(!strcmp(cookie, "abXcdYefghijklZm"));
    free(cookie);
  }
}

static void
test_cookie_fails_closed(void)
{
  init_globals();
  random_mode = RANDOM_FAIL;
  CHECK(_tt_GenerateMagicCookie(16) == 0);
  /* the generator ends before the cookie does */
  random_feed = "0123456789";
  random_mode = RANDOM_FEED;
  CHECK(_tt_GenerateMagicCookie(16) == 0);
  /* and one that only returns zeros is no generator */
  random_feed = std::string(100, '\0');
  CHECK(_tt_GenerateMagicCookie(16) == 0);
  random_mode = RANDOM_REAL;
}

static void
test_cookie_real_generator(void)
{
  init_globals();
  char *a = _tt_GenerateMagicCookie(16);
  char *b = _tt_GenerateMagicCookie(16);
  CHECK(a != 0 && b != 0);
  if(a && b) {
    CHECK(strlen(a) == 16 && strlen(b) == 16);
    CHECK(memcmp(a, b, 16) != 0);
  }
  free(a);
  free(b);
}

static void
test_cookie_session_start_refused(void)
{
  init_globals();
  std::string dir = make_tmp_dir();
  std::string authfile = dir + "/TTauthority";
  setenv("TTAUTHORITY", authfile.c_str(), 1);

  _Tt_auth auth;
  auth.set_sessionid(100099, _TT_AUTH_ICEAUTH, "127.0.0.1", 4);
  random_mode = RANDOM_FAIL;
  Tt_status st = auth.generate_auth_cookie();
  random_mode = RANDOM_REAL;
  CHECK(st == TT_ERR_INTERNAL);
  CHECK(auth.auth_cookie().len() == 0);
  struct stat sb;
  CHECK(stat(authfile.c_str(), &sb) != 0);

  /* with a generator: the cookie is stored and read back whole */
  random_feed = std::string("\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) +
		"ABCDEFGHIJKLMNOP";
  random_mode = RANDOM_FEED;
  _Tt_auth other;
  other.set_sessionid(100099, _TT_AUTH_ICEAUTH, "127.0.0.1", 4);
  st = other.generate_auth_cookie();
  random_mode = RANDOM_REAL;
  CHECK(st == TT_OK);
  CHECK(!strcmp((char *)other.auth_cookie(), "ABCDEFGHIJKLMNOP"));
  _Tt_auth reader;
  reader.set_sessionid(100099, _TT_AUTH_ICEAUTH, "127.0.0.1", 4);
  CHECK(reader.retrieve_auth_cookie() == TT_OK);
  CHECK(reader.auth_cookie() == other.auth_cookie());
}

/* ---- RPC requests ------------------------------------------------------- */

struct Reply {
  int stat;			/* accepted_stat, or auth_stat when denied */
  bool denied;
  std::string results;		/* an encoded SUCCESS result */
};

static std::vector<Reply> replies;
static XDR request_xdrs;
static Stream request_stream;
static std::string request_record;

static bool_t
fake_recv(SVCXPRT *, struct rpc_msg *)
{
  return FALSE;
}

static enum xprt_stat
fake_stat(SVCXPRT *)
{
  return XPRT_IDLE;
}

static bool_t
fake_getargs(SVCXPRT *, xdrproc_t proc, void *args)
{
  return (*proc)(&request_xdrs, args);
}

static bool_t
fake_reply(SVCXPRT *, struct rpc_msg *msg)
{
  Reply r;
  r.denied = msg->rm_reply.rp_stat == MSG_DENIED;
  r.stat = 0;
  if(r.denied) {
    r.stat = (int)msg->rjcted_rply.rj_why;
  } else {
    r.stat = (int)msg->acpted_rply.ar_stat;
    if(msg->acpted_rply.ar_stat == SUCCESS &&
       msg->acpted_rply.ar_results.proc) {
      char out[1024];
      XDR enc;
      xdrmem_create(&enc, out, sizeof out, XDR_ENCODE);
      if((*msg->acpted_rply.ar_results.proc)(&enc,
					     msg->acpted_rply.ar_results.where))
	r.results.assign(out, xdr_getpos(&enc));
    }
  }
  replies.push_back(r);
  return TRUE;
}

static bool_t
fake_freeargs(SVCXPRT *, xdrproc_t, void *)
{
  return TRUE;
}

static void
fake_destroy(SVCXPRT *)
{
}

static const struct __rpc_svcxprt::xp_ops fake_ops = {
  fake_recv, fake_stat, fake_getargs, fake_reply, fake_freeargs, fake_destroy
};

/* The session whose cookie the requests have to carry. */
class CookieSession : public _Tt_s_session {
public:
  Tt_status give_cookie(const char *cookie) {
    random_feed = cookie;
    random_mode = RANDOM_FEED;
    _auth.set_sessionid(100099, _TT_AUTH_ICEAUTH, "127.0.0.1", 4);
    Tt_status st = _auth.generate_auth_cookie();
    random_mode = RANDOM_REAL;
    return st;
  }
};

static const char *cookie_text = "0123456789abcdef";

static void
init_server(bool with_cookie = true)
{
  init_globals();
  static std::string dir = make_tmp_dir();
  setenv("TTAUTHORITY", (dir + "/TTauthority").c_str(), 1);
  CookieSession *s = new CookieSession;
  session_cookie.clear();
  if(with_cookie) {
    CHECK(s->give_cookie(cookie_text) == TT_OK);
    _Tt_string c = s->auth_cookie();
    CHECK(c.len() > 0);
    if(c.len() > 0)
      session_cookie.assign((char *)c, c.len());
  }
  _tt_s_mp->initial_s_session = s;
  _tt_s_mp->initial_session = s;
  CHECK(_tt_s_mp->initial_s_session->auth_level() == _TT_AUTH_ICEAUTH);
}

/* Runs one request through the production dispatcher. */
static void
serve(int proc, int flavor, Body &body, int version = TT_RPC_VERSION)
{
  replies.clear();
  /* the body arrives as ttsession gets it from a client: a record stream */
  uint32_t mark = htonl(0x80000000u | body.size());
  request_record.assign((const char *)&mark, 4);
  request_record.append(body.data(), body.size());
  request_stream.data = request_record.data();
  request_stream.size = (int)request_record.size();
  request_stream.pos = 0;
  xdrrec_create(&request_xdrs, 4000, 4000, (char *)&request_stream,
		(record_io)stream_read, (record_io)stream_write);
  request_xdrs.x_op = XDR_DECODE;
  xdrrec_skiprecord(&request_xdrs);
  SVCXPRT xprt;
  memset(&xprt, 0, sizeof xprt);
  xprt.xp_ops = &fake_ops;
  struct svc_req rq;
  memset(&rq, 0, sizeof rq);
  rq.rq_prog = 100099;
  rq.rq_vers = version;
  rq.rq_proc = proc;
  rq.rq_cred.oa_flavor = (enum_t)flavor;
  authunix_parms cred;
  memset(&cred, 0, sizeof cred);
  cred.aup_uid = geteuid();
  rq.rq_clntcred = (caddr_t)&cred;
  rq.rq_xprt = &xprt;
  _tt_service_rpc(&rq, &xprt);
  xdr_destroy(&request_xdrs);
}

static void
test_rpc_cookie_checks(void)
{
  init_server();
  /* the right cookie gets the NULL procedure answered */
  Body ok;
  ok.cookie();
  serve(NULLPROC, AUTH_NULL, ok);
  CHECK(replies.size() == 1 && !replies[0].denied && replies[0].stat == SUCCESS);

  /* a wrong one is refused */
  Body wrong;
  std::string other = session_cookie;
  other[other.size() - 1] ^= 1;
  wrong.str(other.data(), (int)other.size());
  serve(NULLPROC, AUTH_NULL, wrong);
  CHECK(replies.size() == 1 && replies[0].denied && replies[0].stat == AUTH_BADCRED);

  /* so is none: a null string, an empty one, and no body at all */
  Body null_cookie;
  null_cookie.str(0);
  serve(NULLPROC, AUTH_NULL, null_cookie);
  CHECK(replies.size() == 1 && replies[0].denied);
  Body empty_cookie;
  empty_cookie.str("");
  serve(NULLPROC, AUTH_NULL, empty_cookie);
  CHECK(replies.size() == 1 && replies[0].denied);
  Body nothing;
  serve(NULLPROC, AUTH_NULL, nothing);
  CHECK(replies.size() == 1 && replies[0].denied);

  /* credentials of the unix kind are too weak for a cookie session */
  Body unix_cred;
  unix_cred.cookie();
  serve(NULLPROC, AUTH_UNIX, unix_cred);
  CHECK(replies.size() == 1 && replies[0].denied && replies[0].stat == AUTH_TOOWEAK);
}

/*
 * The cookie is decoded before it is checked, so what a client with no
 * credentials sends is decoded.
 */
static void
test_rpc_cookie_hostile_lengths(void)
{
  init_server();
  const uint32_t lengths[] = { 0x7fffffffu, 0x7ffffff0u, 0x80000000u,
			       0xffffffffu, 0xfffffffbu, 1u << 24 };
  for(uint32_t len : lengths) {
    Body b;
    b.i((int)len);
    b.raw("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 32);
    serve(NULLPROC, AUTH_NULL, b);
    CHECK(replies.size() == 1 && replies[0].denied);
  }
}

/* A session without a cookie authenticates no one. */
static void
test_rpc_no_session_cookie(void)
{
  init_server(false);
  CHECK(_tt_s_mp->initial_s_session->auth_cookie().len() == 0);
  Body empty_cookie;
  empty_cookie.str("");
  serve(NULLPROC, AUTH_NULL, empty_cookie);
  CHECK(replies.size() == 1 && replies[0].denied);
  Body null_cookie;
  null_cookie.str(0);
  serve(NULLPROC, AUTH_NULL, null_cookie);
  CHECK(replies.size() == 1 && replies[0].denied);
}

struct Proc {
  const char *name;
  int number;
  bool more_after_procid;	/* arguments follow the procid */
};

#define PROC(n) { #n, n, false }
#define PROC_MORE(n) { #n, n, true }

/* The procedures whose first argument is an object that may not be null. */
static const Proc object_procs[] = {
  PROC(TT_RPC_DISPATCH), PROC(TT_RPC_DISPATCH_2),
  PROC(TT_RPC_DISPATCH_ON_EXIT), PROC(TT_RPC_HDISPATCH),
  PROC(TT_RPC_UPDATE_MSG_2),
  PROC_MORE(TT_RPC_JOIN_FILE), PROC_MORE(TT_RPC_QUIT_FILE),
  PROC(TT_RPC_JOIN_SESSION), PROC(TT_RPC_QUIT_SESSION),
  PROC(TT_RPC_NEXT_MESSAGE),
  PROC_MORE(TT_RPC_ADD_PATTERN), PROC_MORE(TT_RPC_DEL_PATTERN),
  PROC_MORE(TT_RPC_DECLARE_PTYPE), PROC_MORE(TT_RPC_UNDECLARE_PTYPE),
  PROC_MORE(TT_RPC_EXISTS_PTYPE), PROC_MORE(TT_RPC_UNBLOCK_PTYPE),
  PROC_MORE(TT_RPC_SET_FD_CHANNEL), PROC(TT_RPC_CLOSE_PROCID),
  PROC_MORE(TT_RPC_JOIN_CONTEXT), PROC_MORE(TT_RPC_QUIT_CONTEXT),
  PROC(TT_RPC_HUPDATE_MSG),
};

static const Proc *
find_proc(const char *name)
{
  for(const Proc &p : object_procs)
    if(!strcmp(p.name, name))
      return &p;
  return 0;
}

/*
 * The encoding of an object may say "null"; a request that needs the object
 * is refused, and ttsession goes on.
 */
static void
test_rpc_null_object(const char *name)
{
  const Proc *p = find_proc(name);
  CHECK(p != 0);
  if(!p)
    return;
  init_server();
  /* the update procedures take the new state first, the others the object */
  Body b;
  b.cookie();
  if(p->number == TT_RPC_UPDATE_MSG_2 || p->number == TT_RPC_HUPDATE_MSG)
    b.i((int)TT_HANDLED);
  /* the object is null, and so is whatever else a procedure reads next */
  for(int n = 0; n < 8; n++)
    b.i(1);
  serve(p->number, AUTH_NULL, b);
  CHECK(replies.size() == 1);
  if(replies.size() == 1) {
    CHECK(!replies[0].denied);
    CHECK(replies[0].stat == GARBAGE_ARGS);
  }
  /* the server is still in one piece, with the buffer of updates */
  CHECK(!_tt_s_mp->update_args.message.is_null());
  Body ping;
  ping.cookie();
  serve(NULLPROC, AUTH_NULL, ping);
  CHECK(replies.size() == 1 && replies[0].stat == SUCCESS);
}

/* a procid that is there, then a body that ends */
static void
test_rpc_truncated_body(const char *name)
{
  const Proc *p = find_proc(name);
  CHECK(p != 0);
  if(!p)
    return;
  init_server();
  Body b;
  b.cookie();
  b.procid("nobody", "");
  /* ... and nothing of what the procedure needs next */
  serve(p->number, AUTH_NULL, b);
  /* one answer, and it says the arguments were bad */
  CHECK(replies.size() == 1);
  if(!replies.empty())
    CHECK(replies[0].stat == GARBAGE_ARGS);
}

static void
test_rpc_load_types_null_database(void)
{
  init_server();
  _Tt_typedb_ptr tdb = new _Tt_typedb();
  _tt_s_mp->tdb = tdb;
  Body b;
  b.cookie();
  /* the types: an XDR version, then a null database */
  Body types;
  types.i(TT_XDR_VERSION);
  types.i(1);
  std::string blob(types.data(), types.size());
  b.str(blob.data(), (int)blob.size());
  serve(TT_RPC_LOAD_TYPES, AUTH_NULL, b);
  CHECK(replies.size() == 1);
  CHECK(!_tt_s_mp->tdb.is_null());
  CHECK(_tt_s_mp->tdb.is_eq(tdb));
  if(replies.size() == 1 && replies[0].results.size() == 4) {
    XDR x;
    int status = TT_OK;
    xdrmem_create(&x, (char *)replies[0].results.data(), 4, XDR_DECODE);
    xdr_int(&x, &status);
    CHECK(status != TT_OK);
  } else {
    CHECK(0);
  }
}

static _Tt_string_list_ptr
parents_of(const char *parent)
{
  _Tt_string_list_ptr l = new _Tt_string_list();
  if(parent)
    l->append(_Tt_string(parent));
  return l;
}

static void
add_otype(const char *id, const char *parent)
{
  _Tt_otype_ptr o = new _Tt_otype(_Tt_string(id));
  o->set_ancestors(parents_of(parent));
  _tt_s_mp->otable->insert(o);
}

static void
test_otype_children(void)
{
  init_server();
  add_otype("Base", 0);
  add_otype("Mid", "Base");
  add_otype("Leaf", "Mid");
  add_otype("Orphan", "NoSuchParent");
  _Tt_otype_ptr base = _tt_s_mp->otable->lookup(_Tt_string("Base"));
  /* a type whose parent is not in the table does not stop the search */
  _Tt_string_list_ptr kids = base->children();
  CHECK(!kids.is_null());
  CHECK(kids->count() == 1);
  if(kids->count() == 1)
  {
    _Tt_string first = (*kids)[0];
    CHECK(first == _Tt_string("Mid"));
  }
}

static void
test_rpc_otype_is_derived_cycle(void)
{
  init_server();
  add_otype("A", "B");
  add_otype("B", "A");
  Body b;
  b.cookie();
  b.str("A");			/* derived */
  b.str("Z");			/* base */
  b.i(0);
  b.i(0);
  serve(TT_RPC_OTYPE_IS_DERIVED, AUTH_NULL, b);
  CHECK(replies.size() == 1 && replies[0].stat == SUCCESS);
}

/* ---- main --------------------------------------------------------------- */

struct Case {
  std::string name;
  void (*plain)(void);
  void (*with_arg)(const char *);
  const char *arg;
};

static std::vector<Case> cases;

static void
add(const char *name, void (*f)(void))
{
  cases.push_back({ name, f, 0, 0 });
}

static void
add_arg(const char *base, const char *arg, void (*f)(const char *))
{
  cases.push_back({ std::string(base) + ":" + arg, 0, f, arg });
}

int
main(int argc, char **argv)
{
  add("string_v2_huge_length", test_string_v2_huge_length);
  add("string_v2_length_beyond_data", test_string_v2_length_beyond_data_leaves_nothing);
  add("string_v2_negative_length", test_string_v2_negative_length);
  add("string_v2_roundtrip", test_string_v2_roundtrip);
  add("string_v1_huge_length", test_string_v1_huge_length);
  add("string_v1_lengths_disagree", test_string_v1_lengths_disagree);
  add("string_v1_negative_length", test_string_v1_negative_length);
  add("string_v1_roundtrip", test_string_v1_roundtrip);
  add("cstring_list_bad_counts", test_cstring_list_bad_counts);
  add("bstring_negative_length", test_bstring_negative_length);
  add("session_address_long_host", test_session_address_long_host);
  add("session_address_short", test_session_address_short);
  add("session_address_valid_and_level", test_session_address_valid_and_level);
  add("session_set_id_long_host", test_session_set_id_long_host);
  add("cookie_from_random_generator", test_cookie_from_random_generator);
  add("cookie_fails_closed", test_cookie_fails_closed);
  add("cookie_real_generator", test_cookie_real_generator);
  add("cookie_session_start_refused", test_cookie_session_start_refused);
  add("rpc_cookie_checks", test_rpc_cookie_checks);
  add("rpc_cookie_hostile_lengths", test_rpc_cookie_hostile_lengths);
  add("rpc_no_session_cookie", test_rpc_no_session_cookie);
  for(const Proc &p : object_procs)
    add_arg("rpc_null_object", p.name, test_rpc_null_object);
  for(const Proc &p : object_procs)
    if(p.more_after_procid)
      add_arg("rpc_truncated_body", p.name, test_rpc_truncated_body);
  add("rpc_load_types_null_database", test_rpc_load_types_null_database);
  add("otype_children", test_otype_children);
  add("rpc_otype_is_derived_cycle", test_rpc_otype_is_derived_cycle);

  if(argc == 2 && !strcmp(argv[1], "--list")) {
    for(const Case &c : cases)
      printf("%s\n", c.name.c_str());
    return 0;
  }
  if(argc != 2) {
    fprintf(stderr, "usage: %s --list | CASE\n", argv[0]);
    return 2;
  }
  for(const Case &c : cases) {
    if(c.name != argv[1])
      continue;
    if(c.plain)
      c.plain();
    else
      c.with_arg(c.arg);
    return failures ? 1 : 0;
  }
  fprintf(stderr, "unknown case %s\n", argv[1]);
  return 2;
}
