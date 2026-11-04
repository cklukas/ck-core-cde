MAINTAINERCLEANFILES = Makefile.in

bin_PROGRAMS = dtsrcreate dtsrdbrec dtsrhan dtsrload dtsrindex dtsrdelete \
	       huffcode dtsrkdump dtsrclean

AM_CFLAGS = -I../../lib/DtSearch -I../../lib/DtSearch/raima	\
	$(DT_INCDIR) -DMAIN_PROGRAM -DNO_XLIB

BUILT_SOURCES = MsgCat.c
CLEANFILES = MsgCat.c

MsgCat.c: ${top_srcdir}/lib/DtSvc/DtUtil2/MsgCat.c
	$(RM) MsgCat.c && ln -s ${top_srcdir}/lib/DtSvc/DtUtil2/MsgCat.c MsgCat.c


LDADD = ../../lib/DtSearch/libDtSearch.la

dtsrcreate_SOURCES = dtsrcreate.c MsgCat.c

dtsrdbrec_SOURCES = dtsrdbrec.c MsgCat.c

dtsrhan_SOURCES = dtsrhan.c MsgCat.c

dtsrload_SOURCES = dtsrload.c MsgCat.c

dtsrclean_SOURCES = dtsrclean.c MsgCat.c

dtsrindex_SOURCES = dtsrindex.c MsgCat.c

dtsrdelete_SOURCES = tomita.c MsgCat.c

huffcode_SOURCES = huffcode.c MsgCat.c

dtsrkdump_SOURCES = dtsrkdump.c MsgCat.c

# The tests make a database with these tools, with the data dictionary and
# the language files of programs/dtdocbook/dtsr, and query it through the
# DtSrAPI in a program of their own, which runs under the sanitizers when the
# compiler has them.
EXTRA_DIST = tests/check-dtsearch.py tests/dtsearch-api-test.c

check-local: all
	$(MAKE) -C $(top_builddir)/programs/dtdocbook/dtsr dtsearch.dbd
	DTSR_COMPILE='$(CC) $(DEFS) $(DEFAULT_INCLUDES) $(AM_CFLAGS) $(CPPFLAGS) $(CFLAGS)' \
	DTSR_LINK='$(LIBTOOL) --tag=CC --mode=link $(CCLD) $(DEFS) $(DEFAULT_INCLUDES) $(AM_CFLAGS) $(CPPFLAGS) $(CFLAGS) $(AM_LDFLAGS) $(LDFLAGS)' \
	DTSR_LIBS='../../lib/DtSearch/libDtSearch.la $(LIBS)' \
	python3 $(srcdir)/tests/check-dtsearch.py . $(top_builddir) $(top_srcdir) $(srcdir)
