---
title: Vector PDF & CUPS IPP Printing
description: Modern Unicode-clean Cairo/Pango vector PDF generation, Print to PDF, and direct libcups / RFC 8011 IPP job submission across CDE applications.
---

# Vector PDF & CUPS IPP Printing

Classic CDE relied on legacy PostScript/`lp` shell pipelines or the retired X11
`Xp` (`Xprint`) server extension (`XmPrintShell` / `XpStartJob`), leaving
**File → Print...** disabled when `Xprint` was removed from X.Org. CK Core CDE
backs `libDtPrint` (`cde/lib/DtPrint/PrintPdfCups.c` + `<Dt/Print.h>`) with a
modern, Unicode-clean **Cairo PDF + PangoCairo** vector document pipeline and
direct **CUPS / IPP Everywhere** job submission across all core CDE
applications.

## Unicode-clean Cairo/Pango vector PDF generation

- **`DtPrintRenderTextToPdf` & `DtPrintRenderCustomPagesToPdf`:** Uses
  `cairo_pdf_surface_create` and `pango_cairo_create_layout` (`HAVE_DTPRINT_PDF`)
  to shape and subset multi-script UTF-8 text (Latin, Greek, Cyrillic, CJK,
  mathematical symbols, and bidirectional runs) into searchable vector PDF 1.5+
  pages without rasterizing text to X11 bitmaps.
- **Two-pass pagination, margins & headers/footers:** Measures exact total page
  counts before emitting pages so header and footer rules (`Page %d of %d`,
  document title, timestamp, and user name) carry accurate page totals, honors
  explicit `\f` form-feed breaks and word-wrap settings, parses physical margin
  specifiers (`in`, `mm`, `cm`, `pt`), and filters output pages by range
  (`DtPrintPageInRange`, e.g. `1,3-4`).
- **Built-in `Print to PDF`:** Every `DtPrintSetupBox` defaults **Print to
  File** output names to `.pdf` and exposes a guaranteed `Print_to_PDF` virtual
  queue (`~/PrintOutput.pdf`) even on workstations with zero configured physical
  printers.

## CUPS / IPP Everywhere queue discovery & direct IPP submission

- **`DtPrintSetupBox` queue discovery & **Info...** dialog (`PsubDefProc.c`, `PsubUtil.c`, `PrintSetupB.c`):**
  Populates the printer combo box and **Select Printer...** dialog directly from
  `libcups.so.2` (`cupsGetDests2`, `$LPDEST` / `$PRINTER`, `$DTPRINT_IPP_URI`,
  and `$CK_CUPS_MOCK_FILE`) without opening an `Xprint` display connection,
  displaying printer model, location, URI, and `application/pdf` format in
  **Info...**.
- **Native Motif **Page & IPP Job Setup** dialog (`DtPrintPopupJobSetupDialog`):**
  Clicking **Setup...** in `DtPrintSetupBox` opens a Motif setup dialog
  controlling **Paper Size** (`US Letter`, `US Legal`, `US Executive`,
  `ISO A4`, `JIS B5`), **Two-Sided (Duplex)** (`One-Sided`,
  `Two-Sided Long-Edge`, `Two-Sided Short-Edge`), and **Page Range** (`all` or
  `1-3,5`), alongside **Copies** (`DtNcopies`).
- **Direct `libcups` & RFC 8011 IPP/1.1 submission (`DtPrintSubmitCupsPdfJob`):**
  Streams `application/pdf` jobs with `copies`, `sides`, `media`, and
  `page-ranges` attributes through `libcups.so.2` (`cupsCreateJob` /
  `cupsStartDocument` / `cupsWriteRequestData` / `cupsFinishDocument`, with
  `cupsPrintFile2` fallback) or directly over HTTP/1.1 `POST` using binary
  RFC 8011 IPP/1.1 `Print-Job` (`0x0002`) requests against `ipp://` endpoints.

## Core CDE application coverage

- **Text Editor (`dtpad`):** Renders editor buffers with configured word-wrap,
  margins, and four-corner header/footer fields (`printJob.c`, `printSetup.c`).
- **Mailer (`dtmail`):** Formats single or multi-message selections with
  configured RFC 5322 headers and message separators to `.pdf` or CUPS/IPP
  queues (`DmxPrintJob.C`).
- **Calendar (`dtcm`):** Prints Day, Week, Month, Year (12-month calendar
  grids), Appointment List, and To-Do List reports honoring CSA privacy filters
  (`print.c`).
- **Help Viewer (`dthelpview`, `libDtHelp`, and `dthelpprint`):** Renders SDL
  and SGML help topics, tables of contents, man pages, and help files to vector
  PDF (`-outputFile *.pdf` or `.pdf` / `file:` target in the Help Print dialog)
  and submits directly to CUPS/IPP queues (`Print.c`, `PrintUtil.c`).

## Validation

- `cde/lib/DtPrint/tests/check-dtprint-pdf-cups.py` (`make check` in
  `cde/lib/DtPrint`) verifies `libXm.so.9` runtime linkage, multi-page
  Unicode/CJK ISO A4 vector PDF generation with `1,3-4` page-range filtering,
  `DtPrintSetupBox` CUPS queue discovery, `DtPrintPopupJobSetupDialog`,
  `dthelpprint` PDF/IPP modes, and binary RFC 8011 IPP/1.1 attribute delivery
  (`copies=3`, `sides=two-sided-long-edge`, `media=na_legal_8.5x14in`,
  `page-ranges=2-4`) against a local IPP/1.1 HTTP test server.
