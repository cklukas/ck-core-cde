/*
 * Regression tests for the request path of rpc.ttdbserverd: the XDR layer
 * (lib/db/db_server_xdr.C), the dispatcher and the transaction log of
 * db_server_svc.C, and the NetISAM procedures of dm_server.C.
 *
 * The program includes the production db_server_svc.C, so that it can run
 * the real dispatcher, _tt_dbserver_prog_1(), on a transport of its own: a
 * request is a buffer of XDR the program makes up, the dispatcher decodes it
 * with the production XDR routines, calls the procedure and encodes the
 * reply into a buffer that the program reads back.  The rest of the server
 * is linked from the build.  Each case runs in its own process
 * (requests-test CASE), so that a crash in one case is reported for that
 * case alone; check-requests.py runs them all under the sanitizers.
 */
#define main ttdbserverd_main
#include "../db_server_svc.C"
#undef main

#include "tt_db_message_info_utils.h"
#include "tt_db_server_consts.h"
#include "db/tt_db_create_objid.h"
#include "db/tt_db_key.h"
#include "util/tt_file_system_entry.h"
#include "util/tt_path.h"
#include "util/tt_xdr_utils.h"

#include <new>
#include <stddef.h>
#include <string>
#include <vector>
#include <sys/resource.h>

#if defined(__SANITIZE_ADDRESS__)
/* from the AddressSanitizer runtime (sanitizer/allocator_interface.h) */
extern "C" size_t __sanitizer_get_current_allocated_bytes(void);
#endif

static int failures;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++; \
    } \
  } while (0)

/* ---- the transport ---------------------------------------------------------- */

struct Reply {
	bool		replied;	/* the dispatcher sent something */
	bool		accepted;	/* ... and did not reject the call */
	int		stat;		/* accept_stat, when it did accept */
	bool		results;	/* the results were encoded */
	std::string	body;		/* the XDR of the results */
};

static std::string	g_request;	/* what svc_getargs() decodes */
static Reply		g_reply;
static uid_t		g_uid;
static std::vector<gid_t> g_gids;	/* the first is the primary group */
static int		g_flavor = AUTH_UNIX;
static SVCXPRT		g_xprt;

static bool_t
fake_recv(SVCXPRT *, struct rpc_msg *)
{
	return FALSE;
}

static enum xprt_stat
fake_stat(SVCXPRT *)
{
	return XPRT_IDLE;
}

static bool_t
fake_getargs(SVCXPRT *, xdrproc_t proc, void *args)
{
	XDR xdrs;

	xdrmem_create(&xdrs, (char *)g_request.data(), g_request.size(),
		      XDR_DECODE);
	return (*proc)(&xdrs, args);
}

static bool_t
fake_reply(SVCXPRT *, struct rpc_msg *msg)
{
	g_reply.replied = true;
	g_reply.accepted = (msg->rm_reply.rp_stat == MSG_ACCEPTED);
	if (!g_reply.accepted) {
		return TRUE;
	}
	g_reply.stat = msg->acpted_rply.ar_stat;
	if (g_reply.stat != SUCCESS) {
		return TRUE;
	}
	static char out[1 << 17];
	XDR xdrs;

	xdrmem_create(&xdrs, out, sizeof(out), XDR_ENCODE);
	g_reply.results = ((*msg->acpted_rply.ar_results.proc)
			   (&xdrs, msg->acpted_rply.ar_results.where) != 0);
	g_reply.body.assign(out, xdr_getpos(&xdrs));
	return g_reply.results;
}

static bool_t
fake_freeargs(SVCXPRT *, xdrproc_t proc, void *args)
{
	XDR xdrs;

	memset(&xdrs, 0, sizeof(xdrs));
	xdrs.x_op = XDR_FREE;
	return (*proc)(&xdrs, args);
}

static void
fake_destroy(SVCXPRT *)
{
}

static const struct __rpc_svcxprt::xp_ops fake_ops = {
	fake_recv, fake_stat, fake_getargs, fake_reply, fake_freeargs,
	fake_destroy
};

/* Send one request to the dispatcher. */
static Reply
rpc(u_long proc, const std::string &request)
{
	struct authunix_parms	cred;
	std::vector<gid_t>	gids = g_gids;
	struct svc_req		rqst;

	memset(&cred, 0, sizeof(cred));
	cred.aup_uid = g_uid;
	cred.aup_gid = gids.empty() ? getgid() : gids[0];
	cred.aup_len = gids.size();
	cred.aup_gids = gids.empty() ? NULL : &gids[0];
	memset(&rqst, 0, sizeof(rqst));
	rqst.rq_prog = TT_DBSERVER_PROG;
	rqst.rq_vers = TT_DBSERVER_VERS;
	rqst.rq_proc = proc;
	rqst.rq_cred.oa_flavor = g_flavor;
	rqst.rq_clntcred = (char *)&cred;
	rqst.rq_xprt = &g_xprt;

	g_request = request;
	g_reply = Reply();
	_tt_dbserver_prog_1(&rqst, &g_xprt);
	return g_reply;
}

/* ---- requests and replies ---------------------------------------------------- */

/* The XDR of ARGS, as the client library would send it. */
static std::string
encode(xdrproc_t proc, void *args)
{
	static char	buf[1 << 20];
	XDR		xdrs;

	xdrmem_create(&xdrs, buf, sizeof(buf), XDR_ENCODE);
	if (!(*proc)(&xdrs, args)) {
		fprintf(stderr, "cannot encode a request\n");
		exit(2);
	}
	return std::string(buf, xdr_getpos(&xdrs));
}

/* Bytes of a request, made up by hand. */
struct Raw {
	std::string s;

	Raw &u32(unsigned v) {
		v = htonl(v);
		s.append((const char *)&v, 4);
		return *this;
	}
	/* an array of characters, as xdr_array(.., xdr_char) sends it */
	Raw &chars(const void *p, unsigned n) {
		u32(n);
		for (unsigned i = 0; i < n; i++) {
			u32(((const unsigned char *)p)[i]);
		}
		return *this;
	}
	Raw &string(const char *p) {
		unsigned n = strlen(p);
		u32(n);
		s.append(p, n);
		s.append((4 - n % 4) % 4, '\0');
		return *this;
	}
	/* opaque data of any content, as xdr_bytes() and xdr_string() send it */
	Raw &bytes(const std::string &d) {
		u32(d.size());
		s.append(d);
		s.append((4 - d.size() % 4) % 4, '\0');
		return *this;
	}
};

static int
reply_result(const Reply &r)
{
	_Tt_isam_results	res;
	XDR			xdrs;

	if (!r.replied || !r.accepted || r.stat != SUCCESS) {
		return INT_MIN;
	}
	memset(&res, 0, sizeof(res));
	xdrmem_create(&xdrs, (char *)r.body.data(), r.body.size(), XDR_DECODE);
	if (!xdr_Tt_isam_results(&xdrs, &res)) {
		return INT_MIN;
	}
	return res.result;
}

/* The request was understood, whatever its result. */
static bool
answered(const Reply &r)
{
	return r.replied && r.accepted && r.stat == SUCCESS && r.results;
}

/* ---- the server -------------------------------------------------------------- */

static std::string g_dir;
static std::string g_mount;	/* the mount point start_partition redirects to g_dir */

/*
 * The same set-up as main() of db_server_svc.C makes, without the network.
 * DIR is a new directory for the files of the case.
 */
static void
start_server(const char *name)
{
	const char	*base = getenv("TTDB_TEST_DIR");
	struct rlimit	rl;

	if (base == NULL) {
		fprintf(stderr, "TTDB_TEST_DIR is not set\n");
		exit(2);
	}
	g_dir = std::string(base) + "/" + name;
	mkdir(g_dir.c_str(), 0700);
	if (chdir(g_dir.c_str()) != 0) {
		perror("chdir");
		exit(2);
	}
	if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
		rl.rlim_cur = rl.rlim_max;
		setrlimit(RLIMIT_NOFILE, &rl);
	}

	errstr = fopen((g_dir + "/server.log").c_str(), "w");
	g_uid = getuid();
	g_gids.assign(1, getgid());
	g_flavor = AUTH_UNIX;
	g_xprt.xp_ops = &fake_ops;

	/* Partitions are the mount points of the host and the names in the
	   partition map.  The legacy tests keep their tables in the directory
	   of the case: it is a partition that maps to itself (a name that maps
	   to the same name would be a loop, which maps to nothing). */
	std::string map = g_dir + "/partition_map";
	FILE *f = fopen(map.c_str(), "w");
	if (f == NULL) {
		perror("partition_map");
		exit(2);
	}
	fprintf(f, "%s %s/.\n", g_dir.c_str(), g_dir.c_str());
	fclose(f);
	setenv("TT_PARTITION_MAP", map.c_str(), 1);

	_tt_global = new _Tt_global;
	db_pr_map = new _Tt_db_partition_redirection_map;
	_tt_auth_level = AUTH_UNIX;

	_tt_oid_keydesc.k_flags = ISNODUPS;
	_tt_oid_keydesc.k_nparts = 1;
	_tt_oid_keydesc.k_part[0].kp_start = 0;
	_tt_oid_keydesc.k_part[0].kp_leng = TT_DB_KEY_LENGTH;
	_tt_oid_keydesc.k_part[0].kp_type = BINTYPE;

	if (iscntl(ALLISFD, ISCNTL_FATAL, isamfatalerror) == -1) {
		exit(2);
	}
	umask(_TT_UMASK);
	_tt_oa_cache = new _Tt_oid_access_queue();
	_tt_la_cache = new _Tt_link_access_queue();
	for (int i = 0; i < _TT_MAX_ISFD; i++) {
		_tt_db_table[i].db_path = 0;
		_tt_db_table[i].opener_uid = (uid_t)-1;
		_tt_db_table[i].reftime = 0;
		_tt_db_table[i].server_has_open = 0;
		_tt_db_table[i].client_has_open = 0;
	}
}

static keydesc
oid_key(void)
{
	keydesc k;

	memset(&k, 0, sizeof(k));
	k.k_flags = ISNODUPS;
	k.k_nparts = 1;
	k.k_part[0].kp_start = 0;
	k.k_part[0].kp_leng = TT_DB_KEY_LENGTH;
	k.k_part[0].kp_type = BINTYPE;
	return k;
}

/* The path the client sends for a table of the directory of the case. */
static std::string
table_path(const char *old_name)
{
	return g_dir + "/TT_DB/" + old_name;
}

static const int fixed_mode = ISINOUT + ISFIXLEN + ISMANULOCK;
static const int var_mode = ISINOUT + ISVARLEN + ISMANULOCK;
static const int fixed_reclen = 64;

/*
 * isbuild a table of the directory: "oid_access" is a table of fixed records
 * of fixed_reclen bytes, "oid_prop" one of records of 80 to ISMAXRECLEN
 * bytes.  Returns the isfd the server gave, -1 if it refused.
 */
static int
build(const char *old_name)
{
	bool			fixed = (strcmp(old_name, "oid_prop") != 0);
	std::string		path = table_path(old_name);
	keydesc			key = oid_key();
	_Tt_isbuild_args	args;

	args.path = (char *)path.c_str();
	args.reclen = fixed ? fixed_reclen : ISMAXRECLEN;
	args.key = &key;
	args.mode = fixed ? fixed_mode : var_mode;
	args.isreclen = fixed ? fixed_reclen : TT_DB_KEY_LENGTH + MAX_PROP_LENGTH;
	return reply_result(rpc(_TT_ISBUILD,
				encode((xdrproc_t)xdr_Tt_isbuild_args, &args)));
}

/* isopen the table; returns the isfd, -1 if the server refused */
static int
open_table(const char *old_name, int mode)
{
	std::string		path = table_path(old_name);
	_Tt_isopen_args		args;

	args.path = (char *)path.c_str();
	args.mode = mode;
	return reply_result(rpc(_TT_ISOPEN,
				encode((xdrproc_t)xdr_Tt_isopen_args, &args)));
}

static int
close_table(int isfd)
{
	return reply_result(rpc(_TT_ISCLOSE,
				encode((xdrproc_t)xdr_int, &isfd)));
}

static std::string
record(char fill, unsigned len)
{
	std::string r(len, fill);

	/* the key is the first 16 bytes */
	for (int i = 0; i < TT_DB_KEY_LENGTH && (unsigned)i < len; i++) {
		r[i] = 'a' + i;
	}
	return r;
}

static int
write_record(int isfd, const std::string &rec)
{
	_Tt_iswrite_args args;

	args.isfd = isfd;
	args.rec.rec_len = rec.size();
	args.rec.rec_val = (char *)rec.data();
	return reply_result(rpc(_TT_ISWRITE,
				encode((xdrproc_t)xdr_Tt_iswrite_args, &args)));
}

/* isread by ISFIRST: the record, or an empty string if there is none */
static std::string
read_first(int isfd, bool *ok)
{
	_Tt_isread_args		args;
	_Tt_isread_results	res;
	XDR			xdrs;
	Reply			r;
	std::string		out;

	char key[TT_DB_KEY_LENGTH] = { 0 };

	args.isfd = isfd;
	args.rec.rec_len = sizeof(key);
	args.rec.rec_val = key;
	args.mode = ISFIRST;
	args.isrecnum = 0;
	r = rpc(_TT_ISREAD, encode((xdrproc_t)xdr_Tt_isread_args, &args));
	*ok = false;
	if (!answered(r)) {
		return out;
	}
	memset(&res, 0, sizeof(res));
	xdrmem_create(&xdrs, (char *)r.body.data(), r.body.size(), XDR_DECODE);
	if (!xdr_Tt_isread_results(&xdrs, &res)) {
		return out;
	}
	*ok = (res.isresult.result == 0);
	if (res.rec.rec_val != NULL) {
		out.assign(res.rec.rec_val, res.rec.rec_len);
	}
	xdr_free((xdrproc_t)xdr_Tt_isread_results, (char *)&res);
	return out;
}

/* ---- cases ------------------------------------------------------------------- */

/* Every procedure that takes an isfd refuses numbers that are no entry of
   the table of open files. */
static void
case_isfd_range(void)
{
	static const int bad[] = { -1, _TT_MAX_ISFD, _TT_MAX_ISFD + 1, 1000,
				   65536, 1 << 20, INT_MAX, INT_MIN };
	keydesc key = oid_key();
	std::string rec = record('x', 64);
	char oid[TT_DB_KEY_LENGTH];

	memset(oid, 'o', sizeof(oid));
	start_server("isfd-range");
	CHECK(build("oid_access") >= 0);

	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		int isfd = bad[i];
		Reply r;

		r = rpc(_TT_ISCLOSE, encode((xdrproc_t)xdr_int, &isfd));
		CHECK(reply_result(r) == -1);

		_Tt_isread_args ra = { isfd, { 0, NULL }, ISFIRST, 0 };
		r = rpc(_TT_ISREAD, encode((xdrproc_t)xdr_Tt_isread_args, &ra));
		CHECK(answered(r));
		CHECK(reply_result(r) == -1);

		_Tt_isrewrec_args wa = { isfd, 1,
					 { (u_int)rec.size(), (char *)rec.data() } };
		r = rpc(_TT_ISREWREC,
			encode((xdrproc_t)xdr_Tt_isrewrec_args, &wa));
		CHECK(reply_result(r) == -1);

		_Tt_isstart_args sa = { isfd, &key, TT_DB_KEY_LENGTH,
					{ (u_int)rec.size(), (char *)rec.data() },
					ISEQUAL };
		r = rpc(_TT_ISSTART, encode((xdrproc_t)xdr_Tt_isstart_args, &sa));
		CHECK(reply_result(r) == -1);

		_Tt_iswrite_args ia = { isfd,
					{ (u_int)rec.size(), (char *)rec.data() } };
		r = rpc(_TT_ISWRITE, encode((xdrproc_t)xdr_Tt_iswrite_args, &ia));
		CHECK(reply_result(r) == -1);

		_Tt_test_and_set_args ta = { isfd, &key, TT_DB_KEY_LENGTH,
					     { (u_int)rec.size(),
					       (char *)rec.data() } };
		r = rpc(_TT_TEST_AND_SET,
			encode((xdrproc_t)xdr_Tt_test_and_set_args, &ta));
		CHECK(reply_result(r) == -1);

		_Tt_isdelrec_args da = { isfd, 1,
					 { (u_int)rec.size(), (char *)rec.data() } };
		r = rpc(_TT_ISDELREC,
			encode((xdrproc_t)xdr_Tt_isdelrec_args, &da));
		CHECK(reply_result(r) == -1);

		_Tt_isaddindex_args xa = { isfd, &key };
		r = rpc(_TT_ISADDINDEX,
			encode((xdrproc_t)xdr_Tt_isaddindex_args, &xa));
		CHECK(reply_result(r) == -1);

		_Tt_iscntl_args ca = { isfd, 0, { 0, NULL } };
		r = rpc(_TT_ISCNTL, encode((xdrproc_t)xdr_Tt_iscntl_args, &ca));
		CHECK(reply_result(r) == -1);

		_Tt_transaction_args tr = { isfd, NULL };
		r = rpc(_TT_TRANSACTION,
			encode((xdrproc_t)xdr_Tt_transaction_args, &tr));
		CHECK(reply_result(r) == -1);

		/* the session procedures */
		_Tt_session_args ss;
		memset(&ss, 0, sizeof(ss));
		ss.isfd = isfd;
		ss.key = &key;
		ss.key_len = TT_DB_KEY_LENGTH;
		ss.oidkey.oidkey_len = sizeof(oid);
		ss.oidkey.oidkey_val = oid;
		char sid[] = "abcd";	/* xdr_char writes what it encodes */
		ss.session.session_len = 4;
		ss.session.session_val = sid;
		r = rpc(_TT_ADDSESSION,
			encode((xdrproc_t)xdr_Tt_session_args, &ss));
		CHECK(reply_result(r) == -1);
		r = rpc(_TT_DELSESSION,
			encode((xdrproc_t)xdr_Tt_session_args, &ss));
		CHECK(reply_result(r) == -1);

		_Tt_spec_props sp;
		memset(&sp, 0, sizeof(sp));
		sp.isfd = isfd;
		sp.key = &key;
		sp.key_len = TT_DB_KEY_LENGTH;
		r = rpc(_TT_GETTYPE, encode((xdrproc_t)xdr_Tt_spec_props, &sp));
		CHECK(answered(r));
	}
}

/* The isfd of an isbuild or isopen is always an entry of the table: the
   NetISAM library has more descriptors than the table, and the server
   treats those like a full NetISAM table. */
static void
case_isfd_table_full(void)
{
	static const int opens = _TT_MAX_ISFD + 4;
	std::vector<int> fds;
	int ok = 0;

	start_server("isfd-table-full");
	CHECK(build("oid_access") >= 0);

	for (int i = 0; i < opens; i++) {
		int isfd = open_table("oid_access", fixed_mode);

		CHECK(isfd == -1 || (isfd >= 0 && isfd < _TT_MAX_ISFD));
		if (isfd >= 0) {
			ok++;
		}
		fds.push_back(isfd);
	}
	/* the descriptors of the least recently used files are taken back
	   for new ones, as the server always did when NetISAM ran out */
	CHECK(ok == opens);
	for (size_t i = 0; i < fds.size(); i++) {
		if (fds[i] >= 0) {
			close_table(fds[i]);
		}
	}
}

/* Nothing the server could close to make room: the open fails. */
static void
case_isfd_nothing_to_close(void)
{
	int built = 0;

	start_server("isfd-nothing-to-close");

	/* files that are built, not opened, are not the server's to close:
	   one table in each of as many directories as there are entries, and
	   each directory is a partition of its own */
	FILE *map = fopen((g_dir + "/partition_map").c_str(), "a");
	CHECK(map != NULL);
	for (int i = 0; i < _TT_MAX_ISFD; i++) {
		fprintf(map, "%s/d%d %s/d%d/.\n", g_dir.c_str(), i, g_dir.c_str(), i);
	}
	fclose(map);
	for (int i = 0; i < _TT_MAX_ISFD; i++) {
		char dir[16];
		snprintf(dir, sizeof(dir), "d%d", i);
		mkdir((g_dir + "/" + dir).c_str(), 0700);
		mkdir((g_dir + "/" + dir + "/TT_DB").c_str(), 0700);

		std::string path = g_dir + "/" + dir + "/TT_DB/oid_access";
		keydesc key = oid_key();
		_Tt_isbuild_args args;

		args.path = (char *)path.c_str();
		args.reclen = fixed_reclen;
		args.key = &key;
		args.mode = fixed_mode;
		args.isreclen = fixed_reclen;
		if (reply_result(rpc(_TT_ISBUILD,
				     encode((xdrproc_t)xdr_Tt_isbuild_args,
					    &args))) >= 0) {
			built++;
		}
	}
	CHECK(built == _TT_MAX_ISFD);

	std::string mapped = g_dir + "/d0/TT_DB/access_table";
	CHECK(cached_isopen(mapped.c_str(), fixed_mode) == -1);
	CHECK(iserrno == ETOOMANY);
}

/* Opening what is not there does not use up an isfd. */
static void
case_isfd_failed_open(void)
{
	start_server("isfd-failed-open");
	CHECK(build("oid_access") == 0);

	std::string missing = g_dir + "/TT_DB/no-such-table";
	for (int i = 0; i < 4 * _TT_MAX_ISFD; i++) {
		CHECK(cached_isopen(missing.c_str(), fixed_mode) == -1);
	}

	/* the next file takes the isfd after the one that is in use */
	CHECK(open_table("oid_access", fixed_mode) == 1);
}

static std::string
transaction_chain(unsigned records, unsigned isfd)
{
	Raw r;

	r.u32(isfd);
	for (unsigned i = 0; i < records; i++) {
		r.u32(1);	/* there is a record */
		r.u32(1);	/* newp */
		r.u32(0);	/* recnum */
		r.u32(0);	/* no data */
	}
	r.u32(0);		/* the end of the list */
	return r.s;
}

/* A transaction of a great many records decodes, and frees, without a stack
   frame for each of them. */
static void
case_transaction_chain(void)
{
	static const unsigned records = 400000;
	std::string request = transaction_chain(records, 3);
	_Tt_transaction_args args;
	XDR xdrs;
	unsigned n = 0;

	memset(&args, 0, sizeof(args));
	xdrmem_create(&xdrs, (char *)request.data(), request.size(), XDR_DECODE);
	CHECK(xdr_Tt_transaction_args(&xdrs, &args));
	CHECK(args.isfd == 3);
	for (_Tt_trans_record *t = args.recs; t != NULL; t = t->next) {
		n++;
	}
	CHECK(n == records);
	xdr_free((xdrproc_t)xdr_Tt_transaction_args, (char *)&args);
	CHECK(args.recs == NULL);

	/* and the list is encoded as it was sent */
	_Tt_transaction_args back;
	memset(&back, 0, sizeof(back));
	std::string small = transaction_chain(3, 7);
	xdrmem_create(&xdrs, (char *)small.data(), small.size(), XDR_DECODE);
	CHECK(xdr_Tt_transaction_args(&xdrs, &back));
	CHECK(encode((xdrproc_t)xdr_Tt_transaction_args, &back) == small);
	xdr_free((xdrproc_t)xdr_Tt_transaction_args, (char *)&back);
}

/* A request that does not decode gives back what it had allocated. */
static void
case_decode_failure_frees(void)
{
#if defined(__SANITIZE_ADDRESS__)
	start_server("decode-failure-frees");

	/* a list that ends in the middle of its third record */
	Raw r;
	r.u32(3);
	for (int i = 0; i < 2; i++) {
		r.u32(1).u32(1).u32(0).chars("abcdefgh", 8);
	}
	r.u32(1).u32(1).u32(0);

	(void)rpc(_TT_TRANSACTION, r.s);	/* once, for what is kept */
	size_t before = __sanitizer_get_current_allocated_bytes();
	Reply reply = rpc(_TT_TRANSACTION, r.s);
	size_t after = __sanitizer_get_current_allocated_bytes();

	CHECK(reply.replied && reply.accepted && reply.stat == GARBAGE_ARGS);
	CHECK(after == before);
#else
	fprintf(stderr, "not built with AddressSanitizer: nothing to measure\n");
#endif
}

/* An error in a transaction does not close the isfd the client names, a
   descriptor of the server's, nor remove the log file of a former request. */
static void
case_transaction_stale_fd(void)
{
	int fds[2];
	std::string marker;

	start_server("transaction-stale-fd");
	CHECK(pipe(fds) == 0);
	CHECK(build("oid_access") >= 0);

	/* the log file a former request left in _tt_log_file */
	marker = g_dir + "/log_file";
	FILE *f = fopen(marker.c_str(), "w");
	CHECK(f != NULL);
	fclose(f);
	snprintf(_tt_log_file, MAXPATHLEN, "%s", marker.c_str());

	/* ... and a number that is no isfd of this client but a
	   descriptor of the server */
	_Tt_transaction_args args = { fds[0], NULL };
	Reply r = rpc(_TT_TRANSACTION,
		      encode((xdrproc_t)xdr_Tt_transaction_args, &args));

	CHECK(reply_result(r) == -1);
	CHECK(fcntl(fds[0], F_GETFD) != -1);
	CHECK(access(marker.c_str(), F_OK) == 0);
}

/* A log file with a record longer than the buffer it is read to. */
static void
case_transaction_log_bounds(void)
{
	start_server("transaction-log-bounds");
	CHECK(build("oid_prop") >= 0);
	std::string db = table_path("property_table");
	std::string log = g_dir + "/TT_DB/log_file";

	/* flag 0 (committed), the table, one record that claims 2 GB */
	std::string content;
	int flag = 0, newp = 1;
	long recnum = 0;
	unsigned len = 0x7fffffffu;
	content.append((char *)&flag, sizeof(flag));
	content.append(db.c_str(), db.size() + 1);
	content.append((char *)&newp, sizeof(newp));
	content.append((char *)&recnum, sizeof(recnum));
	content.append((char *)&len, sizeof(len));
	content.append(ISMAXRECLEN + 8000, 'A');
	FILE *f = fopen(log.c_str(), "w");
	CHECK(f != NULL);
	fwrite(content.data(), 1, content.size(), f);
	fclose(f);

	/* opening a table of the directory runs the log */
	int isfd = open_table("oid_prop", var_mode);
	CHECK(isfd >= 0);

	/* the table has no record the log would have written */
	bool ok;
	(void)read_first(isfd, &ok);
	CHECK(!ok);
}

/* The log file is not followed when it is a link. */
static void
case_transaction_log_symlink(void)
{
	start_server("transaction-log-symlink");
	CHECK(build("oid_prop") >= 0);
	std::string target = g_dir + "/failed-log";
	std::string log = g_dir + "/TT_DB/log_file";

	/* a log of a transaction that failed: the flag is 1 */
	int flag = 1;
	FILE *f = fopen(target.c_str(), "w");
	CHECK(f != NULL);
	fwrite(&flag, sizeof(flag), 1, f);
	fclose(f);
	CHECK(symlink(target.c_str(), log.c_str()) == 0);

	int isfd = open_table("oid_prop", var_mode);
	CHECK(isfd >= 0);

	/* a log that is read is removed after it; this one was not read */
	struct stat st;
	CHECK(lstat(log.c_str(), &st) == 0 && S_ISLNK(st.st_mode));
	CHECK(stat(target.c_str(), &st) == 0);
}

/* isbuild of a path that is no table, or without a key. */
static void
case_isbuild_bad_args(void)
{
	start_server("isbuild-bad-args");
	mkdir((g_dir + "/TT_DB").c_str(), 0700);

	std::string path = g_dir + "/TT_DB/not-a-table";
	keydesc key = oid_key();
	_Tt_isbuild_args args;

	args.path = (char *)path.c_str();
	args.reclen = fixed_reclen;
	args.key = &key;
	args.mode = fixed_mode;
	args.isreclen = fixed_reclen;
	Reply r = rpc(_TT_ISBUILD,
		      encode((xdrproc_t)xdr_Tt_isbuild_args, &args));
	CHECK(answered(r));
	CHECK(reply_result(r) == -1);

	path = table_path("oid_access");
	args.path = (char *)path.c_str();
	args.key = NULL;
	r = rpc(_TT_ISBUILD, encode((xdrproc_t)xdr_Tt_isbuild_args, &args));
	CHECK(answered(r));
	CHECK(reply_result(r) == -1);
}

/* isopen of paths that cannot be tables reports it. */
static void
case_isopen_bad_path(void)
{
	start_server("isopen-bad-path");
	CHECK(build("oid_access") >= 0);
	int isfd = open_table("oid_access", fixed_mode);
	CHECK(isfd >= 0);

	_Tt_isopen_args args;
	args.mode = fixed_mode;

	/* no directory at all */
	args.path = (char *)"TT_DB";
	Reply r = rpc(_TT_ISOPEN, encode((xdrproc_t)xdr_Tt_isopen_args, &args));
	CHECK(answered(r));
	CHECK(reply_result(r) == -1);

	/* a directory, but no table */
	std::string path = g_dir + "/TT_DB/not-a-table";
	args.path = (char *)path.c_str();
	r = rpc(_TT_ISOPEN, encode((xdrproc_t)xdr_Tt_isopen_args, &args));
	CHECK(answered(r));
	CHECK(reply_result(r) == -1);

	/* no TT_DB */
	path = g_dir + "/oid_access";
	args.path = (char *)path.c_str();
	r = rpc(_TT_ISOPEN, encode((xdrproc_t)xdr_Tt_isopen_args, &args));
	CHECK(answered(r));
	CHECK(reply_result(r) == -1);
}

/* A request without a key descriptor is refused by the procedures that
   need one. */
static void
case_null_key(void)
{
	std::string rec = record('x', 64);

	start_server("null-key");
	int isfd = build("oid_access");
	CHECK(isfd >= 0);

	_Tt_isstart_args sa = { isfd, NULL, TT_DB_KEY_LENGTH,
				{ (u_int)rec.size(), (char *)rec.data() },
				ISEQUAL };
	Reply r = rpc(_TT_ISSTART, encode((xdrproc_t)xdr_Tt_isstart_args, &sa));
	CHECK(answered(r));
	CHECK(reply_result(r) == -1);

	sa.mode = ISFIRST;
	r = rpc(_TT_ISSTART, encode((xdrproc_t)xdr_Tt_isstart_args, &sa));
	CHECK(answered(r));
	CHECK(reply_result(r) == -1);

	_Tt_isaddindex_args xa = { isfd, NULL };
	r = rpc(_TT_ISADDINDEX, encode((xdrproc_t)xdr_Tt_isaddindex_args, &xa));
	CHECK(answered(r));
	CHECK(reply_result(r) == -1);

	_Tt_test_and_set_args ta = { isfd, NULL, TT_DB_KEY_LENGTH,
				     { (u_int)rec.size(), (char *)rec.data() } };
	r = rpc(_TT_TEST_AND_SET,
		encode((xdrproc_t)xdr_Tt_test_and_set_args, &ta));
	CHECK(answered(r));
	CHECK(reply_result(r) == -1);

	/* ... and an empty record is no record at all: a read without a key
	   to look for, a start without one to begin with */
	keydesc key = oid_key();
	_Tt_isread_args ra = { isfd, { 0, NULL }, ISNEXT, 0 };
	r = rpc(_TT_ISREAD, encode((xdrproc_t)xdr_Tt_isread_args, &ra));
	CHECK(answered(r));
	_Tt_isstart_args ea = { isfd, &key, TT_DB_KEY_LENGTH, { 0, NULL }, ISEQUAL };
	r = rpc(_TT_ISSTART, encode((xdrproc_t)xdr_Tt_isstart_args, &ea));
	CHECK(answered(r));
	CHECK(reply_result(r) == -1);
}

/* The reply of iscntl is encoded from storage the encoder may write to:
   xdr_char() stores what it sends. */
static void
case_iscntl_reply(void)
{
	start_server("iscntl-reply");
	int isfd = build("oid_access");
	CHECK(isfd >= 0);

	_Tt_iscntl_args args = { isfd, 0, { 0, NULL } };
	Reply r = rpc(_TT_ISCNTL, encode((xdrproc_t)xdr_Tt_iscntl_args, &args));
	CHECK(answered(r));
	CHECK(reply_result(r) == 0);
}

/* A record shorter than the records of the file is written padded with
   zeros, not with what follows it in the server's memory. */
static void
case_short_record(void)
{
	start_server("short-record");
	int isfd = build("oid_access");
	CHECK(isfd >= 0);

	std::string rec = record('x', 20);	/* the file has 64 bytes */
	CHECK(write_record(isfd, rec) == 0);

	bool ok;
	std::string back = read_first(isfd, &ok);
	CHECK(ok);
	CHECK(back.size() == (size_t)fixed_reclen);
	if (back.size() == (size_t)fixed_reclen) {
		CHECK(back.compare(0, 20, rec) == 0);
		CHECK(back.find_first_not_of('\0', 20) == std::string::npos);
	}

	/* and the same for a record that is not there at all */
	std::string empty;
	CHECK(write_record(isfd, empty) == -1);
}

/* The oid access procedures work on a whole key. */
static void
case_oid_access_keys(void)
{
	start_server("oid-access-keys");
	static const u_long procs[] = { _TT_GETOIDACCESS, _TT_SETOIDUSER,
					_TT_SETOIDGROUP, _TT_SETOIDMODE };

	for (size_t i = 0; i < sizeof(procs) / sizeof(procs[0]); i++) {
		for (unsigned len = 0; len <= TT_DB_KEY_LENGTH; len += 5) {
			char key[TT_DB_KEY_LENGTH + 1];
			memset(key, 'k', sizeof(key));
			_Tt_oidaccess_args args;
			args.isfd = -1;
			args.key.key_len = len;
			args.key.key_val = len ? key : NULL;
			args.value = 3;

			Reply r = rpc(procs[i],
				      encode((xdrproc_t)xdr_Tt_oidaccess_args,
					     &args));
			CHECK(answered(r));
		}
	}
}

/* The session procedures check the key and the session they are given. */
static void
case_session_requests(void)
{
	start_server("session-requests");
	int isfd = build("oid_prop");
	CHECK(isfd >= 0);
	keydesc key = oid_key();
	char oid[TT_DB_KEY_LENGTH];
	memset(oid, 'o', sizeof(oid));
	char session[ISMAXRECLEN + 8000];
	const unsigned maxsession = ISMAXRECLEN - TT_DB_KEY_LENGTH - MAX_PROP_LENGTH;
	memset(session, 's', sizeof(session));

	static const u_long procs[] = { _TT_ADDSESSION, _TT_DELSESSION };
	for (size_t p = 0; p < 2; p++) {
		struct {
			int		key_len;
			keydesc		*key;
			unsigned	oids;
			unsigned	sessions;
		} t[] = {
			/* a session that is more than the value of a record */
			{ TT_DB_KEY_LENGTH, &key, sizeof(oid), maxsession + 1 },
			{ TT_DB_KEY_LENGTH, &key, sizeof(oid), sizeof(session) },
			{ -1, &key, sizeof(oid), 4 },
			{ 10000, &key, sizeof(oid), 4 },
			{ (int)sizeof(key) + 1, &key, sizeof(oid), 4 },
			{ TT_DB_KEY_LENGTH, NULL, sizeof(oid), 4 },
			{ TT_DB_KEY_LENGTH, &key, 0, 4 },
		};
		for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
			_Tt_session_args args;
			args.isfd = isfd;
			args.key = t[i].key;
			args.key_len = t[i].key_len;
			args.oidkey.oidkey_len = t[i].oids;
			args.oidkey.oidkey_val = t[i].oids ? oid : NULL;
			args.session.session_len = t[i].sessions;
			args.session.session_val = t[i].sessions ? session : NULL;

			std::string req;
			if (t[i].sessions <= maxsession) {
				req = encode((xdrproc_t)xdr_Tt_session_args, &args);
			} else {
				/* longer than the encoder takes: by hand */
				Raw raw;
				raw.u32(isfd).u32(1);
				raw.s += encode((xdrproc_t)xdr_keydesc, &key);
				raw.u32(TT_DB_KEY_LENGTH);
				raw.chars(oid, sizeof(oid));
				raw.chars(session, t[i].sessions);
				req = raw.s;
			}
			Reply r = rpc(procs[p], req);
			CHECK(r.replied && r.accepted);
			if (r.stat == SUCCESS) {
				CHECK(reply_result(r) == -1);
			} else {
				/* too long a session is refused when decoded */
				CHECK(t[i].sessions > maxsession);
			}
		}
	}

	/* gettype */
	for (int key_len = -1; key_len <= 2 * (int)sizeof(key); key_len += 9) {
		_Tt_spec_props sp;
		memset(&sp, 0, sizeof(sp));
		sp.isfd = isfd;
		sp.key = (key_len & 1) ? &key : NULL;
		sp.key_len = key_len;
		Reply r = rpc(_TT_GETTYPE,
			      encode((xdrproc_t)xdr_Tt_spec_props, &sp));
		CHECK(answered(r));
	}
}

/* A client cannot pass a NetISAM position to the server. */
static void
case_sessions_position(void)
{
	start_server("sessions-position");
	int isfd = build("oid_prop");
	CHECK(isfd >= 0);
	/* this server keeps the sessions in the tables it has open */
	CHECK(open_table("oid_prop", var_mode) >= 0);

	/* the first two bytes of a position are its length */
	char pos[TT_DB_KEY_LENGTH];
	memset(pos, 0xff, sizeof(pos));
	_tt_get_all_sessions_args args;
	args.oidkey.oidkey_len = sizeof(pos);
	args.oidkey.oidkey_val = pos;
	Reply r = rpc(TT_GET_ALL_SESSIONS,
		      encode((xdrproc_t)xdr_tt_get_all_sessions_args, &args));
	CHECK(answered(r));

	/* the answers are free'd, and asked for again, as often as asked */
	args.oidkey.oidkey_len = 0;
	args.oidkey.oidkey_val = NULL;
	for (int i = 0; i < 3; i++) {
		r = rpc(TT_GET_ALL_SESSIONS,
			encode((xdrproc_t)xdr_tt_get_all_sessions_args, &args));
		CHECK(answered(r));
	}
}

/* A failed request does not send the record of an earlier one. */
static void
case_reply_leftovers(void)
{
	start_server("reply-leftovers");
	int isfd = build("oid_access");
	CHECK(isfd >= 0);
	std::string rec = record('x', fixed_reclen);
	CHECK(write_record(isfd, rec) == 0);
	bool ok;
	CHECK(read_first(isfd, &ok).size() == (size_t)fixed_reclen);
	CHECK(ok);

	/* a read that fails: an isfd of the table that nobody has opened */
	_Tt_isread_args ra = { _TT_MAX_ISFD - 1, { 0, NULL }, ISFIRST, 0 };
	Reply r = rpc(_TT_ISREAD, encode((xdrproc_t)xdr_Tt_isread_args, &ra));
	CHECK(answered(r));
	_Tt_isread_results rr;
	XDR xdrs;
	memset(&rr, 0, sizeof(rr));
	xdrmem_create(&xdrs, (char *)r.body.data(), r.body.size(), XDR_DECODE);
	CHECK(xdr_Tt_isread_results(&xdrs, &rr));
	CHECK(rr.isresult.result == -1);
	CHECK(rr.rec.rec_len == 0);
	xdr_free((xdrproc_t)xdr_Tt_isread_results, (char *)&rr);

	/* a test_and_set that does */
	keydesc key = oid_key();
	_Tt_test_and_set_args ta = { _TT_MAX_ISFD - 1, &key, TT_DB_KEY_LENGTH,
				     { (u_int)rec.size(), (char *)rec.data() } };
	r = rpc(_TT_TEST_AND_SET,
		encode((xdrproc_t)xdr_Tt_test_and_set_args, &ta));
	CHECK(answered(r));
	_Tt_test_and_set_results tr;
	memset(&tr, 0, sizeof(tr));
	xdrmem_create(&xdrs, (char *)r.body.data(), r.body.size(), XDR_DECODE);
	CHECK(xdr_Tt_test_and_set_results(&xdrs, &tr));
	CHECK(tr.isresult.result == -1);
	CHECK(tr.rec.rec_len == 0);
	xdr_free((xdrproc_t)xdr_Tt_test_and_set_results, (char *)&tr);

	/* an isopen that fails says so, whatever the one before it said */
	CHECK(open_table("oid_access", fixed_mode) >= 0);
	_Tt_isopen_args oa;
	oa.mode = fixed_mode;
	oa.path = (char *)"TT_DB";
	r = rpc(_TT_ISOPEN, encode((xdrproc_t)xdr_Tt_isopen_args, &oa));
	CHECK(reply_result(r) == -1);
}

/*
 * The requests that work on files and objects keep them in the database of
 * the partition the file is in.  Redirect that partition to the directory of
 * the case (what a partition map does), and make a file in it.
 */
static std::string
start_partition(const char *name)
{
	start_server(name);

	std::string	dir = g_dir;
	char		*path = (char *)dir.c_str();
	_tt_file_partition_results *pr = _tt_get_file_partition_1(&path, NULL);
	std::string	map = g_dir + "/partition_map";
	FILE		*f = fopen(map.c_str(), "w");

	CHECK(f != NULL);
	fprintf(f, "%s %s\n", pr->partition, g_dir.c_str());
	fprintf(f, "%s %s/.\n", g_dir.c_str(), g_dir.c_str());
	fclose(f);
	g_mount = pr->partition;
	free(pr->partition);
	free(pr->network_path);
	setenv("TT_PARTITION_MAP", map.c_str(), 1);

	return "localhost:" + g_dir + "/file";
}

static std::string
create_file(const std::string &file)
{
	_tt_access	acc = { g_uid, getgid(), 0600 };
	_tt_property_list none = { 0, NULL };
	_tt_create_file_args cf = { (char *)file.c_str(), none, acc };
	Reply		r = rpc(TT_CREATE_FILE,
				encode((xdrproc_t)xdr_tt_create_file_args, &cf));
	_tt_db_cache_results res;
	XDR		xdrs;

	memset(&res, 0, sizeof(res));
	xdrmem_create(&xdrs, (char *)r.body.data(), r.body.size(), XDR_DECODE);
	CHECK(answered(r));
	CHECK(xdr_tt_db_cache_results(&xdrs, &res));
	if (res.results != TT_DB_OK) {
		fprintf(stderr, "create_file: the server says %d\n", res.results);
	}
	CHECK(res.results == TT_DB_OK);
	return file;
}

/* set a property of the file to one value; returns the result of the server */
static int
set_property(const std::string &file, const char *name,
	     const std::string &value, int *cache_level = NULL)
{
	_tt_access	acc = { g_uid, getgid(), 0600 };
	std::string	val = value;
	std::string	nm = name;
	_tt_property_value pv;
	_tt_set_file_prop_args sp;
	_tt_db_cache_results res;
	XDR		xdrs;

	pv.value.value_len = val.size();
	pv.value.value_val = &val[0];
	sp.file = (char *)file.c_str();
	sp.property.name = &nm[0];
	sp.property.values.values_len = 1;
	sp.property.values.values_val = &pv;
	sp.access = acc;
	Reply r = rpc(TT_SET_FILE_PROP,
		      encode((xdrproc_t)xdr_tt_set_file_prop_args, &sp));
	CHECK(answered(r));
	memset(&res, 0, sizeof(res));
	xdrmem_create(&xdrs, (char *)r.body.data(), r.body.size(), XDR_DECODE);
	CHECK(xdr_tt_db_cache_results(&xdrs, &res));
	if (cache_level != NULL) {
		*cache_level = res.cache_level;
	}
	return res.results;
}

/* The cache level of a file is a property clients can set: one that is
   shorter than an int is no cache level. */
static void
case_cache_level_value(void)
{
	std::string file = start_partition("cache-level-value");

	create_file(file);
	int level = -1;
	CHECK(set_property(file, "ordinary", std::string("value"), &level) ==
	      TT_DB_OK);
	CHECK(level >= 0);

	/* the next change of the file adds one to what it finds */
	CHECK(set_property(file, TT_DB_PROPS_CACHE_LEVEL_PROPERTY,
			   std::string("x")) != TT_DB_OK);
	CHECK(set_property(file, "ordinary", std::string("again")) ==
	      TT_DB_ERR_PROPS_CACHE_ERROR);

	/* ... and the highest level there is has no higher one to count to:
	   the change that sets it counts it up at once */
	int highest = INT_MAX;
	CHECK(set_property(file, TT_DB_PROPS_CACHE_LEVEL_PROPERTY,
			   std::string((char *)&highest, sizeof(highest))) ==
	      TT_DB_ERR_PROPS_CACHE_ERROR);
	CHECK(set_property(file, "ordinary", std::string("once more")) ==
	      TT_DB_ERR_PROPS_CACHE_ERROR);

	/* a caller that says it has that level is told there is nothing newer */
	_tt_access acc = { g_uid, getgid(), 0600 };
	_tt_get_file_objs_args go = { (char *)file.c_str(), acc, INT_MAX };
	Reply r = rpc(TT_GET_FILE_OBJS,
		      encode((xdrproc_t)xdr_tt_get_file_objs_args, &go));
	CHECK(answered(r));
	_tt_file_objs_results res;
	XDR xdrs;
	memset(&res, 0, sizeof(res));
	xdrmem_create(&xdrs, (char *)r.body.data(), r.body.size(), XDR_DECODE);
	CHECK(xdr_tt_file_objs_results(&xdrs, &res));
	CHECK(res.cache_level == INT_MAX && res.objids.values_len == 0);
	xdr_free((xdrproc_t)xdr_tt_file_objs_results, (char *)&res);
}

/* The message info a client queued is a property, which a client can set to
   anything: the parts of a message do not go beyond the size it says. */
static void
case_dequeue_sizes(void)
{
	std::string file = start_partition("dequeue-sizes");

	create_file(file);

	_Tt_db_message_info_ptr info = new _Tt_db_message_info;
	info->messageID = 7;
	info->numParts = 3;
	info->messageSize = 1;
	info->ptypes = new _Tt_string_list;
	info->ptypes->append(_Tt_string("ptype"));
	_Tt_xdr_size_stream sz;
	CHECK(info->xdr((XDR *)sz));
	std::string bytes(sz.getsize(), '\0');
	XDR xdrs;
	xdrmem_create(&xdrs, &bytes[0], bytes.size(), XDR_ENCODE);
	CHECK(info->xdr(&xdrs));
	CHECK(set_property(file, "_TT_QUEUED_MSGS", bytes) == TT_DB_OK);

	/* ... and three parts, each of which is far more than the message */
	for (int i = 0; i < 3; i++) {
		char name[32];
		snprintf(name, sizeof(name), "_TT_MSG_7_%d", i);
		CHECK(set_property(file, name, std::string(30000, 'A' + i)) ==
		      TT_DB_OK);
	}

	char ptype[] = "ptype";
	_tt_string one = { ptype };
	_tt_dequeue_msgs_args da = { (char *)file.c_str(), { 1, &one } };
	Reply r = rpc(TT_DEQUEUE_MESSAGES,
		      encode((xdrproc_t)xdr_tt_dequeue_msgs_args, &da));
	CHECK(answered(r));

	_tt_dequeue_msgs_results res;
	memset(&res, 0, sizeof(res));
	xdrmem_create(&xdrs, (char *)r.body.data(), r.body.size(), XDR_DECODE);
	CHECK(xdr_tt_dequeue_msgs_results(&xdrs, &res));
	CHECK(res.results == TT_DB_OK);
	/* the message is as long as its info says */
	CHECK(res.messages.messages_len == 1);
	if (res.messages.messages_len == 1) {
		CHECK(res.messages.messages_val[0].body.body_len == 1);
		CHECK(res.messages.messages_val[0].body.body_val[0] == 'A');
	}
	xdr_free((xdrproc_t)xdr_tt_dequeue_msgs_results, (char *)&res);

	/* a message of 64 bytes of which there is one: the rest is zeros,
	   not what the server had in memory */
	info->messageID = 8;
	info->numParts = 1;
	info->messageSize = 64;
	_Tt_xdr_size_stream sz2;
	CHECK(info->xdr((XDR *)sz2));
	std::string bytes2(sz2.getsize(), '\0');
	xdrmem_create(&xdrs, &bytes2[0], bytes2.size(), XDR_ENCODE);
	CHECK(info->xdr(&xdrs));
	CHECK(set_property(file, "_TT_QUEUED_MSGS", bytes2) == TT_DB_OK);
	CHECK(set_property(file, "_TT_MSG_8_0", std::string("Z")) == TT_DB_OK);
	r = rpc(TT_DEQUEUE_MESSAGES,
		encode((xdrproc_t)xdr_tt_dequeue_msgs_args, &da));
	CHECK(answered(r));
	memset(&res, 0, sizeof(res));
	xdrmem_create(&xdrs, (char *)r.body.data(), r.body.size(), XDR_DECODE);
	CHECK(xdr_tt_dequeue_msgs_results(&xdrs, &res));
	CHECK(res.messages.messages_len == 1);
	if (res.messages.messages_len == 1) {
		_tt_message &m = res.messages.messages_val[0];
		CHECK(m.body.body_len == 64);
		if (m.body.body_len == 64) {
			CHECK(m.body.body_val[0] == 'Z');
			CHECK(std::string(m.body.body_val + 1, 63) ==
			      std::string(63, '\0'));
		}
	}
	xdr_free((xdrproc_t)xdr_tt_dequeue_msgs_results, (char *)&res);

	/* and a value that is not a message info at all is dropped */
	CHECK(set_property(file, "_TT_QUEUED_MSGS", std::string("junk")) ==
	      TT_DB_OK);
	r = rpc(TT_DEQUEUE_MESSAGES,
		encode((xdrproc_t)xdr_tt_dequeue_msgs_args, &da));
	CHECK(answered(r));
}

/* A file or an object id with nothing in it belongs to no partition: the
   requests say so, as no mount point matches the path "". */
static void
case_empty_paths(void)
{
	start_server("empty-paths");
	char empty[] = "";
	char objid[] = "no-colons-at-all";
	_tt_access acc = { g_uid, getgid(), 0600 };
	_tt_property_list none = { 0, NULL };
	_tt_property prop;
	char name[] = "name";
	_tt_string_list ptypes = { 0, NULL };
	Reply r;

	memset(&prop, 0, sizeof(prop));
	prop.name = name;

	char *path = empty;
	r = rpc(TT_GET_FILE_PARTITION, encode((xdrproc_t)xdr_wrapstring, &path));
	CHECK(r.replied);
	r = rpc(_TT_MFS, encode((xdrproc_t)xdr_wrapstring, &path));
	CHECK(r.replied);

	_tt_create_file_args cf = { empty, none, acc };
	r = rpc(TT_CREATE_FILE, encode((xdrproc_t)xdr_tt_create_file_args, &cf));
	CHECK(r.replied);
	_tt_remove_file_args rf = { empty, acc };
	r = rpc(TT_REMOVE_FILE, encode((xdrproc_t)xdr_tt_remove_file_args, &rf));
	CHECK(r.replied);
	_tt_move_file_args mf = { empty, empty, acc };
	r = rpc(TT_MOVE_FILE, encode((xdrproc_t)xdr_tt_move_file_args, &mf));
	CHECK(r.replied);
	char other[] = "/a/file";
	mf.new_file = other;
	r = rpc(TT_MOVE_FILE, encode((xdrproc_t)xdr_tt_move_file_args, &mf));
	CHECK(r.replied);
	_tt_get_file_prop_args gp = { empty, name, acc, 0 };
	r = rpc(TT_GET_FILE_PROP,
		encode((xdrproc_t)xdr_tt_get_file_prop_args, &gp));
	CHECK(r.replied);
	_tt_set_file_prop_args sp = { empty, prop, acc };
	r = rpc(TT_SET_FILE_PROP,
		encode((xdrproc_t)xdr_tt_set_file_prop_args, &sp));
	CHECK(r.replied);
	_tt_is_file_in_db_args fi = { empty, acc };
	r = rpc(TT_IS_FILE_IN_DB,
		encode((xdrproc_t)xdr_tt_is_file_in_db_args, &fi));
	CHECK(r.replied);
	char body[] = "message";
	_tt_queue_msg_args qm = { empty, ptypes, { { sizeof(body), body } } };
	r = rpc(TT_QUEUE_MESSAGE, encode((xdrproc_t)xdr_tt_queue_msg_args, &qm));
	CHECK(r.replied);
	_tt_dequeue_msgs_args dm = { empty, ptypes };
	r = rpc(TT_DEQUEUE_MESSAGES,
		encode((xdrproc_t)xdr_tt_dequeue_msgs_args, &dm));
	CHECK(r.replied);

	/* an object id that has no partition gets no database in the
	   directory the server happens to be in */
	_tt_get_obj_type_args ot = { objid, acc };
	r = rpc(TT_GET_OBJ_TYPE, encode((xdrproc_t)xdr_tt_get_obj_type_args, &ot));
	CHECK(r.replied);
	_tt_is_obj_in_db_args oi = { objid, acc };
	r = rpc(TT_IS_OBJ_IN_DB, encode((xdrproc_t)xdr_tt_is_obj_in_db_args, &oi));
	CHECK(r.replied);
	CHECK(::access("TT_DB", F_OK) != 0);
}

static void
put(const std::string &path, const std::string &content)
{
	FILE *f = fopen(path.c_str(), "w");

	CHECK(f != NULL);
	if (f != NULL) {
		fwrite(content.data(), 1, content.size(), f);
		fclose(f);
	}
}

static std::string
make_objid(const std::string &partition)
{
	_Tt_db_key_ptr key = new _Tt_db_key;

	return (char *)_tt_db_create_objid(key, "NFS", "localhost",
					   _Tt_string(partition.c_str()));
}

/* ---- the XDR layer: strings that are not there, and the limits --------------- */

/* PROC encodes IN and decodes the result into OUT. */
template <class T>
static bool
roundtrip(bool_t (*proc)(XDR *, T *), T *in, T *out)
{
	static char	buf[1 << 16];
	XDR		xdrs;

	xdrmem_create(&xdrs, buf, sizeof(buf), XDR_ENCODE);
	if (!(*proc)(&xdrs, in)) {
		return false;
	}
	u_int len = xdr_getpos(&xdrs);
	memset(out, 0, sizeof(*out));
	xdrmem_create(&xdrs, buf, len, XDR_DECODE);
	return (*proc)(&xdrs, out) != 0;
}

static bool
is_empty(const char *s)
{
	return s != NULL && s[0] == '\0';
}

/* A string a reply has none of, such as the type of an object that is not
   there or the forward pointer of one that has none, is sent as the empty
   string, and so is one of an argument. */
static void
case_xdr_null_strings(void)
{
	_tt_is_obj_in_db_results ir = { NULL, TT_DB_ERR_NO_SUCH_OBJECT }, ir2;
	CHECK(roundtrip(xdr_tt_is_obj_in_db_results, &ir, &ir2));
	CHECK(is_empty(ir2.forward_pointer));
	CHECK(ir2.results == TT_DB_ERR_NO_SUCH_OBJECT);
	xdr_free((xdrproc_t)xdr_tt_is_obj_in_db_results, (char *)&ir2);

	_tt_obj_type_results tr = { NULL, TT_DB_ERR_NO_SUCH_OBJECT }, tr2;
	CHECK(roundtrip(xdr_tt_obj_type_results, &tr, &tr2));
	CHECK(is_empty(tr2.otype));
	CHECK(tr2.results == TT_DB_ERR_NO_SUCH_OBJECT);
	xdr_free((xdrproc_t)xdr_tt_obj_type_results, (char *)&tr2);

	_tt_obj_file_results fr = { NULL, TT_DB_ERR_NO_SUCH_OBJECT }, fr2;
	CHECK(roundtrip(xdr_tt_obj_file_results, &fr, &fr2));
	CHECK(is_empty(fr2.file));
	xdr_free((xdrproc_t)xdr_tt_obj_file_results, (char *)&fr2);

	/* a property that is not there has no name and no values */
	_tt_obj_prop_results pr, pr2;
	memset(&pr, 0, sizeof(pr));
	pr.cache_level = 3;
	pr.results = TT_DB_ERR_NO_SUCH_PROPERTY;
	CHECK(roundtrip(xdr_tt_obj_prop_results, &pr, &pr2));
	CHECK(is_empty(pr2.property.name) && pr2.property.values.values_len == 0);
	CHECK(is_empty(pr2.file));
	CHECK(pr2.cache_level == 3 && pr2.results == TT_DB_ERR_NO_SUCH_PROPERTY);
	xdr_free((xdrproc_t)xdr_tt_obj_prop_results, (char *)&pr2);

	_tt_file_prop_results gr, gr2;
	memset(&gr, 0, sizeof(gr));
	CHECK(roundtrip(xdr_tt_file_prop_results, &gr, &gr2));
	CHECK(is_empty(gr2.property.name));
	xdr_free((xdrproc_t)xdr_tt_file_prop_results, (char *)&gr2);

	/* ... and a client that removes an object with no forward pointer */
	_tt_access acc = { getuid(), getgid(), 0600 };
	char objid[] = "key:NFS:host:/";
	_tt_remove_obj_args ra = { objid, NULL, acc }, ra2;
	CHECK(roundtrip(xdr_tt_remove_obj_args, &ra, &ra2));
	CHECK(is_empty(ra2.forward_pointer));
	CHECK(strcmp(ra2.objid, objid) == 0);
	xdr_free((xdrproc_t)xdr_tt_remove_obj_args, (char *)&ra2);

	_tt_file_partition_results pa = { NULL, NULL, TT_DB_ERR_ILLEGAL_FILE }, pa2;
	CHECK(roundtrip(xdr_tt_file_partition_results, &pa, &pa2));
	CHECK(is_empty(pa2.partition) && is_empty(pa2.network_path));
	xdr_free((xdrproc_t)xdr_tt_file_partition_results, (char *)&pa2);

	/* the requests that have such a reply are answered */
	std::string file = start_partition("xdr-null-strings");
	create_file(file);

	_tt_access acc2 = { g_uid, getgid(), 0600 };
	char name[] = "no such property";
	_tt_get_file_prop_args gp = { (char *)file.c_str(), name, acc2, -1 };
	Reply r = rpc(TT_GET_FILE_PROP,
		      encode((xdrproc_t)xdr_tt_get_file_prop_args, &gp));
	CHECK(answered(r));
	_tt_file_prop_results res;
	XDR xdrs;
	memset(&res, 0, sizeof(res));
	xdrmem_create(&xdrs, (char *)r.body.data(), r.body.size(), XDR_DECODE);
	CHECK(xdr_tt_file_prop_results(&xdrs, &res));
	CHECK(res.results == TT_DB_ERR_NO_SUCH_PROPERTY);
	CHECK(is_empty(res.property.name));
	xdr_free((xdrproc_t)xdr_tt_file_prop_results, (char *)&res);

	char absent[] = "key:NFS:localhost:/nowhere";
	_tt_get_obj_type_args ot = { absent, acc2 };
	r = rpc(TT_GET_OBJ_TYPE, encode((xdrproc_t)xdr_tt_get_obj_type_args, &ot));
	CHECK(answered(r));
	_tt_is_obj_in_db_args oi = { absent, acc2 };
	r = rpc(TT_IS_OBJ_IN_DB, encode((xdrproc_t)xdr_tt_is_obj_in_db_args, &oi));
	CHECK(answered(r));
}

/* DECODE decodes BYTES into a T. */
template <class T>
static bool
decodes(bool_t (*proc)(XDR *, T *), const std::string &bytes)
{
	T	obj;
	XDR	xdrs;

	memset(&obj, 0, sizeof(obj));
	xdrmem_create(&xdrs, (char *)bytes.data(), bytes.size(), XDR_DECODE);
	bool ok = (*proc)(&xdrs, &obj) != 0;
	xdr_free((xdrproc_t)proc, (char *)&obj);
	return ok;
}

/* the string of N characters, as the protocol sends it */
static std::string
text(size_t n)
{
	return Raw().bytes(std::string(n, 'x')).s;
}

/* N empty strings, empty byte arrays, empty properties and so on */
static std::string
repeat(size_t n, const std::string &one)
{
	std::string out;

	out.reserve(n * one.size());
	for (size_t i = 0; i < n; i++) {
		out += one;
	}
	return out;
}

/*
 * Every string, byte array and list of the protocol has a limit, which is
 * served and which is the last one: no longer string is decoded, however
 * much of it is there.
 */
static void
case_xdr_limits(void)
{
	const std::string none = Raw().u32(0).s;	/* "", a list of none */

	/* strings */
	CHECK(decodes<char *>(xdr_tt_file_name, text(TT_DB_LIMIT_FILE)));
	CHECK(!decodes<char *>(xdr_tt_file_name, text(TT_DB_LIMIT_FILE + 1)));
	CHECK(decodes<char *>(xdr_tt_objid_name, text(TT_DB_LIMIT_OBJID)));
	CHECK(!decodes<char *>(xdr_tt_objid_name, text(TT_DB_LIMIT_OBJID + 1)));
	CHECK(decodes<char *>(xdr_Tt_path, text(PATH_LENGTH)));
	CHECK(!decodes<char *>(xdr_Tt_path, text(PATH_LENGTH + 1)));
	CHECK(decodes<_tt_string>(xdr_tt_ptype, text(TT_DB_LIMIT_TYPE)));
	CHECK(!decodes<_tt_string>(xdr_tt_ptype, text(TT_DB_LIMIT_TYPE + 1)));
	CHECK(decodes<_tt_string>(xdr_tt_objid, text(TT_DB_LIMIT_OBJID)));
	CHECK(!decodes<_tt_string>(xdr_tt_objid, text(TT_DB_LIMIT_OBJID + 1)));
	CHECK(decodes<_tt_string>(xdr_tt_session, text(TT_DB_LIMIT_SESSION)));
	CHECK(!decodes<_tt_string>(xdr_tt_session, text(TT_DB_LIMIT_SESSION + 1)));

	/* the name of a property, the type of an object, the partition of a file */
	CHECK(decodes<_tt_property>(xdr_tt_property,
				    text(TT_DB_LIMIT_PROPERTY_NAME) + none));
	CHECK(!decodes<_tt_property>(xdr_tt_property,
				     text(TT_DB_LIMIT_PROPERTY_NAME + 1) + none));
	CHECK(decodes<_tt_obj_type_results>(xdr_tt_obj_type_results,
					    text(TT_DB_LIMIT_TYPE) + none));
	CHECK(!decodes<_tt_obj_type_results>(xdr_tt_obj_type_results,
					     text(TT_DB_LIMIT_TYPE + 1) + none));
	CHECK(decodes<_tt_file_partition_results>(xdr_tt_file_partition_results,
						  text(TT_DB_LIMIT_PATH) + none + none));
	CHECK(!decodes<_tt_file_partition_results>(xdr_tt_file_partition_results,
						   text(TT_DB_LIMIT_PATH + 1) + none + none));
	CHECK(decodes<_tt_file_partition_results>(xdr_tt_file_partition_results,
						  none + text(TT_DB_LIMIT_FILE) + none));
	CHECK(!decodes<_tt_file_partition_results>(xdr_tt_file_partition_results,
						   none + text(TT_DB_LIMIT_FILE + 1) + none));
	CHECK(decodes<_tt_delete_session_args>(xdr_tt_delete_session_args,
					       text(TT_DB_LIMIT_SESSION)));
	CHECK(!decodes<_tt_delete_session_args>(xdr_tt_delete_session_args,
						text(TT_DB_LIMIT_SESSION + 1)));

	/* byte arrays: the value of a property, the body of a message */
	CHECK(decodes<_tt_property_value>(xdr_tt_property_value,
					  text(TT_DB_LIMIT_PROPERTY_VALUE)));
	CHECK(!decodes<_tt_property_value>(xdr_tt_property_value,
					   text(TT_DB_LIMIT_PROPERTY_VALUE + 1)));
	CHECK(decodes<_tt_message>(xdr_tt_message, text(TT_DB_LIMIT_MESSAGE)));
	CHECK(!decodes<_tt_message>(xdr_tt_message, text(TT_DB_LIMIT_MESSAGE + 1)));

	/* lists: of properties, of the values of a property, of ptypes, of the
	   objects of a file, of messages */
	const std::string empty_property = none + none;	/* no name, no values */
	const std::string tail = none + none;		/* cache level, results */

	CHECK(decodes<_tt_file_props_results>(
		      xdr_tt_file_props_results,
		      Raw().u32(TT_DB_LIMIT_PROPERTIES).s +
		      repeat(TT_DB_LIMIT_PROPERTIES, empty_property) + tail));
	CHECK(!decodes<_tt_file_props_results>(
		      xdr_tt_file_props_results,
		      Raw().u32(TT_DB_LIMIT_PROPERTIES + 1).s +
		      repeat(TT_DB_LIMIT_PROPERTIES + 1, empty_property) + tail));
	CHECK(decodes<_tt_property>(
		      xdr_tt_property,
		      none + Raw().u32(TT_DB_LIMIT_PROPERTY_VALUES).s +
		      repeat(TT_DB_LIMIT_PROPERTY_VALUES, none)));
	CHECK(!decodes<_tt_property>(
		      xdr_tt_property,
		      none + Raw().u32(TT_DB_LIMIT_PROPERTY_VALUES + 1).s +
		      repeat(TT_DB_LIMIT_PROPERTY_VALUES + 1, none)));
	CHECK(decodes<_tt_dequeue_msgs_args>(
		      xdr_tt_dequeue_msgs_args,
		      none + Raw().u32(TT_DB_LIMIT_PTYPES).s +
		      repeat(TT_DB_LIMIT_PTYPES, none)));
	CHECK(!decodes<_tt_dequeue_msgs_args>(
		      xdr_tt_dequeue_msgs_args,
		      none + Raw().u32(TT_DB_LIMIT_PTYPES + 1).s +
		      repeat(TT_DB_LIMIT_PTYPES + 1, none)));
	CHECK(decodes<_tt_queue_msg_args>(
		      xdr_tt_queue_msg_args,
		      none + Raw().u32(TT_DB_LIMIT_PTYPES).s +
		      repeat(TT_DB_LIMIT_PTYPES, none) + none));
	CHECK(!decodes<_tt_queue_msg_args>(
		      xdr_tt_queue_msg_args,
		      none + Raw().u32(TT_DB_LIMIT_PTYPES + 1).s +
		      repeat(TT_DB_LIMIT_PTYPES + 1, none) + none));
	CHECK(decodes<_tt_file_objs_results>(
		      xdr_tt_file_objs_results,
		      Raw().u32(TT_DB_LIMIT_OBJECTS).s +
		      repeat(TT_DB_LIMIT_OBJECTS, none) + tail));
	CHECK(!decodes<_tt_file_objs_results>(
		      xdr_tt_file_objs_results,
		      Raw().u32(TT_DB_LIMIT_OBJECTS + 1).s +
		      repeat(TT_DB_LIMIT_OBJECTS + 1, none) + tail));
	CHECK(decodes<_tt_dequeue_msgs_results>(
		      xdr_tt_dequeue_msgs_results,
		      Raw().u32(TT_DB_LIMIT_MESSAGES).s +
		      repeat(TT_DB_LIMIT_MESSAGES, none) + none));
	CHECK(!decodes<_tt_dequeue_msgs_results>(
		      xdr_tt_dequeue_msgs_results,
		      Raw().u32(TT_DB_LIMIT_MESSAGES + 1).s +
		      repeat(TT_DB_LIMIT_MESSAGES + 1, none) + none));

	/* the properties of a spec, and the name of one (a character a word) */
	const std::string no_pointer = none;	/* a null keydesc */
	const std::string spec_head = Raw().u32(3).s + no_pointer + Raw().u32(80).s +
		none;				/* isfd, key, key_len, oidkey */
	const std::string spec_prop = Raw().u32(0).u32(0).u32(0).s;
	const std::string spec_tail = Raw().u32(0).u32(0).s;
	CHECK(decodes<_Tt_spec_props>(
		      xdr_Tt_spec_props,
		      spec_head + Raw().u32(TT_DB_LIMIT_SPEC_PROPERTIES).s +
		      repeat(TT_DB_LIMIT_SPEC_PROPERTIES, spec_prop) + spec_tail));
	CHECK(!decodes<_Tt_spec_props>(
		      xdr_Tt_spec_props,
		      spec_head + Raw().u32(TT_DB_LIMIT_SPEC_PROPERTIES + 1).s +
		      repeat(TT_DB_LIMIT_SPEC_PROPERTIES + 1, spec_prop) + spec_tail));
	CHECK(decodes<_Tt_prop>(
		      xdr_Tt_prop,
		      Raw().u32(MAX_PROP_LENGTH).s +
		      repeat(MAX_PROP_LENGTH, Raw().u32('p').s) + Raw().u32(0).u32(0).s));
	CHECK(!decodes<_Tt_prop>(
		      xdr_Tt_prop,
		      Raw().u32(MAX_PROP_LENGTH + 1).s +
		      repeat(MAX_PROP_LENGTH + 1, Raw().u32('p').s) + Raw().u32(0).u32(0).s));
}

/* The requests of the new interface as they are sent, one of each. */
struct Sound {
	u_long		proc;
	std::string	request;
};

static Raw &
access_of(Raw &r)
{
	return r.u32(getuid()).u32(getgid()).u32(0600);
}

static Raw &
one_property(Raw &r)
{
	return r.string("property").u32(1).bytes("value");
}

static Raw &
properties_of(Raw &r)
{
	return one_property(r.u32(1));
}

static std::vector<Sound>
sound_requests(const std::string &file, const std::string &objid)
{
	const char *f = file.c_str(), *o = objid.c_str();
	std::vector<Sound> v;

#define SOUND(proc, build) \
	do { Raw r; build; Sound s = { proc, r.s }; v.push_back(s); } while (0)
	SOUND(_TT_MIN_AUTH_LEVEL, r.string(""));
	SOUND(_TT_ISERASE, r.string("/no/such/directory/TT_DB/oid_prop"));
	SOUND(_TT_MFS, r.string(f));
	SOUND(TT_GET_FILE_PARTITION, r.string(f));
	SOUND(TT_CREATE_FILE, access_of(properties_of(r.string(f))));
	SOUND(TT_CREATE_OBJ, access_of(properties_of(r.string(f).string(o).string("otype"))));
	SOUND(TT_REMOVE_FILE, access_of(r.string(f)));
	SOUND(TT_REMOVE_OBJ, access_of(r.string(o).string("forward")));
	SOUND(TT_MOVE_FILE, access_of(r.string(f).string(f)));
	SOUND(TT_SET_FILE_PROPS, access_of(properties_of(r.string(f))));
	SOUND(TT_SET_FILE_PROP, access_of(one_property(r.string(f))));
	SOUND(TT_ADD_FILE_PROP, access_of(one_property(r.string(f)).u32(0)));
	SOUND(TT_DELETE_FILE_PROP, access_of(one_property(r.string(f))));
	SOUND(TT_GET_FILE_PROP, access_of(r.string(f).string("property")).u32(0));
	SOUND(TT_GET_FILE_PROPS, access_of(r.string(f)).u32(0));
	SOUND(TT_GET_FILE_OBJS, access_of(r.string(f)).u32(0));
	SOUND(TT_SET_FILE_ACCESS, access_of(access_of(r.string(f))));
	SOUND(TT_GET_FILE_ACCESS, access_of(r.string(f)));
	SOUND(TT_SET_OBJ_PROPS, access_of(properties_of(r.string(o))).u32(0));
	SOUND(TT_SET_OBJ_PROP, access_of(one_property(r.string(o))).u32(0));
	SOUND(TT_ADD_OBJ_PROP, access_of(one_property(r.string(o)).u32(0)).u32(0));
	SOUND(TT_DELETE_OBJ_PROP, access_of(one_property(r.string(o))).u32(0));
	SOUND(TT_GET_OBJ_PROP, access_of(r.string(o).string("property")).u32(0));
	SOUND(TT_GET_OBJ_PROPS, access_of(r.string(o)).u32(0));
	SOUND(TT_SET_OBJ_TYPE, access_of(r.string(o).string("otype")));
	SOUND(TT_GET_OBJ_TYPE, r.string(o));
	SOUND(TT_SET_OBJ_FILE, access_of(r.string(o).string(f)));
	SOUND(TT_GET_OBJ_FILE, access_of(r.string(o)));
	SOUND(TT_SET_OBJ_ACCESS, access_of(access_of(r.string(o))));
	SOUND(TT_GET_OBJ_ACCESS, access_of(r.string(o)));
	SOUND(TT_IS_FILE_IN_DB, access_of(r.string(f)));
	SOUND(TT_IS_OBJ_IN_DB, access_of(r.string(o)));
	SOUND(TT_QUEUE_MESSAGE, r.string(f).u32(2).string("ptype").string("other").bytes("a message"));
	SOUND(TT_DEQUEUE_MESSAGES, r.string(f).u32(2).string("ptype").string("other"));
	SOUND(TTDB_FILE_NETFILE, r.string(f));
	SOUND(TTDB_NETFILE_FILE, r.string(f));
	SOUND(TT_DELETE_SESSION, r.string("session"));
#undef SOUND
	return v;
}

/*
 * A request of which a length is changed to one that no sender may use is
 * refused when it is decoded.  Whatever the number at the place of a length,
 * count or integer of the requests, the server does not allocate for it more
 * than the limits allow (check-requests.py has the allocator refuse what is
 * above 64 MiB, the largest message being 16 MiB).
 */
static void
case_xdr_hostile_lengths(void)
{
	std::string file = start_partition("xdr-hostile-lengths");
	std::string objid = make_objid(g_mount);

	if (getuid() == 0) {
		/* a request on a partition that is not redirected would make
		   its TT_DB where the superuser can */
		fprintf(stderr, "not run as the superuser\n");
		return;
	}
	put(g_dir + "/file", "the file");
	std::vector<Sound> sound = sound_requests(file, objid);
	static const unsigned hostile[] = { 0xffffffffu, 0x7fffffffu, 0x80000000u,
					    0x20000000u, 0x04000001u };
	size_t requests = 0;

	for (size_t i = 0; i < sound.size(); i++) {
		/* a request made as the client makes it is served */
		Reply r = rpc(sound[i].proc, sound[i].request);
		CHECK(r.replied);

		for (size_t at = 0; at + 4 <= sound[i].request.size(); at += 4) {
			unsigned word;

			memcpy(&word, sound[i].request.data() + at, 4);
			if (ntohl(word) > 4096) {
				continue;	/* characters, not a length */
			}
			for (size_t h = 0; h < sizeof(hostile) / sizeof(hostile[0]); h++) {
				std::string req = sound[i].request;
				unsigned v = htonl(hostile[h]);

				memcpy(&req[at], &v, 4);
				r = rpc(sound[i].proc, req);
				CHECK(r.replied);
				requests++;
			}
		}
	}
	CHECK(requests > 400);
}

/* ---- identities, tables and results of the access cases ----------------------- */

/* Send the requests that follow as another user, for as long as this lives. */
class Caller {
public:
	Caller(uid_t uid, const std::vector<gid_t> &gids)
		: uid_(g_uid), gids_(g_gids), flavor_(g_flavor)
	{
		g_uid = uid;
		g_gids = gids;
	}
	Caller(uid_t uid, gid_t gid)
		: uid_(g_uid), gids_(g_gids), flavor_(g_flavor)
	{
		g_uid = uid;
		g_gids.assign(1, gid);
	}
	~Caller()
	{
		g_uid = uid_;
		g_gids = gids_;
		g_flavor = flavor_;
	}
private:
	uid_t			uid_;
	std::vector<gid_t>	gids_;
	int			flavor_;
};

static bool
exists(const std::string &path)
{
	struct stat st;

	return lstat(path.c_str(), &st) == 0;
}

static std::string
slurp(const std::string &path)
{
	std::string out;
	char buf[256];
	FILE *f = fopen(path.c_str(), "r");

	while (f != NULL) {
		size_t n = fread(buf, 1, sizeof(buf), f);
		if (n == 0) {
			break;
		}
		out.append(buf, n);
	}
	if (f != NULL) {
		fclose(f);
	}
	return out;
}

/* The two numbers of an answer of NetISAM. */
struct Isam {
	int	result;
	int	iserrno;
};

static Isam
isam_of(const Reply &r)
{
	Isam		out = { INT_MIN, INT_MIN };
	_Tt_isam_results res;
	XDR		xdrs;

	if (!answered(r)) {
		return out;
	}
	memset(&res, 0, sizeof(res));
	xdrmem_create(&xdrs, (char *)r.body.data(), r.body.size(), XDR_DECODE);
	if (xdr_Tt_isam_results(&xdrs, &res)) {
		out.result = res.result;
		out.iserrno = res.iserrno;
	}
	return out;
}

/* The answer of a request that is refused for want of permission. */
static bool
is_denied(const Isam &i)
{
	return i.result == -1 && i.iserrno == DM_ACCESS_DENIED;
}

static int
build_at(const std::string &path, bool fixed = true)
{
	keydesc			key = oid_key();
	_Tt_isbuild_args	args;

	args.path = (char *)path.c_str();
	args.reclen = fixed ? fixed_reclen : ISMAXRECLEN;
	args.key = &key;
	args.mode = fixed ? fixed_mode : var_mode;
	args.isreclen = fixed ? fixed_reclen : TT_DB_KEY_LENGTH + MAX_PROP_LENGTH;
	return reply_result(rpc(_TT_ISBUILD,
				encode((xdrproc_t)xdr_Tt_isbuild_args, &args)));
}

static int
open_at(const std::string &path, int mode)
{
	_Tt_isopen_args args;

	args.path = (char *)path.c_str();
	args.mode = mode;
	return reply_result(rpc(_TT_ISOPEN,
				encode((xdrproc_t)xdr_Tt_isopen_args, &args)));
}

static int
erase_at(const std::string &path)
{
	char *p = (char *)path.c_str();

	return reply_result(rpc(_TT_ISERASE, encode((xdrproc_t)xdr_wrapstring, &p)));
}

extern "C" { int isbuild(const char *, int, struct keydesc *, int); }

/* A table of NetISAM's own, as an old server would have left it: closed at
   once, as the server has nothing to do with it. */
static bool
make_table(const std::string &path)
{
	keydesc	key = oid_key();
	int	fd = isbuild(path.c_str(), fixed_reclen, &key, fixed_mode);

	if (fd < 0) {
		return false;
	}
	isclose(fd);
	return true;
}

/* ---- the table paths of the legacy interface ---------------------------------- */

/* A legacy client names a table of its database <partition>/TT_DB/<table>.
   isbuild, isopen and iserase serve that and nothing else. */
static void
case_legacy_table_paths(void)
{
	start_server("legacy-table-paths");

	int isfd = build("oid_access");
	CHECK(isfd >= 0);
	CHECK(exists(g_dir + "/TT_DB/access_table.rec"));
	CHECK(close_table(isfd) == 0);
	isfd = open_table("oid_access", fixed_mode);
	CHECK(isfd >= 0);
	CHECK(close_table(isfd) == 0);

	/* directories that are not TT_DB, but have TT_DB in their names, and
	   paths that get to the table by way of one */
	static const char *dirs[] = { "other", "xTT_DBx", "TT_DB.d", "notTT_DB",
				      "TT_DB/sub", "sub", "rel", "rel/TT_DB",
				      "evil" };
	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		mkdir((g_dir + "/" + dirs[i]).c_str(), 0700);
	}
	/* tables of an old server where they have no business to be: erase
	   must leave them alone */
	static const char *homes[] = { "other", "xTT_DBx", "TT_DB.d", "notTT_DB",
				       "TT_DB/sub", "" };
	for (size_t i = 0; i < sizeof(homes) / sizeof(homes[0]); i++) {
		CHECK(make_table(g_dir + "/" + homes[i] + "/oid_access"));
	}
	static const char *bad[] = {
		"/other/oid_access", "/xTT_DBx/oid_access",
		"/TT_DB.d/oid_access", "/notTT_DB/oid_access",
		"/TT_DB/sub/oid_access", "/sub/../TT_DB/oid_access",
		"/TT_DB/../other/oid_access", "/./TT_DB/oid_access",
		"/TT_DB/./oid_access", "//TT_DB/oid_access",
		"/TT_DB/oid_access/", "/oid_access",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		std::string path = g_dir + bad[i];

		CHECK(build_at(path) == -1);
		CHECK(open_at(path, fixed_mode) == -1);
		CHECK(erase_at(path) == -1);
	}
	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		CHECK(!exists(g_dir + "/" + dirs[i] + "/access_table.rec"));
	}
	for (size_t i = 0; i < sizeof(homes) / sizeof(homes[0]); i++) {
		CHECK(exists(g_dir + "/" + homes[i] + "/oid_access.rec"));
		CHECK(exists(g_dir + "/" + homes[i] + "/oid_access.ind"));
	}

	/* a relative path, whatever it leads to */
	CHECK(build_at("rel/TT_DB/oid_access") == -1);
	CHECK(open_at("rel/TT_DB/oid_access", fixed_mode) == -1);
	CHECK(!exists(g_dir + "/rel/TT_DB/access_table.rec"));

	/* a directory that is not a partition gets no TT_DB */
	CHECK(build_at(g_dir + "/evil/TT_DB/oid_access") == -1);
	CHECK(open_at(g_dir + "/evil/TT_DB/oid_access", fixed_mode) == -1);
	CHECK(!exists(g_dir + "/evil/TT_DB"));

	/* iserase takes away a table of an old server by the name it had,
	   never the tables of the layout that replaced them */
	CHECK(erase_at(g_dir + "/TT_DB/access_table") == -1);
	CHECK(exists(g_dir + "/TT_DB/access_table.rec"));
	CHECK(erase_at(g_dir + "/TT_DB/no-such-table") == -1);
	CHECK(make_table(g_dir + "/TT_DB/oid_prop"));
	CHECK(erase_at(g_dir + "/TT_DB/sub/../oid_prop") == -1);
	CHECK(exists(g_dir + "/TT_DB/oid_prop.rec"));
	CHECK(erase_at(g_dir + "/TT_DB/oid_prop") == 0);
	CHECK(!exists(g_dir + "/TT_DB/oid_prop.rec"));
	CHECK(!exists(g_dir + "/TT_DB/oid_prop.ind"));
}

/* Links do not lead the server out of the TT_DB directory of the partition. */
static void
case_legacy_table_links(void)
{
	start_server("legacy-table-links");

	/* TT_DB is a link to a directory somewhere else */
	CHECK(mkdir((g_dir + "/elsewhere").c_str(), 0700) == 0);
	CHECK(symlink((g_dir + "/elsewhere").c_str(), (g_dir + "/TT_DB").c_str()) == 0);
	CHECK(build("oid_access") == -1);
	CHECK(open_table("oid_access", fixed_mode) == -1);
	CHECK(!exists(g_dir + "/elsewhere/access_table.rec"));
	CHECK(!exists(g_dir + "/elsewhere/access_table.ind"));

	/* ... and so is a file of the table */
	CHECK(unlink((g_dir + "/TT_DB").c_str()) == 0);
	CHECK(mkdir((g_dir + "/TT_DB").c_str(), 0700) == 0);
	put(g_dir + "/victim", "not a table");
	CHECK(symlink((g_dir + "/victim").c_str(),
		      (g_dir + "/TT_DB/access_table.rec").c_str()) == 0);
	CHECK(build("oid_access") == -1);
	CHECK(open_table("oid_access", fixed_mode) == -1);
	CHECK(slurp(g_dir + "/victim") == "not a table");
}

/* ---- the oid access procedures ------------------------------------------------ */

static Reply
oid_access_call(u_long proc, int isfd, const char *key, long value)
{
	_Tt_oidaccess_args args;

	args.isfd = isfd;
	args.key.key_len = TT_DB_KEY_LENGTH;
	args.key.key_val = key;
	args.value = (short)value;
	return rpc(proc, encode((xdrproc_t)xdr_Tt_oidaccess_args, &args));
}

static bool
oid_access_get(int isfd, const char *key, int *result, long *uid)
{
	Reply			r = oid_access_call(_TT_GETOIDACCESS, isfd, key, 0);
	_Tt_oidaccess_results	res;
	XDR			xdrs;

	if (!answered(r)) {
		return false;
	}
	memset(&res, 0, sizeof(res));
	xdrmem_create(&xdrs, (char *)r.body.data(), r.body.size(), XDR_DECODE);
	if (!xdr_Tt_oidaccess_results(&xdrs, &res)) {
		return false;
	}
	*result = res.result;
	*uid = (long)res.uid;
	return true;
}

/* Like every procedure that works on an isfd, those that read and write the
   access of an oid refuse a descriptor another user opened. */
static void
case_oid_access_stale_fd(void)
{
	char oid[TT_DB_KEY_LENGTH];
	const uid_t owner = getuid() + 10, stranger = getuid() + 11;
	const gid_t group = getgid();

	memset(oid, 'k', sizeof(oid));
	start_server("oid-access-stale-fd");

	int isfd;
	{
		Caller as(owner, group);
		isfd = build("oid_access");
		CHECK(isfd >= 0);
		/* the owner writes himself into the table: this is allowed */
		CHECK(isam_of(oid_access_call(_TT_SETOIDUSER, isfd, oid, owner)).result
		      == DM_OK);
	}

	Caller as(stranger, group);
	static const u_long procs[] = { _TT_SETOIDUSER, _TT_SETOIDGROUP,
					_TT_SETOIDMODE };
	for (size_t i = 0; i < sizeof(procs) / sizeof(procs[0]); i++) {
		Isam r = isam_of(oid_access_call(procs[i], isfd, oid, stranger));

		CHECK(r.result == DM_ERROR);
		CHECK(r.iserrno == ERPC);
	}
	int result = 0;
	long uid = 0;
	CHECK(oid_access_get(isfd, oid, &result, &uid));
	CHECK(result == DM_ERROR);

	/* the owner has not been touched */
	Caller back(owner, group);
	CHECK(oid_access_get(isfd, oid, &result, &uid));
	CHECK(result == DM_OK);
	CHECK(uid == (long)owner);
}

/* ---- who may read and write what ---------------------------------------------- */

static keydesc
prop_key(void)
{
	keydesc k;

	memset(&k, 0, sizeof(k));
	k.k_flags = ISDUPS;
	k.k_nparts = 2;
	k.k_part[0].kp_start = 0;
	k.k_part[0].kp_leng = TT_DB_KEY_LENGTH;
	k.k_part[0].kp_type = BINTYPE;
	k.k_part[1].kp_start = TT_DB_KEY_LENGTH;
	k.k_part[1].kp_leng = MAX_PROP_LENGTH;
	k.k_part[1].kp_type = CHARTYPE;
	return k;
}

static int
build_table(const char *old_name, bool fixed, int reclen, int minlen,
	    const keydesc &k)
{
	std::string	path = table_path(old_name);
	keydesc		key = k;
	_Tt_isbuild_args args;

	args.path = (char *)path.c_str();
	args.reclen = reclen;
	args.key = &key;
	args.mode = fixed ? fixed_mode : var_mode;
	args.isreclen = minlen;
	return reply_result(rpc(_TT_ISBUILD,
				encode((xdrproc_t)xdr_Tt_isbuild_args, &args)));
}

/* a record of the property table: key, name padded to its field, value */
static std::string
prop_record(const char *oid, const char *name, const char *value)
{
	std::string r(oid, TT_DB_KEY_LENGTH);

	r += name;
	r.resize(TT_DB_KEY_LENGTH + MAX_PROP_LENGTH, '\0');
	r += value;
	return r;
}

/* The keys of the objects of a database the server made: the file F is the
   file on disk, and holds the objects O0.. (a mode of its own for each); P
   belongs to somebody else; X has no access record. */
struct Db {
	char	f[TT_DB_KEY_LENGTH];
	char	o[8][TT_DB_KEY_LENGTH];
	char	p[TT_DB_KEY_LENGTH];
	char	x[TT_DB_KEY_LENGTH];
};

extern "C" { int isread(int, char *, int); }
static int create_object(const std::string &objid, const std::string &file,
			 unsigned mode);

/* the key of the object an object id names, as the tables keep it */
static void
key_of(const std::string &objid, char *key)
{
	_Tt_db_key	k(_Tt_string(objid.c_str()));
	_Tt_string	b = k.binary();

	memcpy(key, (char *)b, TT_DB_KEY_LENGTH);
}

/* the first record of a table of the database, read as NetISAM has it */
static std::string
first_record_of(const char *table)
{
	std::string	path = g_dir + "/TT_DB/" + table;
	static char	buf[ISMAXRECLEN];
	int		fd = isopen(path.c_str(), ISINOUT + ISVARLEN + ISMANULOCK);
	std::string	out;

	CHECK(fd >= 0);
	if (fd >= 0) {
		if (isread(fd, buf, ISFIRST) == 0) {
			out.assign(buf, isreclen);
		}
		isclose(fd);
	}
	return out;
}

/* A database as clients of libtt make it, through the requests of the new
   interface: the file, objects of it created by CREATOR with the modes
   given, and one of OTHER_OWNER. */
static void
make_db(Db *db, uid_t creator, gid_t creator_group, const unsigned *modes,
	size_t nmodes, uid_t other_owner, uid_t file_owner, gid_t file_group,
	const std::string &netfile)
{
	{
		Caller as(file_owner, file_group);
		create_file(netfile);
	}
	for (size_t i = 0; i < nmodes; i++) {
		std::string objid = make_objid(g_mount);
		Caller as(creator, creator_group);

		CHECK(create_object(objid, netfile,
				    modes[i] == 0xffff ? (unsigned)-1 : modes[i])
		      == TT_DB_OK);
		key_of(objid, db->o[i]);
	}
	std::string objid = make_objid(g_mount);
	{
		Caller as(other_owner, creator_group);
		CHECK(create_object(objid, netfile, 0600) == TT_DB_OK);
	}
	key_of(objid, db->p);

	/* the key of the file is that of the record of its table */
	std::string rec = first_record_of("file_table");
	CHECK(rec.size() > (size_t)TT_DB_KEY_LENGTH);
	if (rec.size() >= (size_t)TT_DB_KEY_LENGTH) {
		memcpy(db->f, rec.data(), TT_DB_KEY_LENGTH);
	}
	memset(db->x, 'x', sizeof(db->x));
}

/* The requests of a legacy client on the property table: read a record, add
   one, rewrite one and test-and-set one, all with the key of an object. */
static Isam
read_prop(int isfd, const char *key)
{
	_Tt_isread_args ra = { isfd, { TT_DB_KEY_LENGTH, (char *)key }, ISFIRST, 0 };

	return isam_of(rpc(_TT_ISREAD, encode((xdrproc_t)xdr_Tt_isread_args, &ra)));
}

static Isam
write_prop(int isfd, const char *key)
{
	std::string rec = prop_record(key, "added", "by the test");
	_Tt_iswrite_args wa = { isfd, { (u_int)rec.size(), (char *)rec.data() } };

	return isam_of(rpc(_TT_ISWRITE, encode((xdrproc_t)xdr_Tt_iswrite_args, &wa)));
}

static Isam
rewrite_prop(int isfd, const char *key)
{
	std::string rec = prop_record(key, "name", "value");
	_Tt_isrewrec_args wa = { isfd, 1,
				 { (u_int)rec.size(), (char *)rec.data() } };

	return isam_of(rpc(_TT_ISREWREC,
			   encode((xdrproc_t)xdr_Tt_isrewrec_args, &wa)));
}

static Isam
test_and_set_prop(int isfd, const char *key)
{
	keydesc k = prop_key();
	std::string rec = prop_record(key, "tas", "set");
	_Tt_test_and_set_args ta = { isfd, &k, TT_DB_KEY_LENGTH + MAX_PROP_LENGTH,
				     { (u_int)rec.size(), (char *)rec.data() } };
	Reply r = rpc(_TT_TEST_AND_SET,
		      encode((xdrproc_t)xdr_Tt_test_and_set_args, &ta));
	_Tt_test_and_set_results res;
	XDR xdrs;
	Isam out = { INT_MIN, INT_MIN };

	if (!answered(r)) {
		return out;
	}
	memset(&res, 0, sizeof(res));
	xdrmem_create(&xdrs, (char *)r.body.data(), r.body.size(), XDR_DECODE);
	if (xdr_Tt_test_and_set_results(&xdrs, &res)) {
		out.result = res.isresult.result;
		out.iserrno = res.isresult.iserrno;
		xdr_free((xdrproc_t)xdr_Tt_test_and_set_results, (char *)&res);
	}
	return out;
}

/* What the rule says.  The owner may do everything; the group and the others
   what the bits of their class grant, and a request that reads and writes
   needs both. */
static bool
class_grants(unsigned mode, int klass, bool read, bool write)
{
	unsigned r = (klass == 1) ? 040 : 04, w = (klass == 1) ? 020 : 02;

	if (klass == 0) {
		return true;
	}
	return (!read || (mode & r) != 0) && (!write || (mode & w) != 0);
}

struct Who {
	const char		*name;
	int			klass;		/* 0 owner, 1 group, 2 others */
	uid_t			uid;
	std::vector<gid_t>	gids;
};

/* Do the four requests on the object KEY get the answer the rule gives?  What
   the file allows to the class of the user, and what the mode of the object
   does, if it has one, counts together: a request is allowed if each of the
   permissions it needs is given by one of them. */
static void
check_access(const Who &who, int isfd, const char *key, unsigned file_mode,
	     unsigned object_mode)
{
	unsigned effective = file_mode | (object_mode != 0xffff ? object_mode : 0);
	bool want_r = class_grants(effective, who.klass, true, false);
	bool want_w = class_grants(effective, who.klass, false, true);
	bool want_rw = class_grants(effective, who.klass, true, true);

	Caller as(who.uid, who.gids);
	Isam r = read_prop(isfd, key);
	Isam w = write_prop(isfd, key);
	Isam rw = test_and_set_prop(isfd, key);
	Isam re = rewrite_prop(isfd, key);

	if (is_denied(r) == want_r || is_denied(w) == want_w ||
	    is_denied(rw) == want_rw || is_denied(re) == want_w) {
		fprintf(stderr, "  %s, file %04o, object %04o: read %s (%d %d), "
			"write %s (%d %d), test-and-set %s (%d %d), "
			"rewrite %s (%d %d)\n",
			who.name, file_mode, object_mode & 07777,
			is_denied(r) ? "denied" : "allowed", r.result, r.iserrno,
			is_denied(w) ? "denied" : "allowed", w.result, w.iserrno,
			is_denied(rw) ? "denied" : "allowed", rw.result, rw.iserrno,
			is_denied(re) ? "denied" : "allowed", re.result, re.iserrno);
	}
	CHECK(is_denied(r) == !want_r);
	CHECK(is_denied(w) == !want_w);
	CHECK(is_denied(rw) == !want_rw);
	CHECK(is_denied(re) == !want_w);
}

static int
open_as(const Who &who)
{
	Caller as(who.uid, who.gids);

	return open_table("oid_prop", var_mode);
}

static void
close_as(const Who &who, int isfd)
{
	Caller as(who.uid, who.gids);

	CHECK(close_table(isfd) == 0);
}

/* A transaction of two records writes both: the access to each is checked. */
static Isam
transaction_of(int isfd, const char *first, const char *second)
{
	std::string a = prop_record(first, "txn", "1");
	std::string b = prop_record(second, "txn", "2");
	_Tt_trans_record two = { 1, 0, { (u_int)b.size(), (char *)b.data() }, NULL };
	_Tt_trans_record one = { 1, 0, { (u_int)a.size(), (char *)a.data() }, &two };
	_Tt_transaction_args args = { isfd, &one };

	return isam_of(rpc(_TT_TRANSACTION,
			   encode((xdrproc_t)xdr_Tt_transaction_args, &args)));
}

/* The protection of the file decides who may read and write the properties of
   its objects: the owner of the file or of the object may do anything, the
   group and the others what the group and other bits of the file give them,
   or those of the mode of the object. */
static void
case_access_rules(void)
{
	const std::string netfile = start_partition("access-rules");
	const std::string file = g_dir + "/file";
	const uid_t file_owner = getuid() ? getuid() : 4000;
	const gid_t file_group = getuid() ? getgid() : 4000;
	const uid_t creator = file_owner + 2;
	Db db;

	put(file, "the file");
	if (getuid() == 0) {
		CHECK(chown(file.c_str(), file_owner, file_group) == 0);
	}

	static const unsigned file_modes[] = { 0600, 0640, 0660, 0604, 0606,
					       0644, 0666, 0400, 0200 };
	static const unsigned object_modes[] = { 0600, 0640, 0606, 0620, 0xffff };
	const size_t nfile = sizeof(file_modes) / sizeof(file_modes[0]);
	const size_t nobj = sizeof(object_modes) / sizeof(object_modes[0]);

	make_db(&db, creator, file_group + 1, object_modes, nobj, file_owner + 5,
		file_owner, file_group, netfile);

	Who owner = { "owner of the file", 0, file_owner, { file_group } };
	Who made = { "creator of the object", 0, creator, { file_group + 1 } };
	Who member = { "member of the group", 1, file_owner + 1, { file_group } };
	Who other = { "others", 2, file_owner + 1, { file_group + 1 } };
	Who all[] = { owner, made, member, other };

	for (size_t w = 0; w < 4; w++) {
		int isfd = open_as(all[w]);

		CHECK(isfd >= 0);
		for (size_t i = 0; i < nfile; i++) {
			CHECK(chmod(file.c_str(), file_modes[i]) == 0);
			for (size_t m = 0; m < nobj; m++) {
				check_access(all[w], isfd, db.o[m], file_modes[i],
					     object_modes[m]);
			}
		}
		close_as(all[w], isfd);
	}

	/* the superuser may, whatever the modes say */
	CHECK(chmod(file.c_str(), 0000) == 0);
	{
		Who root = { "root", 0, 0, { 0 } };
		int isfd = open_as(root);

		CHECK(isfd >= 0);
		Caller as(0, 0);
		CHECK(!is_denied(read_prop(isfd, db.o[0])));
		CHECK(!is_denied(write_prop(isfd, db.o[0])));
		CHECK(!is_denied(read_prop(isfd, db.x)));
		CHECK(close_table(isfd) == 0);
	}

	/* the properties of the file itself are not guarded by its mode, but an
	   object without an access record, or a key that is not one, is
	   nobody's to read */
	CHECK(chmod(file.c_str(), 0600) == 0);
	int isfd = open_as(other);
	CHECK(isfd >= 0);
	{
		Caller as(other.uid, other.gids);
		CHECK(!is_denied(read_prop(isfd, db.f)));
		CHECK(is_denied(read_prop(isfd, db.x)));
		CHECK(is_denied(read_prop(isfd, db.p)));
		char shorter[TT_DB_KEY_LENGTH - 1];
		memset(shorter, 'a', sizeof(shorter));
		_Tt_isread_args ra = { isfd, { sizeof(shorter), shorter }, ISFIRST, 0 };
		CHECK(is_denied(isam_of(rpc(_TT_ISREAD,
					    encode((xdrproc_t)xdr_Tt_isread_args, &ra)))));
		_Tt_isread_args none = { isfd, { 0, NULL }, ISFIRST, 0 };
		CHECK(is_denied(isam_of(rpc(_TT_ISREAD,
					    encode((xdrproc_t)xdr_Tt_isread_args, &none)))));
	}
	close_as(other, isfd);

	/* a transaction is for every record in it */
	isfd = open_as(made);
	CHECK(isfd >= 0);
	{
		Caller as(made.uid, made.gids);
		CHECK(is_denied(transaction_of(isfd, db.o[0], db.p)));
		CHECK(is_denied(transaction_of(isfd, db.p, db.o[0])));
		CHECK(transaction_of(isfd, db.o[0], db.o[1]).result == 0);
	}
	close_as(made, isfd);

	/* the server without access checking (rpc.ttdbserverd -n) */
	isfd = open_as(other);
	CHECK(isfd >= 0);
	{
		Caller as(other.uid, other.gids);
		CHECK(is_denied(read_prop(isfd, db.o[0])));
		access_checking = 0;
		CHECK(!is_denied(read_prop(isfd, db.o[0])));
		CHECK(!is_denied(write_prop(isfd, db.p)));
		access_checking = 1;
		CHECK(is_denied(read_prop(isfd, db.o[0])));
	}
	close_as(other, isfd);

	/* a file that cannot be looked at leaves the object to its own owner,
	   group and mode */
	CHECK(unlink(file.c_str()) == 0);
	isfd = open_as(member);
	CHECK(isfd >= 0);
	{
		/* the object's group is file_group + 1: the member of the group
		   of the file is a stranger to it */
		Caller as(member.uid, member.gids);
		CHECK(is_denied(write_prop(isfd, db.o[0])));	/* mode 0600 */
		CHECK(!is_denied(read_prop(isfd, db.o[2])));	/* mode 0606 */
		CHECK(!is_denied(write_prop(isfd, db.o[2])));
	}
	close_as(member, isfd);
}

/* An access record is read in the layout of the access table: the key, and a
   long for each number, which holds the number in network order. */
extern int _tt_decode_access(const char *rec, int reclen, uid_t *uid,
			     gid_t *group, mode_t *mode) __attribute__((weak));

static std::string
access_record(unsigned long user, unsigned long group, unsigned long mode)
{
	std::string	r(TT_DB_KEY_LENGTH, 'k');
	long		n;

	n = htonl(user); r.append((char *)&n, TT_DB_LONG_SIZE);
	n = htonl(group); r.append((char *)&n, TT_DB_LONG_SIZE);
	n = htonl(mode); r.append((char *)&n, TT_DB_LONG_SIZE);
	return r;
}

static void
case_access_layouts(void)
{
	uid_t uid;
	gid_t group;
	mode_t mode;

	if (_tt_decode_access == NULL) {
		/* a server that does not read access records by their layout */
		CHECK(_tt_decode_access != NULL);
		return;
	}

	std::string rec = access_record(123456, 7890, 0100600);
	CHECK(_tt_decode_access(rec.data(), rec.size(), &uid, &group, &mode));
	CHECK(uid == 123456 && group == 7890 && mode == 0100600);
	rec = access_record((uid_t)-1, (gid_t)-1, (mode_t)-1);
	CHECK(_tt_decode_access(rec.data(), rec.size(), &uid, &group, &mode));
	CHECK(uid == (uid_t)-1 && group == (gid_t)-1 && mode == (mode_t)-1);

	/* any other length is no access record, nor is the layout of the
	   clients of the first servers, 16 bits for each number */
	CHECK(!_tt_decode_access(rec.data(), rec.size() - 1, &uid, &group, &mode));
	CHECK(!_tt_decode_access(rec.data(), 0, &uid, &group, &mode));
	CHECK(!_tt_decode_access(rec.data(), rec.size() + 1, &uid, &group, &mode));
	std::string shorts(TT_DB_KEY_LENGTH + 6, 's');
	CHECK(!_tt_decode_access(shorts.data(), shorts.size(), &uid, &group, &mode));

	/* and the record the server wrote for an object is read back whole */
	const std::string netfile = start_partition("access-layouts");
	const uid_t owner = getuid() + 30;
	const gid_t gid = getgid();
	std::string objid = make_objid(g_mount);

	put(g_dir + "/file", "the file");
	{
		Caller as(owner, gid);
		create_file(netfile);
		CHECK(create_object(objid, netfile, 0640) == TT_DB_OK);
	}
	char key[TT_DB_KEY_LENGTH];
	key_of(objid, key);

	std::string path = g_dir + "/TT_DB/access_table";
	static char buf[ISMAXRECLEN];
	int fd = isopen(path.c_str(), ISINOUT + ISFIXLEN + ISMANULOCK);
	CHECK(fd >= 0);
	int found = 0;
	for (int r = isread(fd, buf, ISFIRST); fd >= 0 && r == 0;
	     r = isread(fd, buf, ISNEXT)) {
		if (memcmp(buf, key, TT_DB_KEY_LENGTH) == 0) {
			found++;
			CHECK(isreclen == TT_DB_KEY_LENGTH + 3 * TT_DB_LONG_SIZE);
			CHECK(_tt_decode_access(buf, isreclen, &uid, &group, &mode));
			CHECK(uid == owner && group == gid && mode == 0640);
		}
	}
	CHECK(found == 1);
	if (fd >= 0) {
		isclose(fd);
	}
}

/* ---- credentials -------------------------------------------------------------- */

static int
create_object(const std::string &objid, const std::string &file, unsigned mode)
{
	_tt_access	acc = { g_uid, g_gids.empty() ? getgid() : g_gids[0], mode };
	_tt_property_list none = { 0, NULL };
	char		otype[] = "otype";
	_tt_create_obj_args co = { (char *)file.c_str(), (char *)objid.c_str(),
				   otype, none, acc };
	Reply		r = rpc(TT_CREATE_OBJ,
				encode((xdrproc_t)xdr_tt_create_obj_args, &co));
	_tt_db_cache_results res;
	XDR		xdrs;

	if (!answered(r)) {
		return INT_MIN;
	}
	memset(&res, 0, sizeof(res));
	xdrmem_create(&xdrs, (char *)r.body.data(), r.body.size(), XDR_DECODE);
	if (!xdr_tt_db_cache_results(&xdrs, &res)) {
		return INT_MIN;
	}
	return res.results;
}

static int
is_obj_in_db(const std::string &objid, Reply *reply = NULL)
{
	_tt_access	acc = { g_uid, g_gids.empty() ? getgid() : g_gids[0], 0 };
	_tt_is_obj_in_db_args oi = { (char *)objid.c_str(), acc };
	Reply		r = rpc(TT_IS_OBJ_IN_DB,
				encode((xdrproc_t)xdr_tt_is_obj_in_db_args, &oi));
	_tt_is_obj_in_db_results res;
	XDR		xdrs;

	if (reply != NULL) {
		*reply = r;
	}
	if (!answered(r)) {
		return INT_MIN;
	}
	memset(&res, 0, sizeof(res));
	xdrmem_create(&xdrs, (char *)r.body.data(), r.body.size(), XDR_DECODE);
	if (!xdr_tt_is_obj_in_db_results(&xdrs, &res)) {
		return INT_MIN;
	}
	int out = res.results;
	xdr_free((xdrproc_t)xdr_tt_is_obj_in_db_results, (char *)&res);
	return out;
}

static int
is_file_in_db(const std::string &file)
{
	_tt_access	acc = { g_uid, g_gids.empty() ? getgid() : g_gids[0], 0 };
	_tt_is_file_in_db_args fi = { (char *)file.c_str(), acc };
	Reply		r = rpc(TT_IS_FILE_IN_DB,
				encode((xdrproc_t)xdr_tt_is_file_in_db_args, &fi));
	_tt_is_file_in_db_results res;
	XDR		xdrs;

	if (!answered(r)) {
		return INT_MIN;
	}
	memset(&res, 0, sizeof(res));
	xdrmem_create(&xdrs, (char *)r.body.data(), r.body.size(), XDR_DECODE);
	if (!xdr_tt_is_file_in_db_results(&xdrs, &res)) {
		return INT_MIN;
	}
	return res.results;
}

static int
get_obj_type(const std::string &objid)
{
	char		*name = (char *)objid.c_str();
	Reply		r = rpc(TT_GET_OBJ_TYPE,
				encode((xdrproc_t)xdr_tt_objid_name, &name));
	_tt_obj_type_results res;
	XDR		xdrs;

	if (!answered(r)) {
		return INT_MIN;
	}
	memset(&res, 0, sizeof(res));
	xdrmem_create(&xdrs, (char *)r.body.data(), r.body.size(), XDR_DECODE);
	if (!xdr_tt_obj_type_results(&xdrs, &res)) {
		return INT_MIN;
	}
	int out = res.results;
	xdr_free((xdrproc_t)xdr_tt_obj_type_results, (char *)&res);
	return out;
}

/* Every request that is not about the server itself runs with the
   credentials it came with: the answer to a user does not depend on who
   asked before, and a request without credentials is refused. */
static void
case_credentials_per_request(void)
{
	std::string file = start_partition("credentials");
	const uid_t owner = getuid() + 20, stranger = getuid() + 21;
	const gid_t group = getgid();
	std::string objid = make_objid(g_mount);

	put(g_dir + "/file", "the file");
	{
		Caller as(owner, group);
		create_file(file);
		CHECK(create_object(objid, file, 0600) == TT_DB_OK);
		CHECK(get_obj_type(objid) == TT_DB_OK);
		CHECK(is_obj_in_db(objid) == TT_DB_OK);
	}

	/* the owner's request leaves nothing behind for the stranger */
	{
		Caller o(owner, group);
		CHECK(get_obj_type(objid) == TT_DB_OK);
	}
	{
		Caller as(stranger, group);
		/* the object is the owner's, and nobody else's to ask about */
		CHECK(is_obj_in_db(objid) == TT_DB_ERR_ACCESS_DENIED);
	}
	/* ... and not the other way round */
	{
		Caller o(owner, group);
		CHECK(is_obj_in_db(objid) == TT_DB_OK);
		CHECK(is_file_in_db(file) == TT_DB_OK);
	}

	/* a request with no credentials, or with ones of a kind the server does
	   not serve, is refused before it is looked at */
	static const int flavors[] = { AUTH_NULL, 9999 };
	for (size_t i = 0; i < sizeof(flavors) / sizeof(flavors[0]); i++) {
		Caller as(owner, group);
		g_flavor = flavors[i];
		Reply r;
		CHECK(is_obj_in_db(objid, &r) == INT_MIN);
		CHECK(r.replied && !r.accepted);
		CHECK(is_file_in_db(file) == INT_MIN);
	}
	/* those that carry no credentials of the user stay open */
	{
		Caller as(owner, group);
		g_flavor = AUTH_NULL;
		char *path = (char *)g_dir.c_str();
		Reply r = rpc(TT_GET_FILE_PARTITION,
			      encode((xdrproc_t)xdr_wrapstring, &path));
		CHECK(answered(r));
		r = rpc(TT_GET_MIN_AUTH_LEVEL, encode((xdrproc_t)xdr_void, NULL));
		CHECK(answered(r));
	}
}

/* ---- partitions --------------------------------------------------------------- */

/* The partition of an object id is a name for the directory where the
   database of the object is: a mount point, or a name of the partition map.
   Anything else gets no database made for it. */
static void
case_object_partitions(void)
{
	std::string file = start_partition("object-partitions");
	const uid_t me = getuid();

	put(g_dir + "/file", "the file");
	CHECK(mkdir((g_dir + "/evil").c_str(), 0700) == 0);
	CHECK(mkdir((g_dir + "/relative").c_str(), 0700) == 0);
	CHECK(mkdir((g_dir + "/relative/path").c_str(), 0700) == 0);
	create_file(file);
	CHECK(create_object(make_objid(g_mount), file, 0600) == TT_DB_OK);

	/* the directories of the host that are none of that */
	const std::string dirs[] = { g_dir + "/evil", g_dir + "/evil/",
				     g_dir + "/..", g_dir + "/evil/../evil",
				     g_dir + "/no/such/directory", "relative/path",
				     g_mount + "/", g_dir + "/evil:more" };
	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		std::string objid = make_objid(dirs[i]);
		bool had = exists(dirs[i] + "/TT_DB");

		CHECK(create_object(objid, file, 0600) != TT_DB_OK);
		CHECK(get_obj_type(objid) != TT_DB_OK);
		CHECK(is_obj_in_db(objid) != TT_DB_OK);
		CHECK(exists(dirs[i] + "/TT_DB") == had);
	}
	CHECK(!exists(g_dir + "/evil/TT_DB"));
	CHECK(!exists(g_dir + "/relative/path/TT_DB"));
	CHECK(!exists(g_dir + "/relative/TT_DB"));

	/* the answer is the one for an object id that is none */
	CHECK(get_obj_type(make_objid(g_dir + "/evil")) == TT_DB_ERR_ILLEGAL_OBJECT);

	/* partitions that are known are served */
	std::string objid = make_objid(g_mount);
	CHECK(create_object(objid, file, 0600) == TT_DB_OK);
	CHECK(is_obj_in_db(objid) == TT_DB_OK);
	objid = make_objid(g_dir);
	CHECK(create_object(objid, file, 0600) == TT_DB_OK);
	CHECK(get_obj_type(objid) == TT_DB_OK);
	(void)me;
}

/* ---- sessions ----------------------------------------------------------------- */

static Isam
session_call(u_long proc, int isfd, const char *oid, const std::string &session)
{
	keydesc k = prop_key();
	_Tt_session_args args;

	memset(&args, 0, sizeof(args));
	args.isfd = isfd;
	args.key = &k;
	args.key_len = TT_DB_KEY_LENGTH + MAX_PROP_LENGTH;
	args.oidkey.oidkey_len = TT_DB_KEY_LENGTH;
	args.oidkey.oidkey_val = (char *)oid;
	args.session.session_len = session.size();
	args.session.session_val = (char *)session.data();
	return isam_of(rpc(proc, encode((xdrproc_t)xdr_Tt_session_args, &args)));
}

/* the values of the property NAME of the object OID, in the order the table
   keeps them */
static std::vector<std::string>
prop_values(int isfd, const char *oid, const char *name)
{
	std::vector<std::string>	out;
	keydesc				k = prop_key();
	std::string			rec = prop_record(oid, name, "");
	_Tt_isstart_args		sa = { isfd, &k, TT_DB_KEY_LENGTH + MAX_PROP_LENGTH,
					       { (u_int)rec.size(), (char *)rec.data() },
					       ISEQUAL };

	if (isam_of(rpc(_TT_ISSTART, encode((xdrproc_t)xdr_Tt_isstart_args, &sa))).result
	    != 0) {
		return out;
	}
	for (;;) {
		_Tt_isread_args	ra = { isfd, { (u_int)rec.size(), (char *)rec.data() },
				       ISNEXT, 0 };
		Reply		r = rpc(_TT_ISREAD,
					encode((xdrproc_t)xdr_Tt_isread_args, &ra));
		_Tt_isread_results res;
		XDR		xdrs;

		if (!answered(r)) {
			break;
		}
		memset(&res, 0, sizeof(res));
		xdrmem_create(&xdrs, (char *)r.body.data(), r.body.size(), XDR_DECODE);
		if (!xdr_Tt_isread_results(&xdrs, &res) || res.isresult.result != 0) {
			xdr_free((xdrproc_t)xdr_Tt_isread_results, (char *)&res);
			break;
		}
		std::string got(res.rec.rec_val, res.rec.rec_len);
		xdr_free((xdrproc_t)xdr_Tt_isread_results, (char *)&res);
		if (got.compare(0, TT_DB_KEY_LENGTH, oid, TT_DB_KEY_LENGTH) != 0 ||
		    strncmp(got.c_str() + TT_DB_KEY_LENGTH, name, MAX_PROP_LENGTH) != 0) {
			break;
		}
		out.push_back(got.substr(TT_DB_KEY_LENGTH + MAX_PROP_LENGTH));
	}
	return out;
}

/* the value of the property _NODE_TYPE of the object OID, "" if gettype
   finds none */
static std::string
gettype_of(int isfd, const char *oid)
{
	keydesc		k = prop_key();
	_Tt_prop	p;
	_Tt_spec_props	sp;
	char		name[] = "_NODE_TYPE";	/* xdr_char writes what it encodes */

	memset(&sp, 0, sizeof(sp));
	memset(&p, 0, sizeof(p));
	p.propname.propname_len = strlen(name);
	p.propname.propname_val = name;
	sp.isfd = isfd;
	sp.key = &k;
	sp.key_len = TT_DB_KEY_LENGTH + MAX_PROP_LENGTH;
	sp.oidkey.oidkey_len = TT_DB_KEY_LENGTH;
	sp.oidkey.oidkey_val = oid;
	sp.props.props_len = 1;
	sp.props.props_val = &p;

	Reply		r = rpc(_TT_GETTYPE, encode((xdrproc_t)xdr_Tt_spec_props, &sp));
	_Tt_spec_props	res;
	XDR		xdrs;
	std::string	out;

	if (!answered(r)) {
		return "?";
	}
	memset(&res, 0, sizeof(res));
	xdrmem_create(&xdrs, (char *)r.body.data(), r.body.size(), XDR_DECODE);
	if (xdr_Tt_spec_props(&xdrs, &res)) {
		if (res.result == 0 && res.props.props_len == 1) {
			out.assign(res.props.props_val[0].value.value_val,
				   res.props.props_val[0].value.value_len);
		}
		xdr_free((xdrproc_t)xdr_Tt_spec_props, (char *)&res);
	}
	return out;
}

/* A session is added to the sessions of an object once, and taken away
   again by its id. */
static void
case_session_polarity(void)
{
	start_server("session-polarity");
	g_uid = 0;		/* the superuser reads the table back */

	char oid[TT_DB_KEY_LENGTH], other[TT_DB_KEY_LENGTH];
	memset(oid, 'o', sizeof(oid));
	memset(other, 'p', sizeof(other));

	int isfd = build_table("oid_prop", false, ISMAXRECLEN,
			       TT_DB_KEY_LENGTH + MAX_PROP_LENGTH, prop_key());
	CHECK(isfd >= 0);
	/* the object has a modification date, as the objects of old servers did */
	CHECK(write_record(isfd, prop_record(oid, "_MODIFICATION_DATE", "0")) == 0);
	/* ... and so has another one, with a session of its own */
	CHECK(write_record(isfd, prop_record(other, "_TT_SESSIONS", "s1")) == 0);

	const std::string s1 = "01 first session", s2 = "01 second session";
	std::vector<std::string> v;

	CHECK(session_call(_TT_ADDSESSION, isfd, oid, s1).result == 0);
	v = prop_values(isfd, oid, "_TT_SESSIONS");
	CHECK(v.size() == 1 && v[0] == s1);

	/* the same session again changes nothing */
	CHECK(session_call(_TT_ADDSESSION, isfd, oid, s1).result == 0);
	v = prop_values(isfd, oid, "_TT_SESSIONS");
	CHECK(v.size() == 1);

	/* another one is added next to it */
	CHECK(session_call(_TT_ADDSESSION, isfd, oid, s2).result == 0);
	v = prop_values(isfd, oid, "_TT_SESSIONS");
	CHECK(v.size() == 2);
	CHECK(session_call(_TT_ADDSESSION, isfd, oid, s2).result == 0);
	CHECK(prop_values(isfd, oid, "_TT_SESSIONS").size() == 2);

	/* every change counts in the modification date */
	v = prop_values(isfd, oid, "_MODIFICATION_DATE");
	CHECK(v.size() == 1 && v[0] == "2");

	/* a session that is not there cannot be taken away */
	Isam gone = session_call(_TT_DELSESSION, isfd, oid, "01 third session");
	CHECK(gone.result == -1 && gone.iserrno == ENOREC);
	CHECK(prop_values(isfd, oid, "_TT_SESSIONS").size() == 2);

	/* the one that is named goes, the other one stays */
	CHECK(session_call(_TT_DELSESSION, isfd, oid, s1).result == 0);
	v = prop_values(isfd, oid, "_TT_SESSIONS");
	CHECK(v.size() == 1 && v[0] == s2);
	gone = session_call(_TT_DELSESSION, isfd, oid, s1);
	CHECK(gone.result == -1 && gone.iserrno == ENOREC);
	CHECK(session_call(_TT_DELSESSION, isfd, oid, s2).result == 0);
	CHECK(prop_values(isfd, oid, "_TT_SESSIONS").empty());

	/* the sessions of the other object were not touched */
	v = prop_values(isfd, other, "_TT_SESSIONS");
	CHECK(v.size() == 1 && v[0] == "s1");
	CHECK(session_call(_TT_DELSESSION, isfd, oid, "s1").result == -1);
	CHECK(prop_values(isfd, other, "_TT_SESSIONS").size() == 1);

	/* the type of an object is the value of a property of it */
	CHECK(write_record(isfd, prop_record(oid, "_NODE_TYPE", "my_otype")) == 0);
	CHECK(gettype_of(isfd, oid) == "my_otype");
	CHECK(gettype_of(isfd, other) == "");
}


/* ---- the path and mount table functions of libtt ------------------------------ */

/* A path that no mount point leads to is in no partition, and none of the
   requests that look for the partition of a file crashes on it. */
static void
case_no_mount_point(void)
{
	std::string file = start_partition("no-mount-point");

	/* the working directory is gone, so that a relative path leads nowhere
	   ("/" is the start of every other one) */
	CHECK(mkdir((g_dir + "/gone").c_str(), 0700) == 0);
	CHECK(chdir((g_dir + "/gone").c_str()) == 0);
	CHECK(rmdir((g_dir + "/gone").c_str()) == 0);

	_Tt_file_system fs;
	CHECK(fs.bestMatchToPath("relative/path").is_null());
	CHECK(fs.bestMatchToPath("/").is_null() == false);
	CHECK(_tt_local_network_path("relative/path").len() == 0);

	_tt_access acc = { g_uid, getgid(), 0600 };
	char *path = (char *)"relative/path";
	char *netpath = (char *)"localhost:relative/path";
	Reply r = rpc(TT_GET_FILE_PARTITION, encode((xdrproc_t)xdr_tt_file_name, &path));
	CHECK(answered(r));
	_tt_file_partition_results pr;
	XDR xdrs;
	memset(&pr, 0, sizeof(pr));
	xdrmem_create(&xdrs, (char *)r.body.data(), r.body.size(), XDR_DECODE);
	CHECK(xdr_tt_file_partition_results(&xdrs, &pr));
	CHECK(pr.partition != NULL && pr.partition[0] == '\0');
	xdr_free((xdrproc_t)xdr_tt_file_partition_results, (char *)&pr);

	r = rpc(_TT_MFS, encode((xdrproc_t)xdr_tt_file_name, &path));
	CHECK(answered(r));
	_tt_move_file_args mf = { netpath, path, acc };
	r = rpc(TT_MOVE_FILE, encode((xdrproc_t)xdr_tt_move_file_args, &mf));
	CHECK(answered(r));
	_tt_create_file_args cf = { netpath, { 0, NULL }, acc };
	r = rpc(TT_CREATE_FILE, encode((xdrproc_t)xdr_tt_create_file_args, &cf));
	CHECK(answered(r));
	_tt_get_file_props_args gp = { netpath, acc, -1 };
	r = rpc(TT_GET_FILE_PROPS, encode((xdrproc_t)xdr_tt_get_file_props_args, &gp));
	CHECK(answered(r));
}

/* The directory and the base name of a path, wherever the first '/' is. */
static void
case_path_names(void)
{
	static const struct {
		const char	*path;
		int		dirname;
		int		basename;
	} t[] = {
		{ "/home3/dynamo/tan/text", 16, 18 },
		{ "/home3/dynamo/tan/", 16, 18 },
		{ "/a/b", 1, 3 },
		{ "/a", -1, 1 },
		{ "/", -1, 1 },
		{ "text", -1, 0 },
		{ "", -1, 0 },
	};

	for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		/* in storage of its own, so that a look before it is seen */
		char *copy = strdup(t[i].path);

		CHECK(_Tt_dirname(copy) == t[i].dirname);
		CHECK(_Tt_basename(copy) == t[i].basename);
		free(copy);
	}
	/* the length of the path may be given: the search starts there */
	char *copy = strdup("/a/b/c");
	CHECK(_Tt_dirname(copy, 4) == 3);
	CHECK(_Tt_basename(copy, 4) == 5);
	CHECK(_Tt_dirname(copy, 3) == 1);
	CHECK(_Tt_basename(copy, 3) == 3);
	free(copy);
}

/* ---- the sessions the server lists ---------------------------------------------- */

/* the session ids TT_GET_ALL_SESSIONS answers with */
static std::vector<std::string>
get_all_sessions(bool *answered_ok)
{
	std::vector<std::string>	out;
	_tt_get_all_sessions_args	args;
	_tt_get_all_sessions_results	res;
	XDR				xdrs;

	args.oidkey.oidkey_len = 0;
	args.oidkey.oidkey_val = NULL;
	Reply r = rpc(TT_GET_ALL_SESSIONS,
		      encode((xdrproc_t)xdr_tt_get_all_sessions_args, &args));
	*answered_ok = answered(r);
	if (!*answered_ok) {
		return out;
	}
	memset(&res, 0, sizeof(res));
	xdrmem_create(&xdrs, (char *)r.body.data(), r.body.size(), XDR_DECODE);
	if (xdr_tt_get_all_sessions_results(&xdrs, &res)) {
		for (u_int i = 0; i < res.session_list.values_len; i++) {
			out.push_back(res.session_list.values_val[i].value);
		}
	}
	xdr_free((xdrproc_t)xdr_tt_get_all_sessions_results, (char *)&res);
	return out;
}

/* The sessions joined to a file are values of its property _TT_SESSIONS, in
   the property table of the database, whose records start with a key as long
   as the key of an object: the server finds and takes them away there. */
static void
case_sessions_real_table(void)
{
	const std::string netfile = start_partition("sessions-real-table");
	bool ok;

	put(g_dir + "/file", "the file");
	create_file(netfile);
	CHECK(set_property(netfile, "_TT_SESSIONS", "01 a session") == TT_DB_OK);

	/* this server lists the sessions of the tables it has open for the old
	   interface */
	CHECK(open_table("oid_prop", var_mode) >= 0);
	std::vector<std::string> sessions = get_all_sessions(&ok);
	CHECK(ok);
	/* (a table that is open with two modes is listed twice) */
	CHECK(!sessions.empty());
	for (size_t i = 0; i < sessions.size(); i++) {
		CHECK(sessions[i] == "01 a session");
	}

	_tt_delete_session_args da;
	char session[] = "01 a session";
	da.session.value = session;
	Reply r = rpc(TT_DELETE_SESSION,
		      encode((xdrproc_t)xdr_tt_delete_session_args, &da));
	CHECK(answered(r));
	sessions = get_all_sessions(&ok);
	CHECK(ok && sessions.empty());
}

/* ---- the key of an object ------------------------------------------------------- */

/* A key is kept in the tables as it is in memory, and its length is the
   length of the structure; the bytes between its fields are zero, however the
   memory it is made in was used before. */
static void
case_key_bytes(void)
{
	CHECK(OID_KEY_LENGTH == (int)sizeof(_Tt_key_data));
	CHECK(TT_DB_KEY_LENGTH == OID_KEY_LENGTH);

	/* the bytes that belong to no field */
	std::vector<bool> field(sizeof(_Tt_key_data), false);
	for (size_t i = 0; i < sizeof(unsigned short); i++) {
		field[offsetof(_Tt_key_data, version) + i] = true;
		field[offsetof(_Tt_key_data, padding) + i] = true;
	}
	for (size_t i = 0; i < sizeof(unsigned long); i++) {
		field[offsetof(_Tt_key_data, hostid) + i] = true;
		field[offsetof(_Tt_key_data, time_sec) + i] = true;
		field[offsetof(_Tt_key_data, counter) + i] = true;
	}
	/* (the padding field is zero too) */
	for (size_t i = 0; i < sizeof(unsigned short); i++) {
		field[offsetof(_Tt_key_data, padding) + i] = false;
	}

	for (int made = 0; made < 2; made++) {
		static char mem[sizeof(_Tt_db_key)];
		memset(mem, 0xaa, sizeof(mem));
		_Tt_db_key *k = made ?
			::new (mem) _Tt_db_key(0) :
			::new (mem) _Tt_db_key(_Tt_string("0|a|b|c"));
		_Tt_string b = k->binary();

		CHECK(b.len() == OID_KEY_LENGTH);
		for (size_t i = 0; i < sizeof(_Tt_key_data); i++) {
			if (!field[i]) {
				CHECK(((const unsigned char *)(char *)b)[i] == 0);
			}
		}
	}
}

/* ---- random requests --------------------------------------------------------- */

static unsigned long long fuzz_state = 88172645463325252ULL;

static unsigned
fuzz_next(void)
{
	fuzz_state ^= fuzz_state << 13;
	fuzz_state ^= fuzz_state >> 7;
	fuzz_state ^= fuzz_state << 17;
	return (unsigned)(fuzz_state >> 11);
}

/* Requests that start as sound ones for the NetISAM procedures, with some of
   their bytes changed, and cut short: the server answers, or refuses to
   decode them, but does not crash. */
static void
case_fuzz_dispatch(void)
{
	/* not _TT_ISADDINDEX: NetISAM here may not be able to add an index */
	static const u_long procs[] = { _TT_ISCLOSE, _TT_ISCNTL,
					_TT_ISDELREC, _TT_ISREAD, _TT_ISREWREC,
					_TT_ISSTART, _TT_ISWRITE,
					_TT_TEST_AND_SET, _TT_TRANSACTION,
					_TT_GETOIDACCESS, _TT_SETOIDUSER,
					_TT_SETOIDGROUP, _TT_SETOIDMODE,
					_TT_READSPEC, _TT_WRITESPEC,
					_TT_ADDSESSION, _TT_DELSESSION,
					_TT_GETTYPE, TT_GET_ALL_SESSIONS,
					TT_DELETE_SESSION };
	int iterations = getenv("TTDB_FUZZ_ITERATIONS") ?
		atoi(getenv("TTDB_FUZZ_ITERATIONS")) : 3000;

	if (getenv("TTDB_FUZZ_SEED")) {
		fuzz_state ^= strtoull(getenv("TTDB_FUZZ_SEED"), NULL, 10) * 2654435761ULL;
	}
	start_server("fuzz-dispatch");
	int fixed = build("oid_access");
	int var = build("oid_prop");
	CHECK(fixed >= 0 && var >= 0);
	int isfd = fixed;
	keydesc key = oid_key();
	std::string rec = record('x', fixed_reclen);
	char oid[TT_DB_KEY_LENGTH];
	memset(oid, 'o', sizeof(oid));
	char session[40];
	memset(session, 's', sizeof(session));

	std::vector<std::string> sound(sizeof(procs) / sizeof(procs[0]));
	for (size_t i = 0; i < sound.size(); i++) {
		switch (procs[i]) {
		case _TT_ISCLOSE:
			sound[i] = encode((xdrproc_t)xdr_int, &isfd);
			break;
		case _TT_ISCNTL: {
			_Tt_iscntl_args a = { isfd, 0, { 0, NULL } };
			sound[i] = encode((xdrproc_t)xdr_Tt_iscntl_args, &a);
			break; }
		case _TT_ISDELREC: {
			_Tt_isdelrec_args a = { isfd, 1, { 16, (char *)rec.data() } };
			sound[i] = encode((xdrproc_t)xdr_Tt_isdelrec_args, &a);
			break; }
		case _TT_ISREAD: {
			_Tt_isread_args a = { isfd, { 16, (char *)rec.data() }, ISEQUAL, 0 };
			sound[i] = encode((xdrproc_t)xdr_Tt_isread_args, &a);
			break; }
		case _TT_ISREWREC: {
			_Tt_isrewrec_args a = { isfd, 1, { 30, (char *)rec.data() } };
			sound[i] = encode((xdrproc_t)xdr_Tt_isrewrec_args, &a);
			break; }
		case _TT_ISSTART: {
			_Tt_isstart_args a = { isfd, &key, TT_DB_KEY_LENGTH,
					       { 16, (char *)rec.data() }, ISEQUAL };
			sound[i] = encode((xdrproc_t)xdr_Tt_isstart_args, &a);
			break; }
		case _TT_ISWRITE: {
			_Tt_iswrite_args a = { isfd, { 30, (char *)rec.data() } };
			sound[i] = encode((xdrproc_t)xdr_Tt_iswrite_args, &a);
			break; }
		case _TT_TEST_AND_SET: {
			_Tt_test_and_set_args a = { isfd, &key, TT_DB_KEY_LENGTH,
						    { 30, (char *)rec.data() } };
			sound[i] = encode((xdrproc_t)xdr_Tt_test_and_set_args, &a);
			break; }
		case _TT_TRANSACTION:
			sound[i] = transaction_chain(2, isfd);
			break;
		case _TT_GETOIDACCESS:
		case _TT_SETOIDUSER:
		case _TT_SETOIDGROUP:
		case _TT_SETOIDMODE: {
			_Tt_oidaccess_args a = { isfd, { TT_DB_KEY_LENGTH, oid }, 5 };
			sound[i] = encode((xdrproc_t)xdr_Tt_oidaccess_args, &a);
			break; }
		case _TT_ADDSESSION:
		case _TT_DELSESSION: {
			_Tt_session_args a;
			memset(&a, 0, sizeof(a));
			a.isfd = var;
			a.key = &key;
			a.key_len = TT_DB_KEY_LENGTH;
			a.oidkey.oidkey_len = sizeof(oid);
			a.oidkey.oidkey_val = oid;
			a.session.session_len = sizeof(session);
			a.session.session_val = session;
			sound[i] = encode((xdrproc_t)xdr_Tt_session_args, &a);
			break; }
		case _TT_READSPEC:
		case _TT_WRITESPEC:
		case _TT_GETTYPE: {
			_Tt_spec_props a;
			memset(&a, 0, sizeof(a));
			a.isfd = var;
			a.key = &key;
			a.key_len = TT_DB_KEY_LENGTH;
			a.oidkey.oidkey_len = sizeof(oid);
			a.oidkey.oidkey_val = oid;
			sound[i] = encode((xdrproc_t)xdr_Tt_spec_props, &a);
			break; }
		case TT_GET_ALL_SESSIONS: {
			_tt_get_all_sessions_args a;
			a.oidkey.oidkey_len = sizeof(oid);
			a.oidkey.oidkey_val = oid;
			sound[i] = encode((xdrproc_t)xdr_tt_get_all_sessions_args, &a);
			break; }
		case TT_DELETE_SESSION: {
			_tt_delete_session_args a;
			a.session.value = (char *)"session";
			sound[i] = encode((xdrproc_t)xdr_tt_delete_session_args, &a);
			break; }
		}
	}

	for (int n = 0; n < iterations; n++) {
		size_t which = fuzz_next() % sound.size();
		std::string req = sound[which];

		switch (fuzz_next() % 4) {
		case 0:				/* some bytes changed */
			for (unsigned k = fuzz_next() % 6; k > 0 && !req.empty(); k--) {
				req[fuzz_next() % req.size()] = (char)fuzz_next();
			}
			break;
		case 1:				/* words changed */
			for (unsigned k = fuzz_next() % 4; k > 0 && req.size() >= 4; k--) {
				size_t at = (fuzz_next() % (req.size() / 4)) * 4;
				static const unsigned vals[] = { 0, 1, 2, 16, 64, 127, 128,
					32767, 65535, 0x7fffffff, 0x80000000u, 0xffffffffu };
				unsigned v = htonl(vals[fuzz_next() % 12]);
				memcpy(&req[at], &v, 4);
			}
			break;
		case 2:				/* cut short */
			req.resize(fuzz_next() % (req.size() + 1));
			break;
		default:			/* both */
			if (req.size() >= 4) {
				size_t at = (fuzz_next() % (req.size() / 4)) * 4;
				unsigned v = htonl(fuzz_next());
				memcpy(&req[at], &v, 4);
			}
			req.resize(fuzz_next() % (req.size() + 1));
			break;
		}
		/* the server must not be asked to remove the file of the case
		   from under the others: no request to close the fixed table */
		if (getenv("TTDB_FUZZ_TRACE")) {
			fprintf(stderr, "%d: procedure %lu, %zu bytes\n", n,
				procs[which], req.size());
		}
		Reply r = rpc(procs[which], req);
		CHECK(r.replied || procs[which] == TT_DELETE_SESSION);
		(void)fuzz_next();
	}
}

/* ---- driver ------------------------------------------------------------------ */

static const struct {
	const char	*name;
	void		(*run)(void);
} cases[] = {
	{ "isfd-range", case_isfd_range },
	{ "isfd-table-full", case_isfd_table_full },
	{ "isfd-nothing-to-close", case_isfd_nothing_to_close },
	{ "isfd-failed-open", case_isfd_failed_open },
	{ "transaction-chain", case_transaction_chain },
	{ "decode-failure-frees", case_decode_failure_frees },
	{ "transaction-stale-fd", case_transaction_stale_fd },
	{ "transaction-log-bounds", case_transaction_log_bounds },
	{ "transaction-log-symlink", case_transaction_log_symlink },
	{ "isbuild-bad-args", case_isbuild_bad_args },
	{ "isopen-bad-path", case_isopen_bad_path },
	{ "null-key", case_null_key },
	{ "iscntl-reply", case_iscntl_reply },
	{ "short-record", case_short_record },
	{ "oid-access-keys", case_oid_access_keys },
	{ "session-requests", case_session_requests },
	{ "sessions-position", case_sessions_position },
	{ "sessions-real-table", case_sessions_real_table },
	{ "key-bytes", case_key_bytes },
	{ "reply-leftovers", case_reply_leftovers },
	{ "empty-paths", case_empty_paths },
	{ "cache-level-value", case_cache_level_value },
	{ "dequeue-sizes", case_dequeue_sizes },
	{ "legacy-table-paths", case_legacy_table_paths },
	{ "legacy-table-links", case_legacy_table_links },
	{ "oid-access-stale-fd", case_oid_access_stale_fd },
	{ "access-rules", case_access_rules },
	{ "access-layouts", case_access_layouts },
	{ "credentials-per-request", case_credentials_per_request },
	{ "object-partitions", case_object_partitions },
	{ "session-polarity", case_session_polarity },
	{ "xdr-null-strings", case_xdr_null_strings },
	{ "xdr-limits", case_xdr_limits },
	{ "xdr-hostile-lengths", case_xdr_hostile_lengths },
	{ "no-mount-point", case_no_mount_point },
	{ "path-names", case_path_names },
	{ "fuzz-dispatch", case_fuzz_dispatch },
};

int
main(int argc, char **argv)
{
	size_t i;

	if (argc == 2 && strcmp(argv[1], "--list") == 0) {
		for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
			puts(cases[i].name);
		}
		return 0;
	}
	if (argc != 2) {
		fprintf(stderr, "usage: requests-test --list | CASE\n");
		return 2;
	}

	signal(SIGPIPE, SIG_IGN);
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		if (strcmp(cases[i].name, argv[1]) == 0) {
			cases[i].run();
			if (failures) {
				fprintf(stderr, "%d check(s) failed in %s\n",
					failures, argv[1]);
				return 1;
			}
			return 0;
		}
	}
	fprintf(stderr, "no such case: %s\n", argv[1]);
	return 2;
}
