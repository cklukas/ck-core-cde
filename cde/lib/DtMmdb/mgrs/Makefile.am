MAINTAINERCLEANFILES = Makefile.in

noinst_LTLIBRARIES = libmgrs.la

libmgrs_la_CXXFLAGS = -DCOMPACTED_DISK_REP -DPORTABLE_DB -I.. \
	-I$(top_srcdir)/lib/DtMmdb

libmgrs_la_SOURCES = managers.C misc.C query_mgr.C template_mgr.C
