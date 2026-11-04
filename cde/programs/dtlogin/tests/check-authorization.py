#!/usr/bin/env python3
"""Exercise authorization generation and the authority files under ASan and UBSan.

The production code of auth.c and genauth.c is compiled with stand-ins for
the Xau library, the environment helpers and the random device, and is run
in a scratch directory.  Every scenario runs in its own process and must
neither crash nor be flagged by a sanitizer.  Set DTLOGIN_SOURCE to test
another copy of the sources.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

source = Path(os.environ.get("DTLOGIN_SOURCE") or Path(__file__).resolve().parents[1])
read = lambda name: (source / name).read_text()

def until(text, start, stop=None):
    first = text.index(start)
    return text[first:text.index(stop, first) if stop else len(text)]

auth = read("auth.c")
genauth = read("genauth.c")

# server files, the authority file helpers and the per-user authority files; the
# routines that collect the host's network addresses are replaced by stand-ins
pieces = until(auth, "void\nCleanUpFileName (char *src", "void\nSetLocalAuthorization")
pieces += until(auth, "static int\nopenFiles", "static void\nDefineLocal")
user = "/*\n * Build the name of the user's authority file"
pieces += until(auth, user if user in auth else "void\nSetUserAuthorization")

generate_status = "int\nGenerateAuthData" in genauth

harness = r'''
#include "dm.h"
#include "vgmsg.h"
#include <sys/stat.h>
#include <unistd.h>

extern int lock_pending_for_home, write_fails;
extern char *last_logged, *last_logged_argument;
extern char **fake_environment;
extern const unsigned char *random_bytes;
extern int random_length, random_open_fails, random_chunk, random_interrupts;
void RemoveUserAuthorization(struct display *d, struct verify_info *verify);
char *StubMktemp(char *template);
int StubOpen(const char *path, int flags, ...);
ssize_t StubRead(int fd, void *buffer, size_t length);
int test_Generate(char *buffer, int length);
'''

auth_unit = r'''
#include "harness.h"
#define mktemp StubMktemp

static void writeLocalAuth(FILE *file, Xauth *auth, char *name);
static void writeRemoteAuth(FILE *file, Xauth *auth, XdmcpNetaddr peer, int peerlen, char *name);
''' + pieces + r'''
static void writeLocalAuth(FILE *file, Xauth *auth, char *name)
{ (void)name; writeAddr(FamilyInternet, 4, "\n\0\0\1", file, auth); }
static void writeRemoteAuth(FILE *file, Xauth *auth, XdmcpNetaddr peer, int peerlen, char *name)
{ (void)file; (void)auth; (void)peer; (void)peerlen; (void)name; }
'''

genauth_unit = r'''
#include "harness.h"
#define open StubOpen
#define read StubRead
''' + genauth + r'''
int test_Generate(char *buffer, int length)
{
''' + ("    return GenerateAuthData(buffer, length);" if generate_status else
       "    GenerateAuthData(buffer, length); return 1;") + r'''
}
'''

stubs = r'''
#define _GNU_SOURCE
#include "harness.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>

int lock_pending_for_home, write_fails;
char *last_logged, *last_logged_argument;
char **fake_environment;
const unsigned char *random_bytes;
int random_length, random_open_fails, random_chunk, random_interrupts;
int debugLevel;
char *authDir;
static char work[1024];

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(1); } } while (0)
#define FAIL(...) do { fprintf(stderr, "FAIL line %d: ", __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)

void Debug(char *fmt, ...) { (void)fmt; }
void LogOutOfMem(unsigned char *fmt, ...) { (void)fmt; }
unsigned char *ReadCatalog(int set, int msg, char *def) { (void)set; (void)msg; return (unsigned char *)def; }
void LogError(unsigned char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    free(last_logged); free(last_logged_argument);
    last_logged = strdup((char *)fmt);
    last_logged_argument = NULL;
    const char *p = strchr((char *)fmt, '%');
    if (p) {
        for (++p; (*p >= '0' && *p <= '9') || *p == '$'; ++p) ;
        if (*p == 's') last_logged_argument = strdup(va_arg(ap, char *));
    }
    va_end(ap);
}

char *getEnv(char **e, char *name)
{
    size_t length = strlen(name);
    for (; e && *e; ++e)
        if (strncmp(*e, name, length) == 0 && (*e)[length] == '=')
            return *e + length + 1;
    return NULL;
}
char **setEnv(char **e, char *name, char *value)
{
    size_t count = 0;
    while (e && e[count]) ++count;
    e = realloc(e, (count + 2) * sizeof *e);
    asprintf(&e[count], "%s=%s", name, value);
    e[count + 1] = NULL;
    return e;
}

/* mktemp() only invents a name; whoever creates the file later may be handed a link instead */
static int mktemp_used;
char *StubMktemp(char *template)
{
    size_t length = strlen(template);
    ++mktemp_used;
    CHECK(length >= 6);
    memcpy(template + length - 6, "Zq9rLm", 6);
    return template;
}

/* the Xau library: locks are recorded, and only a held lock may be released */
static char *locked[8];
static int Held(const char *name)
{
    for (int i = 0; i < 8; ++i) if (locked[i] && strcmp(locked[i], name) == 0) return i;
    return -1;
}
int XauLockAuth(const char *name, int retries, int timeout, long dead)
{
    (void)retries; (void)timeout; (void)dead;
    size_t length = strlen(name);
    if (length < 1 || length > 1020)
        FAIL("lock requested for a name of %zu bytes", length);
    if (lock_pending_for_home && strstr(name, "/home/.Xauthority"))
        return LOCK_TIMEOUT;
    for (int i = 0; i < 8; ++i)
        if (!locked[i]) { locked[i] = strdup(name); return LOCK_SUCCESS; }
    return LOCK_ERROR;
}
int XauUnlockAuth(const char *name)
{
    int held = Held(name);
    if (held < 0)
        FAIL("unlock of a file that was not locked: \"%.24s\"", name);
    free(locked[held]); locked[held] = NULL;
    return 1;
}
Xauth *XauReadAuth(FILE *file) { (void)file; return NULL; }
int XauWriteAuth(FILE *file, Xauth *a)
{
    if (write_fails) return 0;
    return fprintf(file, "%.*s\n", a->name_length, a->name) > 0;
}
void XauDisposeAuth(Xauth *a) { (void)a; }

/* the random device: a canned stream that can be cut short or interrupted */
static int stream = -1;
int StubOpen(const char *path, int flags, ...)
{
    if (strcmp(path, "/dev/urandom") != 0)
        { errno = ENOENT; return -1; }
    if (random_open_fails) { errno = EACCES; return -1; }
    FILE *f = tmpfile();
    CHECK(f && fwrite(random_bytes, 1, random_length, f) == (size_t)random_length);
    stream = dup(fileno(f));
    fclose(f);
    lseek(stream, 0, SEEK_SET);
    (void)flags;
    return stream;
}
ssize_t StubRead(int fd, void *buffer, size_t length)
{
    if (fd != stream) return read(fd, buffer, length);
    if (random_interrupts > 0) { --random_interrupts; errno = EINTR; return -1; }
    return read(fd, buffer, length < (size_t)random_chunk ? length : (size_t)random_chunk);
}

static struct display ScratchDisplay(void)
{
    static Xauth cookie = { .family = FamilyWild, .name_length = 18, .name = "MIT-MAGIC-COOKIE-1",
                            .data_length = 16, .data = "0123456789abcdef", .number_length = 0 };
    static Xauth *list[1] = { &cookie };
    struct display d;
    memset(&d, 0, sizeof d);
    d.name = ":0";
    d.displayType.location = Local;
    d.authorizations = list;
    d.authNum = 1;
    d.userAuthDir = strdup(work);
    return d;
}

static struct verify_info ScratchVerify(const char *home)
{
    struct verify_info v;
    memset(&v, 0, sizeof v);
    v.uid = getuid();
    v.groups[0] = getgid();
    v.userEnviron = calloc(3, sizeof(char *));
    if (home) asprintf(&v.userEnviron[0], "HOME=%s", home);
    return v;
}

static char *Path(const char *format, ...)
{
    char *path;
    va_list ap;
    va_start(ap, format);
    vasprintf(&path, format, ap);
    va_end(ap);
    return path;
}

static int Exists(const char *path, mode_t *mode)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (mode) *mode = st.st_mode & 07777;
    return 1;
}

static void NoLocksLeft(void)
{
    for (int i = 0; i < 8; ++i)
        if (locked[i]) FAIL("lock on \"%.60s\" still held", locked[i]);
}

static void ServerFile(void)
{
    struct display d = ScratchDisplay();
    mode_t mode;
    authDir = work;
    umask(022);
    CHECK(SaveServerAuthorizations(&d, d.authorizations, 1));
    CHECK(d.authFile && strstr(d.authFile, "/authdir/authfiles/A:0-"));
    CHECK(Exists(d.authFile, &mode) && mode == 0600);
    CHECK(Exists(Path("%s/authdir", work), &mode) && mode == 0700);
    CHECK(mktemp_used == 0);
    char *first = strdup(d.authFile);
    CHECK(SaveServerAuthorizations(&d, d.authorizations, 1));
    CHECK(strcmp(first, d.authFile) == 0 && Exists(first, &mode) && mode == 0600);
    /* the process umask is not left changed */
    mode_t current = umask(022);
    CHECK(current == 022);
}

static void ServerFileFailure(void)
{
    struct display d = ScratchDisplay();
    authDir = Path("%s/missing/directory", work);
    umask(022);
    CHECK(!SaveServerAuthorizations(&d, d.authorizations, 1));
    mode_t current = umask(022);
    CHECK(current == 022);
}

static void HomeFile(void)
{
    struct display d = ScratchDisplay();
    mode_t mode;
    char *home = Path("%s/home", work), *name = Path("%s/.Xauthority", home);
    mkdir(home, 0700);
    struct verify_info v = ScratchVerify(home);
    SetUserAuthorization(&d, &v);
    CHECK(Exists(name, &mode));
    CHECK(getEnv(v.userEnviron, "XAUTHORITY") == NULL);
    NoLocksLeft();
    RemoveUserAuthorization(&d, &v);
    CHECK(Exists(name, NULL));
    NoLocksLeft();
}

static void BackupFile(const char *home)
{
    struct display d = ScratchDisplay();
    mode_t mode;
    struct verify_info v = ScratchVerify(home);
    SetUserAuthorization(&d, &v);
    char *backup = getEnv(v.userEnviron, "XAUTHORITY");
    CHECK(backup && strncmp(backup, work, strlen(work)) == 0);
    CHECK(strstr(backup, "/.Xauth") != NULL && Exists(backup, &mode) && mode == 0600);
    CHECK(strcmp(getEnv(v.systemEnviron, "XAUTHORITY"), backup) == 0);
    CHECK(mktemp_used == 0);
    NoLocksLeft();
}

static void NoHome(void) { BackupFile(NULL); }

static void ContendedHome(void)
{
    char *home = Path("%s/home", work);
    mkdir(home, 0700);
    lock_pending_for_home = 1;
    BackupFile(home);
    /* the other holder's lock files are left alone */
}

static void LongHome(void)
{
    char *home = Path("%s/%01100d", work, 7);
    BackupFile(home);
}

static void WriteFailure(void)
{
    struct display d = ScratchDisplay();
    char *home = Path("%s/home", work);
    mkdir(home, 0700);
    struct verify_info v = ScratchVerify(home);
    write_fails = 1;
    SetUserAuthorization(&d, &v);
    char *expected = Path("%s/.Xauthority-n", home);
    CHECK(last_logged && strstr(last_logged, "Cannot write"));
    CHECK(last_logged_argument && strcmp(last_logged_argument, expected) == 0);
}

static void RandomSource(void)
{
    static unsigned char stream_bytes[64];
    char data[16];
    for (int i = 0; i < 64; ++i) stream_bytes[i] = (unsigned char)(0xa0 + i);
    random_bytes = stream_bytes; random_length = 64; random_chunk = 5; random_interrupts = 1;
    CHECK(test_Generate(data, sizeof data));
    CHECK(memcmp(data, stream_bytes, sizeof data) == 0);
    CHECK(test_Generate(data, 0));
}

static void RandomFailure(void)
{
    static const unsigned char few[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    char data[16];
    memset(data, 0x7e, sizeof data);
    random_bytes = few; random_length = sizeof few; random_chunk = 3;
    /* a device that runs dry gives no authorization, and no partial secret */
    CHECK(!test_Generate(data, sizeof data));
    for (size_t i = 0; i < sizeof data; ++i) CHECK(data[i] == 0);
    random_open_fails = 1;
    memset(data, 0x7e, sizeof data);
    CHECK(!test_Generate(data, sizeof data));
}

static const struct { const char *name; void (*run)(void); } scenarios[] = {
    {"server-file", ServerFile},
    {"server-file-failure", ServerFileFailure},
    {"home-file", HomeFile},
    {"no-home", NoHome},
    {"contended-home", ContendedHome},
    {"long-home", LongHome},
    {"write-failure", WriteFailure},
    {"random-source", RandomSource},
    {"random-failure", RandomFailure},
};

int main(int argc, char **argv)
{
    CHECK(argc == 2 && getcwd(work, sizeof work));
    for (size_t i = 0; i < sizeof scenarios / sizeof *scenarios; ++i)
        if (strcmp(argv[1], scenarios[i].name) == 0) { scenarios[i].run(); puts("ok"); return 0; }
    fprintf(stderr, "FAIL unknown scenario %s\n", argv[1]);
    return 2;
}
'''

def run_all(prefix, files, units, libs, scenarios):
    with tempfile.TemporaryDirectory(prefix=prefix, dir=os.environ.get("TMPDIR")) as work:
        work = Path(work)
        for name, text in files.items():
            (work / name).write_text(text)
        cc = shlex.split(os.environ.get("CC", "cc"))
        flags = ["-std=c23", "-D_DEFAULT_SOURCE", "-DUNIXCONN", "-DTCPCONN", "-DXDMCP", "-DHASXDMAUTH",
                 "-DANSICPP", "-I", str(work), "-I", str(source), "-I", str(source.parents[1] / "include"),
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
                 "-g", "-O1"]
        objects = []
        for unit, extra in units:
            obj = work / (unit + ".o")
            subprocess.run(cc + flags + extra + ["-c", str(work / (unit + ".c")), "-o", str(obj)], check=True)
            objects.append(str(obj))
        binary = work / "scenarios"
        subprocess.run(cc + flags + objects + ["-o", str(binary)] + libs, check=True)
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=0",
                   UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1")
        failed = 0
        for name in scenarios:
            scratch = work / name
            scratch.mkdir()
            done = subprocess.run([str(binary), name], cwd=scratch, env=env, capture_output=True, text=True)
            lines = (done.stdout + done.stderr).splitlines()
            reason = next((l for l in lines if l.startswith("FAIL") or "ERROR:" in l or "runtime error" in l),
                          lines[0] if lines else "exit status %d" % done.returncode)
            frame = next((l.strip() for l in lines if re.match(r"\s*#[0-4] .* in \w+ .*(_unit|stubs)\.c", l)), "")
            if done.returncode:
                failed += 1
                print("FAIL %s: %s %s" % (name, reason.strip(), frame))
            else:
                print("ok   %s" % name)
        return failed

names = ["server-file", "server-file-failure", "home-file", "no-home", "contended-home",
         "long-home", "write-failure", "random-source", "random-failure"]
failed = run_all("login-authorization-",
                 {"harness.h": harness, "auth_unit.c": auth_unit, "genauth_unit.c": genauth_unit, "stubs.c": stubs},
                 [("auth_unit", ["-w"]), ("genauth_unit", ["-w"]),
                  ("stubs", ["-Wall", "-Wextra", "-Wno-unused-parameter"])],
                 ["-lXdmcp"], names)
if failed:
    sys.exit("Authorization: %d scenario(s) failed" % failed)
print("Authorization: random data, server files, user authority files and error reports passed")
