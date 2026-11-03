MAINTAINERCLEANFILES = Makefile.in

CLEANFILES = DtFuncs.dtsh ksh-work/.built

include $(srcdir)/ksh93.mk

EXTRA_DIST = $(KSH93_TARBALL) prepare-ksh.py run-shell-tests.sh $(KSH93_PATCHES) $(KSH93_TEST_PATCHES) DtFuncs.sh widget-callback.dtsh dtksh.exp
noinst_HEADERS = XtCvtrs.h dtkcmds.h dtkcvt.h dtksh.h extra.h msgs.h widget.h \
                 xmcmds.h xmcvt.h xmksh.h xmwidgets.h

kshlicensedir = $(datadir)/licenses/dtksh
dist_kshlicense_DATA = ksh93.mk
nodist_kshlicense_DATA = $(KSH93SRC)/COPYRIGHT $(KSH93SRC)/LICENSE.md

check_PROGRAMS = dtksh-test

check_LTLIBRARIES = test-widget.la
test_widget_la_SOURCES = test-widget.c
test_widget_la_CPPFLAGS = -I$(KSH93SRC)/include/ast
test_widget_la_LDFLAGS = -module -avoid-version -shrext .so -rpath $(abs_builddir)
test_widget_la_LIBADD = $(XTOOLLIB)

dtksh_test_SOURCES = dtksh-test.c
dtksh_test_CPPFLAGS = '-DDTKSH_TEST_BUILDDIR="$(abs_builddir)"'

SUBDIRS = examples

kshdir = ${libdir}/dtksh

dist_ksh_SCRIPTS = DtFuncs.dtsh

KSH93SRC = ksh-work/source

KSH93_DYNLIB = $(abs_builddir)/ksh-work/lib
kshlibdir = $(kshdir)/ksh93

bin_PROGRAMS = dtksh

BUILT_SOURCES = ksh-work/.built

dtksh_CPPFLAGS = -I$(KSH93SRC)/include/ast -I$(KSH93SRC)/src/cmd/ksh93/include \
		 -I$(KSH93SRC)/src/cmd/ksh93 -I$(top_srcdir)/lib/DtPrint \
		 -I$(top_srcdir)/lib/DtHelp -I$(top_srcdir)/include \
		 -include FEATURE/options -D_BLD_ksh=1 -D_API_ast=20100309

# Mirror the installed layout beside the uninstalled libtool executable.
# Keep the private libraries as direct dependencies so the executable's
# RUNPATH also finds libshell's dependencies before installation.
dtksh_LDFLAGS = -R$(kshlibdir)
dtksh_LDADD = $(DTCLIENTLIBS) $(XTOOLLIB) -L$(KSH93_DYNLIB)
if AIX
dtksh_LDADD += -lshell -lcmd -ldll -last
else
dtksh_LDFLAGS += '-Wl,-rpath,$$ORIGIN/../lib/dtksh/ksh93'
if SOLARIS
dtksh_LDFLAGS += -Wl,-z,record,-lshell,-lcmd,-ldll,-last
else
dtksh_LDFLAGS += -Wl,--push-state,--no-as-needed,-lshell,-lcmd,-ldll,-last,--pop-state
endif
endif

if SOLARIS
dtksh_LDADD += -lsecdb -lsocket -lnsl
endif

if AIX
dtksh_CPPFLAGS += -D_IBMRPC_
dtksh_LDFLAGS += -bE:$(srcdir)/dtksh.exp
endif

dtksh_SOURCES = dtksh-main.c \
                widget.c \
                dtkcvt.c \
                dtkcmds.c \
                XtCvtrs.c \
                xmcvt.c \
                xmcmds.c \
                xmwidgets.c \
                extra.c \
                msgs.c

# The optional archive path can be supplied to make for an offline build.
KSH93_JOBS = 1
KSH93_TARBALL = ksh-$(KSH93_REVISION).tar.gz

.PHONY: prepare-ksh check-widget FORCE
prepare-ksh: ksh-work/.prepared

ksh-work/.prepared: FORCE
	python3 "$(srcdir)/prepare-ksh.py" ksh-work $(KSH93_REVISION) $(KSH93_SHA256) \
		"$(KSH93_URL)" "$(KSH93_ARCHIVE)" "$(CC) $(kshlibdir)" $(addprefix $(srcdir)/,$(KSH93_PATCHES))

ksh-work/.built: ksh-work/.prepared Makefile
	cd "$(KSH93SRC)" && CC="$(CC)" LDFLAGS="-Wl,-rpath,$(kshlibdir)" bin/package flat make -j$(KSH93_JOBS)
	host=`cd "$(KSH93SRC)" && bin/package host type`; \
		test -x "$(KSH93SRC)/arch/$$host/dyn/bin/ksh" || \
		{ echo "dtksh needs ksh93 shared libraries for loadable widget classes" >&2; exit 1; }; \
		ln -sfn "source/arch/$$host/dyn/lib" ksh-work/lib
	$(MKDIR_P) lib/dtksh
	ln -sfn ../../ksh-work/lib lib/dtksh/ksh93
	touch $@

$(dtksh_OBJECTS) $(test_widget_la_OBJECTS) $(nodist_kshlicense_DATA): ksh-work/.built

install-exec-hook:
	$(MKDIR_P) "$(DESTDIR)$(kshlibdir)"
	cp -P "$(KSH93_DYNLIB)"/* "$(DESTDIR)$(kshlibdir)/"

uninstall-hook:
	for name in ast cmd dll shell; do \
		base="$(DESTDIR)$(kshlibdir)/lib$$name"; \
		$(RM) "$$base.so" "$$base.so."[0-9]* "$$base.dylib" "$$base."[0-9]*.dylib; \
	done

clean-local:
	rm -rf "$(KSH93SRC)" ksh-work/lib ksh-work/.prepared lib/dtksh/ksh93
	-rmdir lib/dtksh lib 2>/dev/null

distclean-local:
	rm -rf ksh-work

check-local:
	$(abs_builddir)/dtksh-test -c true && \
		$(abs_builddir)/$(KSH93SRC)/bin/package use "$(abs_builddir)/$(KSH93SRC)" \
			sh "$(abs_srcdir)/run-shell-tests.sh" "$(abs_builddir)" \
			$(addprefix $(abs_srcdir)/,$(KSH93_TEST_PATCHES)) -- $(KSH93_TEST_CASES)

check-widget: dtksh dtksh-test test-widget.la
	$(abs_builddir)/dtksh-test $(srcdir)/widget-callback.dtsh $(abs_builddir)/.libs/test-widget.so

DtFuncs.dtsh: $(srcdir)/DtFuncs.sh
	$(CP) $(srcdir)/DtFuncs.sh DtFuncs.dtsh
