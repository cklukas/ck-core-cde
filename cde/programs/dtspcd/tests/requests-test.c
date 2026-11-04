/*
 * Regression tests for the dtspcd request path: the wire layer in libDtSvc
 * (header and payload limits, dispatch, counted-string parsing, multi-packet
 * reads) and the request handlers of dtspcd, driven with hand-built requests.
 *
 * The program includes the production main.c of dtspcd and links the built
 * libDtSvc, so it exercises the shipped code.  Each case runs in its own
 * process (requests-test CASE) so that a crash in one case is reported for
 * that case alone; check-requests.py runs them all under the sanitizers.
 */
#define main dtspcd_main
#include "../main.c"
#undef main

#include <assert.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/wait.h>

static int failures;

#define CHECK(cond) \
  do { \
    if(!(cond)) { \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++; \
    } \
  } while(0)

/* ---- fixtures ------------------------------------------------------------ */

/* A connection to a socket pair; PEER is the other end. */
static SPC_Connection_Ptr make_connection(int *peer)
{
  int sv[2];
  SPC_Connection_Ptr conn;

  if(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
    perror("socketpair");
    exit(2);
  }
  conn = SPC_Alloc_Connection();
  conn->sid = sv[0];
  conn->connected = TRUE;
  *peer = sv[1];
  return(conn);
}

static void write_all(int fd, const void *data, size_t size)
{
  const char *p = data;

  while(size > 0) {
    ssize_t n = write(fd, p, size);
    if(n <= 0) {
      perror("write");
      exit(2);
    }
    p += n;
    size -= (size_t)n;
  }
}

/* Send one request: a 20 byte header ("%08x%02x%04x%04x" and two blanks)
   followed by the payload.  LENGTH is the value the header claims, which
   tests may make differ from the payload actually sent. */
static void send_request_raw(int fd, unsigned cid, unsigned type,
			     unsigned length, unsigned seq,
			     const void *payload, size_t size)
{
  char header[REQUEST_HEADER_LENGTH + 1];

  snprintf(header, sizeof(header), "%08x%02x%04x%04x  ", cid, type, length, seq);
  write_all(fd, header, REQUEST_HEADER_LENGTH);
  if(size)
    write_all(fd, payload, size);
}

static void send_request(int fd, unsigned cid, unsigned type,
			 const void *payload, size_t size)
{
  send_request_raw(fd, cid, type, (unsigned)size, 1, payload, size);
}

/* A received-request object holding PAYLOAD. */
static protocol_request_ptr make_request(int type, const void *payload,
					 size_t size)
{
  protocol_request_ptr prot = SPC_New_Protocol_Ptr(NULL, (XeChar)type, (int)size);

  memcpy(PDRP(prot->dataptr), payload, size);
  prot->dataptr->data[REQUEST_HEADER_LENGTH + size] = '\0';
  prot->dataptr->len = (int)size;
  prot->dataptr->offset = REQUEST_HEADER_LENGTH;
  return(prot);
}

/* The length of the payload read_reply() last returned. */
static size_t reply_length;

/* Whether the NUL-separated payload contains NEEDLE. */
static int payload_has(const char *payload, size_t length, const char *needle)
{
  size_t n = strlen(needle), i;

  for(i = 0; i + n <= length; i++)
    if(memcmp(payload + i, needle, n) == 0)
      return(TRUE);
  return(FALSE);
}

/* Read one reply request from FD: its type and payload. */
static int read_reply(int fd, int *type, char *payload, size_t capacity)
{
  char header[REQUEST_HEADER_LENGTH + 1];
  unsigned cid, t, length, seq;
  size_t got = 0;

  while(got < REQUEST_HEADER_LENGTH) {
    ssize_t n = read(fd, header + got, REQUEST_HEADER_LENGTH - got);
    if(n <= 0)
      return(FALSE);
    got += (size_t)n;
  }
  header[REQUEST_HEADER_LENGTH] = '\0';
  if(sscanf(header, "%8x%2x%4x%4x", &cid, &t, &length, &seq) != 4 ||
     length >= capacity)
    return(FALSE);
  got = 0;
  while(got < length) {
    ssize_t n = read(fd, payload + got, length - got);
    if(n <= 0)
      return(FALSE);
    got += (size_t)n;
  }
  payload[length] = '\0';
  reply_length = length;
  *type = (int)t;
  if(getenv("SPC_TEST_DUMP")) {
    fprintf(stderr, "reply type %u, %u byte(s):", t, length);
    for(got = 0; got < length; got++)
      fprintf(stderr, " %02x", (unsigned char)payload[got]);
    fprintf(stderr, "\n");
  }
  return(TRUE);
}

/* ---- wire layer ------------------------------------------------------------ */

/* A payload of exactly SPC_BUFSIZ bytes is the largest a request may carry. */
static void case_max_payload(void)
{
  int peer;
  SPC_Connection_Ptr conn = make_connection(&peer);
  char payload[SPC_BUFSIZ];
  protocol_request_ptr prot;

  memset(payload, 'A', sizeof(payload));
  send_request(peer, 0, APPLICATION_DATA, payload, sizeof(payload));
  prot = SPC_Read_Protocol(conn);
  CHECK(prot != NULL);
  if(prot) {
    CHECK(prot->dataptr->len == SPC_BUFSIZ);
    CHECK(prot->dataptr->offset == REQUEST_HEADER_LENGTH);
    CHECK(prot->dataptr->data[REQUEST_HEADER_LENGTH + SPC_BUFSIZ - 1] == 'A');
    CHECK(prot->dataptr->data[REQUEST_HEADER_LENGTH + SPC_BUFSIZ] == '\0');
  }
}

/* A longer payload does not fit the request buffer and ends the connection. */
static void oversize(size_t size)
{
  int peer;
  SPC_Connection_Ptr conn = make_connection(&peer);
  char *payload = malloc(size);
  protocol_request_ptr prot;

  memset(payload, 'B', size);
  if(size >= SPC_BUFSIZ + sizeof(int)) {
    /* The request buffer used to end 20 bytes short of the payload it let
       through, and its length field followed.  Writing the request's own
       length there makes the damage invisible to a length comparison. */
    int forged = (int)size;
    memcpy(payload + SPC_BUFSIZ, &forged, sizeof(forged));
  }
  send_request(peer, 0, APPLICATION_DATA, payload, size);
  prot = SPC_Read_Protocol(conn);
  CHECK(prot == NULL);
  free(payload);
}

static void case_oversize_payload(void) { oversize(SPC_BUFSIZ + 4); }
/* The largest length the old check, against MAXREQLEN, let through. */
static void case_oversize_largest_old(void) { oversize(MAXREQLEN - 1); }

static void bad_header(const char *header20, const char *what)
{
  int peer;
  SPC_Connection_Ptr conn = make_connection(&peer);
  protocol_request_ptr prot;

  write_all(peer, header20, REQUEST_HEADER_LENGTH);
  prot = SPC_Read_Protocol(conn);
  if(prot != NULL)
    fprintf(stderr, "a header with %s was accepted\n", what);
  CHECK(prot == NULL);
}

static void case_bad_header(void)
{
  bad_header("zzzzzzzzzzzzzzzzzz  ", "no hexadecimal digits");
  bad_header("00000000 0 0000000  ", "a blank inside a field");
  bad_header("0000000000-1000000  ", "a sign inside the length field");
}

static void case_bad_type(void)
{
  char header[REQUEST_HEADER_LENGTH + 1];

  snprintf(header, sizeof(header), "%08x%02x%04x%04x  ", 0u, (unsigned)NREQS, 0u, 1u);
  bad_header(header, "a request type one past the last");
  snprintf(header, sizeof(header), "%08x%02x%04x%04x  ", 0u, 0xffu, 0u, 1u);
  bad_header(header, "request type 255");
}

static int handler_calls;
static int counting_handler(protocol_request_ptr prot) { (void)prot; handler_calls++; return(TRUE); }

/* The table has NREQS entries; some types have no handler. */
static void case_dispatch_edges(void)
{
  protocol_request_handler *table = calloc(NREQS, sizeof(*table));
  protocol_request_ptr prot = make_request(0, "", 0);

  table[APPLICATION_DATA] = counting_handler;

  prot->request_type = APPLICATION_DATA;
  CHECK(SPC_Dispatch_Protocol(prot, table) == TRUE && handler_calls == 1);

  prot->request_type = NREQS;			/* one past the table */
  CHECK(SPC_Dispatch_Protocol(prot, table) == FALSE);
  prot->request_type = -1;
  CHECK(SPC_Dispatch_Protocol(prot, table) == FALSE);
  prot->request_type = APPLICATION_STDOUT;	/* a type without a handler */
  CHECK(SPC_Dispatch_Protocol(prot, table) == FALSE);
  CHECK(handler_calls == 1);
}

/* ---- counted strings --------------------------------------------------------- */

#define PAYLOAD(lit) lit, sizeof(lit) - 1

static int read_register(const void *payload, size_t size, XeString *user,
			 XeString *pass, XeString *ver, XeString *host)
{
  protocol_request_ptr prot = make_request(REGISTER, payload, size);
  XeString username = NULL, passwd = NULL, proto_ver = NULL, hostinfo = NULL;
  int ok = READ_REGISTER(prot->dataptr, username, passwd, proto_ver, hostinfo);

  *user = username; *pass = passwd; *ver = proto_ver; *host = hostinfo;
  return(ok);
}

static void case_register_valid(void)
{
  XeString user, pass, ver, host;

  /* two strings: a pre-A.02 client */
  CHECK(read_register(PAYLOAD("2 \0alice\0\0"), &user, &pass, &ver, &host) == TRUE);
  CHECK(user && strcmp(user, "alice") == 0);
  CHECK(pass && strcmp(pass, "") == 0);
  CHECK(ver == NULL);
  CHECK(host && strcmp(host, SPC_UNKNOWN_HOSTINFO_STR) == 0);

  /* the NUL stored after the payload ends the last string, as a sender that
     leaves out its own terminator relies on */
  CHECK(read_register(PAYLOAD("2 \0alice\0pw"), &user, &pass, &ver, &host) == TRUE);
  CHECK(user && strcmp(user, "alice") == 0 && pass && strcmp(pass, "pw") == 0);

  /* four strings */
  CHECK(read_register(PAYLOAD("4 \0bob\0pw\0" "3\0h:s:r:m\0"), &user, &pass, &ver, &host) == TRUE);
  CHECK(user && strcmp(user, "bob") == 0);
  CHECK(pass && strcmp(pass, "pw") == 0);
  CHECK(ver && strcmp(ver, "3") == 0);
  CHECK(host && strcmp(host, "h:s:r:m") == 0);
}

static void expect_register_rejected(const void *payload, size_t size, const char *what)
{
  XeString user, pass, ver, host;
  int ok = read_register(payload, size, &user, &pass, &ver, &host);

  if(ok == TRUE)
    fprintf(stderr, "a REGISTER payload with %s was accepted\n", what);
  CHECK(ok != TRUE);
}

static void case_register_malformed(void)
{
  expect_register_rejected(PAYLOAD("0 \0"), "no strings");
  expect_register_rejected(PAYLOAD("1 \0alice\0"), "one string of the two required");
  expect_register_rejected(PAYLOAD("4 \0alice\0\0"), "a count above the strings present");
  expect_register_rejected(PAYLOAD("ffffffff \0"), "a count of 0xffffffff");
  expect_register_rejected(PAYLOAD("zz \0a\0b\0"), "a count that is not hexadecimal");
  expect_register_rejected(PAYLOAD("2 x\0a\0b\0"), "text after the count");
  expect_register_rejected("", 0, "no payload");
}

static void case_device_and_logfile(void)
{
  protocol_request_ptr prot;
  XeString m0, s0, m1, s1, m2, s2, logfile, ver, host;

  prot = make_request(DEVICE_REPLY, PAYLOAD("6 \0a\0b\0c\0d\0e\0f\0"));
  CHECK(READ_DEVICE_REPLY(prot->dataptr, &m0, &s0, &m1, &s1, &m2, &s2) == TRUE);
  CHECK(m0 && s2 && strcmp(m0, "a") == 0 && strcmp(s2, "f") == 0);

  prot = make_request(DEVICE_REPLY, PAYLOAD("5 \0a\0b\0c\0d\0e\0"));
  CHECK(READ_DEVICE_REPLY(prot->dataptr, &m0, &s0, &m1, &s1, &m2, &s2) != TRUE);

  prot = make_request(LOGFILE_REPLY, PAYLOAD("1 \0/tmp/x\0"));
  CHECK(READ_LOGFILE_REPLY(prot->dataptr, &logfile, &ver, &host) == TRUE);
  CHECK(logfile && strcmp(logfile, "/tmp/x") == 0 && ver == NULL);
  CHECK(host && strcmp(host, SPC_UNKNOWN_HOSTINFO_STR) == 0);

  prot = make_request(LOGFILE_REPLY, PAYLOAD("3 \0/tmp/x\0" "2\0hh\0"));
  CHECK(READ_LOGFILE_REPLY(prot->dataptr, &logfile, &ver, &host) == TRUE);
  CHECK(ver && strcmp(ver, "2") == 0 && host && strcmp(host, "hh") == 0);

  prot = make_request(LOGFILE_REPLY, PAYLOAD("0 \0"));
  CHECK(READ_LOGFILE_REPLY(prot->dataptr, &logfile, &ver, &host) != TRUE);
}

/* ---- request handlers ---------------------------------------------------------- */

static int connect_server(int *peer)
{
  client_connection = make_connection(peer);
  client_channel.connection = client_connection;
  client_validated = 1;
  return(TRUE);
}

/* The environment strings of a multi-packet request arrive in continuation
   packets of the same type. */
static void case_environ_multipacket(void)
{
  int peer;
  protocol_request_ptr prot;

  connect_server(&peer);
  prot = make_request(ENVIRON_RESET, PAYLOAD("3 SPC_TEST_A=1\0SPC_TEST_B=2\0\0"));
  send_request(peer, 0, ENVIRON_RESET, PAYLOAD("SPC_TEST_C=3\0\0"));
  CHECK(Client_Environ_Reset(prot) == TRUE);
  CHECK(getenv("SPC_TEST_A") && strcmp(getenv("SPC_TEST_A"), "1") == 0);
  CHECK(getenv("SPC_TEST_B") && strcmp(getenv("SPC_TEST_B"), "2") == 0);
  CHECK(getenv("SPC_TEST_C") && strcmp(getenv("SPC_TEST_C"), "3") == 0);
}

/* A negative count must not index before the array that is filled. */
static void case_environ_negative_count(void)
{
  int peer;
  protocol_request_ptr prot;

  connect_server(&peer);
  prot = make_request(ENVIRON_RESET, PAYLOAD("-5000 "));
  CHECK(Client_Environ_Reset(prot) != TRUE);
}

static void case_environ_huge_count(void)
{
  int peer;
  protocol_request_ptr prot;

  connect_server(&peer);
  prot = make_request(ENVIRON_RESET, PAYLOAD("2147483647 SPC_TEST_D=4\0\0"));
  CHECK(Client_Environ_Reset(prot) != TRUE);
}

/* A peer that never supplies the strings it announced cannot keep the daemon
   reading empty packets. */
static void case_environ_empty_packets(void)
{
  int peer, i;
  protocol_request_ptr prot;

  connect_server(&peer);
  prot = make_request(ENVIRON_RESET, PAYLOAD("2 SPC_TEST_E=5\0\0"));
  for(i = 0; i < 8; i++)
    send_request(peer, 0, ENVIRON_RESET, PAYLOAD("\0"));
  CHECK(Client_Environ_Reset(prot) != TRUE);
}

/* The counts in an enhanced spawn name positions in the string vector. */
static void case_spawn_bad_counts(void)
{
  int peer;
  protocol_request_ptr prot;
  SPC_Channel_Ptr channel;

  connect_server(&peer);
  channel = XeSPCOpen(NULL, SPCIO_NOIO);
  CHECK(channel != NULL);
  prot = make_request(APP_B00_SPAWN,
		      PAYLOAD("5 /bin/true\0/\0" "1000000\0" "1\0x\0"));
  prot->channel = channel;
  CHECK(Client_Enhanced_Spawn(prot) != TRUE);
  prot = make_request(APP_B00_SPAWN, PAYLOAD("2 /bin/true\0/\0"));
  prot->channel = channel;
  CHECK(Client_Enhanced_Spawn(prot) != TRUE);
}

/* An encoded termios string longer than the handler's buffer. */
static void case_termios_overflow(void)
{
  int peer;
  char payload[3200];
  protocol_request_ptr prot;

  connect_server(&peer);
  memset(payload, 'A', sizeof(payload));
  memcpy(payload, "0 0 ", 4);
  prot = make_request(CHANNEL_TERMIOS, payload, sizeof(payload));
  CHECK(Client_Channel_Termios(prot) != TRUE);
}

/* A debug file name as long as the request allows. */
static void case_debug_name_limit(void)
{
  int peer;
  char payload[SPC_BUFSIZ];
  protocol_request_ptr prot;

  connect_server(&peer);
  memset(payload, 'x', sizeof(payload));
  prot = make_request(SERVER_DEBUG, payload, sizeof(payload));
  CHECK(Client_Server_Debug(prot) != TRUE);
}

/* A request that names a channel the client does not have. */
static void case_unknown_channel(void)
{
  int peer, type = -1;
  char reply[256];
  unsigned retval = 0, errval = 0;

  connect_server(&peer);
  send_request(peer, 0x12345, CHANNEL_CLOSE, "", 0);
  SPCD_Handle_Client_Data(&client_channel, NULL, NULL);
  CHECK(read_reply(peer, &type, reply, sizeof(reply)) == TRUE);
  CHECK(type == REPLY);
  CHECK(sscanf(reply, "%x %x", &retval, &errval) == 2);
  CHECK((int)retval < 0);
}

/* A REGISTER without the strings it must carry is refused. */
static void case_register_refused(void)
{
  int peer, type = -1;
  char reply[256];

  connect_server(&peer);
  client_validated = 0;
  send_request(peer, 0, REGISTER, PAYLOAD("0 \0"));
  SPCD_Handle_Client_Data(&client_channel, NULL, NULL);
  CHECK(read_reply(peer, &type, reply, sizeof(reply)) == TRUE);
  CHECK(type == LOGFILE_REPLY);
  CHECK(payload_has(reply, reply_length, "<!failed>!"));
  CHECK(client_validated == 0);

  /* An empty user name is refused, and that error ends the daemon (exit 3,
     as SPC_Error does for a fatal error), so it runs in a child process.  The
     refusal reaches the client before the daemon exits. */
  {
    pid_t child;
    int status = 0;

    send_request(peer, 0, REGISTER, PAYLOAD("2 \0\0\0"));
    child = fork();
    if(child == 0) {
      SPCD_Handle_Client_Data(&client_channel, NULL, NULL);
      _exit(0);
    }
    CHECK(child > 0 && waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 3);
    CHECK(read_reply(peer, &type, reply, sizeof(reply)) == TRUE);
    CHECK(type == LOGFILE_REPLY && payload_has(reply, reply_length, "<!failed>!"));
  }
  CHECK(client_validated == 0);
}

#ifdef NEW_REGISTER_HELPERS
static void case_authentication_suffix(void)
{
  char a[17], b[17];
  int i;

  CHECK(Authentication_Suffix(a, sizeof(a)) == TRUE);
  CHECK(Authentication_Suffix(b, sizeof(b)) == TRUE);
  CHECK(strlen(a) == 16 && strlen(b) == 16);
  for(i = 0; i < 16; i++)
    CHECK(isxdigit((unsigned char)a[i]));
  CHECK(strcmp(a, b) != 0);
  CHECK(Authentication_Suffix(a, 16) == FALSE);	/* too small for 16 digits */
}

/* The daemon stays itself unless it can become the user completely. */
static void case_become_user(void)
{
  struct passwd *me = getpwuid(getuid());
  char name[256];

  CHECK(me != NULL);
  snprintf(name, sizeof(name), "%s", me->pw_name);
  CHECK(Become_User(name, getuid(), getgid()) == TRUE);
  if(geteuid() != 0) {
    /* only root may become someone else: this must be refused, not ignored */
    CHECK(Become_User("nobody", getuid() + 1, getgid()) == FALSE);
    CHECK(Become_User(name, getuid(), getgid() + 1) == FALSE);
  }
}
#endif

/* ---- driver ---------------------------------------------------------------------- */

static const struct {
  const char *name;
  void (*run)(void);
} cases[] = {
  { "max-payload", case_max_payload },
  { "oversize-payload", case_oversize_payload },
  { "oversize-largest-old", case_oversize_largest_old },
  { "bad-header", case_bad_header },
  { "bad-type", case_bad_type },
  { "dispatch-edges", case_dispatch_edges },
  { "register-valid", case_register_valid },
  { "register-malformed", case_register_malformed },
  { "device-and-logfile", case_device_and_logfile },
  { "environ-multipacket", case_environ_multipacket },
  { "environ-negative-count", case_environ_negative_count },
  { "environ-huge-count", case_environ_huge_count },
  { "environ-empty-packets", case_environ_empty_packets },
  { "spawn-bad-counts", case_spawn_bad_counts },
  { "termios-overflow", case_termios_overflow },
  { "debug-name-limit", case_debug_name_limit },
  { "unknown-channel", case_unknown_channel },
  { "register-refused", case_register_refused },
#ifdef NEW_REGISTER_HELPERS
  { "authentication-suffix", case_authentication_suffix },
  { "become-user", case_become_user },
#endif
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
    fprintf(stderr, "usage: requests-test --list | CASE\n");
    return(2);
  }

  SPC_who_am_i = SPC_I_AM_A_DAEMON;
  XeProgName = SPCD_PROG_NAME;
  signal(SIGPIPE, SIG_IGN);

  for(i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    if(strcmp(cases[i].name, argv[1]) == 0) {
      cases[i].run();
      if(failures) {
	fprintf(stderr, "%d check(s) failed in %s\n", failures, argv[1]);
	return(1);
      }
      return(0);
    }
  fprintf(stderr, "no such case: %s\n", argv[1]);
  return(2);
}
