MAINTAINERCLEANFILES = Makefile.in

TT_VERSION = "CDE Version @CDE_VERSION_MAJOR@.@CDE_VERSION_MINOR@.@CDE_VERSION_MICRO@@CDE_VERSION_DEV@"
TT_VERSION_DEFINE = -DTT_VERSION_STRING=\"$(TT_VERSION)\"

bin_PROGRAMS = ttsession

ttsession_CXXFLAGS = -I../../slib -I../../lib \
		     -I$(top_srcdir)/lib/tt/slib \
		     -I$(top_srcdir)/lib/tt/lib \
		     $(TT_VERSION_DEFINE)

ttsession_LDADD = $(LIBTT) $(X_LIBS) ../../slib/libstt.a

if LINUX
ttsession_LDADD += $(TIRPCLIB) $(XTOOLLIB)
endif

if SOLARIS
ttsession_LDADD += $(XTOOLLIB) -lintl -lsocket -lnsl
endif

if BSD
ttsession_LDADD += $(XTOOLLIB)
endif

ttsession_SOURCES = mp_server.C

# The notice test starts a session of its own and runs two clients against it:
# the observer of a notice with a disposition must not get it twice.  The
# session requests test does the same for the Desktop requests that act on a
# whole process: a process answers only those it asked ttdt_session_join() for.
# The sender imprint test runs ttdt_sender_imprint_on() against a handler that
# answers well or badly: the calling process is changed only as it is asked.
EXTRA_DIST = tests/check-notice.py tests/notice-test.c \
	     tests/check-session-requests.py tests/session-requests-test.c \
	     tests/check-sender-imprint.py tests/imprint-test.c

tt_check_compile = $(CC) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) $(CFLAGS)
tt_check_link = $(LIBTOOL) --tag=CXX --mode=link $(CXXLD) $(AM_CXXFLAGS) $(CXXFLAGS) $(AM_LDFLAGS) $(LDFLAGS)
tt_check_libs = $(LIBTT) $(TIRPCLIB) $(XTOOLLIB) $(X_LIBS) $(LIBS)

check-local: ttsession
	TT_NOTICE_COMPILE='$(tt_check_compile)' \
	TT_NOTICE_LINK='$(tt_check_link)' \
	TT_NOTICE_LIBS='$(tt_check_libs)' \
	python3 $(srcdir)/tests/check-notice.py $(srcdir) ./ttsession
	TT_REQUESTS_COMPILE='$(tt_check_compile)' \
	TT_REQUESTS_LINK='$(tt_check_link)' \
	TT_REQUESTS_LIBS='$(tt_check_libs)' \
	python3 $(srcdir)/tests/check-session-requests.py $(srcdir) ./ttsession
	TT_IMPRINT_COMPILE='$(tt_check_compile)' \
	TT_IMPRINT_LINK='$(tt_check_link)' \
	TT_IMPRINT_LIBS='$(tt_check_libs)' \
	python3 $(srcdir)/tests/check-sender-imprint.py $(srcdir) ./ttsession
