MAINTAINERCLEANFILES = Makefile.in

include ../dtdocbook.am

dtsrdir = $(dtdocbookdatadir)/dtsr

dtsr_DATA = \
	    dtsearch.dbd \
	    deu.sfx \
	    deu.stp \
	    eng.sfx \
	    eng.stp \
	    esp.sfx \
	    esp.stp \
	    fra.sfx \
	    fra.stp \
	    ita.sfx \
	    ita.stp \
	    jpn.knj

noinst_PROGRAMS = mkdbd

dtsearch_build = $(top_builddir)/lib/DtSearch
dtsearch_source = $(top_srcdir)/lib/DtSearch

mkdbd_CPPFLAGS = -DBASE_FILE_NAME=\"dtsearch\"
mkdbd_CFLAGS = $(DT_INCDIR) -I$(dtsearch_build)/raima \
	-I$(dtsearch_source)/raima

CLEANFILES = dtsearch.dbd mkdbd.c

mkdbd.c: $(dtsearch_source)/tiny.c
	$(RM) $@ && $(LN_S) $< $@

dtsearch.dbd: mkdbd
	./$< > $@
