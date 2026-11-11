MAINTAINERCLEANFILES = Makefile.in

SUBDIRS = api db mp util tttk

lib_LTLIBRARIES = libtt.la

libtt_la_SOURCES =

# fake things so libtool creates this as a C++ library
nodist_EXTRA_libtt_la_SOURCES = dummy.cxx
libtt_la_LIBADD = api/c/libapi.la api/dnd/libdnd.la db/libdb.la mp/libmp.la \
		  util/libutil.la tttk/libtttk.la

if SOLARIS
# This stuff should be figured out by configure
libtt_la_LIBADD += -lnsl -lsocket -lintl -lw
endif

if LINUX
libtt_la_LIBADD += $(TIRPCLIB)
endif

# in order to try to keep lib versions the same across platforms, (2.1.0)
if BSD
libtt_la_LDFLAGS = -version-info 2:1:0
else
libtt_la_LDFLAGS = -version-info 3:0:1
endif


# The environment and program-start tests compile the production sources they
# exercise (the settings libtt reads from the environment, the starting of
# ttsession and the running of programs without a shell) with the sanitizers,
# next to the test program, and link them in front of the built libtt, so
# they check the code that is shipped.
#
# The documented-calls check needs no build: the manual pages and guides of every
# language, the examples and the demos must show the calls of the Desktop
# interface the way include/Tt/tttk.h declares them, and the usage of the dtksh
# command ttdt_session_join in the documents and in the message catalogs of every
# locale must name its requests.
EXTRA_DIST = tests/check-environment.py tests/environment-test.C \
	     tests/check-documented-calls.py

tt_environment_libs = libtt.la $(X_LIBS)

if LINUX
tt_environment_libs += $(TIRPCLIB) $(XTOOLLIB)
endif

if SOLARIS
tt_environment_libs += $(XTOOLLIB) -lintl -lsocket -lnsl
endif

if BSD
tt_environment_libs += $(XTOOLLIB)
endif

check-local: libtt.la
	TT_COMPILE='$(CXX) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) -I. -I$(top_srcdir)/lib/tt/lib $(AM_CXXFLAGS) $(CXXFLAGS)' \
	TT_LINK='$(LIBTOOL) --tag=CXX --mode=link $(CXXLD) $(AM_CXXFLAGS) $(CXXFLAGS) $(AM_LDFLAGS) $(LDFLAGS)' \
	TT_LIBS='$(tt_environment_libs) $(LIBS)' \
	python3 $(srcdir)/tests/check-environment.py $(srcdir) $(top_srcdir)/lib/tt
	python3 $(srcdir)/tests/check-documented-calls.py $(top_srcdir)
