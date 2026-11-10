MAINTAINERCLEANFILES = Makefile.in

SUBDIRS = Common RFC

noinst_LIBRARIES = libDtMail.a

libDtMail_a_SOURCES =

# Unfortunately, this does not work -
# libDtMail_a_LIBADD = Common/libCommon.a RFC/libRFC.a
libDtMail_a_LIBADD = Common/*.o RFC/*.o

# The mailbox tests compile the production sources that they exercise (named
# in tests/check-mailbox-files.py) with the sanitizers, open and save mailboxes
# and talk to a mail server of their own through the public interface of
# libDtMail, and link with the built libDtMail.a, so they check the code that is
# shipped.
EXTRA_DIST = tests/check-mailbox-files.py tests/mailbox-files.C

check-local:
	DTMAIL_COMPILE='$(CXX) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) -I$(srcdir)/Common -I$(srcdir)/RFC -I$(top_srcdir)/programs/dtmail/include -I$(top_srcdir)/programs/dtmail/include/utils -DTTLOCK_OFF -DDL_NOT_DYNAMIC $(AM_CXXFLAGS) $(CXXFLAGS)' \
	DTMAIL_LINK='$(LIBTOOL) --tag=CXX --mode=link $(CXX) $(AM_CXXFLAGS) $(CXXFLAGS) $(AM_LDFLAGS) $(LDFLAGS)' \
	DTMAIL_LIBS='$(DTCLIENTLIBS) $(XTOOLLIB) $(LTLIBICONV) $(LIBS)' \
	python3 $(srcdir)/tests/check-mailbox-files.py $(srcdir) libDtMail.a
