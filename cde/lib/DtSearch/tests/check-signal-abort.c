/* @@PROLOGUE@@ */
#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PROGNAME "DTSRAPI"
#define MS_ausapi 2
#define CATGETS(cat, set, num, text) (text)

static FILE *aa_stderr;
static char *aa_argv0;
static char OE_prodname_text[] = "DtSearch";
#define OE_prodname OE_prodname_text
static int dtsearch_catd;

/* Recorders: every call that is not async-signal-safe notes whether SIGUSR1
 * is blocked, which is so only inside the signal handler. */
static int ck_unsafe_calls, ck_blocked_any;
static int ck_exit_calls, ck_exit_code;
static int ck__exit_calls, ck__exit_code;
static int ck_write_calls, ck_write_fd;
static char ck_written[512];
static size_t ck_written_len;
static sigjmp_buf ck_jump;
static int ck_jump_armed;

static int ck_blocked(void)
{
    sigset_t cur;

    sigprocmask(SIG_BLOCK, nullptr, &cur);
    return sigismember(&cur, SIGUSR1) == 1;
}

static void unsafe(void)
{
    ++ck_unsafe_calls;
    if (ck_blocked())
        ck_blocked_any = 1;
}

static char *DtSearchGetMessages(void) { unsafe(); return (char *) ""; }
static int ck_fputs(const char *s, FILE *f) { (void) s; (void) f; unsafe(); return 0; }
static int ck_fprintf(FILE *f, const char *fmt, ...) { (void) f; (void) fmt; unsafe(); return 0; }
static int ck_fflush(FILE *f) { (void) f; unsafe(); return 0; }

[[noreturn]] static void ck_leave(void)
{
    if (ck_jump_armed)
        siglongjmp(ck_jump, 1);
    fputs("exit called outside a guarded call\n", stderr);
    abort();
}

static void DtSearchExit(int code)
{
    ++ck_exit_calls;
    ck_exit_code = code;
    unsafe();
    ck_leave();
}

[[noreturn]] static void ck__exit(int code)
{
    ++ck__exit_calls;
    ck__exit_code = code;
    ck_leave();
}

static ssize_t ck_write(int fd, const void *buf, size_t n)
{
    ++ck_write_calls;
    ck_write_fd = fd;
    if (ck_written_len + n < sizeof ck_written) {
        memcpy(ck_written + ck_written_len, buf, n);
        ck_written_len += n;
        ck_written[ck_written_len] = 0;
    }
    if (ck_blocked())
        ck_blocked_any = 1;
    return (ssize_t) n;
}

#define fputs ck_fputs
#define fprintf ck_fprintf
#define fflush ck_fflush
#define _exit ck__exit
#define write ck_write
/* @@BODY@@ */
#undef fputs
#undef fprintf
#undef fflush
#undef _exit
#undef write

static int failures;
#define CHECK(cond) \
    do { if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static void reset_records(void)
{
    ck_unsafe_calls = ck_blocked_any = 0;
    ck_exit_calls = ck_exit_code = 0;
    ck__exit_calls = ck__exit_code = 0;
    ck_write_calls = ck_write_fd = 0;
    memset(ck_written, 0, sizeof ck_written);
    ck_written_len = 0;
    ck_jump_armed = 0;
}

static void install_handler(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = signal_abort;
    CHECK(sigaction(SIGUSR1, &sa, nullptr) == 0);
}

static void raise_guarded(void)
{
    ck_jump_armed = 1;
    if (sigsetjmp(ck_jump, 1) == 0)
        raise(SIGUSR1);
    ck_jump_armed = 0;
}

#ifdef CK_NEW

int main(void)
{
    char expect[128];

    install_handler();

    /* One fixed line with write(2), then _exit(100 + sig). */
    aa_argv0 = (char *) "prog";
    abort_fd = 41;
    reset_records();
    raise_guarded();
    snprintf(expect, sizeof expect, "\nDTSRAPI216 prog Caught signal %d.\n", SIGUSR1);
    CHECK(strcmp(ck_written, expect) == 0);
    CHECK(ck_write_calls == 1 && ck_write_fd == 41);
    CHECK(ck__exit_calls == 1 && ck__exit_code == 100 + SIGUSR1);
    CHECK(ck_unsafe_calls == 0 && ck_exit_calls == 0);

    /* A second signal while the first is handled ends at once, silently. */
    reset_records();
    raise_guarded();
    CHECK(ck_write_calls == 0);
    CHECK(ck__exit_calls == 1 && ck__exit_code == 100 + SIGUSR1);
    CHECK(ck_unsafe_calls == 0);

    /* Without argv0 the product name is used. */
    abort_entered = 0;
    aa_argv0 = nullptr;
    reset_records();
    raise_guarded();
    snprintf(expect, sizeof expect, "\nDTSRAPI216 DtSearch Caught signal %d.\n", SIGUSR1);
    CHECK(strcmp(ck_written, expect) == 0);

    /* A very long program name is cut, never overruns the line buffer. */
    abort_entered = 0;
    char *longname = malloc(5000);
    memset(longname, 'x', 4999);
    longname[4999] = 0;
    aa_argv0 = longname;
    reset_records();
    raise_guarded();
    CHECK(ck_write_calls == 1 && ck_written_len > 0 && ck_written_len < 256);
    CHECK(ck_written[ck_written_len - 1] == '\n' && ck_written[ck_written_len - 2] == '.');
    free(longname);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("DtSearch: signal_abort writes one fixed line and calls _exit(), nothing else");
    return 0;
}

#else /* a revision whose handler writes with stdio and runs the exit callbacks */

int main(void)
{
    install_handler();
    aa_argv0 = (char *) "prog";
    reset_records();
    raise_guarded();
    if (ck_unsafe_calls) {
        fprintf(stderr, "FAIL: %d unsafe call(s) ran in signal context (signal blocked: %d), "
                "DtSearchExit(%d) calls: %d\n", ck_unsafe_calls, ck_blocked_any, ck_exit_code, ck_exit_calls);
        ++failures;
    }
    return failures ? 1 : 0;
}

#endif
