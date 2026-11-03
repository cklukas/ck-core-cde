MAINTAINERCLEANFILES = Makefile.in

CLEANFILES = init.c DtFuncs.dtsh

check_PROGRAMS = dtksh-test

dtksh_test_SOURCES = dtksh-test.c
dtksh_test_CPPFLAGS = '-DDTKSH_TEST_BUILDDIR="$(abs_builddir)"'

SUBDIRS = examples

kshdir = ${libdir}/dtksh

dist_ksh_SCRIPTS = DtFuncs.dtsh

KSH93SOURCE = $(abs_srcdir)/ksh93
KSH93SRC = $(abs_builddir)/ksh93

KSH93LIBSHELL = $(KSH93SRC)/lib/libshell.a
KSH93_COMPAT_CFLAGS = -std=gnu17

bin_PROGRAMS = dtksh

BUILT_SOURCES = init.c

# Put all SHOPT definitions here.  It is vital that dtksh and ksh93
# are built with the same options.  We pass this list to both the
# dtksh components and to the ksh93 build as well.  They must be
# specifically assigned a 1 or various preprocessor checks (#if SHOPT
# ...) in ksh93 will fail.
KSH93_SHOPTS = -DSHOPT_DYNAMIC=1 -DSHOPT_NAMESPACE=1 -DSHOPT_MULTIBYTE=1 \
	-DSHOPT_STATS=1 -DSHOPT_BGX=1 -DSHOPT_AUDIT=1 \
	-DSHOPT_KIA=1 -DSHOPT_HISTEXPAND=1 -DSHOPT_EDPREDICT=1 \
	-DSHOPT_ESH=1 -DSHOPT_VSH=1 -DSHOPT_FIXEDARRAY=1 -DSHOPT_SUID_EXEC=1 \
	-DSHOPT_BRACEPAT=1 -DSHOPT_ECHOPRINT=1

dtksh_CPPFLAGS = -I$(KSH93SRC)/include/ast -I$(KSH93SRC)/src/cmd/ksh93/include \
		 -I$(KSH93SRC)/src/cmd/ksh93 -I${srcdir}/lib/DtPrint \
		 -I${srcdir}/lib/DtHelp $(KSH93_SHOPTS) \
		 -D_PACKAGE_ast -DKSHELL \
		 '-DERROR_CONTEXT_T=Error_context_t' \
		 -D_API_ast=20100309 -DBUILD_DTKSH

dtksh_LDADD = $(DTCLIENTLIBS) ksh93/src/cmd/ksh93/pmain.o $(XTOOLLIB) \
	      libshell.a $(KSH93SRC)/lib/libcmd.a $(KSH93SRC)/lib/libast.a \
	      $(KSH93SRC)/lib/libdll.a

if SOLARIS
dtksh_LDADD += -lsecdb -lsocket -lnsl
endif

if AIX
dtksh_CPPFLAGS += -D_IBMRPC_
dtksh_LDFLAGS = -bE:dtksh.exp
endif

dtksh_SOURCES = init.c \
                widget.c \
                dtkcvt.c \
                dtkcmds.c \
                XtCvtrs.c \
                xmcvt.c \
                xmcmds.c \
                xmwidgets.c \
                extra.c \
                msgs.c

$(KSH93SRC)/.source-copy: $(KSH93SOURCE)/bin/package
	$(RM) -r $(KSH93SRC)
	$(MKDIR_P) $(KSH93SRC)
	$(CP) -R $(KSH93SOURCE)/. $(KSH93SRC)/
	touch $@

$(KSH93SRC)/bin/ksh: $(KSH93SRC)/.source-copy
	cd $(KSH93SRC) && bin/package flat make CC='$(CC)' \
		CCFLAGS='$(KSH93_COMPAT_CFLAGS) $(KSH93_SHOPTS)'

init.c: $(KSH93SRC)/bin/ksh
	$(CP) $(KSH93SRC)/src/cmd/ksh93/sh/init.c ./

libshell.a: $(KSH93LIBSHELL) dtksh-init.o
	$(CP) $(KSH93LIBSHELL) libshell.a; \
	$(AR) d libshell.a init.o; \
	$(AR) cq libshell.a dtksh-init.o

check-local:
	$(abs_builddir)/dtksh-test -c true && \
		$(KSH93SRC)/bin/shtests --posix --utf8 \
			SHELL=$(abs_builddir)/dtksh-test

clean-local:
	$(RM) -r $(KSH93SRC)

DtFuncs.dtsh: $(srcdir)/DtFuncs.sh
	$(CP) $(srcdir)/DtFuncs.sh $@
