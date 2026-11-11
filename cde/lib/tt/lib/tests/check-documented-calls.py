#!/usr/bin/env python3
"""Check that what the shipped documents and sources show of the Desktop
calls of libtt is the call that include/Tt/tttk.h declares.

check-documented-calls.py TOPSRCDIR

TOPSRCDIR is the top of the source tree (the directory that holds include,
doc and programs).  It needs no build.

The functions are those that tttk.h declares (ttdt_*, tttk_*, ttmedia_*), with
the number of their parameters.  Every document of doc/ (the manual pages and
the guides of every language) and every example and demo source (examples/,
lib/tt/demo/) is searched for a function name followed by an opening
parenthesis.  When the text between that parenthesis and its closing one is
not empty and is not the number of a manual section, it has to have as many
arguments as the function has parameters: a prototype, a synopsis or a call that
was written for an earlier signature fails the check.  The arguments are
counted by their commas outside of any parentheses, brackets, braces and
quotation marks, after the markup and entities of the document are taken out.

The dtksh command ttdt_session_join takes the requests as further arguments, so
that the usage of the command (the guide of dtksh in every language and the
message catalogs of dtksh in every locale) has to name them in square brackets.
"""
from pathlib import Path
import re
import sys

top = Path(sys.argv[1]).resolve()
failures = []


def fail(path, line, text):
    failures.append("%s:%d: %s" % (path.relative_to(top), line, text))


def count_arguments(text):
    """The number of arguments in the text between parentheses, 0 if empty."""
    text = text.strip()
    if not text or text == "void":
        return 0
    depth = 0
    quote = None
    count = 1
    i = 0
    while i < len(text):
        c = text[i]
        if quote:
            if c == "\\":
                i += 1
            elif c == quote:
                quote = None
        elif c in "\"'":
            quote = c
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "," and depth == 0:
            count += 1
        i += 1
    return count


# the functions of tttk.h and the number of their parameters
header = (top / "include/Tt/tttk.h").read_text()
header = re.sub(r"/\*.*?\*/", " ", header, flags=re.S)
parameters = {}
for m in re.finditer(r"\b((?:ttdt|tttk|ttmedia)_\w+)\s*\(", header):
    # find the closing parenthesis
    start = m.end()
    depth = 1
    i = start
    while i < len(header) and depth:
        depth += {"(": 1, ")": -1}.get(header[i], 0)
        i += 1
    if header[i:].lstrip().startswith(";"):
        parameters[m.group(1)] = count_arguments(header[start:i - 1])
if "ttdt_session_join" not in parameters or "ttdt_sender_imprint_on" not in parameters:
    print("tttk.h does not declare the calls this check is about")
    sys.exit(2)

NAME = re.compile(r"\b(%s)[ \t]*\(" % "|".join(sorted(parameters, key=len, reverse=True)))
MARKUP = re.compile(r"<!--.*?-->|<[^>]*>", re.S)
ENTITY = re.compile(r"&[A-Za-z0-9#]+;")
MAN_SECTION = re.compile(r"^\d[A-Za-z]?$")


def plain(text):
    """The text with the markup and the entities taken out, with the offsets of
    the lines kept (the markup is replaced by nothing but its newlines)."""
    text = MARKUP.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    return ENTITY.sub(" ", text)


def check_calls(path):
    original = path.read_text(encoding="utf-8", errors="replace")
    text = plain(original) if path.suffix == ".sgm" else original
    for m in NAME.finditer(text):
        depth = 1
        i = m.end()
        while i < len(text) and depth:
            depth += {"(": 1, ")": -1}.get(text[i], 0)
            i += 1
        if depth:
            continue
        inside = text[m.end():i - 1]
        if not inside.strip() or MAN_SECTION.match(inside.strip()):
            continue            # a mention of the function, not its arguments
        found = count_arguments(inside)
        if found != parameters[m.group(1)]:
            fail(path, text.count("\n", 0, m.start()) + 1,
                 "%s is shown with %d arguments, tttk.h declares %d" %
                 (m.group(1), found, parameters[m.group(1)]))


roots = [top / "doc", top / "examples", top / "lib/tt/demo"]
for root in roots:
    for path in sorted(root.rglob("*")):
        if path.is_file() and path.suffix in (".sgm", ".c", ".C", ".h", ".cc"):
            check_calls(path)

# the usage of the dtksh command names the requests: in the documents, a
# synopsis that begins with the name of the command and the arguments it had
# before, and in the message catalogs message 104
USAGE_DOC = re.compile(r"<(?:filename|command)>ttdt_session_join</(?:filename|command)>"
                       r"\s*<(?:symbol|arg)\b.*?(?:</para>|</cmdsynopsis>)", re.S)
USAGE_MSG = re.compile(r"\[[^\]]*\.\.\.\]")
for path in sorted((top / "doc").rglob("*.sgm")):
    original = path.read_text(encoding="utf-8", errors="replace")
    for m in USAGE_DOC.finditer(original):
        if "request" not in plain(m.group(0)):
            fail(path, original.count("\n", 0, m.start()) + 1,
                 "the usage of the dtksh command ttdt_session_join has no requests")
for path in sorted((top / "programs").rglob("dtksh.msg")):
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if re.match(r"104\s+\"?[^\"]*ttdt_session_join\s", line) and not USAGE_MSG.search(line):
            fail(path, number, "the usage of the dtksh command ttdt_session_join has no requests")

if failures:
    print("\n".join(failures))
    print("%d documents show a call that tttk.h does not declare" % len(failures))
    sys.exit(1)
print("all documented calls are those of tttk.h (%d functions)" % len(parameters))
