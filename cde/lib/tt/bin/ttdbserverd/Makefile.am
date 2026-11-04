MAINTAINERCLEANFILES = Makefile.in

TT_VERSION = "CDE Version @CDE_VERSION_MAJOR@.@CDE_VERSION_MINOR@.@CDE_VERSION_MICRO@@CDE_VERSION_DEV@"
TT_VERSION_DEFINE = -DTT_VERSION_STRING=\"$(TT_VERSION)\"

bin_PROGRAMS = rpc.ttdbserver

rpc_ttdbserver_CXXFLAGS = -I../../slib -I../../lib -I../../mini_isam \
			-I$(top_srcdir)/lib/tt/slib \
			-I$(top_srcdir)/lib/tt/lib \
			-I$(top_srcdir)/lib/tt/mini_isam \
			$(TIRPC_DEFINES) $(TT_VERSION_DEFINE)

rpc_ttdbserver_LDADD = $(LIBTT) ${TIRPCLIB} $(X_LIBS) ../../slib/libstt.a \
		       ../../mini_isam/libisam.a

if LINUX
rpc_ttdbserver_LDADD += $(TIRPCLIB) $(XTOOLLIB)
endif

if SOLARIS
rpc_ttdbserver_LDADD += $(XTOOLLIB) -lintl -lsocket -lnsl
endif

if BSD
rpc_ttdbserver_LDADD += $(XTOOLLIB)
endif

rpc_ttdbserver_SOURCES = db_server_functions.C \
			 db_server_svc.C \
			 dm_access_cache.C \
			 dm_server.C \
			 tt_db_message_info_utils.C \
			 tt_db_msg_q_lock.C \
			 tt_db_msg_q_lock_entry_utils.C \
			 tt_db_partition_global_map_ref.C \
			 tt_db_partition_redirection_map.C \
			 tt_db_server_db.C \
			 tt_db_server_db_utils.C \
			 tt_isam_file.C \
			 tt_isam_file_utils.C \
			 tt_isam_key_descriptor.C \
			 tt_isam_key_descriptor_utils.C \
			 tt_isam_record.C \
			 tt_isam_record_utils.C

# The request-path tests compile the production db_server_svc.C, dm_server.C,
# db_server_functions.C and the XDR layer with the sanitizers, run the real
# dispatcher on requests they make up, and link with the built rpc.ttdbserver
# and libraries, so they check the code that is shipped.
EXTRA_DIST = tests/check-requests.py tests/requests-test.C

check-local:
	TTDB_COMPILE='$(CXX) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) $(rpc_ttdbserver_CXXFLAGS) $(AM_CXXFLAGS) $(CXXFLAGS)' \
	TTDB_LINK='$(LIBTOOL) --tag=CXX --mode=link $(CXXLD) $(AM_CXXFLAGS) $(CXXFLAGS) $(AM_LDFLAGS) $(LDFLAGS)' \
	TTDB_OBJECTS='$(rpc_ttdbserver_OBJECTS)' \
	TTDB_LIBS='$(rpc_ttdbserver_LDADD) $(LIBS)' \
	python3 $(srcdir)/tests/check-requests.py $(srcdir) $(top_srcdir)/lib/tt/lib/db
