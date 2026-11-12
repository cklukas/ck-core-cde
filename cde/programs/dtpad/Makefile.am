MAINTAINERCLEANFILES = Makefile.in

bin_PROGRAMS = dtpad

dtpad_CFLAGS = -DDT_LITE -DWORDWRAP

dtpad_LDADD = $(DTCLIENTLIBS) $(TIRPCLIB) $(XTOOLLIB)

if SOLARIS
dtpad_LDFLAGS = -lintl -lresolv
endif

dtpad_SOURCES = dtpad.c         editCB.c        fileCB.c \
		fileDlg.c       fileIo.c        formatCB.c \
		helpCB.c        helpDlg.c       main.c \
		optionsCB.c     printJob.c      printOutput.c \
		printSetup.c    session.c       ttMsgSupport.c \
		ttSaveSupport.c dirselect.c dirselect.h

EXTRA_DIST = tests/check-dirselect.py tests/dirselect-test.c

# dirselect.c does not depend on Motif or Xt, so its check builds it on its
# own with the sanitizers and runs the test against that code.
check-local:
	DIRSELECT_CC='$(CC)' python3 $(srcdir)/tests/check-dirselect.py $(srcdir)
