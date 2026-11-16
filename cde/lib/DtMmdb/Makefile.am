MAINTAINERCLEANFILES = Makefile.in

SUBDIRS = HardCopy \
	  StyleSheet \
	  api \
	  btree \
	  compression \
	  diskhash \
	  dstr \
	  dti_cc \
	  dti_excs \
	  dynhash \
	  hmphf \
	  index \
	  mgrs \
	  misc \
	  object \
	  oliasdb \
	  schema \
	  storage \
	  utility

lib_LTLIBRARIES = libDtMmdb.la

libDtMmdb_la_SOURCES =

libDtMmdb_la_LIBADD = HardCopy/libHardCopy.la \
		      StyleSheet/libStyleSheet.la \
		      api/libapi.la \
		      btree/libbtree.la \
		      compression/libcompression.la \
		      diskhash/libdiskhash.la \
		      dstr/libdstr.la \
		      dti_cc/libdti_cc.la \
		      dti_excs/libdti_excs.la \
		      dynhash/libdynhash.la \
		      hmphf/libhmphf.la \
		      index/libindex.la \
		      mgrs/libmgrs.la \
		      misc/libmisc.la \
		      object/libobject.la \
		      oliasdb/liboliasdb.la \
		      schema/libschema.la \
		      storage/libstorage.la \
		      utility/libutility.la

libDtMmdb_la_LDFLAGS = -version-info 2:1:0

EXTRA_DIST = tests/check-dtmmdb-hardening.py tests/check-dtmmdb-hardening.C

check-local:
	CXX="$(CXX)" python3 $(srcdir)/tests/check-dtmmdb-hardening.py
