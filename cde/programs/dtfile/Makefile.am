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
             tests/check-filemanip-safety.py tests/filemanip-safety.c \
             tests/check-collision-workflow.py tests/collision-workflow.c \
             tests/check-process-lifecycle.py tests/process-lifecycle.c \
             tests/check-directory-packets.py tests/directory-packets.c \
             tests/directory-packets-baseline.c \
             tests/check-directory-control-packets.py \
             tests/directory-control-packets.c \
             tests/check-file-dialog-safety.py tests/file-dialog-safety.c \
             tests/check-sync-controller.py tests/sync-controller.c \
             tests/check-sync-retry-ui.py SyncFeature.h.in \
             tests/check-stop-signal.py tests/check-stop-signal.c \
             tests/check-access-ids.py tests/check-access-ids.c \
             tests/check-storage-size-hardlinks.py tests/check-storage-size-hardlinks.c \
             tests/check-clipboard-undo.py \
             tests/check-dnd-drag-safety.py \
             tests/check-archive-safety.py org.freedesktop.FileManager1.service.cpp

check-local:
	CC="$(CC)" python3 $(srcdir)/tests/check-stop-signal.py
	CC="$(CC)" python3 $(srcdir)/tests/check-access-ids.py
	CC="$(CC)" python3 $(srcdir)/tests/check-storage-size-hardlinks.py
	CC="$(CC)" PKG_CONFIG="$(PKG_CONFIG)" python3 $(srcdir)/tests/check-clipboard-undo.py --module-source "$(abs_srcdir)"
	CC="$(CC)" PKG_CONFIG="$(PKG_CONFIG)" python3 $(srcdir)/tests/check-dnd-drag-safety.py --build-root "$(abs_top_builddir)" --cde-src "$(abs_top_srcdir)"
	CC="$(CC)" PKG_CONFIG="$(PKG_CONFIG)" python3 $(srcdir)/tests/check-archive-safety.py --module-source "$(abs_srcdir)"
	CC="$(CC)" python3 $(srcdir)/tests/check-trash-writer.py
	CC="$(CC)" python3 $(srcdir)/tests/check-trash-metadata.py
	CC="$(CC)" python3 $(srcdir)/tests/check-trash-info.py
	CC="$(CC)" python3 $(srcdir)/tests/check-restore-workflow.py
	CC="$(CC)" python3 $(srcdir)/tests/check-trash-permissions.py
	CC="$(CC)" python3 $(srcdir)/tests/check-copy-transfer.py
	CC="$(CC)" python3 $(srcdir)/tests/check-transfer-completion.py
	$(RM) -r "$(abs_builddir)/directory-packet-check"
	CC="$(CC)" python3 $(srcdir)/tests/check-directory-packets.py \
		--module-source "$(abs_srcdir)" \
		--work-dir "$(abs_builddir)/directory-packet-check"
	$(RM) -r "$(abs_builddir)/directory-control-check"
	CC="$(CC)" python3 $(srcdir)/tests/check-directory-control-packets.py \
		--module-source "$(abs_srcdir)" \
		--work-dir "$(abs_builddir)/directory-control-check"
	$(RM) -r "$(abs_builddir)/file-dialog-safety-check"
	CC="$(CC)" python3 $(srcdir)/tests/check-file-dialog-safety.py \
		--module-source "$(abs_srcdir)" \
		--work-dir "$(abs_builddir)/file-dialog-safety-check"
	$(RM) -r "$(abs_builddir)/filemanip-helper-check"
	CC="$(CC)" python3 $(srcdir)/tests/check-filemanip-safety.py \
		--module-source "$(abs_srcdir)" --helper-only \
		--work-dir "$(abs_builddir)/filemanip-helper-check"
	$(RM) -r "$(abs_builddir)/filemanip-path-check"
	CC="$(CC)" python3 $(srcdir)/tests/check-filemanip-safety.py \
		--module-source "$(abs_srcdir)" --paths-only \
		--work-dir "$(abs_builddir)/filemanip-path-check"
	$(RM) -r "$(abs_builddir)/filemanip-directory-check"
	CC="$(CC)" python3 $(srcdir)/tests/check-filemanip-safety.py \
		--module-source "$(abs_srcdir)" --directories-only \
		--work-dir "$(abs_builddir)/filemanip-directory-check"
	CC="$(CC)" python3 $(srcdir)/tests/check-operation-outcome.py
	$(RM) -r "$(abs_builddir)/collision-model-check"
	CC="$(CC)" python3 $(srcdir)/tests/check-collision-workflow.py \
		--module-source "$(abs_srcdir)" --model-only \
		--work-dir "$(abs_builddir)/collision-model-check"
	$(RM) -r "$(abs_builddir)/collision-helper-check"
	CC="$(CC)" python3 $(srcdir)/tests/check-collision-workflow.py \
		--module-source "$(abs_srcdir)" --helpers-only \
		--work-dir "$(abs_builddir)/collision-helper-check"
	$(RM) -r "$(abs_builddir)/process-lifecycle-check"
	CC="$(CC)" python3 $(srcdir)/tests/check-process-lifecycle.py \
		--module-source "$(abs_srcdir)" \
		--work-dir "$(abs_builddir)/process-lifecycle-check"
if HAVE_CK_CORE_FS
	CC="$(CC)" PKG_CONFIG="$(PKG_CONFIG)" python3 $(srcdir)/tests/check-sync-controller.py
endif

BUILT_SOURCES = dtfile.config dtfile_error SyncFeature.h org.freedesktop.FileManager1.service
CLEANFILES = dtfile.config dtfile_error org.freedesktop.FileManager1.service

dbusservicedir = $(datadir)/dbus-1/services
dbusservice_DATA = org.freedesktop.FileManager1.service

dist_config_DATA = dtfile.config

dist_bin_SCRIPTS = dtfile_error dterror.ds

dtfile_CPPFLAGS = -I./dtcopy -I$(srcdir)/dtcopy \
		-DSHAPE -D_ILS_MACROS -DSUN_PERF \
		-DCDE_INSTALLATION_TOP=\"$(CDE_INSTALLATION_TOP)\" \
		-DCDE_CONFIGURATION_TOP=\"$(CDE_CONFIGURATION_TOP)\" \
		$(shell $(PKG_CONFIG) --cflags dbus-1 libarchive 2>/dev/null)

nodist_dtfile_SOURCES = SyncFeature.h

dtfile_LDADD = dtcopy/sharedFuncs.o \
	dtcopy/fsrtns.o \
	$(DTCLIENTLIBS) $(TIRPCLIB) $(XTOOLLIB) \
	-ldbus-1 -larchive

if HAVE_CK_CORE_FS
dtfile_CPPFLAGS += $(CK_CORE_FS_CFLAGS)
dtfile_LDADD += $(CK_CORE_FS_LIBS)
endif

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
		 FileProcess.c FileProcess.h FileOpProtocol.h \
		 FileConflict.c FileConflict.h FileConflictWire.c FileConflictWire.h \
		 FileConflictDialog.c FileConflictDialog.h \
		 Filter.c        FilterP.c       Find.c          FindP.c \
		 Help.c          HelpCB.c        HelpP.c         IconWindow.c \
		 IconicPath.c    Shelf.c         Mounts.c        Main.c          Menu.c          MkDir.c \
		 ModAttr.c       ModAttrP.c      MultiView.c     OverWrite.c \
		 Prefs.c         PrefsP.c        SharedMsgs.c    SharedProcs.c \
		 ToolTalk.c      Trash.c         TrashMetadata.c TrashMetadata.h XdgTrashInfo.c XdgTrashInfo.h Utils.c fsDialog.c \
		 StorageSize.c   OpenWith.c      OpenWith.h \
		 Thumbnails.c    Thumbnails.h    Archive.c       Archive.h \
		 FileManagerService.c FileManagerService.h \
		 ClipboardUndo.c ClipboardUndo.h BrowserErgonomics.c BrowserErgonomics.h \
		 XdgTrashSync.c  XdgTrashSync.h  Devices.c       Devices.h

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

org.freedesktop.FileManager1.service: org.freedesktop.FileManager1.service.cpp
	$(RM) $@
	$(GENCPP) -P -DXCOMM=# $(SCRIPTFLAGS) $< > $@
