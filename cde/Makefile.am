docsdir = $(CDE_INSTALLATION_TOP)
docs_DATA = CONTRIBUTORS COPYING copyright HISTORY README.md

MAINTAINERCLEANFILES = Makefile.in \
		configure libtool \
		config.guess \
		config.sub \
		config.log \
		config.h.in \
		install-sh

SUBDIRS = util lib programs include

if BUILD_DOCS
SUBDIRS += doc
endif

# The examples are not built by this tree; the source of the drag and drop
# example that stores a dropped buffer is still checked.
EXTRA_DIST = examples/dtdnd/tests/check-drop-file-create.py \
             examples/dtdnd/tests/check-drop-file-create.c

check-local:
	CC="$(CC)" python3 $(srcdir)/examples/dtdnd/tests/check-drop-file-create.py

