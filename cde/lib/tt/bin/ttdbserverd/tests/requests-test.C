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
#include "db/tt_db_access.h"
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

/* The request was understood, whatever its result. */
static bool
answered(const Reply &r)
{
	return r.replied && r.accepted && r.stat == SUCCESS && r.results;
}

static bool
exists(const std::string &path)
{
	struct stat st;

	return lstat(path.c_str(), &st) == 0;
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
	   partition map.  The directory of the case is a partition that maps to
	   itself (a name that maps to the same name would be a loop, which maps
	   to nothing). */
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

	if (iscntl(ALLISFD, ISCNTL_FATAL, isamfatalerror) == -1) {
		exit(2);
	}
	umask(_TT_UMASK);
}

/* ---- cases ------------------------------------------------------------------- */

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

/* A request that does not decode gives back what it had allocated: a list of
   properties that ends in the middle of its last one. */
static void
case_decode_failure_frees(void)
{
#if defined(__SANITIZE_ADDRESS__)
	std::string file = start_partition("decode-failure-frees");
	char name[3][8] = { "first", "second", "third" };
	char value[] = "some value";
	_tt_property_value pv[3];
	_tt_property props[3];
	_tt_access acc = { g_uid, getgid(), 0600 };
	_tt_set_file_props_args args;

	for (int i = 0; i < 3; i++) {
		pv[i].value.value_len = sizeof(value);
		pv[i].value.value_val = value;
		props[i].name = name[i];
		props[i].values.values_len = 1;
		props[i].values.values_val = &pv[i];
	}
	args.file = (char *)file.c_str();
	args.properties.properties_len = 3;
	args.properties.properties_val = props;
	args.access = acc;

	std::string whole = encode((xdrproc_t)xdr_tt_set_file_props_args, &args);
	/* ... as far as the value of the third property */
	std::string cut = whole.substr(0, whole.size() - 28);

	(void)rpc(TT_SET_FILE_PROPS, cut);	/* once, for what is kept */
	size_t before = __sanitizer_get_current_allocated_bytes();
	Reply reply = rpc(TT_SET_FILE_PROPS, cut);
	size_t after = __sanitizer_get_current_allocated_bytes();

	CHECK(reply.replied && reply.accepted && reply.stat == GARBAGE_ARGS);
	CHECK(after == before);
#else
	fprintf(stderr, "not built with AddressSanitizer: nothing to measure\n");
#endif
}

/* The procedures 1 to 24 were those of the servers before this one, which let
   a client work on the tables of the server by their paths.  They are not
   served: the request is answered as one for a procedure that does not exist,
   and nothing is done with its arguments. */
static void
case_legacy_procedures(void)
{
	start_server("legacy-procedures");

	/* an isbuild as the clients of those servers sent it: the path of a
	   table in the directory of the case, its record length, a key (the
	   first 16 bytes), the mode and the least length of a record */
	const std::string table = g_dir + "/TT_DB/oid_access";
	Raw build;

	build.string(table.c_str()).u32(64).u32(1).u32(0).u32(1);
	build.u32(0).u32(16).u32(0);		/* the first part of the key */
	for (int i = 1; i < 8; i++) {
		build.u32(0).u32(0).u32(0);
	}
	build.u32(ISINOUT + ISFIXLEN + ISMANULOCK).u32(64);
	CHECK(mkdir((g_dir + "/TT_DB").c_str(), 0700) == 0);

	for (u_long proc = 1; proc <= 24; proc++) {
		Reply r = rpc(proc, build.s);

		CHECK(r.replied && r.accepted && r.stat == PROC_UNAVAIL);
		r = rpc(proc, "");
		CHECK(r.replied && r.accepted && r.stat == PROC_UNAVAIL);
	}
	CHECK(!exists(table + ".rec"));
	CHECK(!exists(table + ".ind"));
	CHECK(!exists(table + ".var"));

	/* ... and those that never were */
	static const u_long others[] = { 25, 50, 100, 138, 1000, 0xffffffffUL };
	for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
		Reply r = rpc(others[i], "");

		CHECK(r.replied && r.accepted && r.stat == PROC_UNAVAIL);
	}

	/* the procedures of the server answer */
	Reply r = rpc(TT_GET_MIN_AUTH_LEVEL, "");
	CHECK(answered(r));
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

/* The user, the group and the mode of an access go over the wire as 32 bits
   each, -1 as -1, and the routine touches the 12 bytes of the access and no
   others: it used to move each field as a long, which on a 64 bit host is 8
   bytes of a 4 byte field, the last of them beyond the end of the access. */
static void
case_xdr_access(void)
{
	static const struct {
		uid_t	user;
		gid_t	group;
		mode_t	mode;
	} values[] = {
		{ 0, 0, 0 },
		{ 1000, 100, 0640 },
		{ 65534, 65534, 07777 },
		{ (uid_t)-1, (gid_t)-1, (mode_t)-1 },
		{ (uid_t)-1, 5, 0600 },
		{ 5, (gid_t)-1, (mode_t)-1 },
		{ 4000000000U, 4000000001U, 0100644 },
	};

	for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		/* an access in a block of its size, which a sanitizer guards
		   on both sides, and a canary behind one that is not */
		_tt_access *in = (_tt_access *)malloc(sizeof(_tt_access));
		struct {
			_tt_access	access;
			unsigned char	canary[16];
		} out;
		unsigned char wire[64];
		XDR xdrs;

		in->user = values[i].user;
		in->group = values[i].group;
		in->mode = values[i].mode;
		xdrmem_create(&xdrs, (char *)wire, sizeof(wire), XDR_ENCODE);
		CHECK(xdr_tt_access(&xdrs, in));
		CHECK(xdr_getpos(&xdrs) == 12);

		/* three words in network order; -1 is all bits set */
		uint32_t word[3];
		memcpy(word, wire, sizeof(word));
		CHECK(ntohl(word[0]) == (in->user == (uid_t)-1 ? 0xffffffffU : in->user));
		CHECK(ntohl(word[1]) == (in->group == (gid_t)-1 ? 0xffffffffU : in->group));
		CHECK(ntohl(word[2]) == (in->mode == (mode_t)-1 ? 0xffffffffU : in->mode));

		memset(&out, 0x5a, sizeof(out));
		xdrmem_create(&xdrs, (char *)wire, 12, XDR_DECODE);
		CHECK(xdr_tt_access(&xdrs, &out.access));
		CHECK(out.access.user == values[i].user);
		CHECK(out.access.group == values[i].group);
		CHECK(out.access.mode == values[i].mode);
		for (size_t c = 0; c < sizeof(out.canary); c++) {
			CHECK(out.canary[c] == 0x5a);
		}

		/* ... and into a block of its size */
		_tt_access *back = (_tt_access *)malloc(sizeof(_tt_access));
		xdrmem_create(&xdrs, (char *)wire, 12, XDR_DECODE);
		CHECK(xdr_tt_access(&xdrs, back));
		CHECK(back->user == values[i].user && back->group == values[i].group &&
		      back->mode == values[i].mode);
		free(back);
		free(in);
	}
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

	/* the server lists the sessions of the property tables it has open: a
	   table it built is open to the server alone, and is one of them once
	   the server has opened it */
	int fd = cached_isopen((g_dir + "/TT_DB/property_table").c_str(),
			       ISINOUT + ISVARLEN + ISMANULOCK);
	CHECK(fd >= 0);
	CHECK(cached_isclose(fd) == 0);
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

/* ---- what the server keeps of one answer for the next ---------------------------- */

static int get_obj_file(const std::string &objid);

/* The answers of the procedures are in storage that is kept from one request
   to the next, and the dispatcher frees what an answer holds after it was sent.
   A request that fails before it has anything to answer with must not send what
   an earlier one had, which is freed by then: any successful request followed
   by one that fails is what a sanitizer sees. */
static void
case_results_after_failure(void)
{
	std::string file = start_partition("results-after-failure");
	put(g_dir + "/file", "the file");
	create_file(file);
	std::string objid = make_objid(g_mount);
	CHECK(create_object(objid, file, 0600) == TT_DB_OK);
	CHECK(set_property(file, "name", "value") == TT_DB_OK);

	/* an object id of a partition that is none, and a path that is in no
	   partition: every request on them fails before it reaches a table */
	const std::string badobj = "key:NFS:localhost:/not/a/partition";
	const std::string badfile = "localhost:relative/path";
	std::vector<Sound> good = sound_requests(file, objid);
	std::vector<Sound> bad = sound_requests(badfile, badobj);

	CHECK(good.size() == bad.size());
	for (int round = 0; round < 3; round++) {
		for (size_t i = 0; i < good.size() && i < bad.size(); i++) {
			CHECK(good[i].proc == bad[i].proc);
			Reply r = rpc(good[i].proc, good[i].request);
			CHECK(r.replied || good[i].proc == TT_DELETE_SESSION);
			r = rpc(bad[i].proc, bad[i].request);
			CHECK(r.replied || bad[i].proc == TT_DELETE_SESSION);
		}
	}

	/* (the requests above have removed the file and the object) and what
	   the failing requests answer is what they found */
	create_file(file);
	objid = make_objid(g_mount);
	CHECK(create_object(objid, file, 0600) == TT_DB_OK);
	CHECK(get_obj_type(badobj) == TT_DB_ERR_ILLEGAL_OBJECT);
	CHECK(get_obj_type(objid) == TT_DB_OK);
	CHECK(get_obj_file(badobj) == TT_DB_ERR_ILLEGAL_OBJECT);
	CHECK(get_obj_file(objid) == TT_DB_OK);
	CHECK(get_obj_type(badobj) == TT_DB_ERR_ILLEGAL_OBJECT);
	CHECK(get_obj_file(badobj) == TT_DB_ERR_ILLEGAL_OBJECT);
}

/* The names in a request are the sender's: an object id with no key in it,
   one with characters above 127, and a file with no host or no path are
   answered, and nothing else. */
static void
case_malformed_names(void)
{
	std::string file = start_partition("malformed-names");
	if (getuid() == 0) {
		/* a name that is a path in no redirected partition would make a
		   TT_DB where the superuser can */
		fprintf(stderr, "not run as the superuser\n");
		return;
	}
	put(g_dir + "/file", "the file");
	create_file(file);

	const std::string part = g_mount;
	const std::string objids[] = {
		"",
		":",
		"::::",
		"nokey:NFS:localhost:" + part,
		":NFS:localhost:" + part,
		"|||:NFS:localhost:" + part,
		"\x80\xff\xfe|\x81:NFS:localhost:" + part,
		"a|b|c|d:NFS:localhost:" + part,
		std::string(300, '\xe9') + ":NFS:localhost:" + part,
	};
	const std::string files[] = {
		"",
		":",
		"/",
		"localhost:",
		"localhost:/",
		"no-slash",
		"host-without-path:",
		"\xff\xfe:\x80",
	};

	for (size_t o = 0; o < sizeof(objids) / sizeof(objids[0]); o++) {
		for (size_t f = 0; f < sizeof(files) / sizeof(files[0]); f++) {
			std::vector<Sound> v = sound_requests(files[f], objids[o]);

			for (size_t i = 0; i < v.size(); i++) {
				Reply r = rpc(v[i].proc, v[i].request);

				CHECK(r.replied || v[i].proc == TT_DELETE_SESSION);
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

/* Requests that start as sound ones, with some of their bytes changed, and
   cut short: the server answers, or refuses to decode them, but does not
   crash. */
static void
case_fuzz_dispatch(void)
{
	int iterations = getenv("TTDB_FUZZ_ITERATIONS") ?
		atoi(getenv("TTDB_FUZZ_ITERATIONS")) : 3000;

	if (getenv("TTDB_FUZZ_SEED")) {
		fuzz_state ^= strtoull(getenv("TTDB_FUZZ_SEED"), NULL, 10) * 2654435761ULL;
	}
	std::string file = start_partition("fuzz-dispatch");
	if (getuid() == 0) {
		/* names that a change turns into others would make a TT_DB
		   where the superuser can */
		fprintf(stderr, "not run as the superuser\n");
		return;
	}
	put(g_dir + "/file", "the file");
	std::string objid = make_objid(g_mount);
	create_file(file);
	CHECK(create_object(objid, file, 0600) == TT_DB_OK);

	std::vector<Sound> sound = sound_requests(file, objid);
	char oid[TT_DB_KEY_LENGTH];
	memset(oid, 'o', sizeof(oid));
	_tt_get_all_sessions_args sessions;
	memset(&sessions, 0, sizeof(sessions));
	sessions.oidkey.oidkey_len = sizeof(oid);
	sessions.oidkey.oidkey_val = oid;
	Sound all = { TT_GET_ALL_SESSIONS,
		      encode((xdrproc_t)xdr_tt_get_all_sessions_args, &sessions) };
	sound.push_back(all);

	for (int n = 0; n < iterations; n++) {
		size_t which = fuzz_next() % sound.size();
		std::string req = sound[which].request;

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
		if (getenv("TTDB_FUZZ_TRACE")) {
			fprintf(stderr, "%d: procedure %lu, %zu bytes\n", n,
				sound[which].proc, req.size());
		}
		Reply r = rpc(sound[which].proc, req);
		CHECK(r.replied || sound[which].proc == TT_DELETE_SESSION);
		(void)fuzz_next();
	}
}

/* ---- the access rule of the new interface --------------------------------------- */

/* What a Unix file system does.  The owner (and the superuser) may do
   anything; anybody else is in one class only, the group if his group is the
   group of the file and the others if not, and a read needs the read bit of
   that class and a write the write bit, whatever the bits of the other
   classes say. */
static bool
unix_permits(unsigned mode, int klass, bool write)
{
	if (klass == 0) {
		return true;
	}
	return (mode & ((klass == 1 ? 040 : 04) >> (write ? 1 : 0))) != 0;
}

/* The rule on its own: every combination of the nine permission bits, every
   kind of caller and of record (an owner or a group that is -1 stands for
   everybody), a read and a write, and a request only the owner may make. */
static void
case_access_rule(void)
{
	struct Caller3 {
		uid_t	user;
		gid_t	group;
	};
	const uid_t owner = 100;
	const gid_t group = 200;
	/* the class each is in for a record of (owner, group) */
	static const struct { Caller3 who; int klass; } callers[] = {
		{ { 0, 7 }, 0 },		/* the superuser */
		{ { owner, 7 }, 0 },		/* the owner, in another group */
		{ { owner, group }, 0 },	/* the owner, in the group */
		{ { owner + 1, group }, 1 },	/* a member of the group */
		{ { owner + 1, group + 1 }, 2 },	/* somebody else */
		{ { owner + 1, (gid_t)-1 }, 2 },	/* ... with no valid group */
	};
	long checked = 0;

	for (unsigned mode = 0; mode <= 0777; mode++) {
		for (int variant = 0; variant < 3; variant++) {
			/* 0: a user and a group; 1: the group is everybody's;
			   2: the owner is everybody */
			_Tt_db_access record;
			record.user = variant == 2 ? (uid_t)-1 : owner;
			record.group = variant == 1 ? (gid_t)-1 : group;
			record.mode = mode;

			for (size_t c = 0; c < sizeof(callers) / sizeof(callers[0]); c++) {
				_Tt_db_access caller;
				int klass = callers[c].klass;

				caller.user = callers[c].who.user;
				caller.group = callers[c].who.group;
				if (variant == 2 && klass != 0) {
					klass = 0;
				}
				else if (variant == 1 && klass == 2) {
					klass = 1;
				}
				for (int write = 0; write < 2; write++) {
					for (int only = 0; only < 2; only++) {
						bool want = klass == 0 ? true :
							    only ? false :
							    unix_permits(mode, klass, write);
						bool got = record.permits(caller, write, only);

						if (got != want) {
							fprintf(stderr, "  mode %04o record %d caller %u/%u "
								"%s%s: %s, should be %s\n",
								mode, variant, (unsigned)caller.user,
								(unsigned)caller.group,
								write ? "write" : "read",
								only ? " by the owner only" : "",
								got ? "allowed" : "denied",
								want ? "allowed" : "denied");
						}
						CHECK(got == want);
						checked++;
					}
				}
			}
		}
	}

	/* a record with no mode lets everybody in, except for what is the
	   owner's alone */
	_Tt_db_access open, stranger;

	stranger.user = 5;
	stranger.group = 6;
	CHECK(open.mode == (mode_t)-1);
	CHECK(open.permits(stranger, false, false));
	CHECK(open.permits(stranger, true, false));
	open.user = 1;
	open.group = 2;
	CHECK(open.permits(stranger, false, false));
	CHECK(open.permits(stranger, true, false));
	CHECK(!open.permits(stranger, true, true));
	CHECK(checked == 512 * 3 * 6 * 4);
}

/* ---- ... and what the requests of the new interface get of it ---- */

template <class R>
static int
results_of(const Reply &r, bool_t (*proc)(XDR *, R *))
{
	R	res;
	XDR	xdrs;

	if (!answered(r)) {
		return INT_MIN;
	}
	memset(&res, 0, sizeof(res));
	xdrmem_create(&xdrs, (char *)r.body.data(), r.body.size(), XDR_DECODE);
	if (!(*proc)(&xdrs, &res)) {
		return INT_MIN;
	}
	int out = res.results;
	xdr_free((xdrproc_t)proc, (char *)&res);
	return out;
}

/* ... and a reply that is nothing but the result */
static int
status_of(const Reply &r)
{
	_tt_db_results	res;
	XDR		xdrs;

	if (!answered(r)) {
		return INT_MIN;
	}
	xdrmem_create(&xdrs, (char *)r.body.data(), r.body.size(), XDR_DECODE);
	if (!xdr_tt_db_results(&xdrs, &res)) {
		return INT_MIN;
	}
	return (int)res;
}

/* who is asking: the credentials of the request win over the access in it,
   which is the caller's own anyway */
static _tt_access
acting(void)
{
	_tt_access acc = { g_uid, g_gids.empty() ? getgid() : g_gids[0], 0 };

	return acc;
}

static int
set_obj_prop(const std::string &objid, const char *name, const char *value)
{
	std::string	nm = name, val = value;
	_tt_property_value pv;
	_tt_set_obj_prop_args sp;

	pv.value.value_len = val.size();
	pv.value.value_val = &val[0];
	memset(&sp, 0, sizeof(sp));
	sp.objid = (char *)objid.c_str();
	sp.property.name = &nm[0];
	sp.property.values.values_len = 1;
	sp.property.values.values_val = &pv;
	sp.access = acting();
	sp.cache_level = INT_MAX;	/* nobody has updated since this caller read */
	return results_of(rpc(TT_SET_OBJ_PROP,
			      encode((xdrproc_t)xdr_tt_set_obj_prop_args, &sp)),
			  xdr_tt_obj_props_results);
}

static int
get_obj_prop(const std::string &objid, const char *name)
{
	std::string	nm = name;
	_tt_get_obj_prop_args gp = { (char *)objid.c_str(), &nm[0], acting(), -1 };

	return results_of(rpc(TT_GET_OBJ_PROP,
			      encode((xdrproc_t)xdr_tt_get_obj_prop_args, &gp)),
			  xdr_tt_obj_prop_results);
}

static int
set_obj_file(const std::string &objid, const std::string &file)
{
	_tt_set_obj_file_args sf = { (char *)objid.c_str(), (char *)file.c_str(),
				     acting() };

	return status_of(rpc(TT_SET_OBJ_FILE,
			     encode((xdrproc_t)xdr_tt_set_obj_file_args, &sf)));
}

static int
set_obj_type(const std::string &objid, const char *otype)
{
	std::string	ot = otype;
	_tt_set_obj_type_args st = { (char *)objid.c_str(), &ot[0], acting() };

	return status_of(rpc(TT_SET_OBJ_TYPE,
			     encode((xdrproc_t)xdr_tt_set_obj_type_args, &st)));
}

static int
get_obj_file(const std::string &objid)
{
	_tt_get_obj_file_args gf = { (char *)objid.c_str(), acting() };

	return results_of(rpc(TT_GET_OBJ_FILE,
			      encode((xdrproc_t)xdr_tt_get_obj_file_args, &gf)),
			  xdr_tt_obj_file_results);
}

static int
get_file_prop(const std::string &file, const char *name)
{
	std::string	nm = name;
	_tt_get_file_prop_args gp = { (char *)file.c_str(), &nm[0], acting(), -1 };

	return results_of(rpc(TT_GET_FILE_PROP,
			      encode((xdrproc_t)xdr_tt_get_file_prop_args, &gp)),
			  xdr_tt_file_prop_results);
}

static int
set_file_access(const std::string &file, uid_t user, gid_t group, unsigned mode)
{
	_tt_access	now = { user, group, mode };
	_tt_set_file_access_args sa = { (char *)file.c_str(), now, acting() };

	return status_of(rpc(TT_SET_FILE_ACCESS,
			     encode((xdrproc_t)xdr_tt_set_file_access_args, &sa)));
}

static int
set_obj_access(const std::string &objid, uid_t user, gid_t group, unsigned mode)
{
	_tt_access	now = { user, group, mode };
	_tt_set_obj_access_args sa = { (char *)objid.c_str(), now, acting() };

	return status_of(rpc(TT_SET_OBJ_ACCESS,
			     encode((xdrproc_t)xdr_tt_set_obj_access_args, &sa)));
}

/* The answer a request on something with this mode has to get: the request
   is let through, or it is refused as TT_DB_ERR_ACCESS_DENIED.  A request
   that comes with the cache level of the properties reads the level first,
   and is refused as TT_DB_ERR_PROPS_CACHE_ERROR when that cannot be read. */
static void
expect_access(const char *what, const char *who, unsigned mode, int got,
	      bool allowed, int also_denied = TT_DB_ERR_ACCESS_DENIED)
{
	bool ok = allowed ? got == TT_DB_OK :
		  (got == TT_DB_ERR_ACCESS_DENIED || got == also_denied);

	if (!ok) {
		fprintf(stderr, "  %s by %s, mode %04o: the server says %d, "
			"should be %s\n", what, who, mode, got,
			allowed ? "allowed" : "denied");
	}
	CHECK(ok);
}

static unsigned
mode_of(unsigned bits)
{
	return 0600 | ((bits & 1) ? 040 : 0) | ((bits & 2) ? 020 : 0) |
	       ((bits & 4) ? 04 : 0) | ((bits & 8) ? 02 : 0);
}

/* Everything a request of the new interface does to a file or an object is
   a read or a write, and each gets what the bits of the class of the user
   give: the read bit lets a user read and not write, the write bit write and
   not read, and neither the one of the other class.  Every combination of
   the group and the other bits, for the file and for an object, as the owner,
   a member of the group and somebody else.  (A property travels with a cache
   level that is read first, so the write of a property needs both.) */
static void
case_access_bits(void)
{
	const std::string netfile = start_partition("access-bits");
	const std::string netfile2 = "localhost:" + g_dir + "/file2";
	const std::string file = g_dir + "/file", file2 = g_dir + "/file2";
	const uid_t owner = getuid() ? getuid() : 4000;
	const gid_t group = getuid() ? getgid() : 4000;
	struct { const char *name; int klass; uid_t uid; gid_t gid; } users[] = {
		{ "the owner", 0, owner, group },
		{ "a member of the group", 1, owner + 1, group },
		{ "somebody else", 2, owner + 2, group + 1 },
	};
	const size_t nusers = sizeof(users) / sizeof(users[0]);
	const int cache = TT_DB_ERR_PROPS_CACHE_ERROR;

	put(file, "the file");
	put(file2, "the second file");
	if (getuid() == 0) {
		CHECK(chown(file.c_str(), owner, group) == 0);
		CHECK(chown(file2.c_str(), owner, group) == 0);
	}
	{
		Caller as(owner, group);
		create_file(netfile);
		create_file(netfile2);
	}

	/* ---- the objects of a file: a mode of the object for each combination */
	std::string objects[16];

	for (unsigned bits = 0; bits < 16; bits++) {
		Caller as(owner, group);

		objects[bits] = make_objid(g_mount);
		CHECK(create_object(objects[bits], netfile, mode_of(bits)) == TT_DB_OK);
		CHECK(set_obj_prop(objects[bits], "p", "value") == TT_DB_OK);
	}
	for (unsigned bits = 0; bits < 16; bits++) {
		unsigned mode = mode_of(bits);

		for (size_t u = 0; u < nusers; u++) {
			Caller as(users[u].uid, users[u].gid);
			bool r = unix_permits(mode, users[u].klass, false);
			bool w = unix_permits(mode, users[u].klass, true);

			expect_access("read of a property", users[u].name, mode,
				      get_obj_prop(objects[bits], "p"), r, cache);
			expect_access("write of a property", users[u].name, mode,
				      set_obj_prop(objects[bits], "q", "new"),
				      r && w, cache);
			expect_access("change of the type", users[u].name, mode,
				      set_obj_type(objects[bits], "otype"), w);
			expect_access("read of the file", users[u].name, mode,
				      get_obj_file(objects[bits]), r);
			expect_access("move to another file", users[u].name, mode,
				      set_obj_file(objects[bits], netfile), w);
		}
	}

	/* only the owner changes the access: a write bit is not that */
	{
		Caller as(owner + 1, group);
		expect_access("change of the access", "a member of the group", 0660,
			      set_obj_access(objects[3], owner + 1, group, 0666), false);
	}
	{
		Caller as(owner, group);
		expect_access("change of the access", "the owner", 0660,
			      set_obj_access(objects[3], owner, group, 0640), true);
	}
	{
		/* ... and the mode it has now is that of a read-only group */
		Caller as(owner + 1, group);
		expect_access("change of the type after that", "a member of the group",
			      0640, set_obj_type(objects[3], "otype"), false);
		expect_access("read of the file after that", "a member of the group",
			      0640, get_obj_file(objects[3]), true);
	}

	/* ---- the file: the same, for the properties of it and for what is
	   put into it */
	{
		Caller as(owner, group);
		CHECK(set_property(netfile2, "p", "value") == TT_DB_OK);
	}
	for (unsigned bits = 0; bits < 16; bits++) {
		unsigned mode = mode_of(bits);

		{
			Caller as(owner, group);
			CHECK(set_file_access(netfile2, owner, group, mode) == TT_DB_OK);
		}
		for (size_t u = 0; u < nusers; u++) {
			Caller as(users[u].uid, users[u].gid);
			bool r = unix_permits(mode, users[u].klass, false);
			bool w = unix_permits(mode, users[u].klass, true);

			expect_access("read of a property of the file", users[u].name,
				      mode, get_file_prop(netfile2, "p"), r, cache);
			/* the cache level of the file is counted up after the
			   write, which reads it: a user who may write and not
			   read has the property written and an error to show
			   for it */
			int wrote = set_property(netfile2, "q", "new");
			if (!w || r) {
				expect_access("write of a property of the file",
					      users[u].name, mode, wrote, w, cache);
			}
			/* an object for the file, and one moved into it: both
			   are additions to the file.  The user is the owner of
			   the object he moves, which is in the first file. */
			expect_access("creation of an object", users[u].name, mode,
				      create_object(make_objid(g_mount), netfile2, 0600), w);
			std::string mine = make_objid(g_mount);
			CHECK(create_object(mine, netfile, 0600) == TT_DB_OK);
			expect_access("move of an object into the file", users[u].name,
				      mode, set_obj_file(mine, netfile2), w);
		}
	}

	/* the mode of a file is for its owner to change */
	{
		Caller as(owner + 1, group);
		expect_access("change of the access of the file",
			      "a member of the group", 0666,
			      set_file_access(netfile2, owner + 1, group, 0666), false);
	}

	/* the server run with -n lets anybody read and write, and leaves what
	   is the owner's alone to the owner */
	{
		Caller as(owner + 2, group + 1);
		const unsigned mode = mode_of(0);	/* 0600: nothing to others */

		expect_access("read", "somebody else", mode,
			      get_obj_file(objects[0]), false);
		access_checking = 0;
		expect_access("read with -n", "somebody else", mode,
			      get_obj_file(objects[0]), true);
		expect_access("write with -n", "somebody else", mode,
			      set_obj_type(objects[0], "otype"), true);
		expect_access("change of the access with -n", "somebody else", mode,
			      set_obj_access(objects[0], owner, group, 0666), false);
		access_checking = 1;
		expect_access("read again", "somebody else", mode,
			      get_obj_file(objects[0]), false);
	}
}

/* ---- driver ------------------------------------------------------------------ */

static const struct {
	const char	*name;
	void		(*run)(void);
} cases[] = {
	{ "decode-failure-frees", case_decode_failure_frees },
	{ "legacy-procedures", case_legacy_procedures },
	{ "sessions-real-table", case_sessions_real_table },
	{ "key-bytes", case_key_bytes },
	{ "cache-level-value", case_cache_level_value },
	{ "dequeue-sizes", case_dequeue_sizes },
	{ "empty-paths", case_empty_paths },
	{ "credentials-per-request", case_credentials_per_request },
	{ "object-partitions", case_object_partitions },
	{ "xdr-null-strings", case_xdr_null_strings },
	{ "xdr-access", case_xdr_access },
	{ "xdr-limits", case_xdr_limits },
	{ "xdr-hostile-lengths", case_xdr_hostile_lengths },
	{ "no-mount-point", case_no_mount_point },
	{ "path-names", case_path_names },
	{ "results-after-failure", case_results_after_failure },
	{ "malformed-names", case_malformed_names },
	{ "fuzz-dispatch", case_fuzz_dispatch },
	{ "access-rule", case_access_rule },
	{ "access-bits", case_access_bits },
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
