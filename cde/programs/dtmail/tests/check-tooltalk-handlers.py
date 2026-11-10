#!/usr/bin/env python3
"""Run what dtmail does with the data of a ToolTalk request under the sanitizers.

Another application in the user's session can ask dtmail to send a message
(Mail), to open a mailbox (Display) or to print one (Print), with the data in a
buffer or in a file it names.  This test cuts the production functions that make
the data of such a request out of dtmail/RoamApp.C and runs them against real
files: a buffer that is saved to a file, a file that is read for a Mail request,
and the files that are not a message (an empty file, a directory, a device, a
FIFO that nobody writes to, a file too large to be one).  The handlers
themselves need a display and a session, so the cases that depend on the data
they do not get (a request with neither buffer nor file) are guarded.

DTMAIL_SOURCE names another copy of the dtmail sources to test, for example one
with a deliberate defect.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

source = Path(os.environ.get("DTMAIL_SOURCE") or Path(__file__).resolve().parents[1])
cxx = shlex.split(os.environ.get("CXX", "g++"))
problems = []
text = (source / "dtmail" / "RoamApp.C").read_text(errors="replace")


def block(pattern, required=True):
    match = re.search(pattern, text, re.M)
    if not match:
        if required:
            problems.append("function not found: " + pattern)
        return None
    opening = text.index("{", match.end())
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[match.start():end] + "\n"


def code_of(t):
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", t, flags=re.S)


saver = block(r"^static char \*tooltalk_save_buffer_to_file\(\n")
mail = block(r"^static bool\ntooltalk_mail_data\(\n", required=False)
limit = re.search(r"^static const long TOOLTALK_MAIL_MAX = [^;]+;\n", text, re.M)
if mail is None or limit is None:
    problems.append("RoamApp.C has no tooltalk_mail_data(): a Mail request reads the file it names without "
                    "looking at what it is, and sends a buffer that was never set when it names none")

harness = r'''
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <new>
#include <dirent.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "failed: %s (line %d)\n", #condition, __LINE__); return 1; } } while (0)

#define DtPERSONAL_TMP_DIRECTORY ".dt/tmp"
typedef struct { void *buffer; unsigned long size; } DtMailBuffer;
static int SafeOpen(const char *path, int oflag, mode_t mode = 0644) { return open(path, oflag, mode); }
static int SafeFStat(int fd, struct stat *buf) { return fstat(fd, buf); }
static ssize_t SafeRead(int fd, void *buf, size_t n) { return read(fd, buf, n); }
static ssize_t SafeWrite(int fd, const void *buf, size_t n) { return write(fd, buf, n); }

''' + (saver or "") + (limit.group(0) if limit else "") + (mail or "") + r'''

static std::string home;
static int open_files(void)
{
    int n = 0;
    DIR *d = opendir("/proc/self/fd");
    if (!d) return 0;
    while (readdir(d)) n++;
    closedir(d);
    return n;
}
static int count_entries(const std::string &dir)
{
    int n = 0;
    DIR *d = opendir(dir.c_str());
    if (!d) return -1;
    while (struct dirent *e = readdir(d)) if (e->d_name[0] != '.') n++;
    closedir(d);
    return n;
}
static std::string slurp(const std::string &path)
{
    std::string r; char b[4096]; ssize_t n;
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return "<missing>";
    while ((n = read(fd, b, sizeof b)) > 0) r.append(b, n);
    close(fd);
    return r;
}
static void write_file(const std::string &path, const std::string &content)
{
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, content.data(), content.size()) != (ssize_t) content.size()) exit(3);
    close(fd);
}

struct Scenario { const char *name; int (*run)(); };
'''
if saver:
    harness += r'''
static int buffer_is_saved_privately() {
    std::string content = "From a@b Mon Nov  9 10:00:00 2026\nSubject: x\n\nbody\n";
    char *p = tooltalk_save_buffer_to_file((unsigned char *) content.data(), (int) content.size());
    CHECK(p != NULL);
    CHECK(std::string(p).compare(0, home.size() + 8, home + "/.dt/tmp") == 0);
    struct stat st;
    CHECK(lstat(p, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0600);
    CHECK(slurp(p) == content);
    free(p);
    return 0;
}
static int every_buffer_gets_a_file_of_its_own() {
    std::string dir = home + "/.dt/tmp";
    for (int i = 0; i < 20; i++) {
        char *p = tooltalk_save_buffer_to_file((unsigned char *) "data", 4);
        CHECK(p != NULL);
        free(p);
    }
    CHECK(count_entries(dir) == 20);
    return 0;
}
static int no_home_is_no_place() {
    unsetenv("HOME");
    std::string tmp = home + "/tmpdir";
    mkdir(tmp.c_str(), 0700);
    setenv("TMPDIR", tmp.c_str(), 1);
    char *p = tooltalk_save_buffer_to_file((unsigned char *) "data", 4);
    CHECK(p == NULL);
    CHECK(count_entries(tmp) == 0);                /* and nothing was made in the directory for temporary files */
    return 0;
}
static int missing_directory_is_no_place() {
    setenv("HOME", (home + "/nowhere").c_str(), 1);
    CHECK(tooltalk_save_buffer_to_file((unsigned char *) "data", 4) == NULL);
    return 0;
}
static int nothing_to_save_is_refused() {
    CHECK(tooltalk_save_buffer_to_file((unsigned char *) "", 0) == NULL);
    CHECK(count_entries(home + "/.dt/tmp") == 0);
    return 0;
}
'''
if mail:
    harness += r'''
static int a_buffer_is_used_as_it_is() {
    unsigned char data[] = "From a@b\n\nbody\n";
    DtMailBuffer b; char *owned = (char *) 1;
    CHECK(tooltalk_mail_data(data, sizeof(data) - 1, NULL, &b, &owned));
    CHECK(b.buffer == data && b.size == sizeof(data) - 1 && owned == NULL);
    return 0;
}
static int a_file_is_read_whole() {
    for (size_t n : {1, 2, 4095, 4096, 4097, 100000, 3000000}) {
        std::string content(n, 'm');
        std::string path = home + "/message";
        write_file(path, content);
        DtMailBuffer b; char *owned = NULL;
        int fds = open_files();
        CHECK(tooltalk_mail_data(NULL, 0, path.c_str(), &b, &owned));
        CHECK(owned != NULL && b.buffer == owned && b.size == n);
        CHECK(memcmp(b.buffer, content.data(), n) == 0);
        CHECK(open_files() == fds);                /* the file is closed again */
        delete [] owned;
    }
    return 0;
}
static int no_data_is_refused() {
    DtMailBuffer b; char *owned = (char *) 1;
    CHECK(!tooltalk_mail_data(NULL, 0, NULL, &b, &owned));
    CHECK(owned == NULL && b.buffer == NULL && b.size == 0);     /* nothing is left unset for the caller */
    CHECK(!tooltalk_mail_data(NULL, -5, NULL, &b, &owned));
    return 0;
}
static int what_is_not_a_message_is_refused() {
    DtMailBuffer b; char *owned = NULL;
    write_file(home + "/empty", "");
    CHECK(!tooltalk_mail_data(NULL, 0, (home + "/empty").c_str(), &b, &owned) && owned == NULL);
    CHECK(!tooltalk_mail_data(NULL, 0, home.c_str(), &b, &owned));                    /* a directory */
    CHECK(!tooltalk_mail_data(NULL, 0, "/dev/zero", &b, &owned));                      /* a device */
    CHECK(!tooltalk_mail_data(NULL, 0, (home + "/missing").c_str(), &b, &owned));
    return 0;
}
static int a_fifo_is_not_waited_for() {
    DtMailBuffer b; char *owned = NULL;
    std::string fifo = home + "/fifo";
    CHECK(mkfifo(fifo.c_str(), 0600) == 0);
    alarm(10);                                     /* waiting for a writer is a failure */
    CHECK(!tooltalk_mail_data(NULL, 0, fifo.c_str(), &b, &owned));
    alarm(0);
    return 0;
}
static int a_file_too_large_is_refused() {
    DtMailBuffer b; char *owned = NULL;
    std::string path = home + "/huge";
    int fd = open(path.c_str(), O_WRONLY | O_CREAT, 0600);
    CHECK(fd >= 0 && ftruncate(fd, 1L << 40) == 0);                                  /* sparse */
    close(fd);
    CHECK(!tooltalk_mail_data(NULL, 0, path.c_str(), &b, &owned) && owned == NULL);
    CHECK(ftruncate(open(path.c_str(), O_WRONLY), TOOLTALK_MAIL_MAX + 1) == 0);
    CHECK(!tooltalk_mail_data(NULL, 0, path.c_str(), &b, &owned) && owned == NULL);
    return 0;
}
'''
harness += "static Scenario scenarios[] = {\n"
if saver:
    harness += '''    {"buffer_is_saved_privately", buffer_is_saved_privately},
    {"every_buffer_gets_a_file_of_its_own", every_buffer_gets_a_file_of_its_own},
    {"no_home_is_no_place", no_home_is_no_place},
    {"missing_directory_is_no_place", missing_directory_is_no_place},
    {"nothing_to_save_is_refused", nothing_to_save_is_refused},
'''
if mail:
    harness += '''    {"a_buffer_is_used_as_it_is", a_buffer_is_used_as_it_is},
    {"a_file_is_read_whole", a_file_is_read_whole},
    {"no_data_is_refused", no_data_is_refused},
    {"what_is_not_a_message_is_refused", what_is_not_a_message_is_refused},
    {"a_fifo_is_not_waited_for", a_fifo_is_not_waited_for},
    {"a_file_too_large_is_refused", a_file_too_large_is_refused},
'''
harness += r'''    {NULL, NULL}
};
int main(int argc, char **argv)
{
    if (argc < 2) { for (int i = 0; scenarios[i].name; i++) puts(scenarios[i].name); return 0; }
    const char *work = getenv("DTMAIL_TEST_DIR");
    if (!work) return 2;
    home = work;
    mkdir((home + "/.dt").c_str(), 0700);
    mkdir((home + "/.dt/tmp").c_str(), 0700);
    setenv("HOME", work, 1);
    for (int i = 0; scenarios[i].name; i++) if (!strcmp(argv[1], scenarios[i].name)) return scenarios[i].run();
    return 2;
}
'''

with tempfile.TemporaryDirectory(prefix="dtmail-tt-", dir=os.environ.get("TMPDIR")) as work:
    work = Path(work)
    cc = work / "tooltalk.cc"
    cc.write_text(harness)
    binary = work / "tooltalk"
    build = cxx + ["-std=gnu++17", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   "-fno-omit-frame-pointer", "-Wall", "-Wno-unused-function", "-Wno-unused-variable",
                   str(cc), "-o", str(binary)]
    compiled = subprocess.run(build, capture_output=True, text=True)
    if compiled.returncode:
        sys.stderr.write(compiled.stderr)
        sys.exit("cannot compile the ToolTalk harness")
    names = subprocess.run([str(binary)], capture_output=True, text=True, check=True).stdout.split()
    passed = 0
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=0", UBSAN_OPTIONS="halt_on_error=1")
    for scenario in names:
        case_dir = work / scenario
        case_dir.mkdir()
        env["DTMAIL_TEST_DIR"] = str(case_dir)
        try:
            run = subprocess.run([str(binary), scenario], capture_output=True, text=True, env=env, timeout=60)
            code, err = run.returncode, run.stderr
        except subprocess.TimeoutExpired:
            code, err = -1, "timed out"
        if code:
            lines = [l for l in err.strip().splitlines() if l.strip()]
            problems.append("%s failed: %s" % (scenario, lines[0] if lines else "exit %d" % code))
        else:
            passed += 1
    print("ToolTalk request data: %d scenarios passed" % passed)

# the handler itself needs a session: guards
handler = block(r"^tooltalk_msg_handler\(\s*\n")
if handler is not None:
    h = code_of(handler)
    if re.search(r"isMboxOpen\(\s*file\s*\)", h.split('"Print"')[-1]):
        problems.append("the Print request opens the mailbox by the name in `file`, which is not set when the data came in a buffer")
    if not re.search(r"\(\s*NULL\s*==\s*file\s*\)\s*return\s+msg", h):
        problems.append("the Display request does not refuse a request that carries neither a buffer nor a file")
    if re.search(r"delete\s*\(\s*char\s*\*\s*\)", h):
        problems.append("a buffer from new[] is released with delete")
    if re.search(r"DtMailBuffer\s+mbuf\s*;", h) and "tooltalk_mail_data" not in h:
        problems.append("the Mail request uses a buffer that is not set when the request carries no data")
if re.search(r"\btempnam\s*\(", code_of(saver or "")):
    problems.append("the buffer of a request is saved under a name from tempnam()")
if problems:
    sys.exit("ToolTalk handler checks failed:\n  " + "\n  ".join(problems))
print("ToolTalk handler source guards: a request without data is refused; the Print request uses the file it saved")
