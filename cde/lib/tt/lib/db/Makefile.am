MAINTAINERCLEANFILES = Makefile.in

noinst_LTLIBRARIES = libdb.la

libdb_la_CXXFLAGS = -I../../lib -I../../mini_isam \
	-I$(top_srcdir)/lib/tt/lib -I$(top_srcdir)/lib/tt/mini_isam

libdb_la_SOURCES = db_server_clnt.C           db_server_xdr.C \
        tt_db_access_utils.C                  tt_db_client.C \
        tt_db_client_utils.C                  tt_db_create_objid.C \
        tt_db_file.C                          tt_db_file_utils.C \
        tt_db_hostname_global_map_ref.C       tt_db_hostname_redirection_map.C \
        tt_db_key.C                           tt_db_key_utils.C \
        tt_db_network_path.C                  tt_db_object.C \
        tt_db_object_utils.C                  tt_db_objid_to_key.C \
        tt_db_property.C                      tt_db_property_utils.C \
        tt_db_rpc_message_routines.C          tt_db_rpc_routines.C
