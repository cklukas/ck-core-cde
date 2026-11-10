MAINTAINERCLEANFILES = Makefile.in

SUBDIRS = dtmailspool libDtMail MotifApp dtmail dtmailpr

# The tests of dtmailspool compile its production sources with the sanitizers,
# start them the way the mailer does and talk to them over the protocol; see
# tests/check-spool-helper.py.
EXTRA_DIST = tests/check-spool-helper.py tests/check-message-text.py \
             tests/check-local-input.py tests/check-tooltalk-handlers.py \
             tests/check-mail-servers.py tests/check-mail-messages.py

check-local:
	DTMAIL_CC='$(CC) $(DEFS) $(AM_CPPFLAGS) $(CPPFLAGS) $(AM_CFLAGS) $(CFLAGS)' \
	python3 $(srcdir)/tests/check-spool-helper.py $(srcdir)
	CXX="$(CXX)" python3 $(srcdir)/tests/check-message-text.py
	CXX="$(CXX)" python3 $(srcdir)/tests/check-local-input.py
	CXX="$(CXX)" python3 $(srcdir)/tests/check-tooltalk-handlers.py
	CXX="$(CXX)" python3 $(srcdir)/tests/check-mail-servers.py
	CXX="$(CXX)" python3 $(srcdir)/tests/check-mail-messages.py
