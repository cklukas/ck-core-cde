MAINTAINERCLEANFILES = Makefile.in

bin_PROGRAMS = dtcm dtcm_delete dtcm_insert dtcm_lookup dtcm_editor

EXTRA_DIST = tests/check-temp-file-create.py tests/check-temp-file-create.c \
             tests/check-sigint-signal.py tests/check-sigint-signal.c \
             tests/check-ics-io.py tests/ics-io-test.c \
             tests/check-caldav.py tests/check-notify-desktop.py \
             tests/check-ux-helpers.py tests/check-undo-stack.py \
             tests/check-overlay-store.py

check-local:
	CC="$(CC)" python3 $(srcdir)/tests/check-temp-file-create.py
	CC="$(CC)" python3 $(srcdir)/tests/check-sigint-signal.py
	CC="$(CC)" python3 $(srcdir)/tests/check-ics-io.py
	CC="$(CC)" python3 $(srcdir)/tests/check-caldav.py
	CC="$(CC)" python3 $(srcdir)/tests/check-notify-desktop.py
	CC="$(CC)" python3 $(srcdir)/tests/check-ux-helpers.py
	CC="$(CC)" python3 $(srcdir)/tests/check-undo-stack.py
	CC="$(CC)" python3 $(srcdir)/tests/check-overlay-store.py

AM_CFLAGS = $(DT_INCDIR) $(CSA_INCDIR) -I$(top_srcdir)/lib/csa	\
-I$(top_srcdir)/programs/dtcm/libDtCmP $(LIBCURL_CFLAGS) \
-DHAVE_LIBCURL=$(DTCM_CALDAV_ENABLED)

LDADD = ../libDtCmP/libDtCmP.a $(LIBCSA) $(DTCLIENTLIBS) $(TIRPCLIB) $(LIBCURL_LIBS) $(XTOOLLIB)

if FREEBSD
LDADD += -lcompat -lutil
endif

if NETBSD
LDADD += -lcompat
endif

dtcm_SOURCES = MonthPanel.c    RFCMIME.c       about.c \
               agenda_ui.c     agenda_ui.h     alarm.c \
               blist.c         browser.c \
               calendarA.c     caldav_client.c caldav_client.h \
               caldav_ui.c     caldav_ui.h     cm_i18n.c       cmtt.c \
               context_menu.c  context_menu.h \
               datefield.c     dayglance.c     deskset.c \
               dnd.c           dssw.c          editor.c \
               find.c          fns.c           font.c \
               format.c        goto.c          graphics.c \
               group_editor.c  help.c          i18nxlate.c \
               icon.c          ics_csa.c       ics_csa.h \
               ics_io.c        ics_io.h        md5.c           misc.c \
               monthglance.c   namesvc.c       notify_desktop.c \
               notify_desktop.h overlay_store.c overlay_store.h \
               overlay_ui.c    overlay_ui.h    print.c \
               props.c         props_pu.c      reminders.c \
               rfp.c           select.c        server_status.c \
               server_status.h session.c \
	       submit.c        tempbr.c        timezone.c \
               todo.c          undo_stack.c    undo_stack.h \
               ux_helpers.c    ux_helpers.h \
               weekglance.c    x_graphics.c    yearglance.c

dtcm_delete_SOURCES = cm_delete.c

dtcm_insert_SOURCES = cm_insert.c

dtcm_lookup_SOURCES = cm_lookup.c

dtcm_editor_SOURCES = datefield.c     deskset.c       dssw.c \
		      dtcm_editor.c   help.c          ics_io.c \
		      misc.c          reminders.c     rfp.c \
		      ux_helpers.c

install-exec-hook:
	$(INSTALL) -m 0755 -d $(DESTDIR)/var/spool/calendar
