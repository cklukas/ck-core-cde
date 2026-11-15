/* @@PROLOGUE@@ */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/*
 * Stand-ins for the types and functions the extracted code uses.  The lock,
 * error, state and termination stand-ins record whether SIGCHLD is blocked in
 * their calling context: the delivered signal is blocked inside its handler
 * and nowhere else in this program.
 */
typedef char *XeString;
typedef char XeChar;
typedef unsigned long SbInputId;
typedef void (*SbInputCallbackProc)(void *data, int *fd, SbInputId *id);
typedef struct SPC_Channel *SPC_Channel_Ptr;
struct SPC_Channel {
    int cid, IOMode, status;
    pid_t pid;
    void (*Terminate_Handler)(SPC_Channel_Ptr, int, int, int, void *);
    void *Terminate_Data;
};
typedef struct { int len, offset; char *data; } buffered_data;
typedef struct { buffered_data *dataptr; int request_type; } protocol_request;

#define SPCIO_SYNC_TERMINATOR 1
#define IS_SPCIO_SYNC_TERM(a) ((a) & SPCIO_SYNC_TERMINATOR)
#define SPCD_DEAD_PROCESS (-1)
#define ERROR (-1)
#define REQUEST_HEADER_LENGTH 20
#define APPLICATION_DIED 1
#define SPC_Internal_Error 1
#define SPC_Reset_Protocol_Ptr(p, c, t, s) ((void)0)
#define WRITE_APPLICATION_DIED(d, s) 0
#define WRITE_HEADER(d, cid, t, l, s) 0
#define print_protocol_request(n, p) ((void)0)

static int ck_blocked(void)
{
    sigset_t cur;
    sigprocmask(SIG_BLOCK, nullptr, &cur);
    return sigismember(&cur, SIGCHLD) == 1;
}

static int ck_lock_calls, ck_lock_in_signal, ck_lock_outside;
static int ck_error_in_signal, ck_state_in_signal, ck_state_calls;
static int ck_term_calls, ck_term_in_signal, ck_term_outside;
static pid_t ck_term_pids[4];
static int ck_status_calls;

static void _DtSvcProcessLock(void)
{
    ck_lock_calls++;
    if (ck_blocked()) ck_lock_in_signal++; else ck_lock_outside++;
}
static void _DtSvcProcessUnlock(void) {}
static void SPC_Error(int code) { if (ck_blocked()) ck_error_in_signal++; }
static int SPC_Change_State(SPC_Channel_Ptr c, int a, int b, int d)
{
    ck_state_calls++;
    if (ck_blocked()) ck_state_in_signal++;
    return 0;
}
static void XeSPCGetProcessStatus(SPC_Channel_Ptr c, int *type, int *cause)
{
    ck_status_calls++;
    *type = 1;
    *cause = WIFEXITED(c->status) ? WEXITSTATUS(c->status) : -1;
}

static pid_t ck_pid_array[4];
static pid_t *SPC_pid_list = ck_pid_array;
static struct SPC_Channel ck_chan[3];
struct ck_conn { int sid; };
static struct ck_conn ck_wt_conn = {-1};
static struct ck_conn *write_terminator = &ck_wt_conn;

static SPC_Channel_Ptr SPC_Find_PID(int pid)
{
    for (int i = 0; i < 3; i++)
        if (ck_chan[i].pid == pid && pid != 0) return &ck_chan[i];
    return nullptr;
}

static void ck_terminated(SPC_Channel_Ptr c, int pid, int type, int cause, void *d)
{
    if (ck_term_calls < 4) ck_term_pids[ck_term_calls] = pid;
    ck_term_calls++;
    if (ck_blocked()) ck_term_in_signal++; else ck_term_outside++;
}

static int ck_hook_calls, ck_hook_fd = -1;
static SbInputCallbackProc ck_hook_cb;
static void *ck_hook_data;
static SbInputId ck_hook(int fd, SbInputCallbackProc cb, void *data)
{
    ck_hook_calls++; ck_hook_fd = fd; ck_hook_cb = cb; ck_hook_data = data;
    return 1;
}
static SbInputId (*SbAddInput_hookfn)(int, SbInputCallbackProc, void *);

/* @@BODY@@ */
static int failures;
#define CHECK(c) do { if (!(c)) { failures++; \
    printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

#define MAGIC_ERRNO 12345
static volatile int ck_errno_after;
static void ck_handler(int sig)
{
    errno = MAGIC_ERRNO;
    SPC_Child_Terminated(sig);
    ck_errno_after = errno;
}

static void install(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = ck_handler;
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGCHLD, &sa, nullptr) == 0);
}

static pid_t spawn(int code)
{
    pid_t p = fork();
    if (p == 0) _exit(code);
    return p;
}

static void reset_counters(void)
{
    ck_lock_calls = ck_lock_in_signal = ck_lock_outside = 0;
    ck_error_in_signal = ck_state_in_signal = ck_state_calls = 0;
    ck_term_calls = ck_term_in_signal = ck_term_outside = 0;
    ck_status_calls = 0;
    ck_errno_after = 0;
}

static void setup_channel(int i, pid_t pid)
{
    memset(&ck_chan[i], 0, sizeof ck_chan[i]);
    ck_chan[i].pid = pid;
    ck_chan[i].cid = i + 1;
    ck_chan[i].Terminate_Handler = ck_terminated;
}

/* wait until the child has terminated, without reaping it */
static void wait_exited(pid_t pid)
{
    siginfo_t si;
    int r;
    do r = waitid(P_PID, pid, &si, WEXITED | WNOWAIT);
    while (r == -1 && errno == EINTR);   /* SIGCHLD interrupts it */
    CHECK(r == 0);
}

static int wait_readable(int fd)
{
    struct pollfd p = {fd, POLLIN, 0};
    int r;
    do r = poll(&p, 1, 3000);
    while (r == -1 && errno == EINTR);
    return r == 1 && (p.revents & POLLIN);
}

#ifdef CK_HAS_PIPE
static int pending(int fd)
{
    int n = -1;
    ioctl(fd, FIONREAD, &n);
    return n;
}

static void case_hook_null(void)
{
    int probe = dup(0), after;
    close(probe);
    SbAddInput_hookfn = nullptr;
    errno = 0;
    CHECK(SPC_Child_Pipe_Open() == -1);
    CHECK(errno == EINVAL);
    CHECK(SPC_child_pipe[0] == -1 && SPC_child_pipe[1] == -1);
    after = dup(0);
    close(after);
    CHECK(after == probe);   /* no descriptor was created */
}

static void case_signal_to_loop(void)
{
    int flags, i;
    pid_t child;

    SbAddInput_hookfn = ck_hook;
    CHECK(SPC_Child_Pipe_Open() == 0);
    CHECK(ck_hook_calls == 1);
    CHECK(ck_hook_fd == SPC_child_pipe[0] && ck_hook_fd != -1);
    CHECK(ck_hook_cb == SPC_Reap_Children);
    for (i = 0; i < 2; i++) {
        flags = fcntl(SPC_child_pipe[i], F_GETFL);
        CHECK(flags != -1 && (flags & O_NONBLOCK));
        flags = fcntl(SPC_child_pipe[i], F_GETFD);
        CHECK(flags != -1 && (flags & FD_CLOEXEC));
    }
    CHECK(SPC_Child_Pipe_Open() == 0);      /* once only */
    CHECK(ck_hook_calls == 1);

    install();
    reset_counters();
    child = spawn(7);
    setup_channel(0, child);
    ck_pid_array[0] = child; ck_pid_array[1] = 0;
    CHECK(wait_readable(SPC_child_pipe[0]));

    /* in the loop, not yet reaped: nothing but the write ran in the handler */
    CHECK(pending(SPC_child_pipe[0]) == 1);
    CHECK(ck_errno_after == MAGIC_ERRNO);
    CHECK(ck_lock_in_signal == 0 && ck_lock_calls == 0);
    CHECK(ck_error_in_signal == 0);
    CHECK(ck_state_calls == 0);
    CHECK(ck_term_calls == 0);

    ck_hook_cb(ck_hook_data, &ck_hook_fd, nullptr);   /* what the loop does */
    CHECK(pending(SPC_child_pipe[0]) == 0);
    CHECK(ck_term_calls == 1 && ck_term_pids[0] == child);
    CHECK(ck_term_in_signal == 0 && ck_term_outside == 1);
    CHECK(ck_state_calls == 1 && ck_state_in_signal == 0);
    CHECK(ck_lock_calls == 1 && ck_lock_outside == 1 && ck_lock_in_signal == 0);
    CHECK(ck_pid_array[0] == SPCD_DEAD_PROCESS);
    CHECK(WIFEXITED(ck_chan[0].status) && WEXITSTATUS(ck_chan[0].status) == 7);
    CHECK(ck_status_calls == 1);
    CHECK(waitpid(child, nullptr, WNOHANG) == -1 && errno == ECHILD);   /* reaped */
}

static void case_two_children(void)
{
    pid_t a, b;
    char buf[16];

    reset_counters();
    a = spawn(3);
    b = spawn(4);
    wait_exited(a);
    wait_exited(b);
    CHECK(wait_readable(SPC_child_pipe[0]));
    setup_channel(0, a);
    setup_channel(1, b);
    ck_pid_array[0] = a; ck_pid_array[1] = b; ck_pid_array[2] = 0;

    ck_hook_cb(ck_hook_data, &ck_hook_fd, nullptr);
    CHECK(ck_term_calls == 2);
    CHECK((ck_term_pids[0] == a && ck_term_pids[1] == b) ||
          (ck_term_pids[0] == b && ck_term_pids[1] == a));
    CHECK(ck_pid_array[0] == SPCD_DEAD_PROCESS && ck_pid_array[1] == SPCD_DEAD_PROCESS);
    CHECK(WEXITSTATUS(ck_chan[0].status) == 3 && WEXITSTATUS(ck_chan[1].status) == 4);
    CHECK(ck_lock_in_signal == 0 && ck_term_in_signal == 0);
    CHECK(read(SPC_child_pipe[0], buf, sizeof buf) == -1);   /* drained */
    CHECK(waitpid(-1, nullptr, WNOHANG) == -1 && errno == ECHILD);
}
#else
/* revision with the reaping handler: it must not do its work in the handler */
static void case_old_handler(void)
{
    pid_t child;
    struct timespec ts = {0, 10 * 1000 * 1000};
    int i;

    install();
    reset_counters();
    child = spawn(7);
    setup_channel(0, child);
    ck_pid_array[0] = child; ck_pid_array[1] = 0;
    for (i = 0; i < 300 && ck_term_calls == 0; i++)
        nanosleep(&ts, nullptr);
    printf("handler ran: lock %d (in signal %d), state in signal %d, terminate %d (in signal %d)\n",
           ck_lock_calls, ck_lock_in_signal, ck_state_in_signal,
           ck_term_calls, ck_term_in_signal);
    CHECK(ck_lock_in_signal == 0);
    CHECK(ck_state_in_signal == 0);
    CHECK(ck_term_in_signal == 0);
}
#endif

int main(void)
{
#ifdef CK_HAS_PIPE
    case_hook_null();
    case_signal_to_loop();
    case_two_children();
#else
    case_old_handler();
#endif
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
