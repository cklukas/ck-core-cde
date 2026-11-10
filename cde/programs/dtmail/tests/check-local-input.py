#!/usr/bin/env python3
"""Run the code of dtmail that reads what the invoking user controls under the sanitizers.

dtmail may be installed set-group-id for the mail group.  The command line, the
resources, the session file and the files named for attachments are the
caller's, and a caller who can overflow a buffer from them runs code with the
saved group of the program.  This test cuts the production functions that read
a session file and name the header widgets out of dtmail/WMSaveSession.C and
dtmail/SendMsgDialog.C and runs them on input of every length; the places that
need a display (the font list, the display name, the messages about an
attachment) are guarded.  Every harness runs under AddressSanitizer and
UndefinedBehaviorSanitizer.

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


def read(relative):
    return (source / relative).read_text(errors="replace")


def block(text, pattern, required=True):
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


def code_of(text):
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)


def run_harness(name, code):
    with tempfile.TemporaryDirectory(prefix="dtmail-input-", dir=os.environ.get("TMPDIR")) as work:
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
#include <string>
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "failed: %s (line %d)\n", #condition, __LINE__); return 1; } } while (0)
struct Scenario { const char *name; int (*run)(); };
static int dispatch(int argc, char **argv, Scenario *scenarios)
{
    if (argc < 2) { for (int i = 0; scenarios[i].name; i++) puts(scenarios[i].name); return 0; }
    for (int i = 0; scenarios[i].name; i++) if (!strcmp(argv[1], scenarios[i].name)) return scenarios[i].run();
    return 2;
}
'''

# ---------------------------------------------------------------------------------------------------
# the words of a session file
session = read("dtmail/WMSaveSession.C")
reader = block(session, r"^static int\nreadSessionWord\(FILE \* fp, char \* word, size_t size\)\n", required=False)
if reader is None:
    problems.append("WMSaveSession.C has no readSessionWord(): the words of a session file are read with "
                    "fscanf() into buffers of a fixed size")
else:
    harness = COMMON + reader + r'''
static FILE *file_with(const std::string &content)
{
    FILE *fp = tmpfile();
    fwrite(content.data(), 1, content.size(), fp);
    rewind(fp);
    return fp;
}
static int words_are_read_one_by_one() {
    FILE *fp = file_with("RoamMenu  /var/mail/x\tws0*ws1\n 0 10 20 300 400\n");
    char w[64];
    CHECK(readSessionWord(fp, w, sizeof(w)) == 8 && !strcmp(w, "RoamMenu"));
    CHECK(readSessionWord(fp, w, sizeof(w)) == 11 && !strcmp(w, "/var/mail/x"));
    CHECK(readSessionWord(fp, w, sizeof(w)) == 7 && !strcmp(w, "ws0*ws1"));
    int a, b, c, d, e;
    CHECK(fscanf(fp, "%d%d%d%d%d", &a, &b, &c, &d, &e) == 5 && a == 0 && e == 400);
    CHECK(readSessionWord(fp, w, sizeof(w)) == -1);
    fclose(fp);
    return 0;
}
static int a_word_that_is_too_long_is_cut() {
    for (size_t n : {7, 8, 9, 63, 64, 65, 4096, 4097, 100000}) {
        std::string word(n, 'w');
        FILE *fp = file_with(word + " next\n");
        char *w = (char *) malloc(8);
        int len = readSessionWord(fp, w, 8);
        CHECK(len == (int) (n < 8 ? n : 7));
        CHECK(strlen(w) == (size_t) len && !strncmp(w, word.c_str(), len));
        free(w);
        char next[16];
        CHECK(readSessionWord(fp, next, sizeof(next)) == 4 && !strcmp(next, "next"));   /* the rest was skipped */
        fclose(fp);
    }
    return 0;
}
static int the_end_of_the_file_is_reported() {
    FILE *fp = file_with("");
    char w[8];
    CHECK(readSessionWord(fp, w, sizeof(w)) == -1);
    fclose(fp);
    fp = file_with("   \n\t \n");
    CHECK(readSessionWord(fp, w, sizeof(w)) == -1);
    fclose(fp);
    fp = file_with("last");
    CHECK(readSessionWord(fp, w, sizeof(w)) == 4 && !strcmp(w, "last"));
    CHECK(readSessionWord(fp, w, sizeof(w)) == -1);
    fclose(fp);
    return 0;
}
static int a_buffer_of_one_byte_holds_the_terminator() {
    FILE *fp = file_with("abc def");
    char w[1] = { 'x' };
    CHECK(readSessionWord(fp, w, 1) == 0 && w[0] == '\0');
    CHECK(readSessionWord(fp, w, 1) == 0 && w[0] == '\0');
    CHECK(readSessionWord(fp, w, 1) == -1);
    fclose(fp);
    return 0;
}
static Scenario scenarios[] = {
    {"words_are_read_one_by_one", words_are_read_one_by_one},
    {"a_word_that_is_too_long_is_cut", a_word_that_is_too_long_is_cut},
    {"the_end_of_the_file_is_reported", the_end_of_the_file_is_reported},
    {"a_buffer_of_one_byte_holds_the_terminator", a_buffer_of_one_byte_holds_the_terminator},
    {NULL, NULL}
};
int main(int argc, char **argv) { return dispatch(argc, argv, scenarios); }
'''
    print("Session file words: %d scenarios passed" % run_harness("sessionwords", harness))

# ---------------------------------------------------------------------------------------------------
# the names of the widgets of a header line
send = read("dtmail/SendMsgDialog.C")
namer = block(send, r"^static char \*\nheaderWidgetName\(const char \* prefix, const char \* label, const char \* suffix\)\n",
              required=False)
if namer is None:
    problems.append("SendMsgDialog.C has no headerWidgetName(): the names of the widgets of a header line "
                    "are built in a buffer of 50 bytes")
else:
    harness = COMMON + r'''
#include <DtMail/TextBuffer.hh>
''' + namer + r'''
static int names_of_every_length() {
    for (size_t n : {0, 1, 5, 43, 44, 45, 46, 49, 50, 51, 100, 5000, 100000}) {
        std::string label(n, 'h');
        char *a = headerWidgetName("form_", label.c_str(), "");
        char *b = headerWidgetName("", label.c_str(), ":");
        char *c = headerWidgetName("field_", label.c_str(), "");
        CHECK(std::string(a) == "form_" + label);
        CHECK(std::string(b) == label + ":");
        CHECK(std::string(c) == "field_" + label);
        delete [] a; delete [] b; delete [] c;
    }
    return 0;
}
static Scenario scenarios[] = { {"names_of_every_length", names_of_every_length}, {NULL, NULL} };
int main(int argc, char **argv) { return dispatch(argc, argv, scenarios); }
'''
    print("Header widget names: %d scenarios passed" % run_harness("widgetnames", harness))

# ---------------------------------------------------------------------------------------------------
# guards for what needs a display


def guard(relative, pattern, forbidden, why):
    fn = block(read(relative), pattern)
    if fn is None:
        return
    fn = code_of(fn)
    for expression in forbidden:
        if re.search(expression, fn):
            problems.append("%s: %s (%s)" % (relative, why, expression))


guard("dtmail/WMSaveSession.C", r"^RoamApp::restoreSession\(void\)\n", [r"\bfscanf\s*\("], "a session word is read with fscanf()")
guard("dtmail/WMSaveSession.C", r"^RoamMenuWindow::restoreSession\(char \*buf\)\n", [r"\bfscanf\s*\(\s*\w+\s*,\s*\"%s"],
      "a session word is read with fscanf()")
guard("dtmail/WMSaveSession.C", r"^SendMsgDialog::restoreSession\(char \*buf\)\n", [r"\bfscanf\s*\(\s*\w+\s*,\s*\"%s"],
      "a session word is read with fscanf()")
if re.search(r"\bfscanf\s*\([^;]*%s", code_of(session)):
    problems.append("dtmail/WMSaveSession.C: a string is read from the session file with fscanf() and %s")
if "S_IRGRP" in code_of(session) or "S_IWGRP" in code_of(session):
    problems.append("dtmail/WMSaveSession.C: the session file is made accessible to the group")

guard("dtmail/SendMsgDialog.C", r"^SendMsgDialog::createHeaders\(Widget header_form\)\n",
      [r"new char\s*\[\s*50\s*\]", r"\bstrcpy\s*\(", r"\bstrcat\s*\(", r"\bstrncat\s*\("],
      "a widget name is built in a buffer of 50 bytes")

roam = read("dtmail/RoamApp.C")
guard("dtmail/RoamApp.C", r"^void RoamApp::initialize\(int \*argcp, char \*\*argv\)\n",
      [r"\bstrcpy\s*\(\s*buf", r"\bstrcat\s*\(\s*buf", r"new char\s*\[\s*1024\s*\]", r"displayenv\s*\[\s*256\s*\]"],
      "a name given on the command line is copied into a buffer of a fixed size")

attach = read("dtmail/AttachArea.C")
add = block(attach, r"^AttachArea::addAttachment\(\n\t\t\t  DtMail::Message\* msg,\n\t\t\t  DtMail::BodyPart \*lastAttBP,\n\t\t\t  char \*filename,\n\t\t\t  char \*name\n")
if add is not None:
    add = code_of(add)
    for expression in (r"new char\s*\[\s*512\s*\]", r"new char\s*\[\s*2048\s*\]", r"(?<!n)\bsprintf\s*\("):
        if re.search(expression, add):
            problems.append("dtmail/AttachArea.C: the messages about an attachment file are built in buffers of a fixed size (%s)" % expression)

if problems:
    sys.exit("local input checks failed:\n  " + "\n  ".join(problems))
print("Local input source guards: no fixed buffer for a font name, a display name, a session word, a header label or an attachment path")
