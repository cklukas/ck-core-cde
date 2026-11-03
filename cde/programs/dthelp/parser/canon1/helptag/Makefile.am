MAINTAINERCLEANFILES = Makefile.in

# Due to the wierd interdepencencies between helptag and parser,
# disable parallel builds here.  This is some really horrible code and
# design - not surprising since it dates to 1989.
.NOTPARALLEL:

HELPTAG       = ..
HELPTAGSRC    = $(top_srcdir)/programs/dthelp/parser/canon1/helptag
HELPPARSERSRC = $(HELPTAG)/parser
HELPELTDEFSRC = $(HELPTAG)/eltdef
HELPDTDFILE   = $(HELPTAGSRC)/hptag.dtd
HELPUTILSRC   = $(HELPTAG)/util
HELPBUILDSRC  = $(HELPTAG)/build
HELPUTILLIB   = $(HELPTAG)/util/libutil.a
HELPIFFILE    = $(HELPTAGSRC)/help.if
HELPTSSFILE   = $(HELPTAGSRC)/htag.tss

XDONE:
	cd ../parser; $(MAKE) CDONE
	touch XDONE

BUILT_SOURCES = XDONE
CLEANFILES = $(BUILT_SOURCES)

AM_CPPFLAGS = -DCANON1 -I../parser -I$(top_srcdir)/programs/dthelp/parser/canon1/parser -I../eltdef -I$(top_srcdir)/programs/dthelp/parser/canon1/eltdef -I../../include/parser -I$(top_srcdir)/programs/dthelp/parser/include/parser -I../../include/util -I$(top_srcdir)/programs/dthelp/parser/include/util \
	      -I../../include/eltdef -I$(top_srcdir)/programs/dthelp/parser/include/eltdef -I../../include/helptag -I$(top_srcdir)/programs/dthelp/parser/include/helptag \
	      $(DT_INCDIR)

# needed by parser/
noinst_LIBRARIES = libhelptag.a
libhelptag_a_SOURCES = custom.c default.c help.c option.c \
		      special.c out.c xref.c global.c
