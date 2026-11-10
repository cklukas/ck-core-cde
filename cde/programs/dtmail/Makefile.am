MAINTAINERCLEANFILES = Makefile.in

SUBDIRS = libDtMail MotifApp dtmail dtmailpr

EXTRA_DIST = tests/check-group-privileges.py tests/check-message-text.py \
             tests/check-local-input.py tests/check-tooltalk-handlers.py

check-local:
	CXX="$(CXX)" python3 $(srcdir)/tests/check-group-privileges.py
	CXX="$(CXX)" python3 $(srcdir)/tests/check-message-text.py
	CXX="$(CXX)" python3 $(srcdir)/tests/check-local-input.py
	CXX="$(CXX)" python3 $(srcdir)/tests/check-tooltalk-handlers.py
