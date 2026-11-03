MAINTAINERCLEANFILES = Makefile.in

.NOTPARALLEL:

noinst_PROGRAMS = eltdef

HELPTAG       = ..
HELPTAGSRC    = $(top_srcdir)/programs/dthelp/parser/pass1/helptag
HELPPARSERSRC = $(HELPTAG)/parser
HELPELTDEFSRC = $(HELPTAG)/eltdef
HELPDTDFILE   = $(HELPTAGSRC)/hptag.dtd
HELPUTILSRC   = $(HELPTAG)/util
HELPBUILDSRC  = $(HELPTAG)/build
HELPUTILLIB   = $(HELPTAG)/util/libutil.a
HELPIFFILE    = $(HELPTAGSRC)/help.if
HELPTSSFILE   = $(HELPTAGSRC)/htag.tss

eltdef_LDADD = $(HELPUTILLIB)

eltdef_CFLAGS = -DPASS1 -I../build -I$(top_srcdir)/programs/dthelp/parser/pass1/build -I../util -I$(top_srcdir)/programs/dthelp/parser/pass1/util -I../../include/util -I$(top_srcdir)/programs/dthelp/parser/include/util -I../../include/build -I$(top_srcdir)/programs/dthelp/parser/include/build \
		-I../../include/eltdef -I$(top_srcdir)/programs/dthelp/parser/include/eltdef

eltdef_SOURCES = eltdef.c eltutil.c entity.c scan.c

CCPRODS = case.c
ECPRODS = efile.c entfile.c pfile.c sfile.c stfile.c tfile.c

CHPRODS = context.h delim.h
EHPRODS = entity2.h globdec.h globdef.h if.h pval.h signon.h signonx.h

CPRODS  = $(CCPRODS) $(CHPRODS)
EPRODS  = $(ECPRODS) $(EHPRODS)

CTRASH  = delim.dat error
ETRASH  = estring.h error
TRASH   = delim.dat estring.h error

BUILT_SOURCES = $(CPRODS) $(EPRODS)
CLEANFILES = $(BUILT_SOURCES) $(TRASH) CDONE EDONE

$(CPRODS): CDONE

$(EPRODS): EDONE


EDONE: eltdef  $(HELPIFFILE) $(HELPTSSFILE)
	$(RM) $(EPRODS) $(ETRASH)
	./eltdef $(HELPIFFILE)
	touch EDONE

CDONE:  $(HELPUTILSRC)/context $(HELPTAGSRC)/delim.elt context.dat
	$(RM) $(CPRODS) $(CTRASH)
	$(CP) $(HELPTAGSRC)/delim.elt delim.dat  # appl.-specific delim.dat
	CDE_CONTEXT_DATA=$(srcdir)/context.dat $(HELPUTILSRC)/context
	touch CDONE

