MAINTAINERCLEANFILES = Makefile.in

SUBDIRS = dbck shell scripts tt_type_comp ttauth ttdbserverd ttsession tttar \
	  tttrace

# The tests of the command line tools run the built ttrm, ttmv, ttcp,
# tttar and tt_type_comp on file names and options that hold blanks and shell
# metacharacters.  The tests of the temporary FIFO of tttrace and the temporary
# directory of tttar compile the production code with the sanitizers.
EXTRA_DIST = tests/check-tools.py \
	     tests/check-tttrace-pipe-create.py tests/check-tttrace-pipe-create.cc \
	     tests/check-tttar-dir-create.py tests/check-tttar-dir-create.cc

check-local:
	python3 $(srcdir)/tests/check-tools.py $(builddir)/shell $(builddir)/tt_type_comp \
		$(builddir)/ttsession/ttsession $(builddir)/tttar/tttar
	CXX="$(CXX)" python3 $(srcdir)/tests/check-tttrace-pipe-create.py
	CXX="$(CXX)" python3 $(srcdir)/tests/check-tttar-dir-create.py
