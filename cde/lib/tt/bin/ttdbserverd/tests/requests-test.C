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
#include "util/tt_xdr_utils.h"

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
static SVCXPRT		g_xprt;
static bool		g_encode_results = true;

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
	if (!g_encode_results) {
		/* some replies carry a null string, which xdr_string() cannot
		   send: a case that does not look at the reply says so */
		g_reply.results = true;
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
	gid_t			gids[1];
	struct svc_req		rqst;

	gids[0] = getgid();
	memset(&cred, 0, sizeof(cred));
	cred.aup_uid = g_uid;
	cred.aup_gid = gids[0];
	cred.aup_len = 1;
	cred.aup_gids = gids;
	memset(&rqst, 0, sizeof(rqst));
	rqst.rq_prog = TT_DBSERVER_PROG;
	rqst.rq_vers = TT_DBSERVER_VERS;
	rqst.rq_proc = proc;
	rqst.rq_cred.oa_flavor = AUTH_UNIX;
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
	g_xprt.xp_ops = &fake_ops;

	_tt_global = new _Tt_global;
	db_pr_map = new _Tt_db_partition_redirection_map;
	_tt_auth_level = AUTH_UNIX;

	_tt_oid_keydesc.k_flags = ISNODUPS;
	_tt_oid_keydesc.k_nparts = 1;
	_tt_oid_keydesc.k_part[0].kp_start = 0;
	_tt_oid_keydesc.k_part[0].kp_leng = OID_KEY_LENGTH;
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
	k.k_part[0].kp_leng = OID_KEY_LENGTH;
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
	args.isreclen = fixed ? fixed_reclen : OID_KEY_LENGTH + MAX_PROP_LENGTH;
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
	for (int i = 0; i < OID_KEY_LENGTH && (unsigned)i < len; i++) {
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

	char key[OID_KEY_LENGTH] = { 0 };

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
	char oid[OID_KEY_LENGTH];

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

		_Tt_isstart_args sa = { isfd, &key, OID_KEY_LENGTH,
					{ (u_int)rec.size(), (char *)rec.data() },
					ISEQUAL };
		r = rpc(_TT_ISSTART, encode((xdrproc_t)xdr_Tt_isstart_args, &sa));
		CHECK(reply_result(r) == -1);

		_Tt_iswrite_args ia = { isfd,
					{ (u_int)rec.size(), (char *)rec.data() } };
		r = rpc(_TT_ISWRITE, encode((xdrproc_t)xdr_Tt_iswrite_args, &ia));
		CHECK(reply_result(r) == -1);

		_Tt_test_and_set_args ta = { isfd, &key, OID_KEY_LENGTH,
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
		ss.key_len = OID_KEY_LENGTH;
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
		sp.key_len = OID_KEY_LENGTH;
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
	   one table in each of as many directories as there are entries */
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

	_Tt_isstart_args sa = { isfd, NULL, OID_KEY_LENGTH,
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

	_Tt_test_and_set_args ta = { isfd, NULL, OID_KEY_LENGTH,
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
	_Tt_isstart_args ea = { isfd, &key, OID_KEY_LENGTH, { 0, NULL }, ISEQUAL };
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
		for (unsigned len = 0; len <= OID_KEY_LENGTH; len += 5) {
			char key[OID_KEY_LENGTH + 1];
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
	char oid[OID_KEY_LENGTH];
	memset(oid, 'o', sizeof(oid));
	char session[ISMAXRECLEN + 8000];
	const unsigned maxsession = ISMAXRECLEN - OID_KEY_LENGTH - MAX_PROP_LENGTH;
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
			{ OID_KEY_LENGTH, &key, sizeof(oid), maxsession + 1 },
			{ OID_KEY_LENGTH, &key, sizeof(oid), sizeof(session) },
			{ -1, &key, sizeof(oid), 4 },
			{ 10000, &key, sizeof(oid), 4 },
			{ (int)sizeof(key) + 1, &key, sizeof(oid), 4 },
			{ OID_KEY_LENGTH, NULL, sizeof(oid), 4 },
			{ OID_KEY_LENGTH, &key, 0, 4 },
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
				raw.u32(OID_KEY_LENGTH);
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
	char pos[OID_KEY_LENGTH];
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
	_Tt_test_and_set_args ta = { _TT_MAX_ISFD - 1, &key, OID_KEY_LENGTH,
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
	fclose(f);
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
	g_encode_results = false;
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
	char oid[OID_KEY_LENGTH];
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
			_Tt_isstart_args a = { isfd, &key, OID_KEY_LENGTH,
					       { 16, (char *)rec.data() }, ISEQUAL };
			sound[i] = encode((xdrproc_t)xdr_Tt_isstart_args, &a);
			break; }
		case _TT_ISWRITE: {
			_Tt_iswrite_args a = { isfd, { 30, (char *)rec.data() } };
			sound[i] = encode((xdrproc_t)xdr_Tt_iswrite_args, &a);
			break; }
		case _TT_TEST_AND_SET: {
			_Tt_test_and_set_args a = { isfd, &key, OID_KEY_LENGTH,
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
			_Tt_oidaccess_args a = { isfd, { OID_KEY_LENGTH, oid }, 5 };
			sound[i] = encode((xdrproc_t)xdr_Tt_oidaccess_args, &a);
			break; }
		case _TT_ADDSESSION:
		case _TT_DELSESSION: {
			_Tt_session_args a;
			memset(&a, 0, sizeof(a));
			a.isfd = var;
			a.key = &key;
			a.key_len = OID_KEY_LENGTH;
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
			a.key_len = OID_KEY_LENGTH;
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
	{ "reply-leftovers", case_reply_leftovers },
	{ "empty-paths", case_empty_paths },
	{ "cache-level-value", case_cache_level_value },
	{ "dequeue-sizes", case_dequeue_sizes },
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
