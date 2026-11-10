MAINTAINERCLEANFILES = Makefile.in

SUBDIRS = libDtMail MotifApp dtmail dtmailpr

EXTRA_DIST = tests/check-group-privileges.py tests/check-message-text.py

check-local:
	CXX="$(CXX)" python3 $(srcdir)/tests/check-group-privileges.py
	CXX="$(CXX)" python3 $(srcdir)/tests/check-message-text.py
