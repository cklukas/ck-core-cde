configdir = $(CDE_INSTALLATION_TOP)/config/C

SUBDIRS = dtcopy
if HAVE_CK_CORE_FS
SUBDIRS += dtsync
endif
DIST_SUBDIRS = dtcopy dtsync

bin_PROGRAMS = dtfile

EXTRA_DIST = tests/check-trash-writer.py tests/check-trash-metadata.py \
             tests/trash-metadata.c tests/check-trash-info.py tests/trash-info.c \
             tests/check-restore-workflow.py tests/restore-workflow.c \
             tests/check-trash-permissions.py tests/trash-permissions.c \
             tests/check-copy-transfer.py tests/copy-transfer.c \
             tests/copy-paths.c tests/check-transfer-completion.py \
             tests/transfer-completion.c tests/check-operation-outcome.py \
             tests/check-sync-controller.py tests/sync-controller.c \
             tests/check-sync-retry-ui.py SyncFeature.h.in

check-local:
	CC="$(CC)" python3 $(srcdir)/tests/check-trash-writer.py
	CC="$(CC)" python3 $(srcdir)/tests/check-trash-metadata.py
	CC="$(CC)" python3 $(srcdir)/tests/check-trash-info.py
	CC="$(CC)" python3 $(srcdir)/tests/check-restore-workflow.py
	CC="$(CC)" python3 $(srcdir)/tests/check-trash-permissions.py
	CC="$(CC)" python3 $(srcdir)/tests/check-copy-transfer.py
	CC="$(CC)" python3 $(srcdir)/tests/check-transfer-completion.py
	CC="$(CC)" python3 $(srcdir)/tests/check-operation-outcome.py
if HAVE_CK_CORE_FS
	CC="$(CC)" PKG_CONFIG="$(PKG_CONFIG)" python3 $(srcdir)/tests/check-sync-controller.py
endif

BUILT_SOURCES = dtfile.config dtfile_error SyncFeature.h
CLEANFILES = dtfile.config dtfile_error

dist_config_DATA = dtfile.config

dist_bin_SCRIPTS = dtfile_error dterror.ds

dtfile_CPPFLAGS = -I./dtcopy -I$(srcdir)/dtcopy \
		-DSHAPE -D_ILS_MACROS -DSUN_PERF \
		-DCDE_INSTALLATION_TOP=\"$(CDE_INSTALLATION_TOP)\" \
		-DCDE_CONFIGURATION_TOP=\"$(CDE_CONFIGURATION_TOP)\"

nodist_dtfile_SOURCES = SyncFeature.h

dtfile_LDADD = dtcopy/sharedFuncs.o \
	dtcopy/fsrtns.o \
	$(DTCLIENTLIBS) $(TIRPCLIB) $(XTOOLLIB) 

if OPENBSD
dtfile_CPPFLAGS += -DFILE_MAP_OPTIMIZE
endif

if LINUX
dtfile_CPPFLAGS += -DFILE_MAP_OPTIMIZE
endif

if SOLARIS
dtfile_LDADD += -lintl -lresolv
endif

dtfile_SOURCES = ChangeDir.c     ChangeDirP.c    Command.c       Common.c \
		 Desktop.c       Directory.c     Encaps.c        File.c \
		 FileDialog.c    FileManip.c     FileMgr.c       FileOp.c \
		 Filter.c        FilterP.c       Find.c          FindP.c \
		 Help.c          HelpCB.c        HelpP.c         IconWindow.c \
		 IconicPath.c    Shelf.c         Mounts.c        Main.c          Menu.c          MkDir.c \
		 ModAttr.c       ModAttrP.c      MultiView.c     OverWrite.c \
		 Prefs.c         PrefsP.c        SharedMsgs.c    SharedProcs.c \
		 ToolTalk.c      Trash.c         TrashMetadata.c TrashMetadata.h XdgTrashInfo.c XdgTrashInfo.h Utils.c fsDialog.c \
		 StorageSize.c

# Mind the quoting here...
SCRIPTFLAGS = -DSHAPE -D_ILS_MACROS -DSUN_PERF \
	-DCDE_INSTALLATION_TOP="$(CDE_INSTALLATION_TOP)" \
	-DCDE_CONFIGURATION_TOP="$(CDE_CONFIGURATION_TOP)"

dtfile.config: dtfile.config.cpp
	$(RM) $@
	$(GENCPP) -P -DXCOMM=# $(SCRIPTFLAGS) $< > $@

dtfile_error: dtfile_error.cpp
	$(RM) $@
	$(GENCPP) -P -DXCOMM=# $(SCRIPTFLAGS) $<  > $@

dterror.ds: dterror.src
	$(RM) $@
	$(GENCPP) -P -DXCOMM=# $(SCRIPTFLAGS) $<  > $@
