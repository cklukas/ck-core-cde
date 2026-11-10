#!/usr/bin/env python3
"""Run the code that builds text from the structure of a message and from the environment under the sanitizers.

The parameters of a Content-Type header, the enriched text of a body, the addresses of
a header, the character set of the locale and the length of a path decide how much
text libDtMail builds.  This test cuts the production functions out of the sources
and runs them on every shape of that data (empty, at a boundary, too long, with bytes
that end a line early) under AddressSanitizer and UndefinedBehaviorSanitizer:

  * the parameter reader and the growing text of message/partial assembly;
  * the enriched text converter;
  * the Unix "From " line, the temporary name of a lock file and the release of the
    .dtmail lock;
  * the address list reader.

The assembly of message/partial parts is run on real mailboxes by
libDtMail/tests/mailbox-files.C.

DTMAIL_SOURCE names another copy of the dtmail sources to test, for example one with a
deliberate defect.  DTMAIL_REGRESS names functions whose old defect is put back into
the code that is run (enriched), which checks that the harness notices it.
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
# Parameters and text of message/partial assembly
partial = read("libDtMail/RFC/MIMEPartial.C")
consts = re.findall(r"^static const (?:unsigned int|size_t)\s+MaxPartial\w+\s*=[^;]*;", partial, re.M)
klass = re.search(r"^class PartialText\n\{.*?^\};\n", partial, re.M | re.S)
append_fn = block(partial, r"^bool\nPartialText::append\(const char \* text, size_t length\)")
release_fn = block(partial, r"^char \*\nPartialText::release\(\)")
value_fn = block(partial, r"^static char\t\*\ngetNamedValueString\(")
int_fn = block(partial, r"^static unsigned int\ngetNamedValueInt\(")
blank_fn = block(partial, r"^static const char \*\nfindBlankLine\(")
if not (len(consts) == 2 and klass and append_fn and release_fn and value_fn and int_fn and blank_fn):
    problems.append("the message/partial helpers were not found")
else:
    limit_text = "\n".join(consts)
    small = re.sub(r"(MaxPartialMessage\s*=)[^;]*;", r"\1 1024 * 1024;", limit_text)
    if small == limit_text:
        problems.append("MIMEPartial.C: the size limit of a message was not found")
    harness = COMMON + small + "\n" + klass.group(0) + append_fn + release_fn + value_fn + int_fn + blank_fn + r'''
static std::string parameter(const std::string &header, const char *name, bool *found = NULL)
{
    char *copy = exact(header + std::string(1, '\0'));
    char *value = getNamedValueString(copy, name);
    if (found) *found = value != NULL;
    std::string result = value ? value : "";
    free(value);
    free(copy);
    return result;
}
static unsigned number(const std::string &header, const char *name)
{
    char *copy = exact(header + std::string(1, '\0'));
    unsigned value = getNamedValueInt(copy, name);
    free(copy);
    return value;
}
static int parameters_are_read_by_name()
{
    const std::string h = "message/partial; id=\"abc@host\"; number=2; total=3";
    CHECK(parameter(h, "id") == "abc@host");
    CHECK(number(h, "number") == 2);
    CHECK(number(h, "total") == 3);
    CHECK(parameter("Message/Partial;ID=x;NUMBER=4", "id") == "x");
    CHECK(number("Message/Partial;ID=x;NUMBER=4", "number") == 4);
    CHECK(parameter("message/partial; id = \"a b\" ; number = 5 ", "id") == "a b");
    CHECK(number("message/partial; id = \"a b\" ; number = 5 ", "number") == 5);
    CHECK(parameter("message/partial; id=\"a\\\"b\"; number=1", "id") == "a\"b");
    return 0;
}
static int a_name_inside_another_value_is_not_a_parameter()
{
    // the id says "number=9", the real number follows
    const std::string h = "message/partial; id=\"number=9\"; number=2; total=5";
    CHECK(number(h, "number") == 2);
    CHECK(number(h, "total") == 5);
    // "total" as the tail of another name does not count
    CHECK(number("message/partial; subtotal=7; number=1", "total") == 0);
    CHECK(number("message/partial; number=1; xnumber=8", "number") == 1);
    // the type itself is not a parameter
    CHECK(number("total=3", "total") == 0);
    return 0;
}
static int missing_or_malformed_parameters_are_absent()
{
    bool found;
    parameter("message/partial", "id", &found);
    CHECK(!found);
    parameter("message/partial;", "id", &found);
    CHECK(!found);
    parameter("message/partial; id", "id", &found);
    CHECK(!found);
    parameter("message/partial; id=", "id", &found);
    CHECK(found);
    parameter("", "id", &found);
    CHECK(!found);
    parameter(";;;;", "id", &found);
    CHECK(!found);
    CHECK(parameter("message/partial; id=\"never closed", "id") == "never closed");
    CHECK(parameter("message/partial; id=\"ends in a backslash\\", "id") == "ends in a backslash\\");
    return 0;
}
static int numbers_are_plain_numbers_in_range()
{
    for (const char *bad : {"0", "-1", "-2147483648", "2147483648", "99999999999999999999", "4x", "x4", "", " ", "1 2", "+", "0x10", "1e3"})
        CHECK(number(std::string("message/partial; number=") + bad, "number") == 0);
    CHECK(number("message/partial; number=1", "number") == 1);
    CHECK(number("message/partial; number=+7", "number") == 7);
    CHECK(number("message/partial; number=\"12\"", "number") == 12);
    CHECK(number("message/partial; number=2147483647", "number") == 2147483647u);
    return 0;
}
static int a_header_of_any_length_is_read()
{
    std::string huge = "message/partial; " + std::string(200000, 'p') + "=1; id=" + std::string(100000, 'i') + "; number=3";
    CHECK(number(huge, "number") == 3);
    CHECK(parameter(huge, "id").size() == 100000);
    std::string many;
    for (int i = 0; i < 20000; i++) many += "; x" + std::to_string(i) + "=y";
    CHECK(number("message/partial" + many + "; number=8", "number") == 8);
    return 0;
}
static int blank_line_search_stays_inside_the_text()
{
    std::string text = "Content-Type: text/plain\n\nbody";
    char *copy = exact(text);
    const char *at = findBlankLine(copy, text.size());
    CHECK(at == copy + 24);
    CHECK(findBlankLine(copy, 25) == NULL);        // the second line feed is outside the text
    CHECK(findBlankLine(copy, 0) == NULL);
    CHECK(findBlankLine(copy, 1) == NULL);
    free(copy);
    std::string none = "no blank line\nat all\n";
    copy = exact(none);
    CHECK(findBlankLine(copy, none.size()) == NULL);
    free(copy);
    return 0;
}
static int text_grows_and_keeps_every_byte()
{
    PartialText text;
    std::string expected;
    for (int i = 0; i < 3000; i++) {
        std::string piece(i % 211, (char)('a' + i % 26));
        if (i % 7 == 0) piece += std::string(1, '\0') + "after the NUL";   // binary parts
        CHECK(text.append(piece.data(), piece.size()));
        expected += piece;
    }
    CHECK(text.append("tail"));
    expected += "tail";
    CHECK(text.length() == expected.size());
    size_t length = text.length();
    char *data = text.release();
    CHECK(memcmp(data, expected.data(), length) == 0 && data[length] == '\0');
    free(data);
    return 0;
}
static int empty_text_releases_an_empty_string()
{
    PartialText text;
    CHECK(text.append("", 0));
    CHECK(text.length() == 0);
    char *data = text.release();
    CHECK(data != NULL && data[0] == '\0');
    free(data);
    return 0;
}
static int text_stops_at_the_size_limit()
{
    PartialText text;
    std::string chunk(100000, 'x');
    size_t total = 0;
    while (total + chunk.size() <= MaxPartialMessage) {
        CHECK(text.append(chunk.data(), chunk.size()));
        total += chunk.size();
    }
    CHECK(!text.failed());
    CHECK(text.append(chunk.data(), MaxPartialMessage - total));    // exactly the limit
    CHECK(text.length() == MaxPartialMessage);
    CHECK(!text.append("x", 1));
    CHECK(text.failed());
    CHECK(!text.append("", 0) || text.failed());
    CHECK(text.length() == MaxPartialMessage);
    return 0;
}
static int a_single_huge_piece_is_refused_without_wrapping()
{
    PartialText text;
    CHECK(text.append("a", 1));
    CHECK(!text.append("b", (size_t)-1));
    CHECK(text.failed());
    PartialText other;
    CHECK(!other.append("b", MaxPartialMessage + 1));
    return 0;
}
static Scenario scenarios[] = {
    {"parameters_are_read_by_name", parameters_are_read_by_name},
    {"a_name_inside_another_value_is_not_a_parameter", a_name_inside_another_value_is_not_a_parameter},
    {"missing_or_malformed_parameters_are_absent", missing_or_malformed_parameters_are_absent},
    {"numbers_are_plain_numbers_in_range", numbers_are_plain_numbers_in_range},
    {"a_header_of_any_length_is_read", a_header_of_any_length_is_read},
    {"blank_line_search_stays_inside_the_text", blank_line_search_stays_inside_the_text},
    {"text_grows_and_keeps_every_byte", text_grows_and_keeps_every_byte},
    {"empty_text_releases_an_empty_string", empty_text_releases_an_empty_string},
    {"text_stops_at_the_size_limit", text_stops_at_the_size_limit},
    {"a_single_huge_piece_is_refused_without_wrapping", a_single_huge_piece_is_refused_without_wrapping},
    {NULL, NULL}
};
int main(int argc, char **argv) { return dispatch(argc, argv, scenarios); }
'''
    print("message/partial helpers: %d scenarios passed" % run_harness("partial", harness))

# ---------------------------------------------------------------------------------------------------
# Enriched text
mime = read("libDtMail/RFC/RFCMIME.C")
enriched = block(mime, r"^void\nRFCMIME::readTextEnriched\(")
enriched = put_back("enriched", enriched, "size_t length = 0;", "char length = 0;")
if enriched is None:
    problems.append("the enriched text converter was not found")
else:
    harness = COMMON + r'''
struct RFCMIME { static void readTextEnriched(char * buf, int & off, const char * bp, const unsigned long bp_len); };
''' + enriched + r'''
/* the converter is given room for the input and one more byte, as MIMEBodyPart::loadBody does */
static std::string converted(const std::string &input, int *off_out = NULL)
{
    char *in = exact(input);
    char *out = (char *)malloc(input.size() + 2);       // the converted text and the terminating NUL
    int off = 0;
    RFCMIME::readTextEnriched(out, off, in, input.size());
    if (off < 0 || (size_t)off > input.size() + 1) { fprintf(stderr, "failed: output of %d bytes for %zu\n", off, input.size()); exit(1); }
    std::string result(out, (size_t)off);
    if (off_out) *off_out = off;
    free(in);
    free(out);
    return result;
}
static int plain_text_and_commands_are_converted()
{
    CHECK(converted("") == "\n");
    CHECK(converted("hello") == "hello\n");
    CHECK(converted("<bold>hello</bold> world") == "hello world\n");
    CHECK(converted("a<<b") == "a<b\n");
    CHECK(converted("one\ntwo") == "one two\n");
    CHECK(converted("one\n\ntwo") == "one\ntwo\n");
    CHECK(converted("<param>hidden</param>shown") == "shown\n");
    CHECK(converted("<nofill>a\nb</nofill>") == "a\nb\n");
    CHECK(converted("<BOLD>x</BOLD>") == "x\n");
    return 0;
}
static int a_command_of_any_length_is_cut_to_its_buffer()
{
    for (size_t length : {10u, 254u, 255u, 256u, 257u, 300u, 1000u, 5000u, 70000u}) {
        std::string tag(length, 'a');
        CHECK(converted("x<" + tag + ">y") == "xy\n");
    }
    // a long command that begins like "param" is still not "param"
    std::string param = "param" + std::string(300, 'z');
    CHECK(converted("<" + param + ">visible") == "visible\n");
    return 0;
}
static int a_command_that_never_ends_stops_at_the_text()
{
    CHECK(converted("<") == "\n");
    CHECK(converted("a<") == "a\n");
    CHECK(converted("<bold") == "\n");
    CHECK(converted("<" + std::string(9000, 'q')) == "\n");
    CHECK(converted("text<param") == "text\n");
    CHECK(converted("\n<") == " \n");
    return 0;
}
static int output_never_exceeds_the_room_it_is_given()
{
    // a fixed sequence of inputs made of the characters that matter
    const char alphabet[] = "<>/\n abnofilparamx";
    unsigned state = 12345;
    for (int round = 0; round < 40000; round++) {
        state = state * 1103515245u + 12345u;
        size_t length = (state >> 16) % 40;
        std::string input;
        for (size_t i = 0; i < length; i++) {
            state = state * 1103515245u + 12345u;
            input += alphabet[(state >> 16) % (sizeof(alphabet) - 1)];
        }
        converted(input);
    }
    return 0;
}
static int nul_bytes_in_the_text_are_ordinary()
{
    std::string input = std::string("a") + std::string(1, '\0') + "b<" + std::string(1, '\0') + "c>d";
    converted(input);
    return 0;
}
static Scenario scenarios[] = {
    {"plain_text_and_commands_are_converted", plain_text_and_commands_are_converted},
    {"a_command_of_any_length_is_cut_to_its_buffer", a_command_of_any_length_is_cut_to_its_buffer},
    {"a_command_that_never_ends_stops_at_the_text", a_command_that_never_ends_stops_at_the_text},
    {"output_never_exceeds_the_room_it_is_given", output_never_exceeds_the_room_it_is_given},
    {"nul_bytes_in_the_text_are_ordinary", nul_bytes_in_the_text_are_ordinary},
    {NULL, NULL}
};
int main(int argc, char **argv) { return dispatch(argc, argv, scenarios); }
'''
    print("enriched text: %d scenarios passed" % run_harness("enriched", harness))

# ---------------------------------------------------------------------------------------------------
# From line and lock file name
fmt = read("libDtMail/RFC/RFCFormat.C")
mailbox = read("libDtMail/RFC/RFCMailBox.C")
from_fn = block(fmt, r"^static char \*\nmakeUnixFromLine\(")
lock_fn = block(mailbox, r"^static bool\nmakeLockTemporaryName\(")
if not (from_fn and lock_fn):
    problems.append("the From line or lock name functions were not found")
else:
    harness = COMMON + r'''
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
''' + from_fn + lock_fn + r'''
static std::string unix_from(const char *sender, const char *date)
{
    char *line = makeUnixFromLine(sender, date);
    std::string result = line;
    delete [] line;
    return result;
}
static int from_line_has_a_sender_and_a_date()
{
    CHECK(unix_from("alice@example.org", "Tue Nov 10 12:16:39 2026\n") == "From alice@example.org Tue Nov 10 12:16:39 2026\n");
    CHECK(unix_from(NULL, "now\n") == "From nobody@nowhere now\n");
    CHECK(unix_from("", "now\n") == "From nobody@nowhere now\n");
    CHECK(unix_from(" leading space", "now\n") == "From nobody@nowhere now\n");
    CHECK(unix_from("a@b c@d", "now\n") == "From a@b now\n");              // one word only
    CHECK(unix_from("a@b\nX-Injected: 1", "now\n") == "From a@b now\n");   // no line break from a sender
    CHECK(unix_from("a@b\r\n", "now\n") == "From a@b now\n");
    CHECK(unix_from("a@b", NULL) == "From a@b ");
    return 0;
}
static int from_line_fits_senders_and_dates_of_any_length()
{
    std::string sender(100000, 's'), date(100000, 'd');
    std::string line = unix_from(sender.c_str(), date.c_str());
    CHECK(line == "From " + sender + " " + date);
    return 0;
}
static int lock_errno;
static std::string lock_name(size_t size, const char *base, bool *ok)
{
    char *name = (char *)malloc(size ? size : 1);
    errno = 0;
    *ok = makeLockTemporaryName(name, size, base);
    lock_errno = errno;
    std::string result = *ok ? name : "";
    free(name);
    return result;
}
static int lock_names_are_random_and_fit_their_buffer()
{
    bool ok;
    const std::string base = "/var/mail/user.lock";
    std::string a = lock_name(1024, base.c_str(), &ok);
    CHECK(ok && a.size() == base.size() + 6 && a.compare(0, base.size(), base) == 0);
    for (size_t i = base.size(); i < a.size(); i++) CHECK(isalnum((unsigned char)a[i]));
    std::string b = lock_name(1024, base.c_str(), &ok);
    CHECK(ok && a != b);
    // the name and its NUL fit, or the name is refused with ENAMETOOLONG
    for (size_t size = 0; size < base.size() + 12; size++) {
        std::string name = lock_name(size, base.c_str(), &ok);
        CHECK(ok == (size >= base.size() + 7));
        if (!ok) CHECK(lock_errno == ENAMETOOLONG);
    }
    return 0;
}
static Scenario scenarios[] = {
    {"from_line_has_a_sender_and_a_date", from_line_has_a_sender_and_a_date},
    {"from_line_fits_senders_and_dates_of_any_length", from_line_fits_senders_and_dates_of_any_length},
    {"lock_names_are_random_and_fit_their_buffer", lock_names_are_random_and_fit_their_buffer},
    {NULL, NULL}
};
int main(int argc, char **argv) { return dispatch(argc, argv, scenarios); }
'''
    print("From line and lock name: %d scenarios passed" % run_harness("textbuilders", harness))

# ---------------------------------------------------------------------------------------------------
# The address list reader
transport = read("libDtMail/RFC/RFCTransport.C")
skin_fn = block(transport, r"^static void\nskin_comma\(")
arpa_fn = block(transport, r"^void\nRFCTransport::arpaPhrase\(")
if not (skin_fn and arpa_fn):
    problems.append("the address list functions were not found")
else:
    harness = COMMON + r'''
#define DtMailAddressDefault "Dt:Mail:Address:Default"
struct DtMailValueAddress { char *dtm_address; char *dtm_person; char *dtm_namespace; };
struct DtMailAddressSeq {
    std::vector<DtMailValueAddress *> items;
    void append(DtMailValueAddress *a) { items.push_back(a); }
    void remove(int i) { items.erase(items.begin() + i); }
    int length() const { return (int)items.size(); }
    DtMailValueAddress *operator[](int i) { return items[i]; }
};
struct RFCTransport { static void arpaPhrase(const char * list, DtMailAddressSeq & tokens); };
''' + skin_fn + arpa_fn + r'''
static std::vector<std::string> addresses(const std::string &list)
{
    DtMailAddressSeq seq;
    char *copy = exact(list + std::string(1, '\0'));
    RFCTransport::arpaPhrase(copy, seq);
    free(copy);
    std::vector<std::string> out;
    for (int i = 0; i < seq.length(); i++) {
        out.push_back(seq[i]->dtm_address);
        free(seq[i]->dtm_address); free(seq[i]->dtm_namespace); delete seq[i];
    }
    return out;
}
static int address_list_is_split_at_commas()
{
    std::vector<std::string> v = addresses("alice@example.org, bob@example.org, carol@example.org");
    CHECK(v.size() == 3 && v[0] == "alice@example.org" && v[2] == "carol@example.org");
    v = addresses("single@example.org");
    CHECK(v.size() == 1 && v[0] == "single@example.org");
    CHECK(addresses("").empty());
    return 0;
}
static int a_quote_the_list_leaves_open_is_closed_inside_the_buffer()
{
    // the closing quote of an unterminated string is a byte the name does not have
    for (const char *list : {"\"", "\"a", "\")(", "\"\\", "x, \"unfinished", "<\"", "a,\"b,c", "\"(\")(\""})
        addresses(list);
    std::string open = "\"" + std::string(10000, 'q');
    addresses(open);
    return 0;
}
static int address_lists_of_any_shape_stay_in_their_buffer()
{
    for (size_t count : {1u, 2u, 7u, 100u, 5000u}) {
        addresses(std::string(count, ','));
        addresses(std::string(count, ',') + "x");
        addresses("x" + std::string(count, ','));
        addresses(std::string(count, '"'));
        addresses(std::string(count, '<'));
        std::string list;
        for (size_t i = 0; i < count; i++) list += "a" + std::to_string(i) + "@h,";
        addresses(list);
        std::string spaced;
        for (size_t i = 0; i < count; i++) spaced += "a,   ";
        addresses(spaced);
        std::string tabs;
        for (size_t i = 0; i < count; i++) tabs += ",\t,\n";
        addresses(tabs);
    }
    // a fixed sequence of lists made of the characters the reader treats specially
    const char alphabet[] = ", <>\"()\\\t\nab@";
    unsigned state = 99;
    for (int round = 0; round < 150000; round++) {
        state = state * 1103515245u + 12345u;
        size_t length = (state >> 16) % 40;
        std::string list;
        for (size_t i = 0; i < length; i++) {
            state = state * 1103515245u + 12345u;
            list += alphabet[(state >> 16) % (sizeof(alphabet) - 1)];
        }
        addresses(list);
    }
    return 0;
}
static Scenario scenarios[] = {
    {"address_list_is_split_at_commas", address_list_is_split_at_commas},
    {"a_quote_the_list_leaves_open_is_closed_inside_the_buffer", a_quote_the_list_leaves_open_is_closed_inside_the_buffer},
    {"address_lists_of_any_shape_stay_in_their_buffer", address_lists_of_any_shape_stay_in_their_buffer},
    {NULL, NULL}
};
int main(int argc, char **argv) { return dispatch(argc, argv, scenarios); }
'''
    print("address list: %d scenarios passed" % run_harness("addresses", harness))

# ---------------------------------------------------------------------------------------------------
# Release of the .dtmail lock
unlock_fn = block(mailbox, r"^void\nRFCMailBox::dotDtmailUnlock\(")
if unlock_fn is None:
    problems.append("the .dtmail unlock function was not found")
else:
    harness = COMMON + r'''
#include <fcntl.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/stat.h>
struct DtMailEnv { };
#define PRIV_ENABLED(status, call) status = (call)
#define SafeOpen open
#define SafeRead read
#define SafeClose close
#define SafeRemove remove
static std::string lock_path;
struct RFCMailBox { bool dotDtmailLockFile(char *name, size_t size); void dotDtmailUnlock(DtMailEnv &); };
bool RFCMailBox::dotDtmailLockFile(char *name, size_t size) { snprintf(name, size, "%s", lock_path.c_str()); return true; }
/* sscanf stops at the first character that is not a digit, so a read past the buffer goes unseen;
   strlen() of the text walks to its end, which the sanitizer sees */
static int checked_sscanf(const char *text, const char *format, int *value) { (void)strlen(text); return sscanf(text, format, value); }
#define sscanf(text, format, value) checked_sscanf((text), (format), (value))
''' + unlock_fn + r'''
static __attribute__((noinline)) void dirty_stack()
{
    volatile char junk[16384];
    for (size_t i = 0; i < sizeof(junk); i++) junk[i] = 'Z';
}
static bool unlocked(const std::string &content)
{
    char dir[] = "/tmp/dtmail-unlock-XXXXXX";
    const char *base = getenv("TMPDIR");
    std::string pattern = std::string(base ? base : "/tmp") + "/dtmail-unlock-XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    if (!mkdtemp(name.data())) { perror("mkdtemp"); exit(3); }
    lock_path = std::string(name.data()) + "/box.dtmail";
    FILE *f = fopen(lock_path.c_str(), "w");
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);
    RFCMailBox box;
    DtMailEnv env;
    dirty_stack();                    // what an earlier call leaves in the stack is not a NUL byte
    box.dotDtmailUnlock(env);
    bool gone = access(lock_path.c_str(), F_OK) != 0;
    remove(lock_path.c_str());
    rmdir(name.data());
    return gone;
}
static int own_lock_is_removed()
{
    CHECK(unlocked(std::to_string((int)getpid()) + "\n"));
    CHECK(unlocked(std::to_string((int)getpid())));
    return 0;
}
static int lock_of_another_process_stays()
{
    CHECK(!unlocked(std::to_string((int)getpid() + 1) + "\n"));
    CHECK(!unlocked("1\n"));
    return 0;
}
static int lock_without_a_number_stays()
{
    CHECK(!unlocked(""));
    CHECK(!unlocked("\n"));
    CHECK(!unlocked("owner unknown\n"));
    return 0;
}
static int lock_that_fills_the_buffer_is_not_read_past_it()
{
    // no line feed and no end for a read of more than a buffer
    CHECK(!unlocked(std::string(3000, '7')));
    CHECK(!unlocked(std::string(1023, '9')));
    CHECK(!unlocked(std::string(1024, '9')));
    CHECK(!unlocked(std::string(5000, ' ')));
    return 0;
}
static Scenario scenarios[] = {
    {"own_lock_is_removed", own_lock_is_removed},
    {"lock_of_another_process_stays", lock_of_another_process_stays},
    {"lock_without_a_number_stays", lock_without_a_number_stays},
    {"lock_that_fills_the_buffer_is_not_read_past_it", lock_that_fills_the_buffer_is_not_read_past_it},
    {NULL, NULL}
};
int main(int argc, char **argv) { return dispatch(argc, argv, scenarios); }
'''
    harness = harness.replace("    char dir[] = \"/tmp/dtmail-unlock-XXXXXX\";\n", "")
    print("lock release: %d scenarios passed" % run_harness("unlock", harness))

# ---------------------------------------------------------------------------------------------------
# Places that cannot be run without a mailbox open


def code_of(relative):
    text = read(relative)
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def guard(relative, patterns, why):
    text = code_of(relative)
    for pattern in patterns:
        if re.search(pattern, text):
            problems.append("%s: %s (%s)" % (relative, why, pattern))


guard("libDtMail/RFC/RFCMailBox.C", [r"\bmktemp\s*\(", r"delete\s+_mra_(?:command|serverpw)\s*;"],
      "a lock file gets a predictable name, or a malloc()ed string is released with delete")
guard("libDtMail/RFC/RFCEnvelope.C", [r"delete\s+str_addr\s*;"], "an array is released with delete")
guard("libDtMail/RFC/MIMEPartial.C", [r"\bstrcat\s*\(", r"\bstrncat\s*\(", r"\batoi\s*\("],
      "the parts of a message are put together with C string functions or numbers are read with atoi()")
guard("libDtMail/RFC/RFCMIME.C", [r"\bstrcpy\s*\(\s*mime_type"], "a MIME type is copied into a buffer of 64 bytes without a bound")
guard("libDtMail/RFC/RFCFormat.C", [r"\bstrcpy\s*\(\s*charset", r"new char\s*\[\s*100\s*\]"],
      "a charset or a sender is copied into a buffer of a guessed size")
for relative, needle in (("libDtMail/RFC/MIMEPartial.C", "MaxPartialParts"),
                         ("libDtMail/RFC/RFCMailBox.C", "makeLockTemporaryName")):
    if needle not in read(relative):
        problems.append("%s no longer has %s" % (relative, needle))

if problems:
    sys.exit("mail message checks failed:\n  " + "\n  ".join(problems))
print("Mail message checks: parameters, enriched text, From line, lock names and addresses are bounded")
