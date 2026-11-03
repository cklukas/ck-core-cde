MAINTAINERCLEANFILES = Makefile.in

BUILT_SOURCES = common/ManLinks.sgm
CLEANFILES = $(BUILT_SOURCES)

$(BUILT_SOURCES):
	$(MKDIR_P) common
	$(RM) $@
	$(LN_S) $(abs_srcdir)/../guides/common/ManLinksMotif.sgm $@


