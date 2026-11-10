// Regression tests for the files libDtMail creates and the paths it follows
// when it opens and saves a mailbox.  The production Session.C and
// RFCMailBox.C are compiled next to this program with the sanitizers and
// linked with the rest of the built libDtMail; every case runs in its own
// process (see check-mailbox-files.py) in an empty directory named by
// DTMAIL_TEST_DIR, which is also HOME.

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <DtMail/DtMail.hh>
#include <DtMail/DtMailError.hh>

// The session of the mailer opens a ToolTalk connection when it is made; these
// stand-ins give it one, so that no session server has to run for the test.
extern "C" {
char *tt_default_procid(void) { static char id[] = "mailbox-files"; return id; }
Tt_status tt_pointer_error(void *) { return TT_OK; }
int tt_fd(void) { return -1; }
}

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "failed: %s (line %d)\n", #condition, __LINE__); return 1; } } while (0)

static std::string dir;

static const char two_messages[] =
    "From alice@example.org Mon Nov  9 10:00:00 2026\n"
    "From: alice@example.org\n"
    "To: bob@example.org\n"
    "Subject: first\n"
    "Date: Mon, 9 Nov 2026 10:00:00 +0000\n"
    "\n"
    "The first body.\n"
    "\n"
    "From carol@example.org Mon Nov  9 11:00:00 2026\n"
    "From: carol@example.org\n"
    "To: bob@example.org\n"
    "Subject: second\n"
    "Date: Mon, 9 Nov 2026 11:00:00 +0000\n"
    "\n"
    "The second body.\n"
    "\n";

static std::string path(const char *name) { return dir + "/" + name; }

static void write_file(const std::string &name, const std::string &content, mode_t mode = 0600)
{
    int fd = open(name.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) { perror(name.c_str()); exit(3); }
    if (write(fd, content.data(), content.size()) != (ssize_t)content.size()) exit(3);
    close(fd);
    chmod(name.c_str(), mode);
}

static std::string read_file(const std::string &name)
{
    std::string content;
    int fd = open(name.c_str(), O_RDONLY);
    if (fd < 0) return "<missing>";
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) content.append(buf, (size_t)n);
    close(fd);
    return content;
}

static DtMail::Session *new_session(DtMailEnv &env)
{
    DtMail::Session *session = new DtMail::Session(env, "mailbox-files");
    if (env.isSet()) {
        fprintf(stderr, "session: %s\n", (const char *)env);
        return NULL;
    }
    return session;
}

// Opens the mailbox the way dtmail does when the user may change it.
static DtMail::MailBox *open_box(DtMail::Session *session, DtMailEnv &env, const std::string &name,
                                 DtMailBoolean create = DTM_FALSE)
{
    DtMail::MailBox *box = session->mailBoxConstruct(env, DtMailFileObject, (void *)name.c_str(), NULL, NULL, NULL);
    if (env.isSet() || box == NULL) return NULL;
    box->open(env, create, O_RDWR, 0600, DTM_TRUE, DTM_TRUE);
    return env.isSet() ? NULL : box;
}

static int delete_first_and_save(DtMail::MailBox *box, DtMailEnv &env)
{
    DtMail::Message *first = box->getFirstMessage(env);
    if (env.isSet() || first == NULL) return 1;
    first->setFlag(env, DtMailMessageDeletePending);
    if (env.isSet()) return 1;
    box->expunge(env);
    return env.isSet() ? 1 : 0;
}

/* a save never opens a name another user could have planted */
static int save_ignores_planted_names(void)
{
    std::string box = path("box"), victim = path("victim");
    write_file(box, two_messages, 0640);
    write_file(victim, "other content that must stay\n");

    // the names the temporary file used to get: the path and the time in hex
    time_t now = time(NULL);
    for (time_t t = now - 2; t <= now + 20; t++) {
        char name[64];
        snprintf(name, sizeof(name), "box.tmp.%08lx", (long)t);
        CHECK(symlink(victim.c_str(), path(name).c_str()) == 0);
    }

    DtMailEnv env;
    DtMail::Session *session = new_session(env);
    CHECK(session != NULL);
    DtMail::MailBox *mbox = open_box(session, env, box);
    CHECK(mbox != NULL);
    CHECK(delete_first_and_save(mbox, env) == 0);

    CHECK(read_file(victim) == "other content that must stay\n");
    std::string saved = read_file(box);
    CHECK(saved.find("Subject: second") != std::string::npos);
    CHECK(saved.find("Subject: first") == std::string::npos);
    return 0;
}

/* a save keeps the mode of the mailbox and leaves no temporary file behind */
static int save_keeps_mode_and_leaves_nothing(void)
{
    std::string box = path("box");
    write_file(box, two_messages, 0640);

    DtMailEnv env;
    DtMail::Session *session = new_session(env);
    CHECK(session != NULL);
    DtMail::MailBox *mbox = open_box(session, env, box);
    CHECK(mbox != NULL);
    CHECK(delete_first_and_save(mbox, env) == 0);

    struct stat st;
    CHECK(stat(box.c_str(), &st) == 0);
    CHECK((st.st_mode & 07777) == 0640);
    CHECK(S_ISREG(st.st_mode));
    std::string saved = read_file(box);
    CHECK(saved.find("Subject: second") != std::string::npos);
    CHECK(saved.find("Subject: first") == std::string::npos);

    // no file is left that looks like a temporary mailbox
    std::string listing;
    FILE *p = popen(("ls -a '" + dir + "'").c_str(), "r");
    CHECK(p != NULL);
    char line[256];
    while (fgets(line, sizeof(line), p)) listing += line;
    pclose(p);
    CHECK(listing.find(".tmp.") == std::string::npos);
    return 0;
}

/* a mailbox that is a link is saved through the link: the link stays, the file it names changes */
static int linked_mailbox_is_saved_through_the_link(void)
{
    std::string real_dir = path("a-directory-with-a-rather-long-name");
    CHECK(mkdir(real_dir.c_str(), 0700) == 0);
    std::string real_file = real_dir + "/the-real-mailbox-file";
    write_file(real_file, two_messages);
    std::string link = path("box");
    CHECK(symlink(real_file.c_str(), link.c_str()) == 0);

    DtMailEnv env;
    DtMail::Session *session = new_session(env);
    CHECK(session != NULL);
    DtMail::MailBox *mbox = open_box(session, env, link);
    CHECK(mbox != NULL);
    CHECK(delete_first_and_save(mbox, env) == 0);

    struct stat st;
    CHECK(lstat(link.c_str(), &st) == 0 && S_ISLNK(st.st_mode));
    std::string saved = read_file(real_file);
    CHECK(saved.find("Subject: second") != std::string::npos);
    CHECK(saved.find("Subject: first") == std::string::npos);
    return 0;
}

/* a chain of links that never ends is refused */
static int circular_links_are_refused(void)
{
    CHECK(symlink("b", path("a").c_str()) == 0);
    CHECK(symlink("a", path("b").c_str()) == 0);
    alarm(20);                                     /* an endless walk is a failure */

    DtMailEnv env;
    DtMail::Session *session = new_session(env);
    CHECK(session != NULL);
    DtMail::MailBox *mbox = open_box(session, env, path("a"));
    CHECK(mbox == NULL);
    CHECK(env.isSet());
    return 0;
}

/* a path of the given length: nested directories and a file in the last one */
static std::string deep_path(size_t length)
{
    std::string p = dir;
    while (p.size() + 1 + 200 + 1 + 10 < length) {
        p += "/" + std::string(200, 'd');
        if (mkdir(p.c_str(), 0700) != 0 && errno != EEXIST) { perror("mkdir"); exit(3); }
    }
    p += "/" + std::string(length - p.size() - 1, 'f');
    return p;
}

/* a path that does not fit the buffers of the library is refused; the longest that does is used */
static int long_paths_are_bounded(void)
{
    DtMailEnv env;
    DtMail::Session *session = new_session(env);
    CHECK(session != NULL);

    static const size_t lengths[] = { 2000, 2046, 2047, 2048, 2049, 2100, 3000, 4000 };
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        std::string name = deep_path(lengths[i]);
        CHECK(name.size() == lengths[i]);

        // not there yet
        DtMailEnv e1;
        CHECK(open_box(session, e1, name) == NULL);

        write_file(name, two_messages);
        DtMailEnv e2;
        DtMail::MailBox *mbox = open_box(session, e2, name);
        if (lengths[i] < 2048) {
            CHECK(mbox != NULL);
            CHECK(delete_first_and_save(mbox, e2) == 0);
            std::string saved = read_file(name);
            CHECK(saved.find("Subject: second") != std::string::npos);
            CHECK(saved.find("Subject: first") == std::string::npos);
        }
        else {
            CHECK(mbox == NULL);
            CHECK(e2.isSet());
        }
        unlink(name.c_str());
    }
    return 0;
}

/* a mailbox is created when asked for under the name it was given */
static int new_mailbox_is_created(void)
{
    std::string name = path("fresh");
    DtMailEnv env;
    DtMail::Session *session = new_session(env);
    CHECK(session != NULL);
    DtMail::MailBox *mbox = open_box(session, env, name, DTM_TRUE);
    CHECK(mbox != NULL);
    struct stat st;
    CHECK(stat(name.c_str(), &st) == 0 && S_ISREG(st.st_mode));
    return 0;
}

static const char *home(void)
{
    struct passwd *pw = getpwuid(getuid());
    return pw ? pw->pw_dir : "/";
}

static std::string expanded(DtMail::Session *session, const std::string &in, bool *failed = NULL)
{
    DtMailEnv env;
    char *out = session->expandPath(env, in.c_str());
    if (failed) *failed = env.isSet();
    std::string result = out ? out : "<null>";
    free(out);
    return result;
}

/* a path with a variable is expanded without a shell: nothing in it is run */
static int paths_are_expanded_without_a_shell(void)
{
    setenv("MAILTEST_DIR", "/var/mail", 1);
    unsetenv("MAILTEST_UNSET");
    DtMailEnv env;
    DtMail::Session *session = new_session(env);
    CHECK(session != NULL);

    CHECK(expanded(session, "$MAILTEST_DIR/box") == "/var/mail/box");
    CHECK(expanded(session, "${MAILTEST_DIR}/box") == "/var/mail/box");
    CHECK(expanded(session, "$MAILTEST_UNSET/box") == "/box");
    CHECK(expanded(session, "/plain/box") == "/plain/box");

    // text the shell would run or expand stays what it is
    std::string marker = path("marker");
    std::string tricky = "/x;touch " + marker + " #$(touch " + marker + ")`touch " + marker + "`$MAILTEST_DIR";
    std::string result = expanded(session, tricky);
    CHECK(access(marker.c_str(), F_OK) != 0);
    CHECK(result.find("touch") != std::string::npos);
    CHECK(result.find("/var/mail") != std::string::npos);

    // the home directory of the user
    CHECK(expanded(session, "~/box") == std::string(home()) + "/box");
    return 0;
}

/* an expansion that does not fit is refused, not written past its buffers */
static int expansions_that_do_not_fit_are_refused(void)
{
    DtMailEnv env;
    DtMail::Session *session = new_session(env);
    CHECK(session != NULL);

    setenv("MAILTEST_BIG", std::string(1500, 'b').c_str(), 1);
    bool failed = false;
    std::string result = expanded(session, "$MAILTEST_BIG/$MAILTEST_BIG", &failed);
    CHECK(failed);
    CHECK(result.empty());

    const size_t home_len = strlen(home());
    for (size_t length = 2000; length < 2060; length++) {
        std::string path_text = "/" + std::string(length, 'p');
        failed = false;
        result = expanded(session, path_text, &failed);
        CHECK(failed == (length + 1 >= 2048));
        if (!failed) CHECK(result == path_text);
        else CHECK(result.empty());

        failed = false;
        result = expanded(session, "~" + path_text, &failed);
        CHECK(failed == (home_len + 1 + length >= 2048));
        if (!failed) CHECK(result == std::string(home()) + path_text);

        failed = false;
        result = expanded(session, "+" + path_text, &failed);
        CHECK(failed == (home_len + 2 + length >= 2048 || 3 + length >= 2048));

        failed = false;
        result = expanded(session, "$HOME" + path_text, &failed);
        CHECK(failed == (dir.size() + 1 + length >= 2048));
    }
    return 0;
}

/* the path relative to the folder keeps working, and a path that does not fit is refused */
static int relative_paths_are_bounded(void)
{
    write_file(path("mailrc"), "set folder=" + dir + "/Mail\n");
    setenv("MAILRC", path("mailrc").c_str(), 1);
    DtMailEnv env;
    DtMail::Session *session = new_session(env);
    CHECK(session != NULL);

    DtMailEnv e;
    char *rel = session->getRelativePath(e, (dir + "/Mail/work/box").c_str());
    CHECK(rel != NULL && std::string(rel) == "work/box");
    free(rel);
    rel = session->getRelativePath(e, "/elsewhere/box");
    CHECK(rel != NULL && std::string(rel) == "/elsewhere/box");
    free(rel);
    rel = session->getRelativePath(e, "+work/box");
    CHECK(rel != NULL && std::string(rel) == "+work/box");
    free(rel);

    for (size_t length = 2040; length < 2060; length++) {
        DtMailEnv e2;
        std::string long_path = dir + "/Mail/" + std::string(length, 'q');
        rel = session->getRelativePath(e2, long_path.c_str());
        CHECK(rel != NULL);
        CHECK(e2.isSet() == (length >= 2048));
        free(rel);
        DtMailEnv e3;
        rel = session->getRelativePath(e3, ("/" + std::string(length, 'q')).c_str());
        CHECK(rel != NULL);
        CHECK(e3.isSet() == (length + 1 >= 2048));
        free(rel);
    }
    return 0;
}

static struct { const char *name; int (*run)(void); } cases[] = {
    {"save_ignores_planted_names", save_ignores_planted_names},
    {"save_keeps_mode_and_leaves_nothing", save_keeps_mode_and_leaves_nothing},
    {"linked_mailbox_is_saved_through_the_link", linked_mailbox_is_saved_through_the_link},
    {"circular_links_are_refused", circular_links_are_refused},
    {"long_paths_are_bounded", long_paths_are_bounded},
    {"new_mailbox_is_created", new_mailbox_is_created},
    {"paths_are_expanded_without_a_shell", paths_are_expanded_without_a_shell},
    {"expansions_that_do_not_fit_are_refused", expansions_that_do_not_fit_are_refused},
    {"relative_paths_are_bounded", relative_paths_are_bounded},
    {NULL, NULL}
};

int main(int argc, char **argv)
{
    if (argc < 2) {
        for (int i = 0; cases[i].name; i++) puts(cases[i].name);
        return 0;
    }
    const char *work = getenv("DTMAIL_TEST_DIR");
    if (!work) { fprintf(stderr, "DTMAIL_TEST_DIR is not set\n"); return 2; }
    dir = work;
    setenv("HOME", work, 1);
    unsetenv("MAILRC");
    for (int i = 0; cases[i].name; i++)
        if (!strcmp(argv[1], cases[i].name)) return cases[i].run();
    return 2;
}
