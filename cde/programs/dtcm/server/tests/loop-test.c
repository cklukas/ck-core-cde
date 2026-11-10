/*
 * Regression tests for the service loop of rpc.cmsd (cmsloop.c): the signals
 * that ask for garbage collection, for the debug switch and for the end of
 * the process are only noted by their handlers, and the work is done between
 * two requests.
 *
 * The loop serves a UDP service on the loopback interface that is not
 * registered with rpcbind.  Processes forked by the test play the client
 * and send signals to the server.  Each case runs in its own process
 * (loop-test CASE); check-loop.py runs them under the sanitizers.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>
#include <rpc/rpc.h>

#include "../cmsloop.h"

#define TEST_PROGRAM	0x2ffffff0u
#define TEST_VERSION	1u
#define PROC_WORK	1u
#define PROC_HANGUP	2u

static int failures;

#define CHECK(cond) \
  do { \
    if(!(cond)) { \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++; \
    } \
  } while(0)

/* what the actions and the requests record */
static volatile int busy;
static int requests, garbage_runs, debug_runs, during_request;
static int signals_open_in_action;
static int hangup_in_request_done;

static int signal_is_blocked(int sig)
{
  sigset_t current;

  sigprocmask(SIG_BLOCK, NULL, &current);
  return(sigismember(&current, sig));
}

/* The four signals must be blocked whenever the loop runs a request or an
   action, and the work of a request must not be interrupted by an action. */
static void note_context(void)
{
  if(busy)
    during_request++;
  if(!signal_is_blocked(SIGALRM) || !signal_is_blocked(SIGUSR1) ||
     !signal_is_blocked(SIGUSR2) || !signal_is_blocked(SIGHUP))
    signals_open_in_action++;
}

static void collect_garbage(void)
{
  note_context();
  garbage_runs++;
}

static void switch_debug(void)
{
  note_context();
  debug_runs++;
}

/* Work that takes a while and allocates, as a request does. */
static void work(void)
{
  struct timespec start, now;
  unsigned i = 0;

  clock_gettime(CLOCK_MONOTONIC, &start);
  busy = 1;
  do {
    void *block = malloc(64 + (size_t)(i++ % 7) * 32);

    free(block);
    clock_gettime(CLOCK_MONOTONIC, &now);
  } while((now.tv_sec - start.tv_sec) * 1000000000L +
	  (now.tv_nsec - start.tv_nsec) < 2000000L);
  busy = 0;
}

static void dispatch(struct svc_req *rq, SVCXPRT *transp)
{
  unsigned count;

  switch(rq->rq_proc) {
  case NULLPROC:
    svc_sendreply(transp, (xdrproc_t)xdr_void, NULL);
    break;
  case PROC_WORK:
    work();
    count = (unsigned)++requests;
    svc_sendreply(transp, (xdrproc_t)xdr_u_int, (char *)&count);
    break;
  case PROC_HANGUP:
    /* SIGHUP arrives while the request is processed */
    kill(getpid(), SIGHUP);
    work();
    hangup_in_request_done = 1;
    count = 7;
    svc_sendreply(transp, (xdrproc_t)xdr_u_int, (char *)&count);
    break;
  default:
    svcerr_noproc(transp);
  }
}

static const _DtCmsLoopActions actions = { collect_garbage, switch_debug };

/* The server: returns the port it listens on. */
static unsigned short start_server(void)
{
  SVCXPRT *transp;
  struct sockaddr_in addr;
  socklen_t length = sizeof(addr);
  int sock = socket(AF_INET, SOCK_DGRAM, 0);

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if(bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    perror("bind");
    exit(2);
  }
  getsockname(sock, (struct sockaddr *)&addr, &length);
  transp = svcudp_create(sock);
  if(transp == NULL || !svc_register(transp, TEST_PROGRAM, TEST_VERSION,
				     dispatch, 0)) {
    fprintf(stderr, "cannot create the service\n");
    exit(2);
  }
  return(ntohs(addr.sin_port));
}

/* The client: calls PROC N times; exits with the number of failed calls. */
static int call_server(unsigned short port, unsigned proc, int n)
{
  struct sockaddr_in addr;
  struct timeval wait = { 1, 0 }, total = { 20, 0 };
  int sock = RPC_ANYSOCK, failed = 0;
  CLIENT *client;
  int i;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  client = clntudp_create(&addr, TEST_PROGRAM, TEST_VERSION, wait, &sock);
  if(client == NULL)
    return(-1);
  for(i = 0; i < n; i++) {
    unsigned reply = 0;

    if(clnt_call(client, proc, (xdrproc_t)xdr_void, NULL,
		 (xdrproc_t)xdr_u_int, (char *)&reply, total) != RPC_SUCCESS)
      failed++;
  }
  clnt_destroy(client);
  return(failed);
}

/* Sends the signals SIGALRM, SIGUSR1 and SIGUSR2 to the server PID until it
   is told to stop (by SIGTERM); then a SIGHUP ends the server. */
static pid_t start_signaller(pid_t server, int pause_us)
{
  pid_t child = fork();

  if(child == 0) {
    static const int signals[] = { SIGUSR1, SIGUSR2, SIGALRM };
    int i = 0;

    for(;;) {
      kill(server, signals[i++ % 3]);
      usleep(pause_us);
    }
  }
  return(child);
}

static pid_t start_client(unsigned short port, unsigned proc, int n)
{
  pid_t child = fork();

  if(child == 0)
    _exit(call_server(port, proc, n) == 0 ? 0 : 1);
  return(child);
}

static int wait_for(pid_t child)
{
  int status;

  waitpid(child, &status, 0);
  return(WIFEXITED(status) ? WEXITSTATUS(status) : 128);
}

/* Signals that arrive while requests are served: the actions run, never
   during a request, and every request is answered. */
static void case_signals_between_requests(void)
{
  unsigned short port;
  pid_t client, signaller;
  int rc;

  _DtCmsInstallSignalHandlers();
  port = start_server();

  /* the server must outlive the client: the client ends with a hangup */
  client = fork();
  if(client == 0) {
    int failed;

    signaller = start_signaller(getppid(), 300);
    failed = call_server(port, PROC_WORK, 300);
    kill(signaller, SIGKILL);
    waitpid(signaller, NULL, 0);
    kill(getppid(), SIGHUP);
    _exit(failed == 0 ? 0 : 1);
  }

  rc = _DtCmsRunServiceLoop(&actions);
  CHECK(rc == 0);
  CHECK(wait_for(client) == 0);
  CHECK(requests == 300);
  CHECK(garbage_runs > 0);
  CHECK(debug_runs > 0);
  CHECK(during_request == 0);
  CHECK(signals_open_in_action == 0);
}

/* A signal that arrives while the loop waits is not lost. */
static void case_idle_signals(void)
{
  pid_t child;

  _DtCmsInstallSignalHandlers();
  (void)start_server();

  child = fork();
  if(child == 0) {
    usleep(200000);
    kill(getppid(), SIGUSR1);
    usleep(200000);
    kill(getppid(), SIGUSR2);
    usleep(200000);
    kill(getppid(), SIGHUP);
    _exit(0);
  }
  CHECK(_DtCmsRunServiceLoop(&actions) == 0);
  CHECK(wait_for(child) == 0);
  CHECK(garbage_runs == 1);
  CHECK(debug_runs == 1);
}

/* A request that is processed when SIGHUP arrives is finished and answered
   before the loop returns. */
static void case_hangup_in_request(void)
{
  unsigned short port;
  pid_t client;

  _DtCmsInstallSignalHandlers();
  port = start_server();
  client = start_client(port, PROC_HANGUP, 1);
  CHECK(_DtCmsRunServiceLoop(&actions) == 0);
  CHECK(hangup_in_request_done);
  CHECK(wait_for(client) == 0);
}

/* A client that goes away must not kill the server with SIGPIPE. */
static void case_sigpipe_ignored(void)
{
  struct sigaction current;

  _DtCmsInstallSignalHandlers();
  sigaction(SIGPIPE, NULL, &current);
  CHECK(current.sa_handler == SIG_IGN);
}

/* The mask of the process is as the loop needs it, and what it blocked is
   open in the wait: a signal arrives when it is sent. */
static void case_signals_blocked(void)
{
  sigset_t current;

  _DtCmsInstallSignalHandlers();
  sigprocmask(SIG_BLOCK, NULL, &current);
  CHECK(sigismember(&current, SIGALRM));
  CHECK(sigismember(&current, SIGUSR1));
  CHECK(sigismember(&current, SIGUSR2));
  CHECK(sigismember(&current, SIGHUP));
  CHECK(!sigismember(&current, SIGTERM));
}

static const struct {
  const char *name;
  void (*run)(void);
} cases[] = {
  { "signals-between-requests", case_signals_between_requests },
  { "idle-signals", case_idle_signals },
  { "hangup-in-request", case_hangup_in_request },
  { "sigpipe-ignored", case_sigpipe_ignored },
  { "signals-blocked", case_signals_blocked },
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
