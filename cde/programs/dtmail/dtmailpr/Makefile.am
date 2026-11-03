MAINTAINERCLEANFILES = Makefile.in

bin_PROGRAMS = dtmailpr

dtmailpr_CXXFLAGS = -I../include -I../../dtcompat -I../../../. \
		    -I$(top_srcdir)/programs/dtmail/include \
		    -I$(srcdir)/lib

dtmailpr_LDADD = ../libDtMail/libDtMail.a $(DTCLIENTLIBS) $(XTOOLLIB) $(LTLIBICONV)

dtmailpr_SOURCES = main.C message.C mailbox.C utils.C
