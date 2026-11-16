MAINTAINERCLEANFILES = Makefile.in

bin_PROGRAMS = dthelpprint
bin_SCRIPTS =  dthelpprint.sh

dthelpprint_CPPFLAGS = -I.. -I ../../../lib/DtHelp \
	-I $(top_srcdir)/lib/DtHelp \
	-DCDE_CONFIGURATION_TOP='"${prefix}"' \
	-DCDE_INSTALLATION_TOP='"$(CDE_INSTALLATION_TOP)"'

dthelpprint_LDADD = $(DTCLIENTLIBS) $(XTOOLLIB)

if SOLARIS
dthelpprint_LDADD += -lw -lgen
endif

dthelpprint_SOURCES = Main.c Initialize.c PrintUtil.c PrintTopics.c \
		      PrintManStrFile.c

EXTRA_DIST = tests/check-dthelpprint-hardening.py \
	     tests/check-dthelpprint-hardening.c

check-local:
	CC="$(CC)" $(PYTHON3) $(srcdir)/tests/check-dthelpprint-hardening.py
