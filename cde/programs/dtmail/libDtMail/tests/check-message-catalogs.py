#!/usr/bin/env python3
"""Check that the message catalogs of libDtMail and dtmail agree with the code that reads them.

check-message-catalogs.py SRCDIR ARCHIVE

SRCDIR is the source directory of libDtMail, ARCHIVE the built libDtMail.a.
Environment, as `make check` supplies it (see libDtMail/Makefile.am):
  DTMAIL_COMPILE  the compile command of libDtMail, flags included
  DTMAIL_LINK     the libtool link command of the build
  DTMAIL_LIBS     the libraries dtmailpr links
  GENCAT          the gencat of the build (default: gencat)
  DTMAIL_GENCAT_LOCALE
                  the UTF-8 locale that gencat runs in (default: C.UTF-8); the sources of the
                  locales are UTF-8, and gencat of the GNU C library refuses their messages
                  in the C locale
  DTMAIL_CATALOG_ROOT
                  the cde directory to check instead of the one SRCDIR belongs to
                  (a revision with a defect is how the test is checked itself)
  DTMAIL_TEST_CASES  run only these cases, a blank separated list of names
  DTMAIL_TEST_KEEP   keep the work directory

Two catalogs are indexed by number.  libDtMail.msg, set 1, holds one message per
enumerator of DTMailError_t: DtMailEnv looks an error up as message "enumerator + 1".
DtMail.msg is read by the numbers in the calls of CATGETS in dtmail/.  Removing a
message from either one renumbers every later message, in the English source and in
the source of every locale, and every call that names one of them.  The cases below
check that this was done everywhere:

  * the enumerators, the English fall-back texts in Common/DtMailError.C and the
    English catalog name the same errors in the same order;
  * every locale numbers its messages the way the English source does: the same
    numbers, the same error named in the comment above a message, the same
    conversions (%s, %d, ...) in a message, and for the locales that the build
    installs (programs/localized/Makefile.am) exactly the English set of messages;
  * every call of CATGETS on set 2 of DtMail.msg names the message whose text is the
    default of the call;
  * none of the removed messages, their enumerator or their help sections is left;
  * every catalog compiles with gencat, and the compiled catalog holds exactly the
    messages that the source holds under the numbers that the source gives;
  * the production lookup (Common/DtMailError.C under the sanitizers) returns the
    text of the right message from the English catalog and from every installed
    locale, the fall-back text without a catalog, and "invalid error" beyond the
    table.

ko_KR, sv_SE and zh_TW carry no comments above their messages of libDtMail.msg, and
the numbering of those messages predates the current enumeration: they are checked
for what can be checked without the comments (numbers and conversions).
"""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

srcdir = Path(sys.argv[1]).resolve()
archive = Path(sys.argv[2]).resolve()
root = Path(os.environ["DTMAIL_CATALOG_ROOT"]).resolve() if os.environ.get("DTMAIL_CATALOG_ROOT") else srcdir.parents[2]
dtmail = root / "programs" / "dtmail"
localized = root / "programs" / "localized"
help_root = root / "doc"
gencat = shlex.split(os.environ.get("GENCAT") or "gencat")
compile_cmd = shlex.split(os.environ["DTMAIL_COMPILE"])
link_cmd = shlex.split(os.environ["DTMAIL_LINK"])
libs = shlex.split(os.environ.get("DTMAIL_LIBS", ""))
builddir = Path.cwd()
work = Path(tempfile.mkdtemp(prefix="dtmail-catalogs-", dir=os.environ.get("TMPDIR")))
sanitize = ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-fno-omit-frame-pointer", "-g"]

ERROR_HEADER = dtmail / "include" / "DtMail" / "DtMailError.hh"
ERROR_TABLE = dtmail / "libDtMail" / "Common" / "DtMailError.C"
LIB_MSG = dtmail / "libDtMail" / "Common" / "libDtMail.msg"
APP_MSG = dtmail / "dtmail" / "DtMail.msg"
# What the removal of "the mailer does not run as group mail" took away.
REMOVED_NAMES = ["DTME_BadRunGroup", "DTMAILHELPBADGROUPID", "DTMAILHELPNOINSTALLMAIL"]


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

def unescape(text):
    """The characters that gencat makes of the escapes of a message."""
    simple = {"n": "\n", "t": "\t", "v": "\v", "b": "\b", "r": "\r", "f": "\f", "\\": "\\"}
    out, i = [], 0
    while i < len(text):
        c = text[i]
        if c != "\\" or i + 1 >= len(text):
            out.append(c)
            i += 1
            continue
        d = text[i + 1]
        if d in "01234567":
            digits = re.match(r"[0-7]{1,3}", text[i + 1:]).group(0)
            out.append(chr(int(digits, 8)))
            i += 1 + len(digits)
        else:
            out.append(simple.get(d, d))
            i += 2
    return "".join(out)


class Catalog:
    """The messages of a message source: sets[set][number] = text, with the error named above each."""

    def __init__(self, path):
        self.path = path
        self.sets = {}
        self.names = {}       # (set, number) -> the name in the comment above the message, if any
        self.order = []       # (set, number) in file order
        self.problems = []
        current, quote, name = None, None, None
        lines = path.read_text(encoding="utf-8").split("\n")
        i = 0
        while i < len(lines):
            line = lines[i]
            i += 1
            m = re.match(r"\$set\s+(\d+)", line)
            if m:
                current = int(m.group(1))
                self.sets.setdefault(current, {})
                name = None
                continue
            m = re.match(r"\$quote(?:\s+(\S))?", line)
            if m:
                quote = m.group(1)
                continue
            if re.match(r"\$(\s|$)", line):
                m = re.match(r"\$\s*//\s*(?:-\s*)?(DTME_\w+)", line)
                if m:
                    name = m.group(1)
                continue
            m = re.match(r"(\d+)(?:\s(.*))?$", line)
            if not m or current is None:
                continue
            number, body = int(m.group(1)), m.group(2) or ""
            while body.endswith("\\") and not body.endswith("\\\\") and i < len(lines):
                body = body[:-1] + lines[i]
                i += 1
            body = body.lstrip()
            if quote and body.startswith(quote):
                end = len(body) - 1
                while end > 0 and body[end] != quote:
                    end -= 1
                body = body[1:end] if end > 0 else body[1:]
            if number in self.sets[current]:
                self.problems.append("%s: set %d message %d is defined twice" % (path.name, current, number))
            self.sets[current][number] = unescape(body)
            self.names[(current, number)] = name
            self.order.append((current, number))
            name = None


def conversions(text):
    """The types of the conversions of a message, in order of appearance and without positions."""
    found = re.findall(r"%(?:\d+\$)?([-+ #0]*[0-9*]*(?:\.[0-9*]+)?(?:hh|h|ll|l|L|z|j|t)?[diouxXeEfFgGaAcspn%])",
                       text)
    return sorted(f for f in found if not f.endswith("%"))


def read_enum():
    text = ERROR_HEADER.read_text()
    body = re.search(r"typedef enum \{(.*?)\}\s*DTMailError_t;", text, re.S).group(1)
    names = re.findall(r"\b(DTME_\w+)\s*,?", re.sub(r"//.*", "", body))
    return names[:-1] if names and names[-1] == "DTME_MAXDTME" else names


def read_table():
    """[(name, text)] of error_strings[] in DtMailError.C, in order; the last name is DTME_MAXDTME."""
    text = ERROR_TABLE.read_text()
    body = text[text.index("error_strings[] = {"):]
    body = body[:body.index("};")]
    entries = []
    for part in re.split(r"//\s*-\s*(?=DTME_)", body)[1:]:
        name = re.match(r"DTME_\w+", part).group(0)
        rest = part[part.index("\n"):]
        literals = re.findall(r"\"((?:[^\"\\]|\\.)*)\"", rest)
        if literals:
            entries.append((name, unescape("".join(literals))))
        elif re.search(r"\bNULL\b", rest):
            entries.append((name, None))
        else:
            entries.append((name, ""))
    return entries


def built_locales():
    text = (localized / "Makefile.am").read_text()
    return sorted(set(re.findall(r"SUBDIRS\s*\+=\s*(\S+)", text)))


def all_locales():
    return sorted(p.name for p in localized.iterdir() if (p / "msg" / "libDtMail.msg").exists())


def hexed(text):
    """The bytes of a message as a program reads it: a C string ends at the first NUL (a message may end in \\0)."""
    return text.split("\0")[0].encode("utf-8").hex()


# ---------------------------------------------------------------------------
# Cases: each returns a list of problems
# ---------------------------------------------------------------------------

def the_error_table_follows_the_enumeration():
    enum = read_enum()
    table = read_table()
    problems = []
    if not table or table[-1] != ("DTME_MAXDTME", None):
        problems.append("error_strings[] does not end with DTME_MAXDTME and NULL")
    names = [n for n, _ in table[:-1]]
    if names != enum:
        for i, (a, b) in enumerate(zip(names, enum)):
            if a != b:
                problems.append("error_strings[%d] is for %s, the enumeration has %s there" % (i, a, b))
                break
        if len(names) != len(enum):
            problems.append("error_strings[] has %d texts, the enumeration %d errors" % (len(names), len(enum)))
    if len(set(enum)) != len(enum):
        problems.append("the enumeration names an error twice")
    return problems


def the_english_catalog_numbers_the_enumeration():
    enum = read_enum()
    catalog = Catalog(LIB_MSG)
    problems = list(catalog.problems)
    messages = catalog.sets.get(1, {})
    if sorted(messages) != list(range(1, len(enum) + 1)):
        problems.append("set 1 of libDtMail.msg has %d messages numbered %s..%s, the enumeration has %d errors"
                        % (len(messages), min(messages, default=0), max(messages, default=0), len(enum)))
    for number in sorted(messages):
        if number > len(enum):
            break
        if catalog.names[(1, number)] != enum[number - 1]:
            problems.append("message %d of libDtMail.msg is for %s, the error with that number is %s"
                            % (number, catalog.names[(1, number)], enum[number - 1]))
            break
    return problems


def check_numbering(locale, path, english, identities, check_conversions=True):
    """Problems of one locale source against the English one; identities: the enumeration or None."""
    catalog = Catalog(path)
    problems = ["%s: %s" % (locale, p) for p in catalog.problems]
    for set_number, messages in catalog.sets.items():
        reference = english.sets.get(set_number)
        if reference is None:
            problems.append("%s: set %d does not exist in the English source" % (locale, set_number))
            continue
        if sorted(messages) != list(range(1, len(messages) + 1)):
            problems.append("%s: set %d is not numbered 1..%d without gaps" % (locale, set_number, len(messages)))
        if len(messages) > len(reference):
            problems.append("%s: set %d has %d messages, the English source %d" % (locale, set_number, len(messages), len(reference)))
        for number, text in messages.items():
            if check_conversions and number in reference and conversions(text) != conversions(reference[number]):
                problems.append("%s: set %d message %d has the conversions %s, the English message %s"
                                % (locale, set_number, number, conversions(text), conversions(reference[number])))
            name = catalog.names.get((set_number, number))
            if identities is not None and set_number == 1 and name is not None and number <= len(identities) \
                    and name != identities[number - 1]:
                problems.append("%s: message %d is for %s, the error with that number is %s"
                                % (locale, number, name, identities[number - 1]))
    return catalog, problems


def every_locale_numbers_the_errors_like_the_english_catalog():
    english = Catalog(LIB_MSG)
    enum = read_enum()
    problems, installed = [], built_locales()
    locales = all_locales()
    if not installed or not set(installed) <= set(locales):
        return ["programs/localized/Makefile.am names the installed locales %s, found libDtMail.msg of %s" % (installed, locales)]
    for locale in locales:
        catalog, found = check_numbering(locale, localized / locale / "msg" / "libDtMail.msg", english, enum)
        problems += found
        if locale in installed:
            for set_number, reference in english.sets.items():
                if sorted(catalog.sets.get(set_number, {})) != sorted(reference):
                    problems.append("%s is installed but has %d messages in set %d, the English source %d"
                                    % (locale, len(catalog.sets.get(set_number, {})), set_number, len(reference)))
            unnamed = [n for n in catalog.sets.get(1, {}) if catalog.names.get((1, n)) is None]
            if unnamed:
                problems.append("%s: messages %s of set 1 have no comment naming their error" % (locale, unnamed[:5]))
    return problems


def every_locale_numbers_the_mailer_messages_like_the_english_catalog():
    english = Catalog(APP_MSG)
    problems, installed = list(english.problems), built_locales()
    for locale in all_locales():
        path = localized / locale / "msg" / "DtMail.msg"
        if not path.exists():
            problems.append("%s has no DtMail.msg" % locale)
            continue
        catalog, found = check_numbering(locale, path, english, None, check_conversions=locale in installed)
        problems += found
        if locale in installed:
            for set_number in sorted(set(english.sets) | set(catalog.sets)):
                if sorted(catalog.sets.get(set_number, {})) != sorted(english.sets.get(set_number, {})):
                    problems.append("%s is installed but set %d has %d messages, the English source %d"
                                    % (locale, set_number, len(catalog.sets.get(set_number, {})), len(english.sets.get(set_number, {}))))
    return problems


def call_arguments(text, start):
    """The top-level arguments of the call whose "(" ends at start."""
    depth, i, current, arguments, in_string = 1, start, "", [], False
    while i < len(text) and depth:
        c = text[i]
        if in_string:
            current += c
            if c == "\\":
                current += text[i + 1]
                i += 1
            elif c == '"':
                in_string = False
        elif c == '"':
            in_string = True
            current += c
        elif c == "(":
            depth += 1
            current += c
        elif c == ")":
            depth -= 1
            if depth:
                current += c
        elif c == "," and depth == 1:
            arguments.append(current.strip())
            current = ""
        else:
            current += c
        i += 1
    arguments.append(current.strip())
    return arguments


def the_mailer_calls_name_the_message_of_their_text():
    english = Catalog(APP_MSG)
    messages = english.sets[2]
    problems, calls = [], 0
    for path in sorted((dtmail / "dtmail").iterdir()):
        if path.suffix not in (".C", ".c", ".h", ".hh"):
            continue
        text = path.read_text(errors="replace")
        for m in re.finditer(r"\bCATGETS\s*\(", text):
            arguments = call_arguments(text, m.end())
            if len(arguments) < 4 or arguments[0] != "DT_catd" or arguments[1] != "2":
                continue
            line = text.count("\n", 0, m.start()) + 1
            where = "%s:%d" % (path.name, line)
            if not arguments[2].isdigit():
                problems.append("%s: CATGETS on set 2 with a number that is not a literal: %s" % (where, arguments[2]))
                continue
            number = int(arguments[2])
            calls += 1
            if number not in messages:
                problems.append("%s: set 2 has no message %d" % (where, number))
                continue
            literals = re.findall(r"\"((?:[^\"\\]|\\.)*)\"", ",".join(arguments[3:]))
            if not literals:
                continue
            default = unescape("".join(literals))
            if " ".join(default.split()) != " ".join(messages[number].split()):
                problems.append("%s: message %d of set 2 reads \"%s\", the call has \"%s\""
                                % (where, number, messages[number][:50], default[:50]))
    if calls == 0:
        problems.append("found no CATGETS call on set 2 in dtmail/")
    return problems


def the_removed_message_is_gone_everywhere():
    problems = []
    places = [p for p in sorted(dtmail.rglob("*")) if p.is_file() and "tests" not in p.relative_to(dtmail).parts]
    places += sorted(localized.glob("*/msg/libDtMail.msg")) + sorted(localized.glob("*/msg/DtMail.msg"))
    places += sorted(help_root.glob("*/help/Mailer/*.sgm"))
    for path in places:
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        for name in REMOVED_NAMES:
            if name in text:
                problems.append("%s still names %s" % (path.relative_to(root), name))
        if path.suffix in (".C", ".c", ".h", ".hh", ".msg"):
            for phrase in ("not running as group", "execution group"):
                if phrase in text:
                    problems.append("%s still has the text \"%s\"" % (path.relative_to(root), phrase))
    volumes = sorted(help_root.glob("*/help/Mailer/Messages.sgm"))
    if not volumes:
        problems.append("found no help volume Messages.sgm")
    for volume in volumes:
        ids = re.findall(r'<Sect1 Id="([^"]+)"', volume.read_text(encoding="utf-8"))
        if len(set(ids)) != len(ids):
            problems.append("%s: a help section exists twice" % volume.relative_to(root))
    if not built_locales():
        problems.append("found no installed locale")
    return problems


def compile_catalog(source, destination):
    locale = os.environ.get("DTMAIL_GENCAT_LOCALE") or "C.UTF-8"
    return subprocess.run(gencat + [str(destination), str(source)], capture_output=True, text=True,
                          env=dict(os.environ, LANG=locale, LC_ALL=locale))


def every_catalog_compiles_to_what_the_source_says():
    harness = build_harness()
    if isinstance(harness, str):
        return [harness]
    problems, produced = [], work / "compiled"
    produced.mkdir(exist_ok=True)
    sources = [("English", LIB_MSG), ("English", APP_MSG)]
    for locale in all_locales():
        sources += [(locale, localized / locale / "msg" / "libDtMail.msg"), (locale, localized / locale / "msg" / "DtMail.msg")]
    count = 0
    for index, (locale, source) in enumerate(sources):
        if not source.exists():
            continue
        out = produced / ("%d.cat" % index)
        r = compile_catalog(source, out)
        label = "%s/%s" % (locale, source.name)
        if r.returncode or r.stderr.strip() or not out.exists():
            problems.append("gencat on %s: exit %d %s" % (label, r.returncode, r.stderr.strip()[:200]))
            continue
        catalog = Catalog(source)
        top_set = max(catalog.sets)
        top_message = max(max(m, default=0) for m in catalog.sets.values())
        dump = subprocess.run([str(harness), "dump", str(out), str(top_set + 1), str(top_message + 1)],
                              capture_output=True, text=True, env=sanitizer_environment())
        if dump.returncode:
            problems.append("reading the compiled %s failed: %s" % (label, (dump.stderr or dump.stdout).strip()[:300]))
            continue
        got = {}
        for line in dump.stdout.splitlines():
            s, n, h = line.split(" ")
            got[(int(s), int(n))] = h
        want = {(s, n): hexed(text) for s, messages in catalog.sets.items() for n, text in messages.items()}
        if got != want:
            wrong = sorted(k for k in set(got) | set(want) if got.get(k) != want.get(k))
            problems.append("the compiled %s differs from its source at (set, message) %s" % (label, wrong[:6]))
        count += 1
    if count == 0:
        problems.append("no catalog was compiled")
    return problems


def the_lookup_finds_the_message_of_every_error():
    harness = build_harness()
    if isinstance(harness, str):
        return [harness]
    enum = read_enum()
    table = dict(read_table())
    problems = []
    scenarios = [("English", LIB_MSG)] + [(l, localized / l / "msg" / "libDtMail.msg") for l in built_locales()] + [("none", None)]
    for label, source in scenarios:
        directory = work / ("nls-" + label)
        directory.mkdir(exist_ok=True)
        messages = {}
        if source is not None:
            r = compile_catalog(source, directory / "libDtMail.cat")
            if r.returncode:
                problems.append("gencat on %s: %s" % (label, r.stderr.strip()[:200]))
                continue
            messages = Catalog(source).sets[1]
        lines = []
        for index, name in enumerate(enum):
            text = messages.get(index + 1)
            lines.append("%s %s" % (name, hexed(text if text is not None else table[name])))
        lines.append("out-of-range %s" % hexed(messages.get(enum.index("DTME_InvalidError") + 1, table["DTME_InvalidError"])))
        expected = work / ("expected-" + label)
        expected.write_text("\n".join(lines) + "\n")
        env = sanitizer_environment()
        env.update(NLSPATH=str(directory) + "/%N.cat", LANG="C", LC_ALL="C")
        r = subprocess.run([str(harness), "errors", str(expected)], capture_output=True, text=True, env=env)
        if r.returncode:
            detail = (r.stdout + r.stderr).strip().splitlines()
            problems.append("lookup with the %s catalog: %s" % (label, "; ".join(l[:160] for l in detail[:4])))
    return problems


# ---------------------------------------------------------------------------
# The compiled program: production Common/DtMailError.C and tests/message-catalogs.C
# ---------------------------------------------------------------------------

_harness = []


def sanitizer_environment():
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = "detect_leaks=0:abort_on_error=0"
    env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    return env


def build_harness():
    """The path of the program, or a string describing why it could not be built."""
    if _harness:
        return _harness[0]
    enum = read_enum()
    include = work / "include"
    include.mkdir(exist_ok=True)
    (include / "error-names.inc").write_text("".join('    { "%s", %s },\n' % (n, n) for n in enum))
    sources = [srcdir / "tests" / "message-catalogs.C", ERROR_TABLE]
    objects = []
    for source in sources:
        obj = work / (source.stem + ".o")
        # The headers of the revision that is checked come before the ones of the build.
        cmd = [compile_cmd[0], "-I", str(dtmail / "include"), "-I", str(include)] + compile_cmd[1:] + sanitize + [
            "-I", str(dtmail / "libDtMail" / "Common"), "-c", str(source), "-o", str(obj)]
        r = subprocess.run(cmd, cwd=builddir, capture_output=True, text=True)
        if r.returncode:
            _harness.append("compile of %s failed:\n%s" % (source.name, r.stderr[-2000:]))
            return _harness[0]
        objects.append(str(obj))
    binary = work / "message-catalogs"
    r = subprocess.run(link_cmd + sanitize + ["-o", str(binary)] + objects + [str(archive)] + libs,
                       cwd=builddir, capture_output=True, text=True)
    if r.returncode:
        _harness.append("link failed:\n" + r.stderr[-2000:])
        return _harness[0]
    _harness.append(binary)
    return binary


CASES = [
    the_error_table_follows_the_enumeration,
    the_english_catalog_numbers_the_enumeration,
    every_locale_numbers_the_errors_like_the_english_catalog,
    every_locale_numbers_the_mailer_messages_like_the_english_catalog,
    the_mailer_calls_name_the_message_of_their_text,
    the_removed_message_is_gone_everywhere,
    every_catalog_compiles_to_what_the_source_says,
    the_lookup_finds_the_message_of_every_error,
]

only = os.environ.get("DTMAIL_TEST_CASES", "").split()
failed, run = [], [c for c in CASES if not only or c.__name__ in only]
for case in run:
    try:
        problems = case()
    except Exception as exc:       # a source that cannot be read or parsed is a failure, not a skip
        problems = ["%s: %s" % (type(exc).__name__, exc)]
    print("%-5s %s" % ("ok" if not problems else "FAIL", case.__name__))
    if problems:
        failed.append(case.__name__)
        for line in problems[:12]:
            print("      " + line)
        if len(problems) > 12:
            print("      ... %d more" % (len(problems) - 12))
print("%d/%d cases passed" % (len(run) - len(failed), len(run)))
if os.environ.get("DTMAIL_TEST_KEEP"):
    print("kept " + str(work))
else:
    shutil.rmtree(work, ignore_errors=True)
sys.exit(1 if failed or not run else 0)
