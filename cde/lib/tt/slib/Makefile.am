MAINTAINERCLEANFILES = Makefile.in

noinst_LIBRARIES = libstt.a

libstt_a_CXXFLAGS = $(TIRPC_DEFINES) -I../lib \
	-I$(top_srcdir)/lib/tt/lib

libstt_a_SOURCES = mp_ce_attrs.C mp_observer.C mp_observer_utils.C  \
mp_otype.C              mp_otype_utils.C           mp_ptype.C           \
mp_ptype_utils.C        mp_rpc_implement.C         mp_rpc_server.C      \
mp_rpc_server_utils.C                                                   \
mp_s_file.C             mp_s_file_utils.C                               \
mp_s_message.C          mp_s_message_utils.C       mp_s_mp.C            \
mp_s_pattern.C          mp_s_pattern_utils.C       mp_s_procid.C        \
mp_s_procid_utils.C     mp_s_msg_context.C         mp_s_pat_context.C   \
mp_s_session.C          mp_s_session_prop.C        mp_s_session_utils.C \
mp_s_xdr_functions.C    mp_self_procid.C                                \
mp_signature.C          mp_signature_utils.C       mp_typedb.C          \
mp_typedb_utils.C       tt_isstrerror.C

# The decoding tests compile the production sources they exercise (string,
# session, authentication and request code of libtt and libstt.a) with the
# sanitizers, next to the test program, and link them in front of the built
# libraries, so they check the code that is shipped.
EXTRA_DIST = tests/check-decode.py tests/decode-test.C

tt_decode_libs = libstt.a $(top_builddir)/lib/tt/lib/libtt.la $(X_LIBS)

if LINUX
tt_decode_libs += $(TIRPCLIB) $(XTOOLLIB)
endif

if SOLARIS
tt_decode_libs += $(XTOOLLIB) -lintl -lsocket -lnsl
endif

if BSD
tt_decode_libs += $(XTOOLLIB)
endif

check-local: libstt.a
	TT_COMPILE='$(CXX) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) $(libstt_a_CXXFLAGS) $(AM_CXXFLAGS) $(CXXFLAGS)' \
	TT_LINK='$(LIBTOOL) --tag=CXX --mode=link $(CXXLD) $(AM_CXXFLAGS) $(CXXFLAGS) $(AM_LDFLAGS) $(LDFLAGS)' \
	TT_LIBS='$(tt_decode_libs) $(LIBS)' \
	python3 $(srcdir)/tests/check-decode.py $(srcdir) $(top_srcdir)/lib/tt
