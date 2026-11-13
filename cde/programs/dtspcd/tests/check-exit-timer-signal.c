/* @@PROLOGUE@@ */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>

typedef char *XeString;
typedef unsigned long SbInputId;
#define SA_HANDLER_INT_ARG 1
#define SPCD_DEAD_PROCESS (-1)
#define SPCD_NO_TIMER (-1)
#define SPCD_REQUEST_PENDING 1
#define SPCD_NO_REQUEST_PENDING 0

/* State of the daemon that the extracted code reads. */
static int exit_timeout = 10;
static int request_pending = SPCD_NO_REQUEST_PENDING;
static int *SPC_pid_list = nullptr;

/* Recorders. */
static int ck_log_calls, ck_log_alarm_blocked;
static int ck_exit_calls, ck_exit_code, ck_exit_alarm_blocked;
static int ck_alarm_calls;
static unsigned ck_alarm_arg;
static sigjmp_buf ck_exit_jump;
static int ck_exit_jump_armed;

static int ck_alarm_blocked(void)
{
    sigset_t cur;

    sigprocmask(SIG_BLOCK, nullptr, &cur);
    return sigismember(&cur, SIGALRM) == 1;
}

static void SPC_Format_Log(XeString format, ...)
{
    (void) format;
    ++ck_log_calls;
    ck_log_alarm_blocked = ck_alarm_blocked();
}

/* Declared noreturn in main.c; the stand-in leaves by a jump, never exits. */
[[noreturn]] void SPCD_Exit(int exitval);
[[noreturn]] void SPCD_Exit(int exitval)
{
    ++ck_exit_calls;
    ck_exit_code = exitval;
    ck_exit_alarm_blocked = ck_alarm_blocked();
    if (ck_exit_jump_armed)
        siglongjmp(ck_exit_jump, 1);
    fputs("SPCD_Exit called outside a guarded call\n", stderr);
    _exit(3);
}

static unsigned ck_alarm(unsigned seconds)
{
    ++ck_alarm_calls;
    ck_alarm_arg = seconds;
    return 0;
}

/* pipe() and fcntl() stand-ins that can be told to fail. */
static int ck_pipe_fail;
static int ck_fcntl_fail_call;	/* fail the n-th fcntl call (1-based), 0 = never */
static int ck_fcntl_calls;
static int ck_fcntl_first_fd = -1;

static int ck_pipe(int fds[2])
{
    if (ck_pipe_fail) {
        errno = EMFILE;
        return -1;
    }
    return pipe(fds);
}

static int ck_fcntl(int fd, int cmd, ...)
{
    va_list ap;
    int arg = 0;

    ++ck_fcntl_calls;
    if (ck_fcntl_first_fd == -1)
        ck_fcntl_first_fd = fd;
    if (ck_fcntl_calls == ck_fcntl_fail_call) {
        errno = EBADF;
        return -1;
    }
    if (cmd == F_SETFL || cmd == F_SETFD) {
        va_start(ap, cmd);
        arg = va_arg(ap, int);
        va_end(ap);
        return fcntl(fd, cmd, arg);
    }
    return fcntl(fd, cmd);
}

#define alarm ck_alarm
#define pipe ck_pipe
#define fcntl ck_fcntl
/* @@BODY@@ */
#undef alarm
#undef pipe
#undef fcntl

static int failures;
#define CHECK(cond) \
    do { if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static void reset_records(void)
{
    ck_log_calls = ck_log_alarm_blocked = 0;
    ck_exit_calls = ck_exit_code = ck_exit_alarm_blocked = 0;
    ck_alarm_calls = 0;
    ck_alarm_arg = 0;
    ck_exit_jump_armed = 0;
}

static void install_handler(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SPCD_Alarm_Handler;
    CHECK(sigaction(SIGALRM, &sa, nullptr) == 0);
}

#ifdef CK_HAS_ALARM_PIPE

static int pending_bytes(int fd)
{
    int n = -1;

    CHECK(ioctl(fd, FIONREAD, &n) == 0);
    return n;
}

static int readable(int fd)
{
    struct pollfd p = { .fd = fd, .events = POLLIN };

    return poll(&p, 1, 1000) == 1 && (p.revents & POLLIN);
}

/* Run the callback the way the event loop does: outside any signal handler. */
static void run_work(void)
{
    int fd = SPCD_alarm_pipe[0];
    SbInputId id = 0;

    ck_exit_jump_armed = 1;
    if (sigsetjmp(ck_exit_jump, 1) == 0)
        SPCD_Alarm_Work(nullptr, &fd, &id);
    ck_exit_jump_armed = 0;
}

static void close_pipe(void)
{
    for (int i = 0; i < 2; i++) {
        if (SPCD_alarm_pipe[i] != -1)
            close(SPCD_alarm_pipe[i]);
        SPCD_alarm_pipe[i] = -1;
    }
}

static int highest_open_fd(void)
{
    int top = 2;

    for (int fd = 3; fd < 1024; fd++)
        if (fcntl(fd, F_GETFD) != -1)
            top = fd;
    return top;
}

int main(void)
{
    /* Case 1: the handler only writes; the work runs in the loop. */
    reset_records();
    CHECK(SPCD_Alarm_Pipe_Open() == 0);
    install_handler();
    errno = 4321;
    raise(SIGALRM);
    CHECK(errno == 4321);
    CHECK(pending_bytes(SPCD_alarm_pipe[0]) == 1);
    CHECK(ck_log_calls == 0);
    CHECK(ck_exit_calls == 0);
    CHECK(ck_alarm_calls == 0);
    CHECK(readable(SPCD_alarm_pipe[0]));
    CHECK(!ck_alarm_blocked());
    run_work();
    CHECK(pending_bytes(SPCD_alarm_pipe[0]) == 0);
    CHECK(ck_log_calls == 1);
    CHECK(ck_exit_calls == 1);
    CHECK(ck_exit_code == 0);
    CHECK(!ck_log_alarm_blocked);
    CHECK(!ck_exit_alarm_blocked);

    /* Several expirations before the loop runs are drained by one call. */
    reset_records();
    raise(SIGALRM);
    raise(SIGALRM);
    CHECK(pending_bytes(SPCD_alarm_pipe[0]) == 2);
    run_work();
    CHECK(pending_bytes(SPCD_alarm_pipe[0]) == 0);
    CHECK(ck_exit_calls == 1);

    /* Case 2: a live child re-arms the timer and neither logs nor exits. */
    int live[2] = { (int) getpid(), 0 };
    SPC_pid_list = live;
    exit_timeout = 7;
    reset_records();
    raise(SIGALRM);
    run_work();
    CHECK(pending_bytes(SPCD_alarm_pipe[0]) == 0);
    CHECK(ck_alarm_calls == 1);
    CHECK(ck_alarm_arg == 7 * 60);
    CHECK(ck_log_calls == 0);
    CHECK(ck_exit_calls == 0);

    /* A pending request keeps the daemon without a re-arm. */
    int dead[2] = { SPCD_DEAD_PROCESS, 0 };
    SPC_pid_list = dead;
    request_pending = SPCD_REQUEST_PENDING;
    reset_records();
    raise(SIGALRM);
    run_work();
    CHECK(ck_log_calls == 0);
    CHECK(ck_exit_calls == 0);
    request_pending = SPCD_NO_REQUEST_PENDING;
    SPC_pid_list = nullptr;
    exit_timeout = 10;

    /* Case 3: descriptor flags. */
    close_pipe();
    CHECK(SPCD_Alarm_Pipe_Open() == 0);
    for (int i = 0; i < 2; i++) {
        CHECK(SPCD_alarm_pipe[i] >= 0);
        CHECK(fcntl(SPCD_alarm_pipe[i], F_GETFL) & O_NONBLOCK);
        CHECK(fcntl(SPCD_alarm_pipe[i], F_GETFD) & FD_CLOEXEC);
    }
    char c;
    errno = 0;
    CHECK(read(SPCD_alarm_pipe[0], &c, 1) == -1 && errno == EAGAIN);
    close_pipe();

    /* pipe() fails because the descriptors are exhausted. */
    struct rlimit old, lim;
    CHECK(getrlimit(RLIMIT_NOFILE, &old) == 0);
    lim = old;
    lim.rlim_cur = (rlim_t) highest_open_fd() + 1;
    CHECK(setrlimit(RLIMIT_NOFILE, &lim) == 0);
    errno = 0;
    CHECK(SPCD_Alarm_Pipe_Open() == -1);
    CHECK(errno == EMFILE);
    CHECK(SPCD_alarm_pipe[0] == -1 && SPCD_alarm_pipe[1] == -1);
    CHECK(setrlimit(RLIMIT_NOFILE, &old) == 0);

    /* A flag call fails after pipe() succeeded: both ends are closed again. */
    for (int call = 1; call <= 6; call++) {
        ck_fcntl_calls = 0;
        ck_fcntl_first_fd = -1;
        ck_fcntl_fail_call = call;
        errno = 0;
        CHECK(SPCD_Alarm_Pipe_Open() == -1);
        CHECK(errno == EBADF);
        CHECK(SPCD_alarm_pipe[0] == -1 && SPCD_alarm_pipe[1] == -1);
        CHECK(ck_fcntl_first_fd != -1 && fcntl(ck_fcntl_first_fd, F_GETFD) == -1);
        ck_fcntl_fail_call = 0;
    }
    ck_pipe_fail = 1;
    errno = 0;
    CHECK(SPCD_Alarm_Pipe_Open() == -1 && errno == EMFILE);
    CHECK(SPCD_alarm_pipe[0] == -1 && SPCD_alarm_pipe[1] == -1);
    ck_pipe_fail = 0;

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("exit timer: handler writes one byte, work runs in the loop, pipe ends are non-blocking and close-on-exec");
    return 0;
}

#else /* the handler alone, as in a revision that does the work in the handler */

int main(void)
{
    reset_records();
    install_handler();
    ck_exit_jump_armed = 1;
    if (sigsetjmp(ck_exit_jump, 1) == 0)
        raise(SIGALRM);
    ck_exit_jump_armed = 0;
    if (ck_log_calls) {
        fprintf(stderr, "FAIL: SPC_Format_Log ran in signal context (SIGALRM blocked: %d)\n",
                ck_log_alarm_blocked);
        ++failures;
    }
    if (ck_exit_calls) {
        fprintf(stderr, "FAIL: SPCD_Exit(%d) ran in signal context (SIGALRM blocked: %d)\n",
                ck_exit_code, ck_exit_alarm_blocked);
        ++failures;
    }
    return failures ? 1 : 0;
}

#endif
