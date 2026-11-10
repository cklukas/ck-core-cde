#!/usr/bin/env python3
"""Run the code that builds text from the headers of a received message under the sanitizers.

dtmail and dtmailpr put the addresses, the date and the subject of a message into
the headers of a reply, into the text that "Save As Text" and the print commands
write, and into the name of a print file.  Those strings come from the sender, and
the buffers that held them were sized by a guess.  This test cuts the production
functions out of dtmail/ComposeCmds.C and dtmail/DmxUtils.C (and the copy in
dtmailpr/utils.C) and runs them on headers of every length; it runs the
TextBuffer class that the other places use; and it guards the places that cannot
be run without a display.  Every harness runs under AddressSanitizer and
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


def run_harness(name, code):
    """Compile and run one harness under the sanitizers; returns the number of scenarios that passed."""
    with tempfile.TemporaryDirectory(prefix="dtmail-text-", dir=os.environ.get("TMPDIR")) as work:
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
'''

# ---------------------------------------------------------------------------------------------------
# TextBuffer and copyBounded
text_buffer = source / "include" / "DtMail" / "TextBuffer.hh"
if not text_buffer.exists():
    problems.append("include/DtMail/TextBuffer.hh does not exist")
else:
    harness = COMMON + r'''
#include <DtMail/TextBuffer.hh>

static int empty_buffer_is_an_empty_string()
{
    TextBuffer t;
    CHECK(t.length() == 0 && strcmp(t.text(), "") == 0);
    char *copy = t.release();
    CHECK(copy[0] == '\0');
    delete [] copy;
    return 0;
}
static int appends_grow_without_a_limit()
{
    TextBuffer t;
    std::string expected;
    for (int i = 0; i < 5000; i++) {
        std::string piece(i % 97 + 1, 'a' + i % 26);
        CHECK(t.append(piece.c_str()));
        expected += piece;
    }
    CHECK(t.length() == expected.size());
    CHECK(strcmp(t.text(), expected.c_str()) == 0);
    char *copy = t.release();
    CHECK(strcmp(copy, expected.c_str()) == 0);
    delete [] copy;
    return 0;
}
static int one_huge_piece_fits()
{
    TextBuffer t;
    std::string big(3000000, 'x');
    CHECK(t.append(big.c_str()));
    CHECK(t.append("tail"));
    CHECK(t.length() == big.size() + 4);
    CHECK(strcmp(t.text() + big.size(), "tail") == 0);
    return 0;
}
static int append_of_part_of_a_string_stops_at_the_length()
{
    TextBuffer t;
    CHECK(t.append("abcdef", 3));
    CHECK(strcmp(t.text(), "abc") == 0);
    CHECK(t.append("xyz", 0));
    CHECK(strcmp(t.text(), "abc") == 0);
    return 0;
}
static int impossible_size_is_refused_and_leaves_the_text()
{
    TextBuffer t;
    CHECK(t.append("kept"));
    CHECK(!t.append("x", (size_t) -1));
    CHECK(!t.append("x", (size_t) -1 - 3));
    CHECK(strcmp(t.text(), "kept") == 0 && t.length() == 4);
    return 0;
}
static int bounded_copy_fits_or_cuts()
{
    char buf[8];
    memset(buf, 'z', sizeof(buf));
    CHECK(copyBounded(buf, sizeof(buf), "1234567") && strcmp(buf, "1234567") == 0);
    CHECK(!copyBounded(buf, sizeof(buf), "12345678") && strcmp(buf, "1234567") == 0);
    CHECK(!copyBounded(buf, sizeof(buf), std::string(10000, 'q').c_str()) && strlen(buf) == 7);
    CHECK(copyBounded(buf, sizeof(buf), "") && buf[0] == '\0');
    CHECK(!copyBounded(buf, 0, "x"));
    char one[1] = { 'z' };
    CHECK(!copyBounded(one, 1, "x") && one[0] == '\0');
    return 0;
}
static Scenario scenarios[] = {
    {"empty_buffer_is_an_empty_string", empty_buffer_is_an_empty_string},
    {"appends_grow_without_a_limit", appends_grow_without_a_limit},
    {"one_huge_piece_fits", one_huge_piece_fits},
    {"append_of_part_of_a_string_stops_at_the_length", append_of_part_of_a_string_stops_at_the_length},
    {"impossible_size_is_refused_and_leaves_the_text", impossible_size_is_refused_and_leaves_the_text},
    {"bounded_copy_fits_or_cuts", bounded_copy_fits_or_cuts},
    {NULL, NULL}
};
int main(int argc, char **argv) { return dispatch(argc, argv, scenarios); }
'''
    print("TextBuffer: %d scenarios passed" % run_harness("textbuffer", harness))

# ---------------------------------------------------------------------------------------------------
# the address list and the subject of a reply (ComposeCmds.C)
compose = read("dtmail/ComposeCmds.C")
to_addr = block(compose, r"^char \*\nComposeFamily::valueToAddrString\(DtMailValueSeq & value\)\n")
subject_fn = block(compose, r"^static char \*\nreplySubjectWithoutOriginal\(const char \* sent\)\n", required=False)
if subject_fn is None:
    problems.append("ComposeCmds.C has no replySubjectWithoutOriginal(): the subject of a reply to a message "
                    "without one is built in a buffer of a fixed size")

if to_addr is not None:
    harness = COMMON + r'''
#if __has_include(<DtMail/TextBuffer.hh>)
#include <DtMail/TextBuffer.hh>
#endif
typedef int DtMailBoolean;
#define DTM_TRUE 1
#define DTM_FALSE 0

/* the parts of the DtMail interface the function uses */
struct DtMailValueAddress { const char *dtm_address; };
struct DtMailAddressSeq {
    std::vector<DtMailValueAddress *> items;
    ~DtMailAddressSeq() { for (auto *a : items) { free((void *) a->dtm_address); delete a; } }
    int length() { return (int) items.size(); }
    DtMailValueAddress *&operator[](int i) { return items[i]; }
};
static bool parser_emits_commas;      /* the shortcoming the function works around */
struct DtMailValue {
    std::string text;
    operator const char *() const { return text.c_str(); }
    DtMailAddressSeq *toAddress() {
        DtMailAddressSeq *seq = new DtMailAddressSeq;
        size_t start = 0;
        for (;;) {
            size_t comma = text.find(',', start);
            std::string token = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!token.empty()) seq->items.push_back(new DtMailValueAddress{strdup(token.c_str())});
            if (comma == std::string::npos) break;
            if (parser_emits_commas) seq->items.push_back(new DtMailValueAddress{strdup(",")});
            start = comma + 1;
        }
        return seq;
    }
};
struct DtMailValueSeq {
    std::vector<DtMailValue *> items;
    ~DtMailValueSeq() { for (auto *v : items) delete v; }
    void add(const std::string &t) { items.push_back(new DtMailValue{t}); }
    int length() { return (int) items.size(); }
    DtMailValue *operator[](int i) { return items[i]; }
};
struct ComposeFamily { char *valueToAddrString(DtMailValueSeq &value); };

''' + (subject_fn or "") + to_addr + r'''

static std::string join(int n, const char *piece, const char *separator)
{
    std::string s;
    for (int i = 0; i < n; i++) { if (i) s += separator; s += piece; }
    return s;
}
static std::string convert(DtMailValueSeq &seq)
{
    ComposeFamily f;
    char *out = f.valueToAddrString(seq);
    std::string result = out;
    delete [] out;
    return result;
}

static int one_address() { DtMailValueSeq s; s.add("a@example.org"); CHECK(convert(s) == "a@example.org"); return 0; }
static int no_address() { DtMailValueSeq s; CHECK(convert(s) == ""); return 0; }
static int several_values_are_joined() {
    DtMailValueSeq s; s.add("a@x"); s.add("b@x"); s.add("c@x");
    CHECK(convert(s) == "a@x, b@x, c@x");
    return 0;
}
static int a_list_in_one_header_is_rejoined() {
    DtMailValueSeq s; s.add("a@x,b@x,c@x");
    CHECK(convert(s) == "a@x, b@x, c@x");
    return 0;
}
static int a_long_list_in_one_header_fits() {
    /* every "," becomes ", ": the result is longer than the header it came from */
    for (int n : {2, 3, 10, 100, 1000, 20000}) {
        DtMailValueSeq s; s.add(join(n, "a", ","));
        CHECK(convert(s) == join(n, "a", ", "));
    }
    return 0;
}
static int many_headers_with_long_lists_fit() {
    DtMailValueSeq s;
    for (int i = 0; i < 50; i++) s.add(join(200, "xy", ","));
    CHECK(convert(s) == join(50 * 200, "xy", ", "));
    return 0;
}
static int separator_tokens_of_the_parser_are_skipped() {
    parser_emits_commas = true;
    DtMailValueSeq s; s.add(join(500, "a", ","));
    CHECK(convert(s) == join(500, "a", ", "));
    parser_emits_commas = false;
    return 0;
}
static Scenario scenarios[] = {
    {"one_address", one_address},
    {"no_address", no_address},
    {"several_values_are_joined", several_values_are_joined},
    {"a_list_in_one_header_is_rejoined", a_list_in_one_header_is_rejoined},
    {"a_long_list_in_one_header_fits", a_long_list_in_one_header_fits},
    {"many_headers_with_long_lists_fit", many_headers_with_long_lists_fit},
    {"separator_tokens_of_the_parser_are_skipped", separator_tokens_of_the_parser_are_skipped},
    {NULL, NULL}
};
int main(int argc, char **argv) { return dispatch(argc, argv, scenarios); }
'''
    print("Address list of a reply: %d scenarios passed" % run_harness("addresslist", harness))

if subject_fn is not None:
    harness = COMMON + r'''
#include <DtMail/TextBuffer.hh>
''' + subject_fn + r'''
static std::string subject(const char *sent) { char *s = replySubjectWithoutOriginal(sent); std::string r = s; delete [] s; return r; }

static int no_time_is_named() { CHECK(subject(NULL) == "Re: Your Message"); return 0; }
static int the_time_is_named() {
    CHECK(subject("Mon, 9 Nov 2026 10:00:00 +0000") == "Re: Your Message Sent on Mon, 9 Nov 2026 10:00:00 +0000");
    return 0;
}
static int a_long_date_fits() {
    for (size_t n : {0, 1, 174, 175, 176, 199, 200, 201, 5000, 1000000}) {
        std::string date(n, 'd');
        CHECK(subject(date.c_str()) == "Re: Your Message Sent on " + date);
    }
    return 0;
}
static int a_date_stays_one_line() {
    CHECK(subject("Mon\r\nBcc: someone@example.org") == "Re: Your Message Sent on Mon");
    CHECK(subject("Mon\nBcc: x") == "Re: Your Message Sent on Mon");
    CHECK(subject("Mon\tTue") == "Re: Your Message Sent on Mon");
    return 0;
}
static Scenario scenarios[] = {
    {"no_time_is_named", no_time_is_named},
    {"the_time_is_named", the_time_is_named},
    {"a_long_date_fits", a_long_date_fits},
    {"a_date_stays_one_line", a_date_stays_one_line},
    {NULL, NULL}
};
int main(int argc, char **argv) { return dispatch(argc, argv, scenarios); }
'''
    print("Subject of a reply: %d scenarios passed" % run_harness("replysubject", harness))

# ---------------------------------------------------------------------------------------------------
# phrase(): the display name of an address (two copies)
for relative in ("dtmail/DmxUtils.C", "dtmailpr/utils.C"):
    phrase = block(read(relative), r"^static char \*\nphrase\(char \*name, int token, int comma\)\n")
    if phrase is None:
        continue
    harness = COMMON + r'''
#define NOSTR ((char *) 0)
#define LINESIZE 5120
static char *savestr(const char *s) { return strdup(s); }
#define equal(a, b) (strcmp(a, b) == 0)
''' + phrase + r'''
static int run_phrase(const std::string &in, int token = 0, int comma = 0)
{
    std::string copy = in;
    char *name = strdup(copy.c_str());
    char *out = phrase(name, token, comma);
    (void) out;
    free(name);
    return 0;
}
static int plain_names_of_every_length() {
    for (size_t n : {0, 1, 100, 5118, 5119, 5120, 5121, 5122, 10000, 100000}) {
        std::string in(n, 'a');
        char *name = strdup(in.c_str());
        char *out = phrase(name, 0, 0);
        CHECK(out != NULL && strcmp(out, in.c_str()) == 0);
        free(name);
    }
    return 0;
}
static int quoted_and_bracketed_names_of_every_length() {
    for (size_t n : {10, 5000, 5119, 5120, 5121, 9000, 50000}) {
        run_phrase("\"" + std::string(n, 'q') + "\" <a@b>");
        run_phrase(std::string(n, 'q') + " <a@b>");
        run_phrase("a@b (" + std::string(n, 'c') + ")");
        run_phrase(std::string(n, ',') + "<");
        run_phrase(std::string(n / 2, ',') + std::string(n / 2, '<'));
        run_phrase(std::string(n, '\"'));
        run_phrase(std::string(n, '('));
        run_phrase("<" + std::string(n, '>'));
    }
    return 0;
}
static int tokens_of_every_length() {
    for (size_t n : {5119, 5120, 5121, 20000}) {
        run_phrase(std::string(n, 'a') + " b", 1, 0);
        run_phrase(std::string(n, 'a') + ", b", 1, 1);
    }
    return 0;
}
static Scenario scenarios[] = {
    {"plain_names_of_every_length", plain_names_of_every_length},
    {"quoted_and_bracketed_names_of_every_length", quoted_and_bracketed_names_of_every_length},
    {"tokens_of_every_length", tokens_of_every_length},
    {NULL, NULL}
};
int main(int argc, char **argv) { return dispatch(argc, argv, scenarios); }
'''
    print("phrase() of %s: %d scenarios passed" % (relative, run_harness("phrase" + relative.replace("/", "_").replace(".", "_"), harness)))

# ---------------------------------------------------------------------------------------------------
# The places that need a display or a mailbox: source guards.
def body_without_comments(text):
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)


def guard(relative, pattern, forbidden, why):
    text = read(relative)
    fn = block(text, pattern)
    if fn is None:
        return
    fn = body_without_comments(fn)
    for expression in forbidden:
        if re.search(expression, fn):
            problems.append("%s: %s (%s)" % (relative, why, expression))


compose_code = body_without_comments(compose)
if re.search(r"new char\[200\]", compose_code):
    problems.append("dtmail/ComposeCmds.C: the subject of a reply is built in a buffer of 200 bytes")

guard("dtmail/DmxMessage.C", r"^DmxMsg::getHeaders \(DtMailBoolean abbreviated_only\)\n",
      [r"\bstrcat\s*\(", r"\bbuffer_size\b"], "the text of the headers is built in a buffer that is doubled once")
guard("dtmail/DmxMessage.C", r"^DmxMsg::display \(\n",
      [r"\bstrcpy\s*\(\s*v3_cs", r"\bstrcat\s*\(\s*v3_cs"], "the charset name is copied without a bound")
guard("dtmailpr/message.C", r"^DmxMsg::display \(void\)\n",
      [r"\bstrcpy\s*\(\s*v3_cs", r"\bstrcat\s*\(\s*v3_cs", r"v3_cs\s*=\s*new"], "the charset name is copied without a bound")
guard("dtmailpr/message.C", r"^DmxMsg::printHeader \(enum DmxHeaderType htype\)\n",
      [r"\bsprintf\s*\(", r"new char \[1024\]"], "the header line is built in a buffer of 1024 bytes")
printjob = body_without_comments(read("dtmail/DmxPrintJob.C"))
if re.search(r"\bsprintf\s*\(\s*filename", printjob):
    problems.append("dtmail/DmxPrintJob.C: the name of the print file is built with sprintf()")

for relative, array, limit in (("dtmail/Dmx.h", r"_messages\s*\[\s*(\w+)\s*\]", "DMXMAXMESSAGES"),
                               ("dtmailpr/dmx.hh", r"\bmsg\s*\[\s*(\w+)\s*\]", "DMXMAXMESSAGES")):
    header = body_without_comments(read(relative))
    match = re.search(array, header)
    if not match or match.group(1) != limit:
        problems.append("%s: the array of messages is not sized by %s" % (relative, limit))
for relative in ("dtmail/DmxMailbox.C", "dtmailpr/mailbox.C"):
    if "DMXMAXMESSAGES" not in body_without_comments(read(relative)):
        problems.append("%s: loadMessages() does not stop at the end of the array of messages" % relative)

if problems:
    sys.exit("message text checks failed:\n  " + "\n  ".join(problems))
print("Message text source guards: no fixed buffer for header text, charsets, print file names or message arrays")
