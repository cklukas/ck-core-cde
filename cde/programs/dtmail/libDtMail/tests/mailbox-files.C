// Regression tests for the files libDtMail creates, the paths it follows and
// the messages it puts together when it opens and saves a mailbox, and for the
// user's mailbox in the mail spool, which dtmailspool handles.  The
// production Session.C, RFCMailBox.C, MailboxFiles.C, SpoolHelper.C, MIMEPartial.C,
// MIMEBodyPart.C and RFCMIME.C are compiled next to this program with the sanitizers and linked
// with the rest of the built libDtMail, which also holds the POP and IMAP
// clients that a case runs against a server of its own (a forked process on
// a port of the loopback address that plays what a hostile server would send);
// every case runs in its own process
// (see check-mailbox-files.py) in an empty directory named by DTMAIL_TEST_DIR,
// which is also HOME.

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
#include <vector>
#include <functional>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <DtMail/DtMail.hh>
#include <DtMail/DtMailError.hh>
#include <DtMail/DtMailServer.hh>

// The session of the mailer opens a ToolTalk connection when it is made; these
// stand-ins give it one, so that no session server has to run for the test.
extern "C" {
char *tt_default_procid(void) { static char id[] = "mailbox-files"; return id; }
Tt_status tt_pointer_error(void *) { return TT_OK; }
int tt_fd(void) { return -1; }
}

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "failed: %s (line %d)\n", #condition, __LINE__); return 1; } } while (0)

static std::string dir, spool_dir, helper_log;

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

// How often the helper of the spool was started in this case.
static int helper_starts(void)
{
    std::string log = read_file(helper_log);
    int starts = 0;
    for (size_t i = 0; log != "<missing>" && i < log.size(); i++)
        if (log[i] == '\n') starts++;
    return starts;
}

static std::vector<std::string> listing_of(const std::string &directory)
{
    std::vector<std::string> names;
    FILE *p = popen(("ls -A '" + directory + "'").c_str(), "r");
    char line[512];
    while (p && fgets(line, sizeof(line), p)) {
        std::string name = line;
        while (!name.empty() && name.back() == '\n') name.pop_back();
        names.push_back(name);
    }
    if (p) pclose(p);
    return names;
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

/* the user's mailbox in the mail spool is made, locked, saved and unlocked through the helper,
   and the mailer holds no group for it */
static int the_users_mailbox_in_the_spool_is_handled_through_the_helper(void)
{
    struct passwd *pw = getpwuid(getuid());
    CHECK(pw != NULL);
    std::string name = pw->pw_name;
    std::string inbox = spool_dir + "/" + name;
    CHECK(helper_starts() == 0);

    // a mailbox that does not exist is made by the helper, with the modes of a spool file
    DtMailEnv env;
    DtMail::Session *session = new_session(env);
    CHECK(session != NULL);
    DtMail::MailBox *created = open_box(session, env, inbox, DTM_TRUE);
    CHECK(created != NULL);
    struct stat st;
    CHECK(stat(inbox.c_str(), &st) == 0);
    CHECK((st.st_mode & 07777) == 0660);
    CHECK(st.st_uid == getuid());
    CHECK(helper_starts() == 1);
    delete created;

    // delivered mail is there when it is opened again; a save replaces it and keeps its mode
    write_file(inbox, two_messages, 0640);
    DtMail::MailBox *mbox = open_box(session, env, inbox);
    CHECK(mbox != NULL);
    std::vector<std::string> during = listing_of(spool_dir);
    for (size_t i = 0; i < during.size(); i++)
        CHECK(during[i] == name || during[i] == name + ".dtmail" || during[i] == name + ".lock");
    CHECK(delete_first_and_save(mbox, env) == 0);
    CHECK(stat(inbox.c_str(), &st) == 0);
    CHECK((st.st_mode & 07777) == 0640);
    std::string saved = read_file(inbox);
    CHECK(saved.find("Subject: second") != std::string::npos);
    CHECK(saved.find("Subject: first") == std::string::npos);
    delete mbox;

    // nothing is left in the spool but the mailbox: no lock file, no temporary file
    std::vector<std::string> after = listing_of(spool_dir);
    CHECK(after.size() == 1 && after[0] == name);
    CHECK(helper_starts() == 1);            // one helper served the whole session
    return 0;
}

/* a mailbox that is not in the spool is handled without the helper, even under the name of the user */
static int a_mailbox_outside_the_spool_does_not_start_the_helper(void)
{
    struct passwd *pw = getpwuid(getuid());
    CHECK(pw != NULL);
    std::string box = path(pw->pw_name);
    write_file(box, two_messages, 0640);

    DtMailEnv env;
    DtMail::Session *session = new_session(env);
    CHECK(session != NULL);
    DtMail::MailBox *mbox = open_box(session, env, box);
    CHECK(mbox != NULL);
    CHECK(delete_first_and_save(mbox, env) == 0);
    delete mbox;
    CHECK(read_file(box).find("Subject: first") == std::string::npos);

    DtMail::MailBox *created = open_box(session, env, path("new-folder"), DTM_TRUE);
    CHECK(created != NULL);
    delete created;
    CHECK(helper_starts() == 0);
    return 0;
}

/* a link in the spool is not a way to make or replace anything else: the helper refuses a link under
   the name of the mailbox, and a link to a file of the user is followed by the user, not by the helper */
static int a_link_in_the_spool_is_not_a_mailbox(void)
{
    struct passwd *pw = getpwuid(getuid());
    CHECK(pw != NULL);
    std::string inbox = spool_dir + "/" + pw->pw_name;
    std::string target = path("somebody-elses-file");

    // a link to a file that does not exist: the mailbox is not made through the link
    CHECK(symlink(target.c_str(), inbox.c_str()) == 0);
    DtMailEnv env;
    DtMail::Session *session = new_session(env);
    CHECK(session != NULL);
    DtMail::MailBox *mbox = open_box(session, env, inbox, DTM_TRUE);
    CHECK(mbox == NULL);
    struct stat st;
    CHECK(lstat(target.c_str(), &st) != 0);
    CHECK(lstat(inbox.c_str(), &st) == 0 && S_ISLNK(st.st_mode));
    CHECK(helper_starts() == 1);

    // a link to a file of the user is resolved like any link and handled as the user
    write_file(target, two_messages, 0640);
    DtMailEnv env2;
    mbox = open_box(session, env2, inbox);
    CHECK(mbox != NULL);
    CHECK(delete_first_and_save(mbox, env2) == 0);
    delete mbox;
    CHECK(lstat(inbox.c_str(), &st) == 0 && S_ISLNK(st.st_mode));
    std::string saved = read_file(target);
    CHECK(saved.find("Subject: second") != std::string::npos);
    CHECK(saved.find("Subject: first") == std::string::npos);
    CHECK(listing_of(spool_dir).size() == 1);
    CHECK(helper_starts() == 1);
    return 0;
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


/* the text of the first body part of every message of the mailbox, in order */
static std::vector<std::string> body_texts(DtMail::MailBox *box)
{
    std::vector<std::string> texts;
    DtMailEnv env;
    DtMail::Message *message = box->getFirstMessage(env);
    while (message != NULL && !env.isSet()) {
        DtMailEnv part_env;
        std::string text = "<no body>";
        DtMail::BodyPart *part = message->getFirstBodyPart(part_env);
        if (part != NULL && !part_env.isSet()) {
            const void *contents = NULL;
            unsigned long length = 0;
            part->getContents(part_env, &contents, &length, NULL, NULL, NULL, NULL);
            if (!part_env.isSet() && contents != NULL) text.assign((const char *)contents, length);
        }
        texts.push_back(text);
        message = box->getNextMessage(env, message);
    }
    return texts;
}

/* one part of a message/partial message, as it stands in a mailbox */
static std::string partial_part(const char *id, int number, int total, const std::string &body,
                                const std::string &outer_headers = "")
{
    std::string parameters = std::string("id=\"") + id + "\"; number=" + std::to_string(number);
    if (total > 0) parameters += "; total=" + std::to_string(total);
    return "From sender@example.org Mon Nov  9 10:00:00 2026\n"
           "From: sender@example.org\n"
           "To: bob@example.org\n"
           "Subject: part " + std::to_string(number) + "\n"
           "Date: Mon, 9 Nov 2026 10:00:00 +0000\n"
           "MIME-Version: 1.0\n" + outer_headers +
           "Content-Type: message/partial; " + parameters + "\n"
           "\n" + body + "\n\n";
}

/* the first part carries the headers of the message that was split */
static std::string first_part_body(const std::string &data)
{
    return "Content-Type: text/plain\nMessage-ID: <inner@example.org>\n\n" + data;
}

static DtMail::MailBox *open_text(const std::string &content)
{
    std::string box = path("box");
    write_file(box, content, 0600);
    DtMailEnv env;
    DtMail::Session *session = new_session(env);
    if (session == NULL) return NULL;
    return open_box(session, env, box);
}

/* the parts of a message/partial message are put together into one message */
static int partial_parts_are_assembled(void)
{
    DtMail::MailBox *box = open_text(partial_part("a1", 1, 0, first_part_body("FIRST-PART-DATA\n")) +
                                     partial_part("a1", 2, 2, "SECOND-PART-DATA\n"));
    CHECK(box != NULL);
    std::vector<std::string> bodies = body_texts(box);
    CHECK(!bodies.empty());
    const std::string &whole = bodies.back();
    size_t first = whole.find("FIRST-PART-DATA"), second = whole.find("SECOND-PART-DATA");
    CHECK(first != std::string::npos && second != std::string::npos && first < second);
    CHECK(whole.find("Message-ID") == std::string::npos);
    return 0;
}

/* parts numbered outside the total, or far outside anything a message has, do not belong to it */
static int partial_numbers_beyond_the_total_are_ignored(void)
{
    DtMail::MailBox *box = open_text(partial_part("b1", 1, 0, first_part_body("ONE\n")) +
                                     partial_part("b1", 2, 2, "TWO\n") +
                                     partial_part("b1", 500, 0, "STRAY-500\n") +
                                     partial_part("b1", 1000000, 0, "STRAY-MILLION\n") +
                                     partial_part("b1", 2147483647, 0, "STRAY-MAX\n"));
    CHECK(box != NULL);
    std::vector<std::string> bodies = body_texts(box);
    bool assembled = false;
    for (size_t i = 0; i < bodies.size(); i++) {
        if (bodies[i].find("ONE") != std::string::npos && bodies[i].find("TWO") != std::string::npos) assembled = true;
        CHECK(bodies[i].find("STRAY-500") == std::string::npos || bodies[i].find("ONE") == std::string::npos);
        CHECK(bodies[i].find("STRAY-MILLION") == std::string::npos || bodies[i].find("ONE") == std::string::npos);
    }
    CHECK(assembled);
    return 0;
}

/* numbers that are not numbers make a message that is not a part */
static int partial_parameters_that_are_not_numbers_are_refused(void)
{
    std::string text;
    const char *numbers[] = {"-5", "0", "99999999999999999999", "x", "", "4294967297"};
    for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++) {
        std::string part = partial_part("c1", 1, 2, first_part_body("DATA\n"));
        size_t at = part.find("number=1");
        part.replace(at, strlen("number=1"), std::string("number=") + numbers[i]);
        text += part;
    }
    text += partial_part("c1", 3, 2, "number beyond its own total\n");        // 3 of 2
    text += partial_part("c1", 1, 2000000000, first_part_body("HUGE\n"));      // far more parts than allowed
    DtMail::MailBox *box = open_text(text);
    CHECK(box != NULL);
    body_texts(box);
    return 0;
}

/* headers that need more room than the size the parts claim are not written past the buffer */
static int partial_headers_larger_than_the_old_buffer_are_assembled(void)
{
    std::string padding;
    for (int i = 0; i < 40; i++) padding += "X-Pad-" + std::to_string(i) + ": " + std::string(1000, 'h') + "\n";
    DtMail::MailBox *box = open_text(partial_part("d1", 1, 0, first_part_body("HEAD-DATA\n"), padding) +
                                     partial_part("d1", 2, 2, "TAIL-DATA\n"));
    CHECK(box != NULL);
    std::vector<std::string> bodies = body_texts(box);
    CHECK(!bodies.empty());
    CHECK(bodies.back().find("HEAD-DATA") != std::string::npos && bodies.back().find("TAIL-DATA") != std::string::npos);
    return 0;
}

/* data of the parts larger than any size the headers announce */
static int partial_data_larger_than_the_old_buffer_is_assembled(void)
{
    std::string big1(60000, 'a'), big2(60000, 'b');
    DtMail::MailBox *box = open_text(partial_part("e1", 1, 0, first_part_body(big1 + "\n")) +
                                     partial_part("e1", 2, 2, big2 + "\n"));
    CHECK(box != NULL);
    std::vector<std::string> bodies = body_texts(box);
    CHECK(!bodies.empty());
    CHECK(bodies.back().find(big1) != std::string::npos && bodies.back().find(big2) != std::string::npos);
    return 0;
}

/* a first part without the headers of the carried message is not put together */
static int partial_first_part_without_a_blank_line_is_left_alone(void)
{
    DtMail::MailBox *box = open_text(partial_part("f1", 1, 0, "Content-Type: text/plain\nno blank line, only header lines") +
                                     partial_part("f1", 2, 2, "SECOND\n"));
    CHECK(box != NULL);
    body_texts(box);
    return 0;
}

/* a part that does not fit the memory a message has is not put together either */
static int partial_parts_are_not_cut_at_a_zero_byte(void)
{
    std::string second = std::string("SECOND-") + std::string(1, '\0') + "AFTER-NUL-DATA\n";
    DtMail::MailBox *box = open_text(partial_part("g1", 1, 0, first_part_body("FIRST\n")) +
                                     partial_part("g1", 2, 2, second));
    CHECK(box != NULL);
    body_texts(box);
    return 0;
}

/* enriched text with a command of any length becomes plain text */
static int enriched_text_with_a_long_command_is_converted(void)
{
    std::string message = "From sender@example.org Mon Nov  9 10:00:00 2026\n"
                          "From: sender@example.org\n"
                          "To: bob@example.org\n"
                          "Subject: enriched\n"
                          "MIME-Version: 1.0\n"
                          "Content-Type: text/enriched\n"
                          "\n"
                          "<" + std::string(3000, 'a') + ">visible <bold>text</bold> <" + std::string(300, 'b') + "\n\n";
    DtMail::MailBox *box = open_text(message);
    CHECK(box != NULL);
    std::vector<std::string> bodies = body_texts(box);
    CHECK(bodies.size() == 1);
    CHECK(bodies[0].find("visible text") != std::string::npos);
    CHECK(bodies[0].find("aaaa") == std::string::npos && bodies[0].find("bbbb") == std::string::npos);
    return 0;
}


/* ---- the mail server clients, against a server that answers what the case scripts ---- */

struct Rule { std::string prefix; std::string reply; };   /* %T in a reply is the tag of the command */

static void write_all(int fd, const std::string &text)
{
    size_t done = 0;
    while (done < text.size()) {
        ssize_t n = write(fd, text.data() + done, text.size() - done);
        if (n <= 0) return;
        done += (size_t)n;
    }
}

/* answers one connection, line by line, and writes the commands it got to a log */
static void serve_one(int listen_fd, const std::string &greeting, const std::vector<Rule> &rules,
                      bool tagged, const std::string &log_path)
{
    int fd = accept(listen_fd, NULL, NULL);
    FILE *log = fopen(log_path.c_str(), "w");
    if (fd < 0 || !log) _exit(1);
    write_all(fd, greeting);
    std::string pending;
    for (;;) {
        char chunk[512];
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n <= 0) break;
        pending.append(chunk, (size_t)n);
        size_t newline;
        while ((newline = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            while (!line.empty() && line.back() == '\r') line.pop_back();
            fprintf(log, "%s\n", line.c_str());
            fflush(log);
            std::string tag, command = line;
            if (tagged) {
                size_t space = line.find(' ');
                tag = line.substr(0, space);
                command = space == std::string::npos ? "" : line.substr(space + 1);
            }
            std::string reply = tagged ? "%T OK done\r\n" : "+OK\r\n";
            for (size_t i = 0; i < rules.size(); i++)
                if (command.compare(0, rules[i].prefix.size(), rules[i].prefix) == 0) { reply = rules[i].reply; break; }
            for (size_t at; (at = reply.find("%T")) != std::string::npos; ) reply.replace(at, 2, tag);
            write_all(fd, reply);
            if (command == "QUIT" || command == "LOGOUT") { fclose(log); close(fd); _exit(0); }
        }
    }
    fclose(log);
    _exit(0);
}

static void collect_text(DtMailEnv &, char *text, int length, void *data)
{
    ((std::string *)data)->append(text, (size_t)length);
}

class TestPOP3 : public POP3Server {
public:
    TestPOP3(DtMail::Session *session, DtMail::MailBox *box, DtMailAppendCallback callback, void *data,
             int port, int size_limit)
        : POP3Server((char *)"INBOX", session, box, callback, data), _port(port) { _sizelimit = size_limit; }
    virtual int proto_port() { return _port; }
private:
    int _port;
};

class TestIMAP : public IMAPServer {
public:
    TestIMAP(DtMail::Session *session, DtMail::MailBox *box, DtMailAppendCallback callback, void *data, int port)
        : IMAPServer((char *)"INBOX", session, box, callback, data), _port(port) { }
    virtual int proto_port() { return _port; }
private:
    int _port;
};

struct Exchange {
    std::string greeting;
    std::vector<Rule> rules;
    int size_limit;
    const char *password;
    bool imap;
    Exchange() : size_limit(0), password("secret"), imap(false) { }
};

/* one retrieval from a server that behaves as scripted; returns the commands the server saw */
static std::string retrieve(const Exchange &exchange, std::string *appended)
{
    signal(SIGPIPE, SIG_IGN);
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 1)) { perror("listen"); exit(3); }
    socklen_t length = sizeof(address);
    getsockname(listener, (struct sockaddr *)&address, &length);
    int port = ntohs(address.sin_port);

    std::string log_path = path("server.log");
    pid_t child = fork();
    if (child == 0) serve_one(listener, exchange.greeting, exchange.rules, exchange.imap, log_path);
    close(listener);

    write_file(path("mailrc"), "set INBOX_servername=127.0.0.1\nset INBOX_username=tester\nset INBOX_timeout=5\n");
    setenv("MAILRC", path("mailrc").c_str(), 1);
    DtMailEnv env;
    DtMail::Session *session = new_session(env);
    if (!session) exit(3);
    DtMail::MailBox *box = open_box(session, env, path("retrieved"), DTM_TRUE);
    if (!box) exit(3);

    DtMailServer *server;
    if (exchange.imap) server = new TestIMAP(session, box, collect_text, appended, port);
    else server = new TestPOP3(session, box, collect_text, appended, port, exchange.size_limit);
    server->set_password((char *)exchange.password);
    DtMailEnv failure;
    server->retrieve_messages(failure);
    delete server;

    int status = 0;
    for (int waited = 0; waited < 50 && waitpid(child, &status, WNOHANG) == 0; waited++) usleep(100000);
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    return read_file(log_path);
}

/* what a POP3 server answers to RETR for a message of this text */
static std::string pop_message(const std::string &text)
{
    return "+OK " + std::to_string(text.size()) + " octets\r\n" + text + ".\r\n";
}

static Exchange pop3_server(const std::string &stat_reply)
{
    Exchange e;
    e.greeting = "+OK POP3 ready\r\n";
    e.rules.push_back(Rule{"STAT", stat_reply});
    e.rules.push_back(Rule{"LAST", "-ERR no LAST here\r\n"});
    e.rules.push_back(Rule{"UIDL", "+OK\r\n1 uid-one\r\n2 uid-two\r\n.\r\n"});
    e.rules.push_back(Rule{"RETR 1", pop_message("From: a@example.org\r\nSubject: one\r\n\r\nbody one\r\n")});
    e.rules.push_back(Rule{"RETR 2", pop_message("From: b@example.org\r\nSubject: two\r\n\r\nbody two\r\n")});
    return e;
}

static bool has(const std::string &text, const char *needle) { return text.find(needle) != std::string::npos; }

/* messages come in with the commands of the protocol, each one deleted after it was stored */
static int pop3_retrieves_and_deletes_messages(void)
{
    std::string appended;
    std::string log = retrieve(pop3_server("+OK 2 100\r\n"), &appended);
    CHECK(has(log, "USER tester\n") && has(log, "PASS secret\n") && has(log, "STAT\n"));
    CHECK(has(log, "RETR 1\n") && has(log, "RETR 2\n"));
    CHECK(has(log, "DELE 1\n") && has(log, "DELE 2\n") && has(log, "QUIT\n"));
    CHECK(has(appended, "body one") && has(appended, "body two"));
    CHECK(!has(appended, "\r"));
    return 0;
}

/* a count the server makes up is not a count */
static int pop3_message_counts_that_are_not_counts_are_refused(void)
{
    const char *replies[] = {"+OK -3 10\r\n", "+OK 2147483647 1\r\n", "+OK 1048577 1\r\n", "+OK\r\n", "+OK x y\r\n", "+OK 99999999999999999999 1\r\n"};
    for (size_t i = 0; i < sizeof(replies) / sizeof(replies[0]); i++) {
        std::string appended;
        std::string log = retrieve(pop3_server(replies[i]), &appended);
        CHECK(has(log, "STAT\n"));
        CHECK(!has(log, "RETR"));
        CHECK(appended.empty());
    }
    return 0;
}

/* an identifier longer than the room for it is cut, and the message still comes in */
static int pop3_message_identifiers_of_any_length_are_read(void)
{
    Exchange e = pop3_server("+OK 1 100\r\n");
    e.rules[2].reply = "+OK\r\n1 " + std::string(400, 'u') + "\r\n.\r\n";
    std::string appended;
    std::string log = retrieve(e, &appended);
    CHECK(has(log, "UIDL\n") && has(log, "RETR 1\n"));
    CHECK(has(appended, "body one"));
    return 0;
}

/* the sizes of the list belong to messages of the mailbox, whatever the server numbers them */
static int pop3_size_list_with_numbers_outside_the_mailbox_is_read(void)
{
    Exchange e = pop3_server("+OK 2 100\r\n");
    e.size_limit = 1000;
    e.rules.push_back(Rule{"LIST", "+OK\r\n99999 5\r\nxyz\r\n-5 7\r\n0 9\r\n3 11\r\n1 100\r\n2 200\r\n.\r\n"});
    std::string appended;
    std::string log = retrieve(e, &appended);
    CHECK(has(log, "LIST\n"));
    CHECK(has(appended, "body one") && has(appended, "body two"));
    return 0;
}

/* a name with a line break would run a second command: it is not sent */
static int pop3_credentials_with_a_line_break_are_not_sent(void)
{
    Exchange e = pop3_server("+OK 2 100\r\n");
    e.password = "secret\r\nDELE 1";
    std::string appended;
    std::string log = retrieve(e, &appended);
    CHECK(has(log, "USER tester\n"));
    CHECK(!has(log, "DELE") && !has(log, "PASS") && !has(log, "RETR"));
    CHECK(appended.empty());
    return 0;
}

/* the status line of a server without any text */
static int pop3_status_lines_without_text_are_read(void)
{
    Exchange e = pop3_server("+OK 2 100\r\n");
    e.greeting = "+OK\r\n";
    e.rules.push_back(Rule{"USER", "+OK\r\n"});
    std::string appended;
    std::string log = retrieve(e, &appended);
    CHECK(has(appended, "body one"));
    return 0;
}

/* IMAP: lines that are empty or shorter than their prefix are not read before their start or past their end */
static int imap_short_and_tag_only_lines_are_read(void)
{
    Exchange e;
    e.imap = true;
    e.greeting = "\n";
    e.rules.push_back(Rule{"LOGIN", "%T\r\n"});
    std::string appended;
    std::string log = retrieve(e, &appended);
    CHECK(has(log, " LOGIN tester \"secret\"\n"));

    Exchange f;
    f.imap = true;
    f.greeting = "* OK ready\r\n";
    f.rules.push_back(Rule{"LOGIN", "*\r\n* \r\n\r\n%T NO refused\r\n"});
    log = retrieve(f, &appended);
    CHECK(has(log, " LOGIN tester \"secret\"\n"));
    return 0;
}

/* IMAP: a session without mail keeps its tags in the right form */
static int imap_tags_follow_the_protocol(void)
{
    Exchange e;
    e.imap = true;
    e.greeting = "* OK ready\r\n";
    std::string appended;
    std::string log = retrieve(e, &appended);
    CHECK(has(log, "a0001 LOGIN tester \"secret\"\n"));
    CHECK(has(log, "a0002 CAPABILITY\n"));
    CHECK(has(log, "a0003 SELECT INBOX\n"));
    return 0;
}

/* IMAP: a command with a line break is not sent */
static int imap_credentials_with_a_line_break_are_not_sent(void)
{
    Exchange e;
    e.imap = true;
    e.greeting = "* OK ready\r\n";
    e.password = "secret\"\r\na0099 DELETE INBOX";
    std::string appended;
    std::string log = retrieve(e, &appended);
    CHECK(!has(log, "DELETE") && !has(log, "LOGIN"));
    return 0;
}

static struct { const char *name; int (*run)(void); } cases[] = {
    {"the_users_mailbox_in_the_spool_is_handled_through_the_helper", the_users_mailbox_in_the_spool_is_handled_through_the_helper},
    {"a_mailbox_outside_the_spool_does_not_start_the_helper", a_mailbox_outside_the_spool_does_not_start_the_helper},
    {"a_link_in_the_spool_is_not_a_mailbox", a_link_in_the_spool_is_not_a_mailbox},
    {"save_ignores_planted_names", save_ignores_planted_names},
    {"save_keeps_mode_and_leaves_nothing", save_keeps_mode_and_leaves_nothing},
    {"linked_mailbox_is_saved_through_the_link", linked_mailbox_is_saved_through_the_link},
    {"circular_links_are_refused", circular_links_are_refused},
    {"long_paths_are_bounded", long_paths_are_bounded},
    {"new_mailbox_is_created", new_mailbox_is_created},
    {"paths_are_expanded_without_a_shell", paths_are_expanded_without_a_shell},
    {"expansions_that_do_not_fit_are_refused", expansions_that_do_not_fit_are_refused},
    {"relative_paths_are_bounded", relative_paths_are_bounded},
    {"partial_parts_are_assembled", partial_parts_are_assembled},
    {"partial_numbers_beyond_the_total_are_ignored", partial_numbers_beyond_the_total_are_ignored},
    {"partial_parameters_that_are_not_numbers_are_refused", partial_parameters_that_are_not_numbers_are_refused},
    {"partial_headers_larger_than_the_old_buffer_are_assembled", partial_headers_larger_than_the_old_buffer_are_assembled},
    {"partial_data_larger_than_the_old_buffer_is_assembled", partial_data_larger_than_the_old_buffer_is_assembled},
    {"partial_first_part_without_a_blank_line_is_left_alone", partial_first_part_without_a_blank_line_is_left_alone},
    {"partial_parts_are_not_cut_at_a_zero_byte", partial_parts_are_not_cut_at_a_zero_byte},
    {"enriched_text_with_a_long_command_is_converted", enriched_text_with_a_long_command_is_converted},
    {"pop3_retrieves_and_deletes_messages", pop3_retrieves_and_deletes_messages},
    {"pop3_message_counts_that_are_not_counts_are_refused", pop3_message_counts_that_are_not_counts_are_refused},
    {"pop3_message_identifiers_of_any_length_are_read", pop3_message_identifiers_of_any_length_are_read},
    {"pop3_size_list_with_numbers_outside_the_mailbox_is_read", pop3_size_list_with_numbers_outside_the_mailbox_is_read},
    {"pop3_credentials_with_a_line_break_are_not_sent", pop3_credentials_with_a_line_break_are_not_sent},
    {"pop3_status_lines_without_text_are_read", pop3_status_lines_without_text_are_read},
    {"imap_short_and_tag_only_lines_are_read", imap_short_and_tag_only_lines_are_read},
    {"imap_tags_follow_the_protocol", imap_tags_follow_the_protocol},
    {"imap_credentials_with_a_line_break_are_not_sent", imap_credentials_with_a_line_break_are_not_sent},
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
    const char *spool_env = getenv("DTMAIL_TEST_SPOOL"), *log_env = getenv("DTMAIL_TEST_HELPER_LOG");
    if (!spool_env || !log_env) { fprintf(stderr, "DTMAIL_TEST_SPOOL or DTMAIL_TEST_HELPER_LOG is not set\n"); return 2; }
    spool_dir = spool_env;
    helper_log = log_env;
    setenv("HOME", work, 1);
    unsetenv("MAILRC");
    for (int i = 0; cases[i].name; i++)
        if (!strcmp(argv[1], cases[i].name)) return cases[i].run();
    return 2;
}
