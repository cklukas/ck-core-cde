MAINTAINERCLEANFILES = Makefile.in

# The only part of the mailer that holds the mail group.  It is plain C with
# nothing but the C library, so that what runs with the group is small enough
# to read.
bin_PROGRAMS = dtmailspool

dtmailspool_CPPFLAGS = -I$(top_srcdir)/programs/dtmail/include \
		       -DDTMAIL_SPOOL_DIR=\"$(MAIL_SPOOL_DIR)\"

dtmailspool_SOURCES = main.c spool.c spool.h

# Give the helper the group of the mail spool and set its group id.  Do not
# fail where the group does not exist or the install is not made by a user who
# may change groups: the helper then runs with the group of the user, which is
# what a spool directory that every user can write to needs.
install-exec-hook:
	-(chgrp mail $(DESTDIR)$(bindir)/dtmailspool && chmod 2755 $(DESTDIR)$(bindir)/dtmailspool)
