MAINTAINERCLEANFILES = Makefile.in

bin_PROGRAMS = dtcreate

EXTRA_DIST = tests/check-icon-temp-file-create.py tests/check-icon-temp-file-create.c \
             tests/check-desktop-apps-hardening.py tests/check-desktop-apps-hardening.c

check-local:
	CC="$(CC)" python3 $(srcdir)/tests/check-icon-temp-file-create.py
	CC="$(CC)" python3 $(srcdir)/tests/check-desktop-apps-hardening.py

dtcreate_LDADD = $(DTCLIENTLIBS) $(XTOOLLIB)

dtcreate_SOURCES = main.c AddFiletype.c Confirmed.c			\
        CreateActionAppShell.c FileCharacteristics.c OpenFile.c		\
        UxXt.c ca_aux.c cmncbs.c cmnutils.c cmnrtns.c ErrorDialog.c	\
        fileio.c af_aux.c icon_selection_dialog.c parser.c		\
        AddFiletype.h Confirmed.h icon_selection_dialog.h af_aux.h	\
        CreateActionAppShell.h OpenFile.h ca_aux.h dtcreate.h		\
        parser.h cmncbs.h ErrorDialog.h UxXt.h cmnrtns.h		\
        FileCharacteristics.h cmnutils.h fileio.h

AM_CFLAGS = $(CK_HARDENING_PHASE2_FLAGS)
