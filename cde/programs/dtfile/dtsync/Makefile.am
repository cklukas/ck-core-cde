MAINTAINERCLEANFILES = Makefile.in

bin_PROGRAMS = dtfile_sync

dtfile_sync_CPPFLAGS = $(CK_CORE_FS_CFLAGS)
dtfile_sync_CFLAGS = -std=c11 $(PTHREAD_CFLAGS)
dtfile_sync_LDADD = $(CK_CORE_FS_LIBS) $(XTOOLLIB) $(PTHREAD_LIBS)
dtfile_sync_SOURCES = main.c SyncJob.c SyncJob.h SyncProfiles.c SyncProfiles.h SyncDates.h SyncPrivateStorage.h

EXTRA_DIST = dtfile_sync.msg
