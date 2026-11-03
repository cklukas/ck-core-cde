MAINTAINERCLEANFILES = Makefile.in

bin_PROGRAMS = dthelp_htag1

# Due to the wierd interdepencencies between helptag and parser,
# disable parallel builds here.  This is some really horrible code and
# design - not surprising since it dates to 1989.
.NOTPARALLEL:

HELPTAG       = ..
HELPTAGSRC    = $(top_srcdir)/programs/dthelp/parser/pass1/helptag
HELPPARSERSRC = $(HELPTAG)/parser
HELPELTDEFSRC = $(HELPTAG)/eltdef
HELPDTDFILE   = $(HELPTAGSRC)/hptag.dtd
HELPUTILSRC   = $(HELPTAG)/util
HELPBUILDSRC  = $(HELPTAG)/build
HELPUTILLIB   = $(HELPTAG)/util/libutil.a

AM_CPPFLAGS = -DPASS1 -I../eltdef -I$(top_srcdir)/programs/dthelp/parser/pass1/eltdef -I../helptag -I$(top_srcdir)/programs/dthelp/parser/pass1/helptag -I../util -I$(top_srcdir)/programs/dthelp/parser/pass1/util -I../build -I$(top_srcdir)/programs/dthelp/parser/pass1/build \
	      -I../../include/helptag -I$(top_srcdir)/programs/dthelp/parser/include/helptag -I../../include/util -I$(top_srcdir)/programs/dthelp/parser/include/util -I../../include/build -I$(top_srcdir)/programs/dthelp/parser/include/build \
	      -I../../include/eltdef -I$(top_srcdir)/programs/dthelp/parser/include/eltdef -I../../include/parser -I$(top_srcdir)/programs/dthelp/parser/include/parser

dthelp_htag1_SOURCES = parser.c actutil.c \
        closent.c code.c cyclent.c dcurelt.c \
        end.c entity.c \
        error.c getsign.c \
        input.c level.c lookent.c \
        minim.c optstrg.c param.c parent.c \
        parutil.c pi.c \
        scan.c setpar.c start.c string.c struct.c \
        text.c

APPLOBJECTS = ../helptag/libhelptag.a

dthelp_htag1_LDADD = $(APPLOBJECTS) $(HELPUTILLIB) $(DTCLIENTLIBS) $(XTOOLLIB)

CPRODS = case.c context.h delim.h
CTRASH = error delim.dat

BUILT_SOURCES = $(CPRODS)
CLEANFILES = $(BUILT_SOURCES) $(CTRASH) CDONE

$(CPRODS): CDONE

CDONE:  $(HELPUTILSRC)/context $(HELPTAGSRC)/delim.par context.dat
	$(RM) $(CPRODS) $(CTRASH)
	$(CP) $(HELPTAGSRC)/delim.par delim.dat  # appl.-specific delim.dat
	CDE_CONTEXT_DATA=$(srcdir)/context.dat $(HELPUTILSRC)/context
	touch CDONE

