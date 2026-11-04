MAITAINERCLEANFILES = Makefile.in

bin_PROGRAMS = dtspcd

dtspcd_CFLAGS = -DCDE_CONFIGURATION_TOP=\"${prefix}\" \
		-DCDE_INSTALLATION_TOP=\"$(CDE_INSTALLATION_TOP)\" \
		$(DT_INCDIR)

dtspcd_LDADD = $(DTCLIENTLIBS) $(XTOOLLIB)

if SOLARIS
dtspcd_LDADD += -lgen
endif

cfgdir = $(CDE_INSTALLATION_TOP)/config
dist_cfg_DATA = dtspcdenv

# I really think we should get rid of dtspcd in general...

dtspcd_SOURCES = main.c spcd_event.c

# The request-path tests include main.c and link the built libDtSvc, so they
# check the code that is shipped.
EXTRA_DIST = tests/check-requests.py tests/requests-test.c

check-local:
	SPC_COMPILE='$(CC) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) $(dtspcd_CFLAGS) $(AM_CFLAGS) $(CFLAGS)' \
	SPC_LINK='$(LIBTOOL) --tag=CC --mode=link $(CCLD) $(AM_CFLAGS) $(CFLAGS) $(AM_LDFLAGS) $(LDFLAGS)' \
	SPC_LIBS='$(dtspcd_LDADD) $(LIBS)' \
	python3 $(srcdir)/tests/check-requests.py $(srcdir)

DEF_SYSTEM_PATH = "/bin:/usr/bin"
LOCAL_CPP_DEFINES = -DCDE_CONFIGURATION_TOP=$(CDE_CONFIGURATION_TOP)	\
	-DCDE_LOGFILES_TOP=$(CDE_LOGFILES_TOP) \
	-DDEF_SYSTEM_PATH=$(DEF_SYSTEM_PATH)

CPP_TARGETS = dtspcdenv
BUILT_SOURCES = $(CPP_TARGETS)
CLEANFILES = $(CPP_TARGETS)

dtspcdenv: dtspcdenv.src
	$(RM) $@
	$(GENCPP) -P -DXCOMM=# $(LOCAL_CPP_DEFINES) $< > $@

