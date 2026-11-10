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
# the observer of a notice with a disposition must not get it twice.
EXTRA_DIST = tests/check-notice.py tests/notice-test.c

check-local: ttsession
	TT_NOTICE_COMPILE='$(CC) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) $(CFLAGS)' \
	TT_NOTICE_LINK='$(LIBTOOL) --tag=CXX --mode=link $(CXXLD) $(AM_CXXFLAGS) $(CXXFLAGS) $(AM_LDFLAGS) $(LDFLAGS)' \
	TT_NOTICE_LIBS='$(LIBTT) $(TIRPCLIB) $(XTOOLLIB) $(X_LIBS) $(LIBS)' \
	python3 $(srcdir)/tests/check-notice.py $(srcdir) ./ttsession
