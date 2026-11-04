#!/usr/bin/env python3
"""Build a DtSearch database with the dtsr tools and query it through the DtSrAPI.

check-dtsearch.py BUILDDIR TOP_BUILDDIR TOP_SRCDIR SRCDIR

BUILDDIR is the build directory of programs/dtsr (the tools are built there),
TOP_BUILDDIR and TOP_SRCDIR those of the whole package, SRCDIR the source
directory of programs/dtsr.  Environment, as `make check` supplies it (see
programs/dtsr/Makefile.am):
  DTSR_COMPILE  the compile command of the tools, flags included
  DTSR_LINK     the libtool link command of the build
  DTSR_LIBS     the libraries the test program links
  DTSR_TEST_KEEP    keep the work directory
The test
  1. runs every tool without arguments: each must print its usage and end,
     not die of a signal;
  2. makes a corpus of 420 documents out of a made-up vocabulary, in which a few
     words are in more than half of the documents (so that their lists of
     addresses run over several blocks of the .d99 file) and the others in few,
     creates a database, and loads and indexes the first 260 documents, then
     the other 160 (the index of the words already known is extended);
  3. queries the database through the DtSrAPI, in a program built here and run
     under the sanitizers when the compiler has them: single words, several
     words, and, or and not, in the exact and in the stemmed search, and the
     statistical search; each must return exactly the documents that have the
     words;
  4. validates and compresses the database with dtsrclean and runs the queries
     on the result.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

builddir = Path(sys.argv[1]).resolve()
top_builddir = Path(sys.argv[2]).resolve()
top_srcdir = Path(sys.argv[3]).resolve()
srcdir = Path(sys.argv[4]).resolve()
compile_cmd = shlex.split(os.environ["DTSR_COMPILE"])
link_cmd = shlex.split(os.environ["DTSR_LINK"])
libs = shlex.split(os.environ.get("DTSR_LIBS", ""))

work = Path(tempfile.mkdtemp(prefix="dtsearch-", dir=os.environ.get("TMPDIR")))
failures = []


def done(code):
    if os.environ.get("DTSR_TEST_KEEP"):
        print("kept " + str(work))
    else:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(code)


def fail(message):
    print("FAIL " + message)
    failures.append(message)


# A tool or the test program that was built with the sanitizers stops at the
# first error they find; the library leaks what it holds until it exits.
env = dict(os.environ)
env["ASAN_OPTIONS"] = "detect_leaks=0:detect_odr_violation=0:abort_on_error=0"
env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"


def run(cmd, cwd=None, env=env, timeout=300):
    r = subprocess.run([str(c) for c in cmd], cwd=cwd or work, text=True,
                       capture_output=True, env=env, timeout=timeout,
                       stdin=subprocess.DEVNULL)
    for marker in ("runtime error:", "ERROR: AddressSanitizer"):
        if marker in r.stderr:
            fail("%s reported by %s:\n%s" % (marker, cmd[0], r.stderr[-1500:]))
    return r


def tool(name):
    return builddir / name


# 1. every tool, without arguments, ends without a signal
for name in ("dtsrcreate", "dtsrdbrec", "dtsrhan", "dtsrload", "dtsrindex",
             "dtsrdelete", "huffcode", "dtsrkdump", "dtsrclean"):
    r = run([tool(name)])
    if r.returncode < 0:
        fail("%s died of signal %d when run without arguments" %
             (name, -r.returncode))
    else:
        print("ok    %s without arguments (exit %d)" % (name, r.returncode))

# 2. the corpus
CONSONANTS = "bdfgklmnprstvz"
VOWELS = "aeiou"
vocabulary = []
i = 0
while len(vocabulary) < 54:
    word = (CONSONANTS[i % 14] + VOWELS[(i // 14) % 5] +
            CONSONANTS[(i * 3 + 1) % 14] + VOWELS[(i * 7 + 2) % 5] +
            CONSONANTS[(i * 5 + 2) % 14] + VOWELS[(i * 11 + 3) % 5])
    if word not in vocabulary:
        vocabulary.append(word)
    i += 1
COMMON = vocabulary[:6]

seed = 20261104


def rand():
    """a number in [0, 1000): the same on every machine"""
    global seed
    seed = (seed * 6364136223846793005 + 1442695040888963407) % (1 << 64)
    return (seed >> 33) % 1000


DOCS = 420
FIRST_BATCH = 260
documents = []          # (key, set of words, text lines)
for n in range(1, DOCS + 1):
    words = [w for w in COMMON if rand() < 550]
    words += [w for w in vocabulary[6:] if rand() < 60]
    if len(words) < 2:
        words += [vocabulary[n % len(vocabulary)],
                  vocabulary[(n * 7 + 3) % len(vocabulary)]]
    words = list(dict.fromkeys(words))
    text = []
    for j in range(0, len(words), 6):
        text.append(" ".join(words[j:j + 6]))
    documents.append(("K%05d" % n, set(words), text))


def fzk(docs):
    out = []
    for key, words, text in docs:
        out.append("fz%s\nABSTRACT: document %s\n%s\n11/04/26~12:00\n%s\n\f\n" %
                   (key, key, key, "\n".join(text)))
    return "".join(out)


for name in ("dtsearch.dbd", "eng.stp", "eng.sfx"):
    src = (top_builddir / "programs/dtdocbook/dtsr" / name)
    if not src.exists():
        src = top_srcdir / "programs/dtdocbook/dtsr" / name
    shutil.copy(src, work / name)
(work / "batch1.fzk").write_text(fzk(documents[:FIRST_BATCH]))
(work / "batch2.fzk").write_text(fzk(documents[FIRST_BATCH:]))

steps = [
    ("dtsrcreate", ["-q", "-o", "-d" + str(work), str(work / "tdb")], 0),
    ("dtsrload", ["-d" + str(work / "tdb"), str(work / "batch1")], 0),
    ("dtsrindex", ["-d" + str(work / "tdb"), str(work / "batch1")], 0),
    ("dtsrload", ["-d" + str(work / "tdb"), str(work / "batch2")], 0),
    ("dtsrindex", ["-d" + str(work / "tdb"), str(work / "batch2")], 0),
]
for name, args, expected in steps:
    r = run([tool(name)] + args)
    if r.returncode != expected:
        fail("%s %s ended with %d:\n%s" % (name, " ".join(args), r.returncode,
                                           (r.stdout + r.stderr)[-1500:]))
        done(1)
    print("ok    %s %s" % (name, args[-1].rsplit("/", 1)[-1]))

r = run([tool("dtsrdbrec"), str(work / "tdb")])
if "reccount) is %d." % DOCS not in r.stdout:
    fail("the database does not say it holds %d documents:\n%s" %
         (DOCS, r.stdout[-800:]))
r = run([tool("dtsrkdump"), "-o", str(work / "tdb")])
if "TOTAL Objects Count = %d" % DOCS not in r.stdout:
    fail("the key dump does not count %d objects:\n%s" % (DOCS, r.stdout[-800:]))

# The address lists of the .d99 file: a header of 4104 bytes, then for each
# word the addresses (8 bytes each, big-endian, the record number of the
# document in all but the lowest byte and a weight in that byte) and the free
# holes after them (8 bytes of 0xff each).
import re
ADDRESS = 8


def check_d99(directory, live, label, holes_are_ones=True):
    """the address lists of the database in `directory` hold the record
    numbers of the documents whose keys are in `live`, and nothing else; the
    free holes behind them are all ones unless dtsrdelete has left what it
    removed in them"""
    words_checked = 0
    r = run([tool("dtsrkdump"), "-t1", str(directory / "tdb")])
    d99 = (directory / "tdb.d99").read_bytes()
    end_of_lists = 0
    for m in re.finditer(r'"(~?)([A-Z]+)"\s+\*?\s*dba=\S+\s+ofs=(\d+)\s+adr=(\d+)\s+fre=(\d+)',
                         r.stdout):
        stem, word = m.group(1), m.group(2).lower()
        offset, count, free = map(int, m.groups()[2:])
        end_of_lists = max(end_of_lists, offset + ADDRESS * (count + free))
        if word not in vocabulary:
            continue
        entries = [int.from_bytes(d99[offset + ADDRESS * j:offset + ADDRESS * (j + 1)], "big")
                   for j in range(count)]
        holes = d99[offset + ADDRESS * count:offset + ADDRESS * (count + free)]
        recnos = {e >> 8 for e in entries}
        expected = {n for n, (key, w, t) in enumerate(documents, 1)
                    if word in w and key in live}
        if recnos != expected or len(entries) != len(expected) or any(e >> 32 for e in entries):
            fail("%s: the .d99 entries of %s%s are not the documents with the word "
                 "(%d entries, %d documents)" % (label, stem, word, len(entries), len(expected)))
        elif holes_are_ones and holes != b"\xff" * len(holes):
            fail("%s: the free holes after the entries of %s%s are not all ones" %
                 (label, stem, word))
        else:
            words_checked += 1
    if words_checked < 40:
        fail("%s: only %d words of the key dump could be checked against the .d99 file" %
             (label, words_checked))
    elif len(d99) < end_of_lists:
        fail("%s: the .d99 file ends at %d, before the last list at %d" %
             (label, len(d99), end_of_lists))
    else:
        print("ok    %s: the .d99 file holds %d address lists of 8-byte entries" %
              (label, words_checked))


check_d99(work, {k for k, w, t in documents}, "the new database")

# 3. the queries
def make_queries(live):
    """the queries and the documents each must return, for the documents `live`"""
    def docs_with(*words):
        return {k for k, w, t in live if all(x in w for x in words)}

    ALL = {k for k, w, t in live}
    a, b, c, d, e, f = COMMON
    r1, r2, r3 = vocabulary[6], vocabulary[20], vocabulary[40]
    queries = []

    def add(types, query, expected):
        for t in types:
            queries.append((t, query, expected))

    for word in COMMON + [r1, r2, r3]:
        add("WS", word, docs_with(word))
    add("WS", "%s & %s" % (a, b), docs_with(a, b))
    add("WS", "%s %s" % (a, b), docs_with(a, b))
    add("WS", "%s %s %s" % (a, b, c), docs_with(a, b, c))
    add("WS", "%s & %s" % (a, r1), docs_with(a) & docs_with(r1))
    add("WS", "%s | %s" % (r1, r2), docs_with(r1) | docs_with(r2))
    add("WS", "%s | %s | %s" % (d, r2, r3),
        docs_with(d) | docs_with(r2) | docs_with(r3))
    add("WS", "%s & ~%s" % (a, b), docs_with(a) - docs_with(b))
    add("WS", "~%s" % c, ALL - docs_with(c))
    add("WS", "(%s | %s) & ~%s" % (a, b, c),
        (docs_with(a) | docs_with(b)) - docs_with(c))
    add("WS", "%s & (%s | %s) & ~(%s & %s)" % (d, e, f, a, b),
        docs_with(d) & (docs_with(e) | docs_with(f)) -
        (docs_with(a) & docs_with(b)))
    add("WS", "~%s & ~%s" % (e, r2), ALL - docs_with(e) - docs_with(r2))
    add("P", "%s %s %s" % (r1, r2, r3),
        docs_with(r1) | docs_with(r2) | docs_with(r3))
    add("P", "%s %s" % (c, d), docs_with(c) | docs_with(d))
    return queries


def write_queries(name, queries):
    (work / name).write_text("".join(
        "%s\t%s\t%s\n" % (t, q, " ".join(sorted(ex))) for t, q, ex in queries))


for word in COMMON:
    n = sum(1 for k, w, t in documents if word in w)
    if n <= 128:
        fail("%s is in only %d documents, too few to fill several blocks" %
             (word, n))

(work / "dtsearch.ocf").write_text("KEYTYPE tdb = 'K' Documents\nPATH tdb = %s/\n" % work)
write_queries("queries.txt", make_queries(documents))

sanitize = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
probe = work / "probe.c"
probe.write_text("int main(void) { return 0; }\n")
r = run(compile_cmd + sanitize + ["-o", work / "probe", probe], cwd=builddir)
if r.returncode:
    print("the compiler has no sanitizers: checking results only")
    sanitize = []

test = work / "dtsearch-api-test"
r = run(link_cmd + sanitize + ["-I" + str(top_srcdir / "include"),
                               "-I" + str(top_builddir / "include"),
                               "-o", test, srcdir / "tests/dtsearch-api-test.c",
                               builddir / "MsgCat.c"]
        + libs, cwd=builddir)
if r.returncode:
    fail("the API test does not build:\n" + r.stderr[-2000:])
    done(1)

def query_all(label, ocf, queries_file):
    r = run([test, ocf, queries_file], env=env)
    if r.returncode:
        fail("%s: %s" % (label, (r.stderr + r.stdout)[-2500:]))
    else:
        print("ok    %s: %s" % (label, r.stdout.strip().splitlines()[-1]))


query_all("queries on the new database", work / "dtsearch.ocf",
          work / "queries.txt")

# 4. validate and compress, then query the result
clean = work / "clean"
clean.mkdir()
env2 = dict(env, DBFPATH=str(work) + "/")
r = run([tool("dtsrclean"), "-v", "-oy", "tdb", str(clean)], env=env2)
if r.returncode != 0:
    fail("dtsrclean -v ended with %d:\n%s" % (r.returncode,
                                              (r.stdout + r.stderr)[-1500:]))
else:
    print("ok    dtsrclean -v")
    for name in os.listdir(work):
        if name.startswith("tdb.") and not (clean / name).exists():
            shutil.copy(work / name, clean / name)
    for name in ("eng.stp", "eng.sfx"):
        shutil.copy(work / name, clean / name)
    (clean / "dtsearch.ocf").write_text(
        "KEYTYPE tdb = 'K' Documents\nPATH tdb = %s/\n" % clean)
    query_all("queries after dtsrclean", clean / "dtsearch.ocf",
              work / "queries.txt")

# 5. delete documents and query again
gone = {key for n, (key, w, t) in enumerate(documents, 1) if n % 7 == 0}
deleted = work / "deleted"
deleted.mkdir()
for name in os.listdir(work):
    if name.startswith("tdb.") or name.startswith("eng."):
        shutil.copy(work / name, deleted / name)
(deleted / "dtsearch.ocf").write_text(
    "KEYTYPE tdb = 'K' Documents\nPATH tdb = %s/\n" % deleted)
(deleted / "remove.lst").write_text("".join("tdb %s test\n" % k for k in sorted(gone)))
r = run([tool("dtsrdelete"), "D", "-iremove.lst", "-y"], cwd=deleted)
if r.returncode != 0 or ("%d were deleted" % len(gone)) not in r.stdout:
    fail("dtsrdelete ended with %d:\n%s" % (r.returncode, (r.stdout + r.stderr)[-1500:]))
else:
    print("ok    dtsrdelete removed %d documents" % len(gone))
    check_d99(deleted, {k for k, w, t in documents} - gone, "after dtsrdelete",
              holes_are_ones=False)
    write_queries("queries-deleted.txt",
                  make_queries([d for d in documents if d[0] not in gone]))
    query_all("queries after dtsrdelete", deleted / "dtsearch.ocf",
              work / "queries-deleted.txt")

print("%d failures" % len(failures))
done(1 if failures else 0)
