#!/usr/bin/env python3
"""Run the tests of dtmailspool, the set-group-id helper of the mailer.

check-spool-helper.py SRCDIR

SRCDIR is programs/dtmail.  The production sources of dtmailspool are compiled
with AddressSanitizer and UndefinedBehaviorSanitizer for a spool directory of
the test, started the way the mailer starts them (a socket as descriptor 3, an
empty environment) and driven over the protocol by this program, which plays
the mailer: the frames of every operation, every truncation of them, frames
that are too long or wrong, descriptors where none are allowed, files and links
a hostile spool directory can hold, the locks and the replacement file of a
save.  The helper is also compiled against a fake kernel for the group ids
that refuses or ignores a change of the effective group and records every
change.  Source checks keep the group calls in one place and out of the rest
of the mailer.

Environment (as `make check` supplies it, see programs/dtmail/Makefile.am):
  DTMAIL_CC            the C compile command of the build, flags included
                       (default: $CC or cc)
  DTMAIL_SPOOL_STAGED  a directory with dtmailspool/main.c, dtmailspool/spool.c,
                       dtmailspool/spool.h and include/DtMail/SpoolProtocol.h of
                       another revision or a changed copy, compiled instead (a
                       helper with a defect is how the test is checked itself)
  DTMAIL_GUARD_ROOT    the tree the source checks read (default: SRCDIR)
  DTMAIL_TEST_CASES    run only these cases, a blank separated list of names
  DTMAIL_TEST_SUDO     run the cases that need another user too, through
                       `sudo -n` (files of another user; the helper run as
                       another user)
  DTMAIL_TEST_KEEP     keep the work directory
"""
import errno
import fcntl
import os
from pathlib import Path
import pwd
import re
import resource
import shlex
import shutil
import signal
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import time

srcdir = Path(sys.argv[1]).resolve()
staged = Path(os.environ["DTMAIL_SPOOL_STAGED"]).resolve() if os.environ.get("DTMAIL_SPOOL_STAGED") else srcdir
guard_root = Path(os.environ.get("DTMAIL_GUARD_ROOT", str(srcdir))).resolve()
cc = shlex.split(os.environ.get("DTMAIL_CC") or os.environ.get("CC") or "cc")
work = Path(tempfile.mkdtemp(prefix="dtmail-spool-", dir=os.environ.get("TMPDIR")))
spool = work / "spool"
user = pwd.getpwuid(os.getuid()).pw_name
use_sudo = bool(os.environ.get("DTMAIL_TEST_SUDO"))

# protocol constants (include/DtMail/SpoolProtocol.h)
MAGIC = 0x44545350
OP_OPEN, OP_LOCK_TRY, OP_LOCK_VERIFY, OP_LOCK_RELEASE, OP_LOCK_BREAK = 1, 2, 3, 4, 5
OP_TEMP_CREATE, OP_TEMP_COMMIT, OP_TEMP_DISCARD = 6, 7, 8
OPEN_WRITE, OPEN_CREATE = 1, 2
ACCESS_READ, ACCESS_WRITE = 1, 2
LOCK_MAIL, LOCK_DTMAIL = 1, 2
OK, BUSY, VANISHED, NOT_OURS, REFUSED, BAD_REQUEST, TEMP_PENDING, NO_TEMP = 0, 1000, 1001, 1002, 1003, 1004, 1005, 1006
MAX_PAYLOAD = 256
EXIT_PROTOCOL, EXIT_IO, EXIT_GROUP, EXIT_SETUP = 10, 11, 20, 21


class Failed(Exception):
    pass


class Skipped(Exception):
    pass


def check(condition, message="check failed"):
    if not condition:
        raise Failed(message)


def run(cmd, **kw):
    return subprocess.run(cmd, text=True, capture_output=True, **kw)


def build(name, extra, sources, spool_dir):
    out = work / name
    inc = staged / "include"
    r = run(cc + ["-O1", "-g", "-fno-omit-frame-pointer", "-fsanitize=address,undefined",
                  "-fno-sanitize-recover=undefined", "-Wall", "-Wextra", "-Werror",
                  "-I", str(inc), '-DDTMAIL_SPOOL_DIR="%s"' % spool_dir] + extra
            + [str(staged / "dtmailspool" / s) for s in sources] + ["-o", str(out)])
    if r.returncode:
        sys.exit("compile of %s failed:\n%s" % (name, r.stderr))
    return out


def frame(op, payload=b""):
    return struct.pack("=III", MAGIC, op, len(payload)) + payload


def u32(*values):
    return b"".join(struct.pack("=I", v) for v in values)


def reset_spool():
    shutil.rmtree(spool, ignore_errors=True)
    spool.mkdir(mode=0o770)
    os.chmod(spool, 0o770)


def listing():
    return sorted(os.listdir(spool))


SANITIZER_ENV = {"ASAN_OPTIONS": "detect_leaks=1:abort_on_error=0", "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}


class Helper:
    """One helper process and the mailer's end of its socket."""

    def __init__(self, binary, close_std=False, leak_fds=(), as_user=None, keep_stderr=True, env=None, limit_fsize=None):
        self.sock, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(15)
        self.errfile = work / ("stderr.%d" % time.monotonic_ns())
        actions = [(os.POSIX_SPAWN_DUP2, child.fileno(), 3)]
        if close_std:
            actions += [(os.POSIX_SPAWN_CLOSE, 0), (os.POSIX_SPAWN_CLOSE, 1), (os.POSIX_SPAWN_CLOSE, 2)]
        else:
            actions += [(os.POSIX_SPAWN_OPEN, 2, str(self.errfile), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)]
        for number, fd in leak_fds:
            actions.append((os.POSIX_SPAWN_DUP2, fd, number))
        argv = [str(binary)]
        old_limit = None
        if limit_fsize is not None:
            old_limit = resource.getrlimit(resource.RLIMIT_FSIZE)
            resource.setrlimit(resource.RLIMIT_FSIZE, (limit_fsize, old_limit[1]))
        try:
            self._spawn(binary, argv, as_user, env, actions)
        finally:
            if old_limit is not None:
                resource.setrlimit(resource.RLIMIT_FSIZE, old_limit)
        child.close()
        self.status = None

    def _spawn(self, binary, argv, as_user, env, actions):
        if as_user:
            argv = ["sudo", "-n", "-u", as_user, "--"] + argv
            self.pid = os.posix_spawn("/usr/bin/sudo", argv, dict(env or SANITIZER_ENV), file_actions=actions)
        else:
            self.pid = os.posix_spawn(str(binary), argv, dict(env if env is not None else SANITIZER_ENV),
                                      file_actions=actions)

    def send(self, data, fds=()):
        if fds:
            socket.send_fds(self.sock, [data], list(fds))
        else:
            self.sock.sendall(data)

    def request(self, op, payload=b"", fds=()):
        self.send(frame(op, payload), fds)

    def read_exactly(self, count, fds):
        data = b""
        while len(data) < count:
            chunk, got, _flags, _addr = socket.recv_fds(self.sock, count - len(data), 4)
            fds.extend(got)
            if not chunk:
                raise Failed("the helper closed the connection after %d of %d bytes" % (len(data), count))
            data += chunk
        return data

    def reply(self):
        """(status, payload, fds) of the next reply."""
        fds = []
        magic, status, length = struct.unpack("=III", self.read_exactly(12, fds))
        check(magic == MAGIC, "reply with magic %#x" % magic)
        check(length <= MAX_PAYLOAD, "reply of %d bytes" % length)
        payload = self.read_exactly(length, fds) if length else b""
        return status, payload, fds

    def call(self, op, payload=b""):
        self.request(op, payload)
        return self.reply()

    def finish(self, close=True, timeout=15):
        """Close the connection (unless it is closed) and wait; the exit status."""
        if close:
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        deadline = time.monotonic() + timeout
        while self.status is None:
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                self.status = status
                break
            if time.monotonic() > deadline:
                os.kill(self.pid, signal.SIGKILL)
                os.waitpid(self.pid, 0)
                raise Failed("the helper did not end")
            time.sleep(0.01)
        self.sock.close()
        return os.waitstatus_to_exitcode(self.status)

    def stderr(self):
        return self.errfile.read_text() if self.errfile.exists() else ""

    def kill(self):
        if self.status is None:
            try:
                os.kill(self.pid, signal.SIGKILL)
            except OSError:
                pass
            try:
                os.waitpid(self.pid, 0)
            except OSError:
                pass
            self.status = 0
        self.sock.close()


binaries = {}
cases = []


def case(function):
    cases.append(function)
    return function


def helper_ends(h, expected):
    code = h.finish()
    check(code == expected, "exit status %d, expected %d" % (code, expected))
    check(h.stderr() == "", "the helper wrote: %r" % h.stderr())


def read_fd(fd):
    os.lseek(fd, 0, os.SEEK_SET)
    chunks = []
    while True:
        chunk = os.read(fd, 65536)
        if not chunk:
            break
        chunks.append(chunk)
    return b"".join(chunks)


# ---------------------------------------------------------------------------
# the protocol

REQUESTS = {
    "open": (OP_OPEN, u32(OPEN_WRITE | OPEN_CREATE)),
    "lock_try": (OP_LOCK_TRY, u32(LOCK_MAIL, 5) + b"0\0abc"),
    "lock_verify": (OP_LOCK_VERIFY, u32(LOCK_MAIL, 5) + b"0\0abc"),
    "lock_release": (OP_LOCK_RELEASE, u32(LOCK_MAIL, 5) + b"0\0abc"),
    "lock_break": (OP_LOCK_BREAK, u32(LOCK_DTMAIL)),
    "temp_create": (OP_TEMP_CREATE, b""),
    "temp_commit": (OP_TEMP_COMMIT, b""),
    "temp_discard": (OP_TEMP_DISCARD, b""),
}


@case
def every_truncated_request_ends_the_helper():
    """A request that stops anywhere inside its frame ends the helper with the protocol status;
    a connection closed between two frames is the normal end."""
    h = Helper(binaries["helper"])
    helper_ends(h, 0)
    for name, (op, payload) in REQUESTS.items():
        data = frame(op, payload)
        for cut in range(1, len(data)):
            reset_spool()
            h = Helper(binaries["helper"])
            try:
                h.send(data[:cut])
                h.sock.shutdown(socket.SHUT_WR)
                helper_ends(h, EXIT_PROTOCOL)
            except Exception as e:
                h.kill()
                raise Failed("%s cut at %d of %d: %s" % (name, cut, len(data), e))
            check(listing() == [], "%s cut at %d left %r" % (name, cut, listing()))


@case
def oversize_and_wrong_frames_end_the_helper():
    """Lengths above the limit, with and without the bytes they announce, a wrong magic number and
    a frame made of nothing but a header of garbage."""
    wrong = [
        ("length 257, no payload", struct.pack("=III", MAGIC, OP_OPEN, 257)),
        ("length 257, payload", struct.pack("=III", MAGIC, OP_OPEN, 257) + b"\0" * 257),
        ("length 0xffffffff", struct.pack("=III", MAGIC, OP_OPEN, 0xFFFFFFFF)),
        ("length 0x80000000, payload", struct.pack("=III", MAGIC, OP_LOCK_TRY, 0x80000000) + b"x" * 300),
        ("magic 0", struct.pack("=III", 0, OP_OPEN, 4) + u32(0)),
        ("magic swapped", struct.pack(">III", MAGIC, OP_OPEN, 4) + u32(0)),
        ("garbage header", bytes(range(12))),
        ("a text line", b"GET / HTTP/1.0\r\n\r\n"),
    ]
    for name, data in wrong:
        reset_spool()
        h = Helper(binaries["helper"])
        try:
            h.send(data)
            helper_ends(h, EXIT_PROTOCOL)
        except Exception as e:
            h.kill()
            raise Failed("%s: %s" % (name, e))
        check(listing() == [], "%s changed the spool: %r" % (name, listing()))


@case
def malformed_requests_are_refused_and_end_the_helper():
    """A frame that is whole but wrong for its operation is answered with BAD_REQUEST and ends the helper."""
    wrong = [
        ("unknown operation 0", 0, b""),
        ("unknown operation 9", 9, b""),
        ("unknown operation 0xffffffff", 0xFFFFFFFF, b""),
        ("a reply code as a request", BUSY, b""),
        ("open without flags", OP_OPEN, b""),
        ("open with 3 bytes", OP_OPEN, b"\0\0\0"),
        ("open with 5 bytes", OP_OPEN, u32(0) + b"\0"),
        ("open with an unknown flag", OP_OPEN, u32(4)),
        ("open with every flag", OP_OPEN, u32(0xFFFFFFFF)),
        ("lock without content", OP_LOCK_TRY, u32(LOCK_MAIL, 0)),
        ("lock with a header only", OP_LOCK_TRY, u32(LOCK_MAIL)),
        ("lock content too long", OP_LOCK_TRY, u32(LOCK_MAIL, 129) + b"a" * 129),
        ("lock content longer than announced", OP_LOCK_TRY, u32(LOCK_MAIL, 3) + b"abcd"),
        ("lock content shorter than announced", OP_LOCK_TRY, u32(LOCK_MAIL, 4) + b"abc"),
        ("lock content of 2**32-1", OP_LOCK_VERIFY, u32(LOCK_MAIL, 0xFFFFFFFF) + b"abc"),
        ("lock kind 0", OP_LOCK_TRY, u32(0, 1) + b"a"),
        ("lock kind 3", OP_LOCK_RELEASE, u32(3, 1) + b"a"),
        ("break without kind", OP_LOCK_BREAK, b""),
        ("break with kind 0", OP_LOCK_BREAK, u32(0)),
        ("break with kind 3", OP_LOCK_BREAK, u32(3)),
        ("break with extra bytes", OP_LOCK_BREAK, u32(LOCK_MAIL) + b"x"),
        ("temp create with a payload", OP_TEMP_CREATE, b"x"),
        ("temp commit with a payload", OP_TEMP_COMMIT, u32(0)),
        ("temp discard with a payload", OP_TEMP_DISCARD, b"\0" * 8),
        ("maximum payload", OP_OPEN, b"\0" * MAX_PAYLOAD),
    ]
    for name, op, payload in wrong:
        reset_spool()
        h = Helper(binaries["helper"])
        try:
            status, body, fds = h.call(op, payload)
            check(status == BAD_REQUEST and not body and not fds, "%s: status %d" % (name, status))
            helper_ends(h, EXIT_PROTOCOL)
        except Exception as e:
            h.kill()
            raise Failed("%s: %s" % (name, e))
        check(listing() == [], "%s changed the spool: %r" % (name, listing()))


@case
def a_request_with_a_descriptor_ends_the_helper():
    """No request carries a descriptor; one that does is refused and the descriptor is closed."""
    reset_spool()
    for how in ("header", "with open"):
        h = Helper(binaries["helper"])
        r, w = os.pipe()
        try:
            data = frame(OP_OPEN, u32(OPEN_WRITE | OPEN_CREATE))
            if how == "header":
                h.send(data, fds=[r])
            else:
                h.send(frame(OP_TEMP_DISCARD) + data[:6])
                h.reply()
                h.send(data[6:], fds=[r])
            helper_ends(h, EXIT_PROTOCOL)
        except Exception as e:
            h.kill()
            raise Failed("%s: %s" % (how, e))
        finally:
            os.close(r)
            os.close(w)
        check(listing() == [], "an open that came with a descriptor changed the spool: %r" % listing())


@case
def several_descriptors_are_refused():
    reset_spool()
    h = Helper(binaries["helper"])
    pipes = [os.pipe() for _ in range(3)]
    try:
        h.send(frame(OP_OPEN, u32(OPEN_WRITE | OPEN_CREATE)), fds=[p[0] for p in pipes])
        helper_ends(h, EXIT_PROTOCOL)
    except Exception:
        h.kill()
        raise
    finally:
        for r, w in pipes:
            os.close(r)
            os.close(w)
    check(listing() == [], "the spool changed: %r" % listing())


@case
def frames_in_pieces_and_in_one_piece_are_read():
    """The stream is cut anywhere by the socket: a frame sent a byte at a time is read, and frames sent
    together are answered one by one in order."""
    reset_spool()
    h = Helper(binaries["helper"])
    try:
        data = frame(OP_OPEN, u32(OPEN_WRITE | OPEN_CREATE))
        for i in range(len(data)):
            h.send(data[i:i + 1])
            time.sleep(0.002)
        status, body, fds = h.reply()
        check(status == OK and len(fds) == 1 and len(body) == 8, "status %d" % status)
        os.close(fds[0])
        both = frame(OP_LOCK_TRY, u32(LOCK_MAIL, 3) + b"abc") + frame(OP_LOCK_TRY, u32(LOCK_MAIL, 3) + b"abc") \
            + frame(OP_LOCK_VERIFY, u32(LOCK_MAIL, 3) + b"abc")
        h.send(both)
        first, second, third = h.reply(), h.reply(), h.reply()
        check(first[0] == OK and second[0] == BUSY and third[0] == OK, "%r" % [first[0], second[0], third[0]])
        check(len(second[1]) == 4, "a busy reply carries the age")
    except Exception:
        h.kill()
        raise
    helper_ends(h, 0)


@case
def a_frame_after_a_good_one_with_a_wrong_magic_ends_the_helper():
    reset_spool()
    h = Helper(binaries["helper"])
    try:
        h.send(frame(OP_LOCK_BREAK, u32(LOCK_MAIL)) + b"JUNKJUNKJUNKJUNK")
        status, body, fds = h.reply()
        check(status == OK, "the first frame is answered")
    except Exception:
        h.kill()
        raise
    helper_ends(h, EXIT_PROTOCOL)


# ---------------------------------------------------------------------------
# the mailbox

def open_box(h, flags):
    status, body, fds = h.call(OP_OPEN, u32(flags))
    if status != OK:
        check(not fds and not body, "a failed open returned something")
        return status, None, 0, 0
    check(len(fds) == 1 and len(body) == 8, "an open returned %d descriptors and %d bytes" % (len(fds), len(body)))
    access, created = struct.unpack("=II", body)
    return status, fds[0], access, created


@case
def open_gives_the_descriptor_of_the_users_mailbox():
    reset_spool()
    box = spool / user
    box.write_bytes(b"From a@b Mon Nov  9 10:00:00 2026\n\nhello\n")
    os.chmod(box, 0o660)
    h = Helper(binaries["helper"])
    try:
        status, fd, access, created = open_box(h, OPEN_WRITE)
        check(status == OK and access == ACCESS_WRITE and created == 0, "status %d access %d" % (status, access))
        check(read_fd(fd).endswith(b"hello\n"), "the descriptor reads the mailbox")
        os.write(fd, b"")  # writable
        check(not (fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_NONBLOCK), "the descriptor is blocking")
        os.close(fd)
        status, fd, access, created = open_box(h, 0)
        check(status == OK and access == ACCESS_READ, "a read-only open is read-only")
        try:
            os.write(fd, b"x")
            raise Failed("a read-only descriptor was written")
        except OSError as e:
            check(e.errno == errno.EBADF, "write: %s" % e)
        os.close(fd)
    except Exception:
        h.kill()
        raise
    helper_ends(h, 0)


@case
def open_creates_a_missing_mailbox_with_the_mail_modes():
    reset_spool()
    old = os.umask(0o277)       # a umask must not decide the modes of a spool file
    try:
        h = Helper(binaries["helper"])
        try:
            check(open_box(h, OPEN_WRITE)[0] == errno.ENOENT, "a missing mailbox is not made without the flag")
            check(listing() == [], "...and nothing is left")
            status, fd, access, created = open_box(h, OPEN_WRITE | OPEN_CREATE)
            check(status == OK and created == 1 and access == ACCESS_WRITE, "status %d" % status)
            st = os.fstat(fd)
            check(stat.S_IMODE(st.st_mode) == 0o660, "mode %o" % stat.S_IMODE(st.st_mode))
            check(st.st_uid == os.getuid() and st.st_gid == os.getegid(), "owner %d group %d" % (st.st_uid, st.st_gid))
            os.close(fd)
            status, fd, access, created = open_box(h, OPEN_WRITE | OPEN_CREATE)
            check(status == OK and created == 0, "an existing mailbox is not made again")
            os.close(fd)
        except Exception:
            h.kill()
            raise
        helper_ends(h, 0)
    finally:
        os.umask(old)
    check(listing() == [user], "the spool holds %r" % listing())


@case
def a_mailbox_that_does_not_allow_writing_is_opened_read_only():
    if os.getuid() == 0:
        raise Skipped("the super user can write every file")
    reset_spool()
    (spool / user).write_bytes(b"x\n")
    os.chmod(spool / user, 0o400)
    h = Helper(binaries["helper"])
    try:
        status, fd, access, created = open_box(h, OPEN_WRITE)
        check(status == OK and access == ACCESS_READ, "status %d access %d" % (status, access))
        os.close(fd)
        # creating asks for write access and is not turned into a read
        status, fd, access, created = open_box(h, OPEN_WRITE | OPEN_CREATE)
        check(status == errno.EACCES and fd is None, "a create is not turned into a read: status %d" % status)
    except Exception:
        h.kill()
        raise
    helper_ends(h, 0)


@case
def a_link_in_place_of_the_mailbox_is_refused():
    """The name of the mailbox is a link: to a file, to a file that does not exist, to a directory."""
    reset_spool()
    target = work / "target"
    target.write_text("secret that is not the user's mailbox\n")
    for what, destination in (("file", target), ("dangling", work / "nothing-here"), ("directory", work)):
        for flags in (0, OPEN_WRITE, OPEN_WRITE | OPEN_CREATE):
            try:
                os.unlink(spool / user)
            except FileNotFoundError:
                pass
            os.symlink(destination, spool / user)
            h = Helper(binaries["helper"])
            try:
                status, fd, access, created = open_box(h, flags)
                check(status == REFUSED and fd is None, "%s link, flags %d: status %d" % (what, flags, status))
            except Exception:
                h.kill()
                raise
            helper_ends(h, 0)
            check(not (work / "nothing-here").exists(), "a file was made through a dangling link")
            check(target.read_text() == "secret that is not the user's mailbox\n", "the target changed")
        os.unlink(spool / user)


@case
def a_directory_or_a_special_file_is_not_a_mailbox():
    reset_spool()
    os.mkdir(spool / user)
    h = Helper(binaries["helper"])
    try:
        for flags in (0, OPEN_WRITE, OPEN_WRITE | OPEN_CREATE):
            status, fd, access, created = open_box(h, flags)
            check(status == errno.EISDIR and fd is None, "directory, flags %d: status %d" % (flags, status))
    except Exception:
        h.kill()
        raise
    helper_ends(h, 0)
    os.rmdir(spool / user)
    os.mkfifo(spool / user, 0o660)
    h = Helper(binaries["helper"])
    try:
        for flags in (0, OPEN_WRITE):
            status, fd, access, created = open_box(h, flags)
            check(status == REFUSED and fd is None, "fifo, flags %d: status %d" % (flags, status))
    except Exception:
        h.kill()
        raise
    helper_ends(h, 0)


def other_user():
    for name in ("nobody", "daemon", "nogroup"):
        try:
            entry = pwd.getpwnam(name)
        except KeyError:
            continue
        if entry.pw_uid != os.getuid():
            return entry
    return None


def sudo(*args):
    return run(["sudo", "-n"] + list(args))


@case
def a_mailbox_of_another_user_is_refused():
    """Needs another user: the file is changed to belong to one with `sudo -n chown`."""
    other = other_user()
    if other is None or (os.getuid() != 0 and not use_sudo):
        raise Skipped("needs another user and DTMAIL_TEST_SUDO (or the super user)")
    reset_spool()
    box = spool / user
    box.write_bytes(b"From a@b Mon Nov  9 10:00:00 2026\n\nmine?\n")
    os.chmod(box, 0o666)
    if os.getuid() == 0:
        os.chown(box, other.pw_uid, other.pw_gid)
    else:
        check(sudo("chown", "%d" % other.pw_uid, str(box)).returncode == 0, "sudo chown")
    h = Helper(binaries["helper"])
    try:
        for flags in (0, OPEN_WRITE, OPEN_WRITE | OPEN_CREATE):
            status, fd, access, created = open_box(h, flags)
            check(status == REFUSED and fd is None, "flags %d: status %d" % (flags, status))
        check(h.call(OP_TEMP_CREATE)[0] == REFUSED, "no replacement file for a mailbox of another user")
        check(h.call(OP_LOCK_TRY, u32(LOCK_MAIL, 1) + b"x")[0] == OK, "the lock files are the user's own")
    except Exception:
        h.kill()
        raise
    helper_ends(h, 0)
    if os.getuid() != 0:
        sudo("rm", "-f", str(box))


# ---------------------------------------------------------------------------
# the lock files

def lock(h, op, content, kind=LOCK_MAIL):
    return h.call(op, u32(kind, len(content)) + content)


@case
def the_lock_file_is_made_whole_and_removed_by_its_owner_only():
    reset_spool()
    text = b"0\0" + b"0000123420261110dtmail"
    h = Helper(binaries["helper"])
    try:
        status, body, fds = lock(h, OP_LOCK_TRY, text)
        check(status == OK and not body and not fds, "status %d" % status)
        check(listing() == [user + ".lock"], "the spool holds %r (no temporary file may stay)" % listing())
        check((spool / (user + ".lock")).read_bytes() == text, "the lock file holds the text")
        st = os.stat(spool / (user + ".lock"))
        check(stat.S_IMODE(st.st_mode) == 0o660 and st.st_uid == os.getuid(), "mode %o" % stat.S_IMODE(st.st_mode))
        check(lock(h, OP_LOCK_VERIFY, text)[0] == OK, "verify")
        check(lock(h, OP_LOCK_VERIFY, text + b"x")[0] == NOT_OURS, "verify of longer text")
        check(lock(h, OP_LOCK_VERIFY, text[:-1])[0] == NOT_OURS, "verify of shorter text")
        check(lock(h, OP_LOCK_VERIFY, b"0\0someone else")[0] == NOT_OURS, "verify of another text")
        # release of a lock that is not ours leaves it
        check(lock(h, OP_LOCK_RELEASE, b"0\0someone else")[0] == NOT_OURS, "release of another text")
        check(listing() == [user + ".lock"], "...and the file stays")
        check(lock(h, OP_LOCK_RELEASE, text)[0] == OK, "release")
        check(listing() == [], "the lock file is gone: %r" % listing())
        check(lock(h, OP_LOCK_RELEASE, text)[0] == errno.ENOENT, "release of a lock that is gone")
        check(lock(h, OP_LOCK_VERIFY, text)[0] == errno.ENOENT, "verify of a lock that is gone")
    except Exception:
        h.kill()
        raise
    helper_ends(h, 0)


@case
def a_lock_that_is_held_is_busy_and_tells_its_age():
    reset_spool()
    h = Helper(binaries["helper"])
    try:
        check(lock(h, OP_LOCK_TRY, b"0\0first")[0] == OK, "the first lock")
        status, body, fds = lock(h, OP_LOCK_TRY, b"0\0second")
        check(status == BUSY and len(body) == 4 and not fds, "status %d" % status)
        check(struct.unpack("=I", body)[0] <= 1, "a lock that was just made is not old")
        check((spool / (user + ".lock")).read_bytes() == b"0\0first", "the held lock is not changed")
        check(listing() == [user + ".lock"], "no temporary file stays after a busy lock: %r" % listing())
        time.sleep(2.2)
        status, body, fds = lock(h, OP_LOCK_TRY, b"0\0third")
        age = struct.unpack("=I", body)[0]
        check(status == BUSY and 2 <= age <= 5, "age %d after two seconds" % age)
        # a lock that is not ours can be broken, whoever made it
        check(h.call(OP_LOCK_BREAK, u32(LOCK_MAIL))[0] == OK, "break")
        check(listing() == [], "...removes it")
        check(h.call(OP_LOCK_BREAK, u32(LOCK_MAIL))[0] == OK, "break of nothing is fine")
        check(lock(h, OP_LOCK_TRY, b"0\0fourth")[0] == OK, "the lock can be taken again")
    except Exception:
        h.kill()
        raise
    helper_ends(h, 0)


@case
def the_two_kinds_of_lock_do_not_meet():
    reset_spool()
    h = Helper(binaries["helper"])
    try:
        check(lock(h, OP_LOCK_TRY, b"0\0mail", LOCK_MAIL)[0] == OK, "mail lock")
        check(lock(h, OP_LOCK_TRY, b"4711\n", LOCK_DTMAIL)[0] == OK, "dtmail lock")
        check(listing() == [user + ".dtmail", user + ".lock"], "%r" % listing())
        check(lock(h, OP_LOCK_VERIFY, b"4711\n", LOCK_MAIL)[0] == NOT_OURS, "kinds are separate")
        check(lock(h, OP_LOCK_RELEASE, b"4711\n", LOCK_DTMAIL)[0] == OK, "release dtmail")
        check(listing() == [user + ".lock"], "%r" % listing())
    except Exception:
        h.kill()
        raise
    helper_ends(h, 0)


@case
def lock_names_never_follow_links():
    """A link under the name of the lock file is not followed to read or to remove anything."""
    reset_spool()
    target = work / "target"
    target.write_text("0\0first")
    os.symlink(target, spool / (user + ".lock"))
    h = Helper(binaries["helper"])
    try:
        check(lock(h, OP_LOCK_VERIFY, b"0\0first")[0] == REFUSED, "verify through a link")
        status = lock(h, OP_LOCK_RELEASE, b"0\0first")[0]
        check(status == REFUSED and target.exists() and os.path.islink(spool / (user + ".lock")),
              "release through a link: status %d" % status)
        status = lock(h, OP_LOCK_TRY, b"0\0mine")[0]
        check(status in (BUSY, VANISHED), "a lock under a name that is a link is held: %d" % status)
        check(target.read_text() == "0\0first", "the target stays")
    except Exception:
        h.kill()
        raise
    helper_ends(h, 0)


@case
def a_directory_in_place_of_the_lock_file_is_not_removed():
    reset_spool()
    os.mkdir(spool / (user + ".lock"))
    h = Helper(binaries["helper"])
    try:
        check(lock(h, OP_LOCK_TRY, b"0\0mine")[0] == BUSY, "a directory holds the name")
        status = h.call(OP_LOCK_BREAK, u32(LOCK_MAIL))[0]
        check(status != OK, "a directory is not removed as a lock file")
        check(os.path.isdir(spool / (user + ".lock")), "the directory stays")
        check(lock(h, OP_LOCK_RELEASE, b"0\0mine")[0] != OK, "...or released")
    except Exception:
        h.kill()
        raise
    helper_ends(h, 0)


# ---------------------------------------------------------------------------
# the replacement file of a save

@case
def a_save_replaces_the_mailbox_with_the_mode_and_group_of_the_old_one():
    reset_spool()
    box = spool / user
    box.write_bytes(b"old content\n")
    os.chmod(box, 0o640)
    old = os.stat(box)
    h = Helper(binaries["helper"])
    try:
        status, body, fds = h.call(OP_TEMP_CREATE)
        check(status == OK and len(fds) == 1 and not body, "status %d" % status)
        fd = fds[0]
        names = [n for n in listing() if n != user]
        check(len(names) == 1 and re.fullmatch(re.escape(user) + r"\.tmp\.[A-Za-z0-9]{6}", names[0]),
              "the temporary file is %r" % names)
        st = os.fstat(fd)
        check(stat.S_IMODE(st.st_mode) == 0o640 and st.st_uid == os.getuid() and st.st_gid == old.st_gid,
              "mode %o group %d" % (stat.S_IMODE(st.st_mode), st.st_gid))
        check(h.call(OP_TEMP_CREATE)[0] == TEMP_PENDING, "one temporary file at a time")
        os.write(fd, b"new content\n")
        check(box.read_bytes() == b"old content\n", "the mailbox is not changed before the commit")
        status, body, fds = h.call(OP_TEMP_COMMIT)
        check(status == OK and not fds, "commit status %d" % status)
        check(box.read_bytes() == b"new content\n", "the mailbox holds the new text")
        check(listing() == [user], "no temporary file stays: %r" % listing())
        check(os.stat(box).st_ino == os.fstat(fd).st_ino, "the descriptor is the new mailbox")
        check(stat.S_IMODE(os.stat(box).st_mode) == 0o640, "the mode stays")
        os.close(fd)
        check(h.call(OP_TEMP_COMMIT)[0] == NO_TEMP, "commit without a file")
        check(h.call(OP_TEMP_DISCARD)[0] == OK, "discard without a file is nothing")
    except Exception:
        h.kill()
        raise
    helper_ends(h, 0)


@case
def a_discarded_or_forgotten_replacement_file_is_removed():
    reset_spool()
    (spool / user).write_bytes(b"x\n")
    os.chmod(spool / user, 0o660)
    h = Helper(binaries["helper"])
    try:
        status, body, fds = h.call(OP_TEMP_CREATE)
        check(status == OK, "create")
        os.close(fds[0])
        check(h.call(OP_TEMP_DISCARD)[0] == OK, "discard")
        check(listing() == [user], "discard leaves %r" % listing())
        status, body, fds = h.call(OP_TEMP_CREATE)
        check(status == OK and len(listing()) == 2, "created again")
        os.close(fds[0])
    except Exception:
        h.kill()
        raise
    helper_ends(h, 0)       # the connection is closed with a file pending
    check(listing() == [user], "the helper removes its temporary file when the connection ends: %r" % listing())


@case
def a_terminated_helper_removes_its_replacement_file():
    reset_spool()
    (spool / user).write_bytes(b"x\n")
    for sig in (signal.SIGTERM,):
        h = Helper(binaries["helper"])
        try:
            status, body, fds = h.call(OP_TEMP_CREATE)
            check(status == OK, "create")
            os.close(fds[0])
            os.kill(h.pid, sig)
            code = h.finish(close=False)
        except Exception:
            h.kill()
            raise
        check(code == 0, "%s ends the helper with status %d" % (sig.name, code))
        check(h.stderr() == "", "the helper wrote %r" % h.stderr())
        check(listing() == [user], "%s leaves %r" % (sig.name, listing()))


@case
def a_save_does_not_replace_a_link_or_a_mailbox_that_became_one():
    reset_spool()
    target = work / "target"
    target.write_text("not a mailbox\n")
    os.symlink(target, spool / user)
    h = Helper(binaries["helper"])
    try:
        check(h.call(OP_TEMP_CREATE)[0] == REFUSED, "no replacement for a link")
        os.unlink(spool / user)
        (spool / user).write_bytes(b"mailbox\n")
        status, body, fds = h.call(OP_TEMP_CREATE)
        check(status == OK, "create")
        os.close(fds[0])
        # the mailbox is replaced by a link to somebody else's file after the temporary file was made
        os.unlink(spool / user)
        os.symlink(target, spool / user)
        check(h.call(OP_TEMP_COMMIT)[0] == REFUSED, "no commit over a link")
        check(os.path.islink(spool / user) and target.read_text() == "not a mailbox\n", "the link and its target stay")
        check(h.call(OP_TEMP_DISCARD)[0] == OK, "discard")
        check(listing() == [user], "%r" % listing())
    except Exception:
        h.kill()
        raise
    helper_ends(h, 0)


@case
def a_save_of_a_mailbox_that_vanished_makes_it_again():
    reset_spool()
    h = Helper(binaries["helper"])
    try:
        status, body, fds = h.call(OP_TEMP_CREATE)
        check(status == OK, "create without a mailbox")
        st = os.fstat(fds[0])
        check(stat.S_IMODE(st.st_mode) == 0o660, "the mail mode %o" % stat.S_IMODE(st.st_mode))
        os.write(fds[0], b"saved\n")
        os.close(fds[0])
        check(h.call(OP_TEMP_COMMIT)[0] == OK, "commit")
        check((spool / user).read_bytes() == b"saved\n" and listing() == [user], "%r" % listing())
    except Exception:
        h.kill()
        raise
    helper_ends(h, 0)


# ---------------------------------------------------------------------------
# what the helper does with what it inherits

@case
def the_helper_starts_clean():
    """Descriptors that were open in the mailer are closed, the helper works with the standard descriptors
    closed, a core file is not written and the process cannot be examined or traced by its user."""
    reset_spool()
    r, w = os.pipe()
    h = Helper(binaries["helper"], close_std=True, leak_fds=[(7, w), (9, w)])
    os.close(w)                                   # the helper holds the only other ends, if it kept them
    try:
        check(lock(h, OP_LOCK_TRY, b"0\0x")[0] == OK, "the helper works with 0, 1 and 2 closed")
        os.set_blocking(r, False)
        try:
            data = os.read(r, 1)
            check(data == b"", "the pipe holds data")
        except BlockingIOError:
            raise Failed("the helper kept a descriptor of the mailer open")
        if Path("/proc/self/limits").exists():
            limits = Path("/proc/%d/limits" % h.pid).read_text()
            check(re.search(r"Max core file size\s+0\s+0", limits), "core files are not written")
            try:
                os.listdir("/proc/%d/fd" % h.pid)
                raise Failed("the process can be examined by its user")
            except PermissionError:
                pass
    except Exception:
        h.kill()
        raise
    finally:
        os.close(r)
    check(h.finish() == 0, "the helper ends")


@case
def the_helper_needs_the_socket_as_descriptor_3():
    """Started by hand with something else as descriptor 3, it stops at once."""
    r, w = os.pipe()
    pid = os.posix_spawn(str(binaries["helper"]), [str(binaries["helper"])], dict(SANITIZER_ENV),
                         file_actions=[(os.POSIX_SPAWN_DUP2, r, 3)])
    _, status = os.waitpid(pid, 0)
    os.close(r)
    os.close(w)
    check(os.waitstatus_to_exitcode(status) == EXIT_SETUP, "status %d" % os.waitstatus_to_exitcode(status))
    pid = os.posix_spawn(str(binaries["helper"]), [str(binaries["helper"])], dict(SANITIZER_ENV),
                         file_actions=[(os.POSIX_SPAWN_CLOSE, 3)])
    _, status = os.waitpid(pid, 0)
    check(os.waitstatus_to_exitcode(status) == EXIT_SETUP, "without descriptor 3: status %d" % os.waitstatus_to_exitcode(status))


OTHER_USER_CLIENT = r"""
import os, pwd, socket, struct, sys
helper = sys.argv[1]
MAGIC = 0x44545350
a, b = socket.socketpair()
pid = os.posix_spawn(helper, [helper], {}, file_actions=[(os.POSIX_SPAWN_DUP2, b.fileno(), 3)])
b.close()
def call(op, payload=b""):
    a.sendall(struct.pack("=III", MAGIC, op, len(payload)) + payload)
    fds = []
    def exactly(n):
        data = b""
        while len(data) < n:
            chunk, got, _f, _a = socket.recv_fds(a, n - len(data), 4)
            fds.extend(got)
            if not chunk:
                raise SystemExit("closed")
            data += chunk
        return data
    magic, status, length = struct.unpack("=III", exactly(12))
    return status, (exactly(length) if length else b""), fds
status, _, _ = call(1, struct.pack("=I", 1))
print("open-missing", status)
status, body, fds = call(1, struct.pack("=I", 3))
print("open-create", status, struct.unpack("=II", body), os.fstat(fds[0]).st_uid)
text = b"0\0other"
status, _, _ = call(2, struct.pack("=II", 1, len(text)) + text)
print("lock", status)
a.close()
os.waitpid(pid, 0)
print("user", os.getuid(), pwd.getpwuid(os.getuid()).pw_name)
"""


@case
def the_helper_run_as_another_user_works_in_that_users_name():
    """Needs another user (DTMAIL_TEST_SUDO): the helper started by `nobody` handles nobody's files only."""
    other = other_user()
    if not use_sudo or other is None or os.getuid() == 0:
        raise Skipped("needs another user and DTMAIL_TEST_SUDO")
    # a directory that other users can reach: the helper under test is built for it
    other_dir = Path("/var/tmp") / ("dtmailspool-other-%d" % os.getpid())
    other_binary = other_dir / "dtmailspool"
    other_spool = other_dir / "spool"
    other_dir.mkdir(mode=0o755)
    try:
        shutil.copy(build("helper-other", [], ["main.c", "spool.c"], other_spool), other_binary)
        os.chmod(other_binary, 0o755)
        other_spool.mkdir()
        os.chmod(other_spool, 0o1777)
        (other_spool / user).write_bytes(b"belongs to the first user\n")
        os.chmod(other_spool / user, 0o666)
        r = sudo("-u", other.pw_name, "--", sys.executable, "-c", OTHER_USER_CLIENT, str(other_binary))
        check(r.returncode == 0, "the client of %s failed: %s %s" % (other.pw_name, r.stdout, r.stderr))
        lines = r.stdout.splitlines()
        check(lines[0] == "open-missing %d" % errno.ENOENT, "the first user's mailbox is not %s's: %r" % (other.pw_name, lines[0]))
        check(lines[1] == "open-create 0 (2, 1) %d" % other.pw_uid, "the new mailbox belongs to %s: %r" % (other.pw_name, lines[1]))
        check(lines[2] == "lock 0", "lock: %r" % lines[2])
        check(sorted(os.listdir(other_spool)) == sorted([user, other.pw_name, other.pw_name + ".lock"]),
              "%r" % sorted(os.listdir(other_spool)))
        check((other_spool / user).read_bytes() == b"belongs to the first user\n", "the other mailbox is not touched")
    finally:
        sudo("rm", "-rf", str(other_dir))


# ---------------------------------------------------------------------------
# the names of the files

SPOOL_INIT_TEST = r"""
#include <stdio.h>
#include <string.h>
#include "spool.h"
int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        struct dtsp_spool spool;
        int result = dtsp_spool_init(&spool, 5, 1000, 8, argv[i]);
        printf("%s\t%s\n", result == 0 ? "accepted" : "refused", result == 0 ? spool.name : "");
    }
    return 0;
}
"""


@case
def only_plain_file_names_are_user_names():
    """Every name the helper uses is made from the user name; one that is a path, a link name, a hidden file
    or not a name at all is refused when the spool is set up."""
    source = work / "spool-init.c"
    source.write_text(SPOOL_INIT_TEST)
    binary = work / "spool-init"
    r = run(cc + ["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-Wall", "-Wextra", "-Werror",
                  "-I", str(staged / "include"), "-I", str(staged / "dtmailspool"), str(source),
                  str(staged / "dtmailspool" / "spool.c"), "-o", str(binary)])
    check(r.returncode == 0, "compile: " + r.stderr)
    refused = ["", ".", "..", ".hidden", "..x", "a/b", "/", "/etc/passwd", "../x", "x/", "a\nb", "tab\tname", "a\x7fb",
               "x" * 101, "x" * 4000]
    accepted = ["user", "a", "a.b", "user-1", "user_2", "Ünï", "x" * 100, "a b", "-x", "~user"]
    r = run([str(binary)] + refused + accepted, env=dict(SANITIZER_ENV))
    check(r.returncode == 0 and r.stderr == "", "the test program failed: " + r.stderr)
    lines = [line.split("\t") for line in r.stdout.split("\n")[:-1]]
    check(len(lines) == len(refused) + len(accepted), "%d answers" % len(lines))
    for name, (verdict, kept) in zip(refused, lines[:len(refused)]):
        check(verdict == "refused", "%r is accepted as a user name" % name[:20])
    for name, (verdict, kept) in zip(accepted, lines[len(refused):]):
        check(verdict == "accepted" and kept == name, "%r is refused" % name[:20])


# ---------------------------------------------------------------------------
# the group ids: a fake kernel that refuses, ignores or records the changes

def fake_kernel_source():
    # A kernel for the group ids: the real group is the one the test runs with, the installed group is another
    # number.  FAKE_MODE: normal (changes work and are recorded on descriptor 2), refuse_all or ignore_all (the
    # drop of the group fails), refuse_enable or ignore_enable (only taking the installed group fails).
    return r"""
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
static gid_t fk_real, fk_mail, fk_eff;
static int fk_ready;
static void fk_init(void)
{
    if (!fk_ready) {
        fk_real = getgid();
        fk_mail = fk_real + 1;
        fk_eff = fk_mail;
        fk_ready = 1;
    }
}
__attribute__((unused)) static gid_t fake_getgid(void) { fk_init(); return fk_real; }
__attribute__((unused)) static gid_t fake_getegid(void) { fk_init(); return fk_eff; }
__attribute__((unused)) static int fake_setegid(gid_t g)
{
    const char *line = g == fk_mail ? "egid mail\n" : "egid user\n";
    fk_init();
    if (write(2, line, strlen(line)) < 0) { }
    if (strcmp(FAKE_MODE, "refuse_all") == 0) return -1;
    if (strcmp(FAKE_MODE, "ignore_all") == 0) return 0;
    if (g != fk_real && g != fk_mail) return -1;
    if (g == fk_mail && strcmp(FAKE_MODE, "refuse_enable") == 0) return -1;
    if (g == fk_mail && strcmp(FAKE_MODE, "ignore_enable") == 0) return 0;
    fk_eff = g;
    return 0;
}
#define getgid fake_getgid
#define getegid fake_getegid
#define setegid fake_setegid
"""


def fake_helper(mode):
    header = work / "fake-kernel.h"
    header.write_text(fake_kernel_source())
    return build("helper-fake-" + mode, ["-include", str(header), '-DFAKE_MODE="%s"' % mode], ["main.c", "spool.c"], spool)


def fake_run(mode, requests=(), prepare=None):
    reset_spool()
    if prepare:
        prepare()
    h = Helper(fake_helper(mode), env=SANITIZER_ENV)
    replies = []
    try:
        for op, payload in requests:
            try:
                replies.append(h.call(op, payload))
            except (Failed, OSError):
                break
    except Exception:
        h.kill()
        raise
    code = h.finish()
    return code, h.stderr().split(), replies


def egids(words):
    return [words[i + 1] for i in range(0, len(words), 2)]


def fake_run_with_mailbox(mode, requests):
    return fake_run(mode, requests, prepare=lambda: (spool / user).write_bytes(b"x\n"))


@case
def the_group_is_given_up_first_and_held_only_for_one_operation():
    code, words, replies = fake_run_with_mailbox("normal", [
        (OP_OPEN, u32(OPEN_WRITE)),
        (OP_LOCK_TRY, u32(LOCK_MAIL, 1) + b"x"),
        (OP_LOCK_VERIFY, u32(LOCK_MAIL, 1) + b"x"),
        (99, b""),                   # a malformed request needs no group
    ])
    check(code == EXIT_PROTOCOL, "status %d" % code)
    seq = egids(words)
    # the drop; the spool directory; then one hold per operation; the malformed request holds none
    check(seq == ["user"] + ["mail", "user"] * 4, "changes of the effective group: %r" % seq)
    check([r[0] for r in replies] == [OK, OK, OK, BAD_REQUEST], "%r" % [r[0] for r in replies])
    for r in replies:
        for fd in r[2]:
            os.close(fd)


@case
def a_group_that_cannot_be_given_up_stops_the_helper():
    for mode in ("refuse_all", "ignore_all"):
        code, words, replies = fake_run(mode, [(OP_LOCK_TRY, u32(LOCK_MAIL, 1) + b"x")])
        check(code == EXIT_GROUP and not replies, "%s: status %d" % (mode, code))
        check(listing() == [], "%s: the helper did something: %r" % (mode, listing()))


@case
def a_group_that_cannot_be_taken_stops_the_helper():
    for mode in ("refuse_enable", "ignore_enable"):
        code, words, replies = fake_run(mode, [(OP_LOCK_TRY, u32(LOCK_MAIL, 1) + b"x")])
        check(code == EXIT_GROUP and not replies, "%s: status %d" % (mode, code))
        check(listing() == [], "%s: the helper did something: %r" % (mode, listing()))


@case
def the_signals_of_a_terminal_do_not_end_the_helper():
    """An interrupt, a quit and a hangup reach the helper with the rest of the mailer's process group; the
    mailer still has locks to release through it."""
    reset_spool()
    for sig in (signal.SIGINT, signal.SIGQUIT, signal.SIGHUP):
        h = Helper(binaries["helper"])
        try:
            check(lock(h, OP_LOCK_TRY, b"0\0term")[0] == OK, "lock")
            os.kill(h.pid, sig)
            time.sleep(0.1)
            check(lock(h, OP_LOCK_VERIFY, b"0\0term")[0] == OK, "%s: the helper answers after the signal" % sig.name)
            check(lock(h, OP_LOCK_RELEASE, b"0\0term")[0] == OK, "%s: release" % sig.name)
        except Exception:
            h.kill()
            raise
        helper_ends(h, 0)
        check(listing() == [], "%s leaves %r" % (sig.name, listing()))


@case
def limits_of_the_mailer_do_not_stop_the_helper():
    """The mailer may have limited the size of the files it writes; the helper has to finish what it has begun
    and is not ended by SIGXFSZ."""
    reset_spool()
    h = Helper(binaries["helper"], limit_fsize=0)
    try:
        check(lock(h, OP_LOCK_TRY, b"0\0limited")[0] == OK, "a lock file is written")
        check(lock(h, OP_LOCK_RELEASE, b"0\0limited")[0] == OK, "release")
    except Exception:
        h.kill()
        raise
    helper_ends(h, 0)
    check(listing() == [], "%r" % listing())


FAKE_PASSWD = r"""
#include <errno.h>
#include <pwd.h>
#include <string.h>
__attribute__((unused)) static int fake_getpwnam_r(const char *name, struct passwd *pwd, char *buf, size_t size,
                                                    struct passwd **result)
{
    size_t length = strlen(name);
    (void) buf; (void) size;
    *result = NULL;
    if (strcmp(FAKE_MODE, "error") == 0)
        return EIO;
    if (length > 5 && strcmp(name + length - 5, ".lock") == 0) {   /* another user is called <name>.lock */
        memset(pwd, 0, sizeof *pwd);
        pwd->pw_name = (char *) name;
        *result = pwd;
    }
    return 0;
}
#define getpwnam_r fake_getpwnam_r
"""

FAKE_LINK = r"""
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
/* the link is made and reported as a failure, as a file system that is not local can */
__attribute__((unused)) static int fake_linkat(int a, const char *b, int c, const char *d, int flags)
{
    int result = linkat(a, b, c, d, flags);
    if (result == 0) {
        errno = EEXIST;
        return -1;
    }
    return result;
}
#define linkat fake_linkat
"""


def helper_with(name, source, mode=""):
    header = work / (name + ".h")
    header.write_text(source)
    return build("helper-" + name, ["-include", str(header), '-DFAKE_MODE="%s"' % mode], ["main.c", "spool.c"], spool)


@case
def a_lock_file_name_that_is_the_login_of_another_user_is_not_used():
    """With a user X.lock the lock file of X is that user's mailbox: X's helper must not make, check or break it.
    A name lookup that fails is taken to find a user."""
    for mode, refused_kinds in (("lock", [LOCK_MAIL]), ("error", [LOCK_MAIL, LOCK_DTMAIL])):
        reset_spool()
        h = Helper(helper_with("fake-passwd-" + mode, FAKE_PASSWD, mode))
        try:
            for kind in (LOCK_MAIL, LOCK_DTMAIL):
                if kind in refused_kinds:
                    for op in (OP_LOCK_TRY, OP_LOCK_VERIFY, OP_LOCK_RELEASE):
                        got = lock(h, op, b"x", kind)[0]
                        check(got == REFUSED, "%s kind %d op %d: status %d" % (mode, kind, op, got))
                    check(h.call(OP_LOCK_BREAK, u32(kind))[0] == REFUSED, "%s break kind %d" % (mode, kind))
                else:
                    check(lock(h, OP_LOCK_TRY, b"4711\n", kind)[0] == OK, "%s: the other kind works" % mode)
            status, fd, access, created = open_box(h, OPEN_WRITE | OPEN_CREATE)
            check(status == OK, "the mailbox itself is not affected")
            os.close(fd)
        except Exception:
            h.kill()
            raise
        helper_ends(h, 0)
        check(all(n == user or n == user + ".dtmail" for n in listing()), "%s: the spool holds %r" % (mode, listing()))


@case
def a_link_that_was_made_and_reported_as_failed_is_a_lock():
    reset_spool()
    h = Helper(helper_with("fake-link", FAKE_LINK))
    try:
        status, body, fds = lock(h, OP_LOCK_TRY, b"0\0mine")
        check(status == OK, "status %d" % status)
        check(listing() == [user + ".lock"] and (spool / (user + ".lock")).read_bytes() == b"0\0mine",
              "the spool holds %r" % listing())
    except Exception:
        h.kill()
        raise
    helper_ends(h, 0)


# ---------------------------------------------------------------------------
# the sources

SOURCE_SUFFIXES = (".c", ".C", ".h", ".hh", ".cc", ".cpp")


def mailer_sources(root):
    base = root / "cde" / "programs" / "dtmail" if (root / "cde").is_dir() else root
    for path in sorted(base.rglob("*")):
        if path.suffix in SOURCE_SUFFIXES and "tests" not in path.relative_to(base).parts:
            yield base, path


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


@case
def only_the_helper_changes_a_group_or_user_id():
    pattern = re.compile(r"\b(setuid|seteuid|setgid|setegid|setreuid|setregid|setresuid|setresgid|setgroups|initgroups)\s*\(")
    found = []
    for base, path in mailer_sources(guard_root):
        rel = path.relative_to(base)
        if rel.parts[0] == "dtmailspool":
            continue
        for match in pattern.finditer(strip_comments(path.read_text(errors="replace"))):
            found.append("%s: %s" % (rel, match.group(1)))
    check(not found, "ids are changed or queried outside dtmailspool: %s" % ", ".join(found))
    main = strip_comments((guard_root / "dtmailspool" / "main.c").read_text())
    calls = re.findall(r"\b(setuid|seteuid|setgid|setegid|setreuid|setregid|setresuid|setresgid)\s*\(", main)
    check(calls == ["setegid"], "main.c of the helper calls %r; the one change of the group is in group_set()" % calls)
    check("group_set" in main and "getegid() != gid" in main, "group_set() checks the result")


@case
def the_helper_trusts_nothing_it_is_given():
    forbidden = re.compile(r"\b(getenv|secure_getenv|putenv|setenv|system|popen|execl|execle|execlp|execv|execve|execvp|"
                           r"fork|vfork|posix_spawn|dlopen|realpath|chdir|chroot|open64)\s*\(")
    found = []
    for name in ("main.c", "spool.c"):
        text = strip_comments((guard_root / "dtmailspool" / name).read_text())
        found += ["%s: %s" % (name, m.group(1)) for m in forbidden.finditer(text)]
        check(re.search(r"\bargv\b|\bargc\b", text) is None, "%s reads its arguments" % name)
    check(not found, "the helper uses %s" % ", ".join(found))
    spool_text = strip_comments((guard_root / "dtmailspool" / "spool.c").read_text())
    # every file in the spool directory is reached through the directory descriptor and without a link
    bare = re.findall(r"(?<![\w])(open|creat|stat|lstat|unlink|rename|link|mkdir|rmdir|symlink|readlink|chmod|chown|access)\s*\(",
                      spool_text)
    check(not [b for b in bare if b != "open"], "spool.c uses a call that is not relative to the directory: %r" % bare)
    opens = re.findall(r"(?<![\w])open\s*\(([^,)]*)", spool_text)
    check(opens == ['"/dev/urandom"'], "spool.c opens paths: %r (only the random source may be opened by path)" % opens)
    for needle in ("O_NOFOLLOW", "O_EXCL", "AT_SYMLINK_NOFOLLOW", "st_uid != spool->uid"):
        check(needle in spool_text, "spool.c no longer uses %s" % needle)
    makefile = (guard_root / "dtmailspool" / "Makefile.am").read_text()
    check(not re.search(r"_LDADD|_LIBADD|\bLIBS\b", makefile), "the helper links more than the C library")
    sources = re.search(r"^dtmailspool_SOURCES\s*=\s*([^\n]*)$", makefile, re.M)
    check(sources and all(f.endswith((".c", ".h")) for f in sources.group(1).split()), "the helper is plain C")


@case
def only_the_helper_is_installed_with_the_group():
    mk = {}
    for path in (guard_root).rglob("Makefile.am"):
        rel = str(path.relative_to(guard_root))
        text = path.read_text()
        if re.search(r"chmod\s+[1-7][0-7]{3}\b|chgrp|chown", text):
            mk[rel] = text
    check(list(mk) == ["dtmailspool/Makefile.am"], "set-id or group rules in %r" % list(mk))
    text = mk["dtmailspool/Makefile.am"]
    check(re.search(r"chgrp mail \$\(DESTDIR\)\$\(bindir\)/dtmailspool && chmod 2755 \$\(DESTDIR\)\$\(bindir\)/dtmailspool", text),
          "the helper is installed set-group-id with the mail group")


@case
def the_mailer_has_no_group_privileges_left():
    banned = ["MailGroup", "PRIV_ENABLED", "enableGroupPrivileges", "disableGroupPrivileges", "initializeGroupPrivileges",
              "MAILGROUP_REQUIRED", "isSetMailGidNeeded", "GetIdForGroupName", "GetGroupName", "DTMAIL_DEFAULT_CREATE_MAILGROUP",
              "__ignore_group_permissions", "DTMAILHELPBADGROUPID"]
    found = []
    for base, path in mailer_sources(guard_root):
        text = path.read_text(errors="replace")
        for word in banned:
            if word in text:
                found.append("%s: %s" % (path.relative_to(base), word))
    check(not found, "left over: %s" % ", ".join(found))
    check(not (guard_root / "include" / "DtMail" / "MailGroup.hh").exists(), "MailGroup.hh still exists")


# ---------------------------------------------------------------------------

def main():
    only = os.environ.get("DTMAIL_TEST_CASES")
    names = only.split() if only else None
    failed = []
    skipped = []
    try:
        reset_spool()
        binaries["helper"] = build("dtmailspool", [], ["main.c", "spool.c"], spool)
        selected = [c for c in cases if names is None or c.__name__ in names]
        for function in selected:
            reset_spool()
            try:
                function()
                print("ok    %s" % function.__name__)
            except Skipped as e:
                print("skip  %s (%s)" % (function.__name__, e))
                skipped.append(function.__name__)
            except Failed as e:
                print("FAIL  %s\n      %s" % (function.__name__, e))
                failed.append(function.__name__)
            except Exception as e:  # a broken test is a failure as well
                print("FAIL  %s\n      %s: %s" % (function.__name__, type(e).__name__, e))
                failed.append(function.__name__)
        print("%d/%d cases passed%s" % (len(selected) - len(failed) - len(skipped), len(selected),
                                         ", %d skipped" % len(skipped) if skipped else ""))
    finally:
        if os.environ.get("DTMAIL_TEST_KEEP"):
            print("kept " + str(work))
        else:
            shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if failed else 0)


main()
