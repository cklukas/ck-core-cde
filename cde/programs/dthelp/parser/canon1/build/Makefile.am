MAINTAINERCLEANFILES = Makefile.in

.NOTPARALLEL:

noinst_PROGRAMS = build

HELPTAG       = ..
HELPTAGSRC    = $(top_srcdir)/programs/dthelp/parser/canon1/helptag
HELPPARSERSRC = $(HELPTAG)/parser
HELPELTDEFSRC = $(HELPTAG)/eltdef
HELPDTDFILE   = $(HELPTAGSRC)/hptag.dtd
HELPUTILSRC   = $(HELPTAG)/util
HELPBUILDSRC  = $(HELPTAG)/build
HELPUTILLIB   = $(HELPTAG)/util/libutil.a

build_CFLAGS = -DCANON1 -I$(HELPUTILSRC) \
	-I$(top_srcdir)/programs/dthelp/parser/canon1/util \
	-I../../include/util -I$(top_srcdir)/programs/dthelp/parser/include/util \
	-I../../include/build -I$(top_srcdir)/programs/dthelp/parser/include/build

build_LDADD = ../util/libutil.a

build_SOURCES = build.c buildutl.c eltree.c except.c fsa.c out.c param.c \
		scan.c sref.c tree.c

CHPRODS = context.h delim.h
CCPRODS = case.c

BHPRODS = arc.h dtd.h entity.h
BCPRODS =

CTRASH  = error delim.dat
BTRASH  = error template

CPRODS  = $(CHPRODS) $(CCPRODS)
BPRODS  = $(BHPRODS) $(BCPRODS)
TRASH   = error delim.dat template

BUILT_SOURCES = $(CPRODS) $(BPRODS)
CLEANFILES = $(BUILT_SOURCES) $(TRASH) BDONE CDONE

# We want the commands that actually create BPRODS and CPRODS to only
# run once during a make so we gate them using these CDONE and BDONE
# targets.  This allows us to enable parallel building too.
$(BPRODS): BDONE

$(CPRODS): CDONE

# build the BRPODS
BDONE: ./build $(HELPDTDFILE)
	$(RM) $(BPRODS) $(BTRASH) && ./build -m < $(HELPDTDFILE) && touch BDONE


# build the CRPODS
CDONE: $(HELPUTILSRC)/context $(HELPTAGSRC)/delim.bld context.dat
	$(RM) $(CPRODS) $(CTRASH)
	$(CP) $(HELPTAGSRC)/delim.bld delim.dat # appl.-specific delim.dat
	CDE_CONTEXT_DATA=$(srcdir)/context.dat $(HELPUTILSRC)/context sparse
	touch CDONE

