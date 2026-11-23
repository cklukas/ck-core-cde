#!/usr/bin/env python3
"""Verify the mail body index and real OpenPGP and CMS signatures."""

import base64
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


def check_index(exe, work):
    root = work / "folders"
    root.mkdir()
    account = root / "account"
    account.mkdir()
    (root / "Inbox").write_bytes(
        b"From fixture Mon Nov 23 10:00:00 2026\nSubject: Plain\n"
        b"Message-ID: <quoted@example.test>\n"
        b"Content-Type: text/plain; charset=utf-8\n"
        b"Content-Transfer-Encoding: quoted-printable\n\n"
        b"first shared phrase and caf=C3=A9\n"
    )
    (account / "Archive").write_bytes(
        b"From fixture Mon Nov 23 10:00:00 2026\nSubject: HTML\n"
        b"Message-ID: <html@example.test>\n"
        b"Content-Type: multipart/mixed; boundary=part\n\n"
        b"--part\nContent-Type: text/html\n\n"
        b"<p>second shared phrase</p><script>scripttoken</script>\n"
        b"--part\nContent-Type: text/plain\nContent-Disposition: attachment; filename=a.txt\n\n"
        b"attachmenttoken\n--part--\n"
    )
    (root / "unrelated.txt").write_text("shared phrase", encoding="utf-8")
    (root / "symlink").symlink_to(account / "Archive")
    subprocess.run([exe, "index", root], check=True)
    print("PASS indexed bodies across folders, HTML and quoted-printable, edits and removals")


def check_sig(exe, message, home, expected):
    actual = subprocess.check_output([exe, "verify", message],
                                     env={**os.environ, "GNUPGHOME": str(home)}, text=True)
    if actual.strip() not in expected:
        raise AssertionError(f"{message.name}: expected {sorted(expected)}, observed {actual.strip()}")


def check_signatures(exe, work):
    if not all(shutil.which(tool) for tool in ("gpg", "gpgsm", "gpgconf", "openssl")):
        print("SKIP signing fixtures: gpg, gpgsm and openssl are required")
        return
    home = work / "keys"
    missing = work / "no-keys"
    home.mkdir(mode=0o700)
    missing.mkdir(mode=0o700)
    signed_part = work / "part"
    signed_part.write_bytes(b"Content-Type: text/plain\r\n\r\nThis is the signed message.")
    signature = work / "signature"
    message = work / "openpgp.eml"
    gpg = ["gpg", "--homedir", str(home), "--batch", "--yes"]
    ident = "Fixture Signer <signer@example.test>"
    try:
        subprocess.run(gpg + ["--pinentry-mode", "loopback", "--passphrase", "",
                              "--quick-generate-key", ident, "ed25519", "sign", "0"],
                       check=True, capture_output=True)
        subprocess.run(gpg + ["--armor", "--output", str(signature), "--detach-sign", str(signed_part)],
                       check=True, capture_output=True)
        pgp_header = (
            b"From: Fixture Signer <signer@example.test>\r\n"
            b"Subject: OpenPGP fixture\r\nMIME-Version: 1.0\r\n"
            b"Content-Type: multipart/signed; boundary=b; protocol=\"application/pgp-signature\"; "
            b"micalg=pgp-sha256\r\n\r\n--b\r\n"
        )
        pgp = pgp_header + signed_part.read_bytes() + (
            b"\r\n--b\r\nContent-Type: application/pgp-signature\r\n\r\n"
        ) + signature.read_bytes().replace(b"\n", b"\r\n") + b"\r\n--b--\r\n"
        message.write_bytes(pgp)
        check_sig(exe, message, home, {"1 1"})
        check_sig(exe, message, missing, {"1 4"})
        message.write_bytes(pgp.replace(b"the signed message", b"a forged message"))
        check_sig(exe, message, home, {"1 3"})

        key, cert = work / "key.pem", work / "cert.pem"
        subprocess.run(["openssl", "req", "-new", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-subj", "/CN=CMS Fixture/emailAddress=signer@example.test", "-days", "1",
                        "-addext", "keyUsage=digitalSignature", "-addext", "extendedKeyUsage=emailProtection",
                        "-keyout", key, "-out", cert], check=True, capture_output=True)
        iso = ["gpgsm", "--homedir", str(home), "--batch", "--no-tty"]
        subprocess.run(iso + ["--import", str(cert)], check=True, capture_output=True)
        fingerprint = subprocess.check_output(
            ["openssl", "x509", "-in", cert, "-noout", "-fingerprint", "-sha1"], text=True,
        ).strip().split("=", 1)[1]
        (home / "trustlist.txt").write_text(f"{fingerprint} S relax\n", encoding="ascii")
        subprocess.run(["gpgconf", "--homedir", str(home), "--kill", "all"], check=True)
        cms_data = work / "cms.der"
        cms = ["openssl", "cms", "-sign", "-binary", "-in", signed_part,
               "-signer", cert, "-inkey", key, "-outform", "DER", "-out", cms_data]
        subprocess.run(cms, check=True, capture_output=True)
        smime_message = work / "smime.eml"
        smime_header = pgp_header.replace(b"OpenPGP", b"S/MIME").replace(
            b"application/pgp-signature\"; micalg=pgp-sha256",
            b"application/pkcs7-signature\"; micalg=sha-256",
        )
        smime = smime_header + signed_part.read_bytes() + (
            b"\r\n--b\r\nContent-Type: application/pkcs7-signature\r\n"
            b"Content-Transfer-Encoding: base64\r\n\r\n"
        ) + base64.encodebytes(cms_data.read_bytes()).replace(b"\n", b"\r\n") + b"\r\n--b--\r\n"
        smime_message.write_bytes(smime)
        check_sig(exe, smime_message, home, {"2 1"})
        smime_message.write_bytes(smime.replace(b"the signed message", b"a forged message"))
        check_sig(exe, smime_message, home, {"2 3"})

        subprocess.run(cms + ["-nodetach"], check=True, capture_output=True)
        opaque = work / "opaque.eml"
        opaque.write_bytes(
            b"MIME-Version: 1.0\r\nContent-Type: application/pkcs7-mime; smime-type=signed-data\r\n"
            b"Content-Transfer-Encoding: base64\r\n\r\n"
            + base64.encodebytes(cms_data.read_bytes()).replace(b"\n", b"\r\n")
        )
        check_sig(exe, opaque, home, {"2 1"})
    finally:
        for keydir in (home, missing):
            subprocess.run(["gpgconf", "--homedir", str(keydir), "--kill", "all"], check=False)
    print("PASS trusted, missing-key and modified OpenPGP, detached and opaque S/MIME")


def main():
    tests = Path(__file__).resolve().parent
    src = tests.parent / "dtmail"
    pkg = ["pkg-config", "gmime-3.0", "sqlite3"]
    if subprocess.run(pkg + ["--exists"], check=False).returncode:
        print("SKIP mail index and signature tests: GMime and SQLite development files are required")
        return
    flags = shlex.split(subprocess.check_output(pkg + ["--cflags", "--libs"], text=True))
    with tempfile.TemporaryDirectory(prefix="dtmail-p3-") as path:
        work = Path(path)
        exe = work / "test"
        subprocess.run([
            *shlex.split(os.environ.get("CXX", "c++")), "-std=c++17", "-Wall", "-Wextra",
            "-DDTMAIL_GMIME_ENABLED=1", "-DDTMAIL_BODY_INDEX_ENABLED=1", "-I", str(src),
            str(tests / "check-mail-p3.cc"), str(src / "MailBodyIndex.C"),
            str(src / "MailSignature.C"), str(src / "HtmlReadable.C"), "-o", exe, *flags,
        ], check=True)
        check_index(exe, work)
        check_signatures(exe, work)


if __name__ == "__main__":
    main()
