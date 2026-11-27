#!/usr/bin/env python3
"""End-to-end regression test for libDtPrint modern Cairo/Pango PDF + CUPS/IPP pipeline:
1. Verifies libDtPrint + libXm.so.9 linkage and DtPrintSetupBox CUPS queue discovery
   (DtPrintDiscoverCupsQueues, DtPrintFillSetupData, DtPrintPopupJobSetupDialog).
2. Verifies multi-page Unicode/CJK vector PDF generation (DtPrintRenderTextToPdf)
   with explicit form-feeds, A4 MediaBox, headers/footers, and page-range filtering ("1,3-4" -> 3 pages).
3. Starts a real RFC 8011 binary IPP/1.1 HTTP server on 127.0.0.1:<port> and verifies
   DtPrintDeliverPdfJob / DtPrintSubmitCupsPdfJob transmits application/pdf with exact
   IPP job-template attributes: copies=3, sides=two-sided-long-edge,
   media=na_legal_8.5x14in, and page-ranges=[(2, 4)].
4. Exercises dthelpprint (used by dthelpview and all libDtHelp Print dialogs) in both
   Print-to-PDF (-outputFile out.pdf) and direct IPP submission modes.
"""

from __future__ import annotations

import http.server
import json
import os
from pathlib import Path
import re
import shutil
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
from typing import Any


HARNESS_C = r"""
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Intrinsic.h>
#include <Xm/Xm.h>
#include <Dt/Print.h>

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <a4_pdf_out> <file_dest_pdf> <printer_name>\n", argv[0]);
        return 2;
    }
    const char *a4_pdf = argv[1];
    const char *file_dest_pdf = argv[2];
    const char *printer_name = argv[3];

    /* Verify runtime libXm.so.9 mapping */
    FILE *mf = fopen("/proc/self/maps", "r");
    if (mf) {
        char line[1024];
        int saw_xm9 = 0;
        while (fgets(line, sizeof(line), mf)) {
            if (strstr(line, "libXm.so.9")) { saw_xm9 = 1; break; }
        }
        fclose(mf);
        if (!saw_xm9) {
            fprintf(stderr, "FAIL: harness did not map libXm.so.9\n");
            return 3;
        }
    }

    /* 1. Multi-page Unicode/CJK vector PDF with A4 + page_range="1,3-4" */
    const char *doc =
        "Page One: ASCII + Accents Café naïve résumé\n"
        "Line 2: CJK 日本語テキスト / 中文测试 / 한국어 텍스트\n"
        "Line 3: Greek Ελληνικά & Math ∑ ∫ √ π\f"
        "Page Two (excluded by page_range 1,3-4)\f"
        "Page Three: Included in range!\f"
        "Page Four: Final page included in range!\n";

    DtPrintJobOptions opts;
    DtPrintInitJobOptions(&opts);
    opts.paper_size = DtPRINT_PAPER_A4;
    opts.page_range = "1,3-4";
    opts.header_left = "CDE Vector Print Test";
    opts.footer_right = "Page %d of %d";
    opts.job_title = "Unicode A4 Test";

    int pages_written = 0;
    char err_buf[256] = {0};
    if (DtPrintRenderTextToPdf(doc, &opts, a4_pdf, &pages_written, err_buf, sizeof(err_buf)) != DtPRINT_SUCCESS) {
        fprintf(stderr, "FAIL: DtPrintRenderTextToPdf failed: %s\n", err_buf);
        return 4;
    }
    if (pages_written != 3) {
        fprintf(stderr, "FAIL: expected 3 pages for range 1,3-4 out of 4, got %d\n", pages_written);
        return 5;
    }

    /* 2. Instantiate real DtPrintSetupDialog on display and verify CUPS discovery + IPP submit */
    XtAppContext app;
    Widget top = XtVaAppInitialize(&app, "DtPrintTest", NULL, 0, &argc, argv, NULL,
                                   XmNmappedWhenManaged, False, XmNwidth, 10, XmNheight, 10, NULL);
    XtRealizeWidget(top);

    DtPrintCupsQueue queues[16];
    int nq = DtPrintDiscoverCupsQueues(queues, 16);
    if (nq < 2) {
        fprintf(stderr, "FAIL: expected >=2 queues (mock + Print_to_PDF), got %d\n", nq);
        return 6;
    }

    Arg pargs[4];
    Cardinal pn = 0;
    XtSetArg(pargs[pn], DtNcopies, 3); pn++;
    XtSetArg(pargs[pn], DtNfileName, (XtArgVal)file_dest_pdf); pn++;
    Widget psub = DtCreatePrintSetupDialog(top, "testPrintSetup", pargs, pn);
    assert(psub != NULL);

    /* Open & exercise Page & IPP Job Setup dialog */
    DtPrintPopupJobSetupDialog(top, psub);
    DtPrintSetWidgetJobOptions(psub, DtPRINT_PAPER_LEGAL, DtPRINT_DUPLEX_LONG_EDGE, "2-4");

    DtPrintSetupData psd;
    memset(&psd, 0, sizeof(psd));
    if (DtPrintFillSetupData(psub, &psd) != DtPRINT_SUCCESS) {
        fprintf(stderr, "FAIL: DtPrintFillSetupData failed\n");
        return 7;
    }
    if (!psd.printer_name || strcmp(psd.printer_name, printer_name) != 0) {
        fprintf(stderr, "FAIL: expected default mock printer %s, got %s\n",
                printer_name, psd.printer_name ? psd.printer_name : "(null)");
        return 8;
    }

    DtPrintJobOptions wopts;
    DtPrintInitJobOptions(&wopts);
    DtPrintReadWidgetJobOptions(psub, &wopts);
    assert(wopts.copies == 3);
    assert(wopts.paper_size == DtPRINT_PAPER_LEGAL);
    assert(wopts.duplex == DtPRINT_DUPLEX_LONG_EDGE);
    assert(strcmp(wopts.page_range, "2-4") == 0);
    wopts.job_title = "DtPrint Legal Duplex Job";

    /* 2a. Deliver Print-to-File */
    DtPrintSetupData f_psd = psd;
    f_psd.destination = DtPRINT_TO_FILE;
    f_psd.dest_info = (String)file_dest_pdf;
    int job_id = 0;
    if (DtPrintDeliverPdfJob(&f_psd, &wopts, a4_pdf, &job_id, err_buf, sizeof(err_buf)) != DtPRINT_SUCCESS) {
        fprintf(stderr, "FAIL: DtPrintDeliverPdfJob(FILE) failed: %s\n", err_buf);
        return 9;
    }

    /* 2b. Deliver Print-to-Printer over RFC 8011 IPP/1.1 */
    psd.destination = DtPRINT_TO_PRINTER;
    if (DtPrintDeliverPdfJob(&psd, &wopts, a4_pdf, &job_id, err_buf, sizeof(err_buf)) != DtPRINT_SUCCESS) {
        fprintf(stderr, "FAIL: DtPrintDeliverPdfJob(PRINTER) failed: %s\n", err_buf);
        return 10;
    }
    if (job_id != 4242) {
        fprintf(stderr, "FAIL: expected IPP job-id 4242 from mock server, got %d\n", job_id);
        return 11;
    }

    DtPrintFreeSetupData(&psd);
    XtDestroyWidget(top);
    printf("HARNESS_OK=1\n");
    return 0;
}
"""


def parse_ipp_request(raw: bytes) -> dict[str, Any]:
    if len(raw) < 8:
        raise ValueError(f"IPP payload too short ({len(raw)} bytes)")
    ver_maj, ver_min, op_id, req_id = struct.unpack(">BBHi", raw[:8])
    pos = 8
    cur_group = 0
    attrs: dict[str, Any] = {}
    last_name = ""

    while pos < len(raw):
        tag = raw[pos]
        pos += 1
        if tag == 0x03:  # end-of-attributes-tag
            break
        if tag <= 0x0F:
            cur_group = tag
            continue
        nl = struct.unpack(">H", raw[pos:pos + 2])[0]
        pos += 2
        name = raw[pos:pos + nl].decode("utf-8", errors="replace") if nl > 0 else last_name
        if nl > 0:
            last_name = name
        pos += nl
        vl = struct.unpack(">H", raw[pos:pos + 2])[0]
        pos += 2
        vbytes = raw[pos:pos + vl]
        pos += vl

        if tag == 0x21 and vl == 4:
            val: Any = struct.unpack(">i", vbytes)[0]
        elif tag == 0x22 and vl == 1:
            val = bool(vbytes[0])
        elif tag == 0x33 and vl == 8:
            lo, hi = struct.unpack(">ii", vbytes)
            val = (lo, hi)
        else:
            val = vbytes.decode("utf-8", errors="replace")

        if name in attrs:
            if isinstance(attrs[name], list):
                attrs[name].append(val)
            else:
                attrs[name] = [attrs[name], val]
        else:
            attrs[name] = val

    doc_bytes = raw[pos:]
    return {
        "version": (ver_maj, ver_min),
        "operation": op_id,
        "request_id": req_id,
        "group": cur_group,
        "attributes": attrs,
        "doc_magic": doc_bytes[:8],
        "doc_len": len(doc_bytes),
    }


def build_ipp_ok_response(req_id: int, job_id: int = 4242) -> bytes:
    out = bytearray()
    out += struct.pack(">BBHi", 1, 1, 0x0000, req_id)  # successful-ok
    out.append(0x01)  # operation-attributes-tag
    for tag, k, v in (
        (0x47, b"attributes-charset", b"utf-8"),
        (0x48, b"attributes-natural-language", b"en"),
    ):
        out.append(tag)
        out += struct.pack(">H", len(k)) + k + struct.pack(">H", len(v)) + v
    out.append(0x02)  # job-attributes-tag
    out.append(0x21)  # integer
    out += struct.pack(">H", 6) + b"job-id" + struct.pack(">Hi", 4, job_id)
    out.append(0x03)  # end-of-attributes-tag
    return bytes(out)


def inspect_pdf(pdf_path: Path) -> tuple[int, str]:
    import zlib
    pdf_bytes = pdf_path.read_bytes()
    expanded = bytearray(pdf_bytes)
    for m in re.finditer(rb"stream\r?\n(.*?)\r?\nendstream", pdf_bytes, re.S):
        try:
            expanded += b"\n" + zlib.decompress(m.group(1))
        except zlib.error:
            pass
    blob = bytes(expanded)
    m_cnt = re.search(rb"/Type\s*/Pages\b[^>]*/Count\s+(\d+)|/Count\s+(\d+)[^>]*/Type\s*/Pages\b", blob, re.S)
    if m_cnt:
        cnt = int(m_cnt.group(1) or m_cnt.group(2))
    else:
        cnt = len(re.findall(rb"/Type\s*/Page\b(?!s)", blob))
    mb = re.search(rb"/MediaBox\s*\[\s*([^\]]+)\]", blob)
    mb_str = mb.group(1).decode("ascii", errors="replace") if mb else ""
    has_font = b"/Font" in blob
    return cnt, f"MediaBox=[{mb_str}] has_font={has_font}"


def main() -> int:
    script_path = Path(__file__).resolve()
    cde_src = script_path.parents[3]

    # Locate cde-build and motif-prefix
    env_build = os.environ.get("CDE_BUILD_DIR")
    build_candidates = (
        [Path(env_build)] if env_build else []
    ) + [
        Path.cwd().parents[1] if (Path.cwd() / ".libs/libDtPrint.so").is_file() else Path("/nonexistent"),
        Path("/var/tmp/ck-wayland-build/wp00/cde-build"),
    ]
    cde_build: Path | None = None
    for c in build_candidates:
        if (c / "lib/DtPrint/.libs/libDtPrint.so").is_file():
            cde_build = c.resolve()
            break
    if cde_build is None:
        print("SKIP: libDtPrint.so build directory not found; set CDE_BUILD_DIR", file=sys.stderr)
        return 77

    motif_prefix = Path(os.environ.get("MOTIF_PREFIX", "/var/tmp/ck-wayland-build/wp00/motif-prefix"))
    if not (motif_prefix / "lib/libXm.so.9").is_file():
        motif_prefix = Path("/usr/dt")

    xvfb_bin = shutil.which("Xvfb")
    if not xvfb_bin:
        print("SKIP: Xvfb not installed", file=sys.stderr)
        return 77

    received_jobs: list[dict[str, Any]] = []

    class IppHandler(http.server.BaseHTTPRequestHandler):
        def do_POST(self) -> None:
            clen = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(clen)
            parsed = parse_ipp_request(raw)
            parsed["path"] = self.path
            received_jobs.append(parsed)
            resp = build_ipp_ok_response(parsed["request_id"], 4242)
            self.send_response(200)
            self.send_header("Content-Type", "application/ipp")
            self.send_header("Content-Length", str(len(resp)))
            self.end_headers()
            self.wfile.write(resp)

        def log_message(self, fmt: str, *args: Any) -> None:
            return

    server = socketserver.TCPServer(("127.0.0.1", 0), IppHandler)
    port = server.server_address[1]
    srv_thread = threading.Thread(target=server.serve_forever, daemon=True)
    srv_thread.start()

    tmp_parent = os.environ.get("TMPDIR")
    xvfb_proc: subprocess.Popen | None = None
    try:
        with tempfile.TemporaryDirectory(prefix="dtprint-pdf-test-", dir=tmp_parent) as td:
            tmp = Path(td)
            mock_cups = tmp / "mock_cups.conf"
            ipp_uri = f"ipp://127.0.0.1:{port}/printers/OfficeLaser"
            mock_cups.write_text(
                f"PRINTER|OfficeLaser|{ipp_uri}|HP LaserJet IPP Everywhere|Lab 3|idle|1|1|1\n",
                encoding="utf-8",
            )

            cc = os.environ.get("CC", "cc")
            harness_c = tmp / "harness.c"
            harness_bin = tmp / "harness"
            harness_c.write_text(HARNESS_C, encoding="utf-8")

            lib_dirs = [
                cde_build / "lib/DtPrint/.libs",
                cde_build / "lib/DtHelp/.libs",
                cde_build / "lib/DtWidget/.libs",
                cde_build / "lib/DtSvc/.libs",
                cde_build / "lib/tt/lib/.libs",
                cde_build / "lib/DtXinerama/.libs",
                motif_prefix / "lib",
            ]
            rpath_flags = [f"-Wl,-rpath,{d}" for d in lib_dirs if d.is_dir()]
            L_flags = [f"-L{d}" for d in lib_dirs if d.is_dir()]

            subprocess.run(
                [
                    cc, "-O2", "-Wall",
                    f"-I{cde_src}/include",
                    f"-I{cde_build}/include",
                    f"-I{motif_prefix}/include",
                    str(harness_c),
                    *L_flags, *rpath_flags,
                    "-lDtPrint", "-lDtHelp", "-lDtWidget", "-lDtSvc", "-ltt", "-lDtXinerama",
                    "-lXm", "-lXt", "-lXpm", "-lX11", "-lm",
                    "-o", str(harness_bin),
                ],
                check=True,
            )

            disp = ":87"
            xvfb_proc = subprocess.Popen(
                [xvfb_bin, disp, "-screen", "0", "1024x768x24", "-nolisten", "tcp", "-ac"],
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
            env = os.environ.copy()
            env.pop("WAYLAND_DISPLAY", None)
            env.pop("CK_MOTIF_BACKEND", None)
            env.update({
                "DISPLAY": disp,
                "HOME": str(tmp),
                "TMPDIR": str(tmp),
                "CK_CUPS_MOCK_FILE": str(mock_cups),
                "LD_LIBRARY_PATH": ":".join(str(d) for d in lib_dirs if d.is_dir()),
            })
            (tmp / ".dt/tmp").mkdir(parents=True, exist_ok=True)

            # Wait briefly for Xvfb socket
            for _ in range(30):
                if Path("/tmp/.X11-unix/X87").exists():
                    break
                import time
                time.sleep(0.05)

            a4_pdf = tmp / "unicode_a4.pdf"
            file_dest_pdf = tmp / "saved_copy.pdf"
            res = subprocess.run(
                [str(harness_bin), str(a4_pdf), str(file_dest_pdf), "OfficeLaser"],
                env=env, capture_output=True, text=True, check=True,
            )
            assert "HARNESS_OK=1" in res.stdout, f"unexpected output: {res.stdout}"

            # Verify generated PDF structure (A4 MediaBox, 3 pages, embedded vector font)
            pdf_data = a4_pdf.read_bytes()
            assert pdf_data.startswith(b"%PDF-1."), "missing %PDF-1.x header"
            npages, info_txt = inspect_pdf(a4_pdf)
            assert npages == 3, f"expected 3 PDF pages, got {npages} ({info_txt!r})"
            assert "595" in info_txt and "841" in info_txt and "has_font=True" in info_txt, (
                f"expected ISO A4 MediaBox + embedded vector /Font: {info_txt}"
            )
            pdftotext = shutil.which("pdftotext")
            if pdftotext:
                extracted = subprocess.check_output([pdftotext, str(a4_pdf), "-"], text=True)
                assert "Café" in extracted and "日本語テキスト" in extracted and "Page Three" in extracted
                assert "Page Two" not in extracted, f"excluded page 2 leaked into PDF: {extracted!r}"
            assert file_dest_pdf.read_bytes() == pdf_data, "Print-to-File bytes mismatch"

            # Verify IPP/1.1 job #1 from DtPrintDeliverPdfJob
            assert len(received_jobs) == 1, f"expected 1 IPP job so far, got {len(received_jobs)}"
            j1 = received_jobs[0]
            attrs1 = j1["attributes"]
            assert j1["operation"] == 0x0002, f"expected Print-Job (0x0002), got {j1['operation']:#x}"
            assert j1["path"] == "/printers/OfficeLaser"
            assert j1["doc_magic"].startswith(b"%PDF-1.")
            assert attrs1.get("document-format") == "application/pdf"
            assert attrs1.get("copies") == 3, f"expected copies=3, got {attrs1}"
            assert attrs1.get("sides") == "two-sided-long-edge", f"expected duplex sides, got {attrs1}"
            assert attrs1.get("media") == "na_legal_8.5x14in", f"expected Legal media, got {attrs1}"
            assert attrs1.get("page-ranges") == (2, 4), f"expected page-ranges=(2,4), got {attrs1}"

            # 3. Verify dthelpprint (used by dthelpview & libDtHelp) Print-to-PDF and IPP submission
            dthelpprint_bin = cde_build / "programs/dthelp/dthelpprint/.libs/dthelpprint"
            if not dthelpprint_bin.is_file():
                dthelpprint_bin = cde_build / "programs/dthelp/dthelpprint/dthelpprint"
            if dthelpprint_bin.is_file():
                help_pdf = tmp / "help_topic.pdf"
                subprocess.run(
                    [
                        str(dthelpprint_bin),
                        "-helpType", "1",
                        "-stringData", "CDE Help Vector PDF 日本語 Topic Body",
                        "-topicTitle", "Help Vector Title",
                        "-paperSize", "help_papersize_a4",
                        "-outputFile", str(help_pdf),
                    ],
                    env=env, check=True,
                )
                hp_bytes = help_pdf.read_bytes()
                hp_pages, _ = inspect_pdf(help_pdf)
                assert hp_bytes.startswith(b"%PDF-1.") and hp_pages >= 1

                subprocess.run(
                    [
                        str(dthelpprint_bin),
                        "-helpType", "1",
                        "-stringData", "CDE Help Direct IPP Submission",
                        "-topicTitle", "Help IPP Job",
                        "-paperSize", "help_papersize_a4",
                        "-copies", "2",
                        "-printer", "OfficeLaser",
                    ],
                    env=env, check=True,
                )
                assert len(received_jobs) == 2, f"expected 2nd IPP job from dthelpprint, got {len(received_jobs)}"
                j2 = received_jobs[1]
                assert j2["doc_magic"].startswith(b"%PDF-1.")
                assert j2["attributes"].get("copies") == 2
                assert j2["attributes"].get("media") == "iso_a4_210x297mm"

            print(
                "PASS: check-dtprint-pdf-cups (Cairo/Pango Unicode/CJK PDF, page-range 1,3-4 -> 3 pages, "
                "DtPrintSetupBox CUPS discovery, RFC 8011 IPP/1.1 copies/duplex/media/page-ranges, dthelpprint)"
            )
            return 0
    finally:
        server.shutdown()
        server.server_close()
        if xvfb_proc is not None:
            xvfb_proc.kill()
            xvfb_proc.wait()


if __name__ == "__main__":
    raise SystemExit(main())
