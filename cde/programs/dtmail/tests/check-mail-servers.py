#!/usr/bin/env python3
"""Run the code of the mail server clients that handles what a server sends under the sanitizers.

A POP or IMAP server answers with lines it chooses, and the mailer builds commands
from names that the user configured.  This test cuts the production helpers out of
the sources and runs them on every shape of that text (empty, at a boundary, too
long, with bytes that end a line early) under AddressSanitizer and
UndefinedBehaviorSanitizer:

  * the line helpers, the command formatter, the transaction tag and the format that
    reads a UIDL line of libDtMail/Common (DtMailServer.C and the header);
  * the ctime wrapper of libDtMail/Common/IO.C.

The clients themselves are run against a server of their own by
libDtMail/tests/mailbox-files.C.

DTMAIL_SOURCE names another copy of the dtmail sources to test, for example one with a
deliberate defect.  DTMAIL_REGRESS names functions whose old defect is put back into
the code that is run (a blank separated list of: chomp, format, tag), which checks
that the harness notices it.
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
regress = os.environ.get("DTMAIL_REGRESS", "").split()
problems = []


def read(relative):
    path = source / relative
    if not path.exists():
        problems.append("missing file: " + relative)
        return ""
    return path.read_text(errors="replace")


def block(text, pattern, required=True):
    """The text of the function whose head matches pattern, up to its closing brace."""
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


def put_back(name, text, old, new):
    """Reintroduce an old defect into the extracted text when the run asks for it."""
    if name not in regress or text is None:
        return text
    if old not in text:
        problems.append("regression %s does not apply" % name)
        return text
    return text.replace(old, new)


def run_harness(name, code):
    """Compile and run one harness under the sanitizers; returns the number of scenarios that passed."""
    with tempfile.TemporaryDirectory(prefix="dtmail-parsers-", dir=os.environ.get("TMPDIR")) as work:
        work = Path(work)
        cc = work / (name + ".cc")
        cc.write_text(code)
        binary = work / name
        build = cxx + ["-std=gnu++17", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                       "-fno-omit-frame-pointer", "-Wall", "-Wno-unused-function", "-Wno-unused-variable",
                       "-I", str(source / "include"), str(cc), "-o", str(binary)]
        compiled = subprocess.run(build, capture_output=True, text=True)
        if compiled.returncode:
            sys.stderr.write(compiled.stderr)
            problems.append("cannot compile the %s harness" % name)
            return 0
        names = subprocess.run([str(binary)], capture_output=True, text=True, check=True).stdout.split()
        passed = 0
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=0", UBSAN_OPTIONS="halt_on_error=1")
        for scenario in names:
            run = subprocess.run([str(binary), scenario], capture_output=True, text=True, env=env)
            if run.returncode:
                lines = [l for l in run.stderr.strip().splitlines() if l.strip()]
                problems.append("%s: %s failed: %s" % (name, scenario, lines[0] if lines else "exit %d" % run.returncode))
            else:
                passed += 1
        return passed


COMMON = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <strings.h>
#include <time.h>
#include <string>
#include <vector>
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "failed: %s (line %d)\n", #condition, __LINE__); return 1; } } while (0)
struct Scenario { const char *name; int (*run)(); };
static int dispatch(int argc, char **argv, Scenario *scenarios)
{
    if (argc < 2) { for (int i = 0; scenarios[i].name; i++) puts(scenarios[i].name); return 0; }
    for (int i = 0; scenarios[i].name; i++) if (!strcmp(argv[1], scenarios[i].name)) return scenarios[i].run();
    return 2;
}
/* a heap copy of exactly the given bytes, so that a read past either end is a sanitizer report */
static char *exact(const std::string &text)
{
    char *copy = (char *)malloc(text.size() ? text.size() : 1);
    if (text.size()) memcpy(copy, text.data(), text.size());
    return copy;
}
'''

# ---------------------------------------------------------------------------------------------------
# Line helpers, UIDL format and transaction tag of the mail server clients
header = read("include/DtMail/DtMailServer.hh")
server = read("libDtMail/Common/DtMailServer.C")
chomp = block(header, r"^static inline void\ndtmasChomp\(")
prefix_fn = block(header, r"^static inline const char \*\ndtmasAfterPrefix\(")
chomp = put_back("chomp", chomp, "if (length > 0 && line[length - 1] == '\\n')", "if (line[length - 1] == '\\n')")
idsize = re.search(r"^#define\s+DTMAS_IDSIZE\s+(\d+)", header, re.M)
stringify = re.findall(r"^#define\s+DTMAS_STRINGIFY_?\(x\).*$", header, re.M)
uidl = re.search(r"^#define\s+DTMAS_UIDL_FORMAT\s+.*$", header, re.M)
tag_lines = re.findall(r"^#define dtmasTAG(?:CLR|GEN|GET)\(\).*$", server, re.M)
format_fn = block(server, r"^static DTMailError_t\nformatServerCommand\(")
format_fn = put_back("format", format_fn, 'if (strpbrk(buf + used, "\\r\\n") != NULL)', "if (0)")
tag_text = "\n".join(tag_lines)
tag_text = put_back("tag", tag_text, "_transnum % 9999 + 1", "_transnum + 1")
if not (chomp and prefix_fn and idsize and len(stringify) == 2 and uidl and len(tag_lines) == 3 and format_fn):
    problems.append("the mail server client helpers were not found")
else:
    harness = COMMON + "\n#define DTMAS_IDSIZE " + idsize.group(1) + "\n#define DTMAS_TAGSIZE 6\n" + "\n".join(stringify) + "\n" \
        + uidl.group(0) + "\n" + chomp + prefix_fn + r'''
typedef int DTMailError_t;
enum { DTME_NoError = 0, DTME_MailServerAccess_Error = 1 };
#define DTMAS_COMMAND_TERMINATOR "\r\n"
''' + format_fn + r'''
''' + "\n" + r'''
static std::string chomped(const std::string &line)
{
    char *copy = exact(line + std::string(1, '\0'));   // the terminating NUL is part of the allocation
    dtmasChomp(copy);
    std::string result(copy);
    free(copy);
    return result;
}
static int chomp_handles_every_line_ending()
{
    CHECK(chomped("") == "");
    CHECK(chomped("\n") == "");
    CHECK(chomped("\r\n") == "");
    CHECK(chomped("\r") == "");
    CHECK(chomped("+OK ready") == "+OK ready");
    CHECK(chomped("+OK ready\n") == "+OK ready");
    CHECK(chomped("+OK ready\r\n") == "+OK ready");
    CHECK(chomped("+OK ready\r") == "+OK ready");
    CHECK(chomped("two\n\n") == "two\n");
    CHECK(chomped("\n\r\n") == "\n");
    return 0;
}
static int prefix_stays_inside_the_line()
{
    const char *line;
    char *copy = exact(std::string("* 4 EXISTS") + std::string(1, '\0'));
    line = dtmasAfterPrefix(copy, 2);
    CHECK(strcmp(line, "4 EXISTS") == 0);
    free(copy);
    for (const char *text : {"", "*", "* "}) {
        copy = exact(std::string(text) + std::string(1, '\0'));
        line = dtmasAfterPrefix(copy, 2);
        CHECK(*line == '\0');
        free(copy);
    }
    return 0;
}
static int uidl_identifier_is_cut_to_its_buffer()
{
    // a server may answer with an identifier of any length
    for (size_t length : {1u, 70u, 127u, 128u, 129u, 400u, 511u}) {
        std::string reply = "12 " + std::string(length, 'u');
        char *line = exact(reply + std::string(1, '\0'));
        char id[DTMAS_IDSIZE + 1];
        int number = -1;
        CHECK(sscanf(line, DTMAS_UIDL_FORMAT, &number, id) == 2);
        CHECK(number == 12);
        CHECK(strlen(id) == (length < (size_t)DTMAS_IDSIZE ? length : (size_t)DTMAS_IDSIZE));
        free(line);
    }
    char id[DTMAS_IDSIZE + 1];
    int number = 0;
    CHECK(sscanf("7 abc", DTMAS_UIDL_FORMAT, &number, id) == 2 && number == 7 && !strcmp(id, "abc"));
    CHECK(sscanf("not a number", DTMAS_UIDL_FORMAT, &number, id) != 2);
    return 0;
}
static std::string formatted(size_t size, const char *prefix, bool *refused, const char *fmt, ...)
{
    char *buf = (char *)malloc(size);
    va_list ap;
    va_start(ap, fmt);
    DTMailError_t status = formatServerCommand(buf, size, prefix, fmt, ap);
    va_end(ap);
    *refused = status != DTME_NoError;
    std::string result = *refused ? "" : std::string(buf);
    free(buf);
    return result;
}
static int command_is_formatted_with_tag_and_terminator()
{
    bool refused;
    CHECK(formatted(513, "", &refused, "USER %s", "bob") == "USER bob\r\n" && !refused);
    CHECK(formatted(513, "a0007 ", &refused, "SELECT %s", "INBOX") == "a0007 SELECT INBOX\r\n" && !refused);
    CHECK(formatted(513, "", &refused, "STAT") == "STAT\r\n" && !refused);
    return 0;
}
static int command_that_does_not_fit_is_refused_at_every_size()
{
    // "a0001 " + "USER bob" + CRLF is 16 bytes and needs 17 with the NUL
    for (size_t size = 1; size < 40; size++) {
        bool refused;
        std::string result = formatted(size, "a0001 ", &refused, "USER %s", "bob");
        CHECK(refused == (size < 17));
        if (!refused) CHECK(result == "a0001 USER bob\r\n");
    }
    bool refused;
    std::string name(5000, 'n');
    formatted(513, "", &refused, "USER %s", name.c_str());
    CHECK(refused);
    return 0;
}
static int line_break_in_an_argument_is_refused()
{
    // the text after a line break would be taken for a second command
    bool refused;
    formatted(513, "", &refused, "USER %s", "bob\r\nDELE 1");
    CHECK(refused);
    formatted(513, "a0001 ", &refused, "SELECT %s", "INBOX\nDELETE all");
    CHECK(refused);
    formatted(513, "", &refused, "PASS %s", "secret\r");
    CHECK(refused);
    formatted(513, "", &refused, "PASS %s", "secret");
    CHECK(!refused);
    return 0;
}
static int tag_stays_inside_its_buffer_for_ever()
{
    // the tag is the only thing in its allocation, so a byte too many is a report
    char *tagbuf = (char *)malloc(DTMAS_TAGSIZE);
    unsigned counter = 0;
    #define _transtag tagbuf
    #define _transnum counter
    ''' + tag_text + r'''
    dtmasTAGCLR();
    for (int i = 0; i < 30000; i++) {
        dtmasTAGGEN();
        CHECK(strlen(dtmasTAGGET()) == 5 && dtmasTAGGET()[0] == 'a');
        for (int d = 1; d < 5; d++) CHECK(isdigit((unsigned char)dtmasTAGGET()[d]));
        CHECK(counter >= 1 && counter <= 9999);
    }
    #undef _transtag
    #undef _transnum
    free(tagbuf);
    return 0;
}
static Scenario scenarios[] = {
    {"chomp_handles_every_line_ending", chomp_handles_every_line_ending},
    {"prefix_stays_inside_the_line", prefix_stays_inside_the_line},
    {"uidl_identifier_is_cut_to_its_buffer", uidl_identifier_is_cut_to_its_buffer},
    {"command_is_formatted_with_tag_and_terminator", command_is_formatted_with_tag_and_terminator},
    {"command_that_does_not_fit_is_refused_at_every_size", command_that_does_not_fit_is_refused_at_every_size},
    {"line_break_in_an_argument_is_refused", line_break_in_an_argument_is_refused},
    {"tag_stays_inside_its_buffer_for_ever", tag_stays_inside_its_buffer_for_ever},
    {NULL, NULL}
};
int main(int argc, char **argv) { return dispatch(argc, argv, scenarios); }
'''
    print("server client helpers: %d scenarios passed" % run_harness("server", harness))

# ---------------------------------------------------------------------------------------------------
# The ctime wrapper of IO.C
iocode = read("libDtMail/Common/IO.C")
ctime_fn = block(iocode, r"^void\nSafeCtime\(")
if ctime_fn is None:
    problems.append("SafeCtime was not found")
else:
    harness = COMMON + r'''
struct _Xctimeparams { char unused; };
static const char *ctime_text = "Tue Nov 10 12:16:39 2026\n";
static char *_XCtime(const time_t *, _Xctimeparams &) { return (char *)ctime_text; }
''' + ctime_fn + r'''
static int ctime_text_ends_inside_the_buffer()
{
    for (int size = 1; size <= 40; size++) {
        char *buf = (char *)malloc(size);
        memset(buf, 'X', size);
        SafeCtime(NULL, buf, size);
        size_t length = strnlen(buf, size);
        CHECK(length < (size_t)size);                           // there is a NUL inside
        CHECK(length == (size_t)(size - 1 < 25 ? size - 1 : 25));
        free(buf);
    }
    char untouched = 'U';
    SafeCtime(NULL, &untouched, 0);
    CHECK(untouched == 'U');
    return 0;
}
static Scenario scenarios[] = {
    {"ctime_text_ends_inside_the_buffer", ctime_text_ends_inside_the_buffer},
    {NULL, NULL}
};
int main(int argc, char **argv) { return dispatch(argc, argv, scenarios); }
'''
    print("ctime wrapper: %d scenarios passed" % run_harness("ctime", harness))

# ---------------------------------------------------------------------------------------------------
# Places that cannot be run without a mail server


def code_of(relative):
    text = read(relative)
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def guard(relative, patterns, why):
    text = code_of(relative)
    for pattern in patterns:
        if re.search(pattern, text):
            problems.append("%s: %s (%s)" % (relative, why, pattern))


for client in ("POP3Server.C", "IMAPServer.C", "POP2Server.C"):
    guard("libDtMail/Common/" + client, [r"buf\s*\[\s*strlen\s*\(\s*buf\s*\)\s*-\s*1\s*\]"],
          "the end of a server line is cut off by reading the byte before an empty line")
guard("libDtMail/Common/POP3Server.C", [r"sscanf\s*\([^;]*\"%d %s\""], "an identifier from the server is read with an unbounded %s")
guard("libDtMail/Common/DtMailServer.C", [r"\bvsprintf\s*\(", r"\bstrcat\s*\(\s*buf"], "a command is assembled without a bound")
guard("libDtMail/Common/DtMailServer.C", [r"sprintf\s*\(\s*buffer"], "the first line of a message is built without a bound")
guard("libDtMail/Common/IO.C", [r"\bvsprintf\s*\(", r"\bsprintf\s*\(\s*\*errorstring", r"\(int\)\s*sockfp"],
      "a socket function formats without a bound or casts a pointer to an int")
if "MD5Final" not in code_of("libDtMail/Common/APOPServer.C"):
    problems.append("libDtMail/Common/APOPServer.C: the digest of the APOP challenge is never finished")
if "formatServerCommand" not in read("libDtMail/Common/DtMailServer.C"):
    problems.append("libDtMail/Common/DtMailServer.C no longer has formatServerCommand")

if problems:
    sys.exit("mail server checks failed:\n  " + "\n  ".join(problems))
print("Mail server checks: lines, commands, tags, identifiers and sockets are bounded")
