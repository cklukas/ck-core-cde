# Include after the configured component Makefile to use the pinned headers.
# This syntax check requires a completed dtksh build.
DTKSH_C23_FLAGS = -std=c23 -Werror=strict-prototypes -Werror=old-style-definition \
                  -Werror=implicit-function-declaration -Werror=incompatible-pointer-types
DTKSH_C23_CHECK = $(CC) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(CPPFLAGS) $(DTKSH_C23_FLAGS) -fsyntax-only

.PHONY: check-dtksh-c23
check-dtksh-c23:
	$(DTKSH_C23_CHECK) $(dtksh_CPPFLAGS) $(srcdir)/dtksh-main.c $(srcdir)/msgs.c
	$(DTKSH_C23_CHECK) $(test_widget_la_CPPFLAGS) $(srcdir)/test-widget.c
	$(DTKSH_C23_CHECK) $(dtksh_test_CPPFLAGS) $(srcdir)/dtksh-test.c
