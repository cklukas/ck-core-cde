MAINTAINERCLEANFILES = Makefile.in

include ../dtdocbook.am

infolib = ../infolib

SUBDIRS = $(infolib) ../dtsr ../instant

noinst_PROGRAMS = \
		  dtdocbook2infolib \
		  MixedGen \
		  NCFGen \
		  NodeParser \
		  StyleUpdate \
		  dbdrv \
		  valBase \
		  validator

noinst_SCRIPTS = dtdocbook2man dtdocbook2sdl

AM_CXXFLAGS = -DCDE_NEXT -I$(top_builddir)/lib/DtMmdb
AM_LDFLAGS = $(DTCLIENTLIBS) $(XTOOLLIB)

dtdcbk_datarootdir = $(abs_top_srcdir)/programs/dtdocbook
dtdcbk_buildexecdir = $(abs_top_builddir)/programs/dtdocbook

AM_CPPFLAGS = \
	      -DDTDCBK_DATAROOTDIR=\"$(dtdcbk_datarootdir)\" \
	      -DDTDCBK_BUILDROOTDIR=\"$(abs_top_builddir)/programs/dtdocbook\" \
	      -DINFOLIB_LIBEXECDIR=\"$(abs_builddir)\"

validator_SOURCES = validator.C
validator_LDFLAGS = $(LIBMMDB) $(LMDBLIB)

CLEANFILES = $(noinst_SCRIPTS) $(validator_SOURCES) dtdocbook2infolib.c

dtdocbook2infolib.c: ../dtdocbook2infolib.c
	$(RM) $@ && $(LN_S) $<

validator.C: $(infolib)/validator.C
	$(RM) $@ && $(LN_S) $<

MixedGen$(EXEEXT): $(infolib)/MixedGen$(EXEEXT)
	$(RM) $@ && $(LN_S) $<

NCFGen$(EXEEXT): $(infolib)/NCFGen$(EXEEXT)
	$(RM) $@ && $(LN_S) $<

NodeParser$(EXEEXT): $(infolib)/NodeParser$(EXEEXT)
	$(RM) $@ && $(LN_S) $<

StyleUpdate$(EXEEXT): $(infolib)/StyleUpdate$(EXEEXT)
	$(RM) $@ && $(LN_S) $<

dbdrv$(EXEEXT): $(infolib)/dbdrv$(EXEEXT)
	$(RM) $@ && $(LN_S) $<

valBase$(EXEEXT): $(infolib)/valBase$(EXEEXT)
	$(RM) $@ && $(LN_S) $<

dtdocbook2man: ../dtdocbook2man
	echo "#!/bin/sh" > $@ && chmod +x $@
	echo "dtdcbk_libexecdir=$(dtdcbk_buildexecdir) \
		dtdcbk_datarootdir=$(dtdcbk_datarootdir) \
		sh $(abs_builddir)/$< \$$@" >> $@

dtdocbook2sdl: ../dtdocbook2sdl
	echo "#!$(KSH)" > $@ && chmod +x $@
	echo "dtdcbk_libdir=$(dtdcbk_datarootdir) \
		dtdcbk_libexecdir=$(dtdcbk_buildexecdir) \
		dtdcbk_datarootdir=$(dtdcbk_datarootdir) \
		$(KSH) $(abs_builddir)/$< \$$@" >> $@

install:

distclean:
