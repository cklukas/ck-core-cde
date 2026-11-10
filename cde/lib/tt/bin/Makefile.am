MAINTAINERCLEANFILES = Makefile.in

SUBDIRS = dbck shell scripts tt_type_comp ttauth ttdbserverd ttsession tttar \
	  tttrace

# The tests of the command line tools run the built ttrm, ttmv, ttcp,
# tttar and tt_type_comp on file names and options that hold blanks and shell
# metacharacters.
EXTRA_DIST = tests/check-tools.py

check-local:
	python3 $(srcdir)/tests/check-tools.py $(builddir)/shell $(builddir)/tt_type_comp \
		$(builddir)/ttsession/ttsession $(builddir)/tttar/tttar
