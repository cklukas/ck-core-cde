MAINTAINERCLEANFILES = Makefile.in

SUBDIRS = dtmailspool libDtMail MotifApp dtmail dtmailpr

# The tests of dtmailspool compile its production sources with the sanitizers,
# start them the way the mailer does and talk to them over the protocol; see
# tests/check-spool-helper.py.
EXTRA_DIST = tests/check-spool-helper.py tests/check-message-text.py \
             tests/check-local-input.py tests/check-tooltalk-handlers.py \
             tests/check-mail-servers.py tests/check-mail-messages.py \
             tests/check-dtmailpr-input-create.py tests/check-dtmailpr-input-create.cc \
             tests/check-roam-temp-file-create.py tests/check-roam-temp-file-create.cc \
             tests/check-session-file-create.py tests/check-session-file-create.cc \
             tests/check-mail-accounts.py tests/check-html-readable.py \
             tests/check-curl-mail.py tests/check-thread-rules.py \
             tests/check-oauth2-pkce.py \
             tests/check-carddav-contacts.py tests/check-carddav-contacts.cc \
             tests/check-mail-p3.py tests/check-mail-p3.cc tests/check-mail-child-processes.py

check-local:
	DTMAIL_CC='$(CC) $(DEFS) $(AM_CPPFLAGS) $(CPPFLAGS) $(AM_CFLAGS) $(CFLAGS)' \
	python3 $(srcdir)/tests/check-spool-helper.py $(srcdir)
	CXX="$(CXX)" python3 $(srcdir)/tests/check-message-text.py
	CXX="$(CXX)" python3 $(srcdir)/tests/check-local-input.py
	CXX="$(CXX)" python3 $(srcdir)/tests/check-tooltalk-handlers.py
	CXX="$(CXX)" python3 $(srcdir)/tests/check-mail-servers.py
	CXX="$(CXX)" python3 $(srcdir)/tests/check-mail-messages.py
	CXX="$(CXX)" python3 $(srcdir)/tests/check-dtmailpr-input-create.py
	CXX="$(CXX)" python3 $(srcdir)/tests/check-roam-temp-file-create.py
	CXX="$(CXX)" python3 $(srcdir)/tests/check-session-file-create.py
	CXX="$(CXX)" python3 $(srcdir)/tests/check-mail-accounts.py
	CXX="$(CXX)" python3 $(srcdir)/tests/check-html-readable.py
	CXX="$(CXX)" python3 $(srcdir)/tests/check-curl-mail.py
	CXX="$(CXX)" python3 $(srcdir)/tests/check-thread-rules.py
	CXX="$(CXX)" python3 $(srcdir)/tests/check-oauth2-pkce.py
	CXX="$(CXX)" python3 $(srcdir)/tests/check-carddav-contacts.py
	CXX="$(CXX)" python3 $(srcdir)/tests/check-mail-p3.py
	CXX="$(CXX)" python3 $(srcdir)/tests/check-mail-child-processes.py
