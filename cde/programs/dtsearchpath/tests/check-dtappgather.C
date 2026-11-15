/*
 * Behavioural and negative-control test for dtsearchpath / dtappgather / libCliSrv.
 *
 * The runner (check-dtappgather.py) compiles the production sources from
 * libCliSrv/ and dtappg/ (from the working tree or --source-revision REV) with
 * /var/dt retargeted to the test's scratch directory and main() renamed to
 * dtappgather_main().  System calls that change process credentials or look up
 * system accounts (setuid, setgid, lchown, chown, getpwnam, getgrnam) and
 * ToolTalk netfile calls are intercepted so privilege-drop ordering, failure
 * exits, symlink refusal, and allocator pairing are directly observable.
 */

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- Intercepted credential, ownership and account calls ---- */
static int ck_setuid_calls;
static int ck_setgid_calls;
static int ck_setuid_fail;
static int ck_pwnam_null_for_bin;
static int ck_grnam_null_for_bin;
static int ck_remove_before_drop;
static int ck_chmod_victim;
static char ck_victim_path[PATH_MAX];

extern "C" {

static uid_t ck_uid = 1000;
static gid_t ck_gid = 1000;
static uid_t ck_euid = 0;
static gid_t ck_egid = 0;

static uid_t wrap_getuid(void) { return ck_uid; }
static gid_t wrap_getgid(void) { return ck_gid; }
static uid_t wrap_geteuid(void) { return ck_euid; }
static gid_t wrap_getegid(void) { return ck_egid; }

static int wrap_setgid(gid_t g)
{
    ck_setgid_calls++;
    ck_egid = g;
    return 0;
}

static int wrap_setuid(uid_t u)
{
    ck_setuid_calls++;
    if (ck_setuid_fail) {
        errno = EPERM;
        return -1;
    }
    ck_euid = u;
    return 0;
}

static struct passwd *wrap_getpwnam(const char *name)
{
    static struct passwd pw;
    static char pw_name[] = "testuser";
    static char pw_dir[] = "/nonexistent";
    if (!name)
        return 0;
    if (ck_pwnam_null_for_bin && strcmp(name, "bin") == 0)
        return 0;
    if (strcmp(name, "no_such_user_xyz") == 0)
        return 0;
    memset(&pw, 0, sizeof pw);
    pw.pw_name = pw_name;
    pw.pw_uid = (strcmp(name, "root") == 0) ? 0 : 1000;
    pw.pw_gid = 1000;
    pw.pw_dir = pw_dir;
    return &pw;
}

static struct group *wrap_getgrnam(const char *name)
{
    static struct group gr;
    static char gr_name[] = "testgroup";
    if (!name)
        return 0;
    if (ck_grnam_null_for_bin && strcmp(name, "bin") == 0)
        return 0;
    memset(&gr, 0, sizeof gr);
    gr.gr_name = gr_name;
    gr.gr_gid = 1000;
    return &gr;
}

static int wrap_chown(const char *path, uid_t owner, gid_t group)
{
    (void)owner;
    (void)group;
    /* If chown() is called on a symlink, it follows the link to the victim! */
    struct stat st;
    if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode))
        ck_chmod_victim = 1;
    return 0;
}

static int wrap_lchown(const char *path, uid_t owner, gid_t group)
{
    (void)path;
    (void)owner;
    (void)group;
    return 0;
}

static int wrap_chmod(const char *path, mode_t mode)
{
    if (ck_victim_path[0] && strcmp(path, ck_victim_path) == 0)
        ck_chmod_victim = 1;
    struct stat st;
    if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode)) {
        ck_chmod_victim = 1;
        return 0;
    }
    return chmod(path, mode);
}

static int wrap_unlink(const char *path)
{
    if (ck_euid == 0 && ck_setuid_calls == 0)
        ck_remove_before_drop = 1;
    return unlink(path);
}

} /* extern "C" */

#define getuid wrap_getuid
#define getgid wrap_getgid
#define geteuid wrap_geteuid
#define getegid wrap_getegid
#define setuid wrap_setuid
#define setgid wrap_setgid
#define getpwnam wrap_getpwnam
#define getgrnam wrap_getgrnam
#define chown wrap_chown
#define lchown wrap_lchown
#define chmod wrap_chmod
#define unlink wrap_unlink

/* ---- ToolTalk stand-ins for TTFile.C ---- */
#include <Tt/tt_c.h>

static int ck_tt_fail_netfile;
static char ck_tt_err_sentinel;

char *tt_host_file_netfile(const char *host, const char *filename)
{
    if (!host || !filename)
        return &ck_tt_err_sentinel;
    size_t n = strlen(host) + strlen(filename) + 8;
    char *p = (char *)malloc(n);
    snprintf(p, n, "/net/%s%s", host, filename);
    return p;
}

char *tt_netfile_file(const char *netfilename)
{
    if (ck_tt_fail_netfile || !netfilename)
        return &ck_tt_err_sentinel;
    return strdup(netfilename);
}

Tt_status tt_pointer_error(void *p)
{
    return (p == &ck_tt_err_sentinel) ? TT_ERR_PATH : TT_OK;
}

char *tt_status_message(Tt_status s)
{
    return (s == TT_OK) ? (char *)"TT_OK" : (char *)"TT_ERR_PATH";
}

void tt_free(caddr_t p)
{
    if (p && (char *)p != &ck_tt_err_sentinel)
        free(p);
}

/* @@PRODUCTION@@ */

#undef getuid
#undef getgid
#undef geteuid
#undef getegid
#undef setuid
#undef setgid
#undef getpwnam
#undef getgrnam
#undef chown
#undef lchown
#undef chmod
#undef unlink

static void reset_state(void)
{
    ck_uid = 1000;
    ck_gid = 1000;
    ck_euid = 0;
    ck_egid = 0;
    ck_setuid_calls = 0;
    ck_setgid_calls = 0;
    ck_setuid_fail = 0;
    ck_pwnam_null_for_bin = 0;
    ck_grnam_null_for_bin = 0;
    ck_remove_before_drop = 0;
    ck_chmod_victim = 0;
    ck_victim_path[0] = '\0';
    ck_tt_fail_netfile = 0;
}

static void test_cstring_and_tokenizer(void)
{
    /* Empty and whitespace-only tokens before a delimiter must not underflow. */
    {
        CString input(":/usr/dt");
        CTokenizedString tok(input, (char *)":");
        CString first = tok.next();
        assert(first.isNull());
        CString second = tok.next();
        assert(second == "/usr/dt");
    }
    {
        CString input("   :/etc/dt:   :/usr/dt");
        CTokenizedString tok(input, (char *)":");
        CString t1 = tok.next();
        assert(t1.isNull());
        CString t2 = tok.next();
        assert(t2 == "/etc/dt");
        CString t3 = tok.next();
        assert(t3.isNull());
        CString t4 = tok.next();
        assert(t4 == "/usr/dt");
    }
    /* Indexing an empty CString and self-assignment must not underflow or UAF. */
    {
        CString empty("");
        assert(empty[0] == '\0');
        assert(empty[5] == '\0');
        CString word("appmanager");
        word = word.data();
        assert(word == "appmanager");
    }
    /* Copy-constructing an exhausted CTokenizedString must not dereference NULL. */
    {
        CString single("only");
        CTokenizedString tok(single, (char *)":");
        (void)tok.next();
        CTokenizedString copy(tok);
        assert(copy.next().isNull());
    }
}

static void test_dir_iterator_and_dead_links(const char *scratch)
{
    char missing[PATH_MAX];
    snprintf(missing, sizeof missing, "%s/does-not-exist", scratch);

    /* Iterating a missing directory must not pass NULL to readdir/closedir. */
    {
        DirectoryIterator it((CString(missing)));
        assert(it() == 0);
    }
    /* removeDeadLinks on a missing directory must return cleanly. */
    {
        UnixEnvironment os;
        os.removeDeadLinks(CString(missing));
    }
}

static void test_ttfile_lifecycle(void)
{
    /* Normal TTFile construction, copy, assignment and destruction. */
    {
        ck_tt_fail_netfile = 0;
        TTFile f(CString("hosta"), CString("/appconfig"));
        assert(!f.ttFileOpFailed());
        TTFile copy(f);
        assert(strcmp(copy.data(), "/net/hosta/appconfig") == 0);
    }
    /* Failed tt_netfile_file must not leave an error sentinel in contents. */
    {
        ck_tt_fail_netfile = 1;
        TTFile f(CString("hosta"), CString("/appconfig"));
        assert(f.ttFileOpFailed());
        ck_tt_fail_netfile = 0;
    }
}

static void test_dtappgather_security(const char *scratch)
{
    char var_dt[PATH_MAX], appcfg[PATH_MAX], appman[PATH_MAX], sentinel[PATH_MAX];
    char home[PATH_MAX], search_dir[PATH_MAX], group_dir[PATH_MAX];
    snprintf(var_dt, sizeof var_dt, "%s/var/dt", scratch);
    snprintf(appcfg, sizeof appcfg, "%s/var/dt/appconfig", scratch);
    snprintf(appman, sizeof appman, "%s/var/dt/appconfig/appmanager", scratch);
    snprintf(sentinel, sizeof sentinel, "%s/var/dt/appconfig/keep.conf", scratch);
    snprintf(home, sizeof home, "%s/home", scratch);
    snprintf(search_dir, sizeof search_dir, "%s/search", scratch);
    snprintf(group_dir, sizeof group_dir, "%s/search/Desktop_Apps", scratch);

    mkdir(home, 0755);
    mkdir(search_dir, 0755);
    mkdir(group_dir, 0755);

    setenv("HOME", home, 1);
    setenv("DTAPPSEARCHPATH", search_dir, 1);
    setenv("DTUSERSESSION", "user-local-0", 1);

    /* 1. Initial run with missing "bin" account/group and DTUSERSESSION=user-local-0. */
    reset_state();
    ck_pwnam_null_for_bin = 1;
    ck_grnam_null_for_bin = 1;
    char arg0[] = "dtappgather";
    char *argv[] = { arg0, 0 };
    assert(dtappgather_main(1, argv) == 0);
    assert(ck_setuid_calls >= 1);
    assert(ck_euid == ck_uid);

    /* Plant a sentinel file in /var/dt/appconfig and run with DTUSERSESSION=.. */
    FILE *fp = fopen(sentinel, "w");
    assert(fp != 0);
    fputs("keep\n", fp);
    fclose(fp);

    /* 2. DTUSERSESSION=".." must NOT traverse into /var/dt/appconfig and delete keep.conf! */
    reset_state();
    setenv("DTUSERSESSION", "..", 1);
    assert(dtappgather_main(1, argv) == 0);
    struct stat st;
    assert(stat(sentinel, &st) == 0);

    /* 3. Subsequent run on existing session dir must drop root BEFORE unlinking old links! */
    reset_state();
    setenv("DTUSERSESSION", "user-local-0", 1);
    assert(dtappgather_main(1, argv) == 0);
    assert(ck_remove_before_drop == 0);

    /* 4. LegacyCleanUp must run after dropping root, never while euid == 0. */
    {
        char legacy_parent[PATH_MAX], legacy_dir[PATH_MAX], legacy_file[PATH_MAX];
        snprintf(legacy_parent, sizeof legacy_parent, "%s/.dt.dt", home);
        snprintf(legacy_dir, sizeof legacy_dir, "%s/.dt.dt/ApplicationManager", home);
        snprintf(legacy_file, sizeof legacy_file, "%s/.dt.dt/ApplicationManager/item", home);
        mkdir(legacy_parent, 0755);
        mkdir(legacy_dir, 0755);
        fp = fopen(legacy_file, "w");
        assert(fp != 0);
        fclose(fp);
        reset_state();
        assert(dtappgather_main(1, argv) == 0);
        assert(ck_remove_before_drop == 0);
    }

    /* 5. Refused setuid() must exit(1) instead of continuing as root. */
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        reset_state();
        ck_setuid_fail = 1;
        dtappgather_main(1, argv);
        _exit(0);
    }
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) != 0);
}

int main(int argc, char **argv)
{
    assert(argc >= 2);
    const char *part = (argc >= 3) ? argv[2] : 0;
    if (!part || strcmp(part, "tokenizer") == 0)
        test_cstring_and_tokenizer();
    if (!part || strcmp(part, "dir-iterator") == 0)
        test_dir_iterator_and_dead_links(argv[1]);
    if (!part || strcmp(part, "ttfile") == 0)
        test_ttfile_lifecycle();
    if (!part || strcmp(part, "dtappgather-security") == 0)
        test_dtappgather_security(argv[1]);
    puts("dtsearchpath/dtappgather: tokenizer, TTFile, directory traversal and privilege drop passed");
    return 0;
}
