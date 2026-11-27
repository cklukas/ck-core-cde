MAINTAINERCLEANFILES = Makefile.in

lib_LTLIBRARIES = libDtPrint.la

libDtPrint_la_CFLAGS = -DMULTIBYTE -DI18N_MSG @DT_INCDIR@ $(DTPRINT_PDF_DEPS_CFLAGS)

# in order to try to keep lib versions the same across platforms, (2.1.0)
if BSD
libDtPrint_la_LDFLAGS = -version-info 2:1:0
else
libDtPrint_la_LDFLAGS = -version-info 3:0:1
endif

libDtPrint_la_LIBADD = $(DTPRINT_PDF_DEPS_LIBS) $(LIBDL) -lm

if SOLARIS
libDtPrint_la_CFLAGS += -DNO_REGCOMP
endif

libDtPrint_la_SOURCES = PrintMsgs.c PrintDlgMgr.c PrintOptions.c PrintSetupB.c \
			PsubDefProc.c PsubUtil.c PrintPdfCups.c

EXTRA_DIST = tests/check-dtprint-pdf-cups.py
TESTS = tests/check-dtprint-pdf-cups.py
