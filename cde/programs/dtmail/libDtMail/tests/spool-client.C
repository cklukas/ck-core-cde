// Tests of the client of dtmailspool (SpoolHelper) and of the two ways libDtMail
// handles the files around a mailbox (MailboxFiles): plain system calls for a
// mailbox in a directory of the user and the helper for the user's mailbox in
// the mail spool.  The production SpoolHelper.C and MailboxFiles.C are compiled
// next to this program with the sanitizers and linked with the rest of the built
// libDtMail (see check-spool-client.py).  The helper that is started is a
// wrapper that counts its starts and then becomes the real helper (built from
// the production sources for a spool directory of the test) or plays a helper
// that answers wrongly, as the mode file says.  Every case runs in its own
// process in an empty directory named by DTMAIL_TEST_DIR.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <DtMail/SpoolHelper.hh>
#include "MailboxFiles.hh"

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "failed: %s (line %d)\n", #condition, __LINE__); return 1; } } while (0)
#define CHECK_EQ(a, b) do { long long x_ = (long long)(a), y_ = (long long)(b); if (x_ != y_) { fprintf(stderr, "failed: %s == %s: %lld != %lld (line %d)\n", #a, #b, x_, y_, __LINE__); return 1; } } while (0)

static std::string dir, spool, wrapper, mode_file, wrapper_log, real_helper, user;

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

static std::string read_fd(int fd)
{
    std::string content;
    char buf[4096];
    ssize_t n;
    lseek(fd, 0, SEEK_SET);
    while ((n = read(fd, buf, sizeof(buf))) > 0) content.append(buf, (size_t)n);
    return content;
}

static std::vector<std::string> names_in(const std::string &directory)
{
    std::vector<std::string> names;
    std::string out;
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

// descriptors above the standard ones that are open
static int open_descriptors(void)
{
    int count = 0;
    for (int fd = 3; fd < 256; fd++)
        if (fcntl(fd, F_GETFD) != -1) count++;
    return count;
}

static int wrapper_starts(void)
{
    std::string log = read_file(wrapper_log);
    int starts = 0;
    for (size_t i = 0; i < log.size(); i++) if (log[i] == '\n') starts++;
    return log == "<missing>" ? 0 : starts;
}

static void set_mode(const std::string &mode) { write_file(mode_file, mode); }

static std::string frame_bytes(uint32_t code, const std::string &payload)
{
    unsigned char header[DTSP_HEADER_SIZE];
    dtsp_put32(header, DTSP_MAGIC);
    dtsp_put32(header + 4, code);
    dtsp_put32(header + 8, (uint32_t)payload.size());
    return std::string((char *)header, sizeof(header)) + payload;
}

static std::string u32s(uint32_t a, uint32_t b)
{
    unsigned char bytes[8];
    dtsp_put32(bytes, a);
    dtsp_put32(bytes + 4, b);
    return std::string((char *)bytes, 8);
}

static std::string hex(const std::string &bytes)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : bytes) { out += digits[c >> 4]; out += digits[c & 15]; }
    return out;
}

// The helper answers the next request with these bytes, with `fds` descriptors
// attached to the first of them, and then closes the connection.
static void helper_answers(const std::string &reply, int fds, bool stay = false)
{
    char count[16];
    snprintf(count, sizeof(count), "%d", fds);
    set_mode(std::string("reply\n") + hex(reply) + "\n" + count + "\n" + (stay ? "hold\n" : "\n"));
}

static SpoolHelper *fresh_client(void)
{
    return new SpoolHelper(wrapper.c_str(), spool.c_str(), user.c_str());
}

// ---------------------------------------------------------------------------
// a helper that answers wrongly

static std::string good_open_reply(void)
{
    return frame_bytes(DTSP_OK, u32s(DTSP_ACCESS_WRITE, 0));
}

/* a reply that stops anywhere is a failure that leaves no descriptor open */
static int truncated_replies_fail_cleanly(void)
{
    std::string reply = good_open_reply();
    int before = open_descriptors();
    for (size_t cut = 0; cut < reply.size(); cut++) {
        SpoolHelper *client = fresh_client();
        helper_answers(reply.substr(0, cut), 1);
        int fd = -1;
        bool writable, created;
        int status = client->open(true, false, fd, writable, created);
        CHECK(status != DTSP_OK);
        CHECK(fd == -1);
        delete client;
        CHECK_EQ(open_descriptors(), before);
    }
    return 0;
}

/* lengths above the limit, a wrong magic number and fragments of other protocols */
static int oversize_and_wrong_replies_fail_cleanly(void)
{
    unsigned char header[DTSP_HEADER_SIZE];
    std::vector<std::string> wrong;
    for (uint32_t length : {257u, 1000u, 0xFFFFFFFFu, 0x80000000u}) {
        dtsp_put32(header, DTSP_MAGIC);
        dtsp_put32(header + 4, DTSP_OK);
        dtsp_put32(header + 8, length);
        wrong.push_back(std::string((char *)header, sizeof(header)) + std::string(300, 'x'));
    }
    wrong.push_back(std::string(12, '\0'));
    wrong.push_back("HTTP/1.0 200 OK\r\n\r\nhello world");
    std::string swapped = good_open_reply();                 // the magic number in the other byte order
    for (size_t i = 0; i < 4; i++) swapped[i] = good_open_reply()[3 - i];
    wrong.push_back(swapped);

    int before = open_descriptors();
    for (size_t i = 0; i < wrong.size(); i++) {
        SpoolHelper *client = fresh_client();
        helper_answers(wrong[i], 1);
        int fd = -1;
        bool writable, created;
        CHECK(client->open(true, false, fd, writable, created) != DTSP_OK);
        CHECK(fd == -1);
        delete client;
        CHECK_EQ(open_descriptors(), before);
    }
    return 0;
}

/* a reply that is whole but is not what the request allows */
static int replies_of_the_wrong_shape_fail_cleanly(void)
{
    struct Shape { const char *name; std::string reply; int fds; int op; };
    std::vector<Shape> shapes = {
        {"open without a descriptor", good_open_reply(), 0, DTSP_OP_OPEN},
        {"open with two descriptors", good_open_reply(), 2, DTSP_OP_OPEN},
        {"open with a short payload", frame_bytes(DTSP_OK, std::string(4, '\0')), 1, DTSP_OP_OPEN},
        {"open with a long payload", frame_bytes(DTSP_OK, std::string(12, '\0')), 1, DTSP_OP_OPEN},
        {"open granted nothing known", frame_bytes(DTSP_OK, u32s(3, 0)), 1, DTSP_OP_OPEN},
        {"open created twice", frame_bytes(DTSP_OK, u32s(DTSP_ACCESS_WRITE, 2)), 1, DTSP_OP_OPEN},
        {"open that made a file nobody asked for", frame_bytes(DTSP_OK, u32s(DTSP_ACCESS_WRITE, 1)), 1, DTSP_OP_OPEN},
        {"open failed with a descriptor", frame_bytes(EACCES, ""), 1, DTSP_OP_OPEN},
        {"open failed with a payload", frame_bytes(EACCES, std::string(8, '\0')), 0, DTSP_OP_OPEN},
        {"open busy", frame_bytes(DTSP_BUSY, std::string(4, '\0')), 0, DTSP_OP_OPEN},
        {"bad request", frame_bytes(DTSP_BAD_REQUEST, ""), 0, DTSP_OP_OPEN},
        {"lock reply with a descriptor", frame_bytes(DTSP_OK, ""), 1, DTSP_OP_LOCK_TRY},
        {"lock reply with a payload", frame_bytes(DTSP_OK, std::string(4, '\0')), 0, DTSP_OP_LOCK_TRY},
        {"busy without an age", frame_bytes(DTSP_BUSY, ""), 0, DTSP_OP_LOCK_TRY},
        {"busy with a long age", frame_bytes(DTSP_BUSY, std::string(8, '\0')), 0, DTSP_OP_LOCK_TRY},
        {"busy for a verify", frame_bytes(DTSP_BUSY, std::string(4, '\0')), 0, DTSP_OP_LOCK_VERIFY},
        {"temporary file without a descriptor", frame_bytes(DTSP_OK, ""), 0, DTSP_OP_TEMP_CREATE},
        {"temporary file with a payload", frame_bytes(DTSP_OK, std::string(1, '\0')), 1, DTSP_OP_TEMP_CREATE},
        {"temporary file failed with a descriptor", frame_bytes(EPERM, ""), 1, DTSP_OP_TEMP_CREATE},
        {"commit with a descriptor", frame_bytes(DTSP_OK, ""), 1, DTSP_OP_TEMP_COMMIT},
    };
    int before = open_descriptors();
    for (size_t i = 0; i < shapes.size(); i++) {
        SpoolHelper *client = fresh_client();
        helper_answers(shapes[i].reply, shapes[i].fds);
        int fd = -1, status;
        bool writable, created;
        uint32_t age;
        switch (shapes[i].op) {
          case DTSP_OP_OPEN: status = client->open(true, false, fd, writable, created); break;
          case DTSP_OP_LOCK_TRY: status = client->lockTry(DTSP_LOCK_MAIL, "x", 1, age); break;
          case DTSP_OP_LOCK_VERIFY: status = client->lockVerify(DTSP_LOCK_MAIL, "x", 1); break;
          case DTSP_OP_TEMP_CREATE: status = client->createTemporary(fd); break;
          default: status = client->commitTemporary(); break;
        }
        if (status == DTSP_OK || (shapes[i].op == DTSP_OP_LOCK_TRY && status == DTSP_BUSY && std::string(shapes[i].name).find("long") == std::string::npos
                                  && std::string(shapes[i].name) != "busy without an age")) {
            fprintf(stderr, "failed: %s was accepted (status %d)\n", shapes[i].name, status);
            return 1;
        }
        if (fd != -1) { fprintf(stderr, "failed: %s returned a descriptor\n", shapes[i].name); return 1; }
        delete client;
        if (open_descriptors() != before) { fprintf(stderr, "failed: %s leaked a descriptor\n", shapes[i].name); return 1; }
    }
    return 0;
}

/* a helper that says it was sent a bad request is not asked again; the next call starts another */
static int a_bad_request_reply_ends_that_helper(void)
{
    mkdir(spool.c_str(), 0770);
    SpoolHelper *client = fresh_client();
    uint32_t age;
    helper_answers(frame_bytes(DTSP_BAD_REQUEST, ""), 0, true);
    CHECK(client->lockTry(DTSP_LOCK_MAIL, "x", 1, age) != DTSP_OK);
    CHECK_EQ(wrapper_starts(), 1);
    set_mode("real\n");
    CHECK_EQ(client->lockTry(DTSP_LOCK_MAIL, "x", 1, age), DTSP_OK);   // another helper answers
    CHECK_EQ(wrapper_starts(), 2);
    CHECK_EQ(client->lockRelease(DTSP_LOCK_MAIL, "x", 1), DTSP_OK);
    delete client;
    return 0;
}

/* a helper that is missing, cannot run or stops at once fails the call, and the next call starts another */
static int a_helper_that_cannot_run_fails_the_call(void)
{
    int fd;
    bool writable, created;
    SpoolHelper missing("/nonexistent/dtmailspool", spool.c_str(), user.c_str());
    CHECK_EQ(missing.open(true, false, fd, writable, created), (int)DTSP_UNAVAILABLE);
    SpoolHelper directory(dir.c_str(), spool.c_str(), user.c_str());
    CHECK(directory.open(true, false, fd, writable, created) != DTSP_OK);
    write_file(path("not-executable"), "#!/bin/sh\n", 0600);
    SpoolHelper plain(path("not-executable").c_str(), spool.c_str(), user.c_str());
    CHECK_EQ(plain.open(true, false, fd, writable, created), (int)DTSP_UNAVAILABLE);

    set_mode("exit\n21\n");
    SpoolHelper *client = fresh_client();
    int before = open_descriptors();
    CHECK(client->open(true, false, fd, writable, created) != DTSP_OK);
    CHECK_EQ(client->lastExitStatus(), 21);        // reaped, and its status known
    CHECK_EQ(open_descriptors(), before);

    // the helper that follows is a good one
    set_mode("real\n");
    mkdir(spool.c_str(), 0770);
    CHECK_EQ(client->open(true, true, fd, writable, created), DTSP_OK);
    CHECK(writable && created && fd >= 0);
    close(fd);
    CHECK_EQ(wrapper_starts(), 2);
    delete client;
    return 0;
}

// Send `signo` (0: none) to every process that runs the real helper; how many there are.
static int signal_helpers(int signo)
{
    int found = 0;
    DIR *proc = opendir("/proc");
    struct dirent *entry;
    while (proc && (entry = readdir(proc)) != NULL) {
        char *end;
        long pid = strtol(entry->d_name, &end, 10);
        if (*end || pid <= 0) continue;
        std::string cmdline = read_file(std::string("/proc/") + entry->d_name + "/cmdline");
        if (cmdline.substr(0, cmdline.find('\0')) != real_helper) continue;
        found++;
        if (signo) kill((pid_t)pid, signo);
    }
    if (proc) closedir(proc);
    return found;
}

/* a helper that dies between two calls is replaced */
static int a_helper_that_died_is_replaced(void)
{
    mkdir(spool.c_str(), 0770);
    set_mode("real\n");
    SpoolHelper *client = fresh_client();
    uint32_t age;
    CHECK_EQ(client->lockTry(DTSP_LOCK_MAIL, "0\0one", 5, age), DTSP_OK);
    CHECK_EQ(wrapper_starts(), 1);
    // end it from outside: the first call after that fails, the next one starts another helper
    CHECK_EQ(signal_helpers(SIGTERM), 1);
    for (int i = 0; i < 500 && signal_helpers(0) > 0; i++) usleep(10000);
    CHECK_EQ(signal_helpers(0), 0);
    int status = client->lockVerify(DTSP_LOCK_MAIL, "0\0one", 5);
    CHECK(status != DTSP_OK);
    CHECK_EQ(client->lockVerify(DTSP_LOCK_MAIL, "0\0one", 5), DTSP_OK);
    CHECK_EQ(wrapper_starts(), 2);
    CHECK_EQ(client->lockRelease(DTSP_LOCK_MAIL, "0\0one", 5), DTSP_OK);
    delete client;
    return 0;
}

// ---------------------------------------------------------------------------
// what the two ways to handle the files around a mailbox have in common

static int contract(MailboxFiles *files, const std::string &mailbox, const std::string &locks, const std::string &listing_dir)
{
    int granted = -1;
    errno = 0;

    // opening
    CHECK_EQ(files->open(O_RDONLY, 0600, &granted), -1);
    CHECK_EQ(errno, ENOENT);
    CHECK_EQ(files->open(O_RDWR, 0600, &granted), -1);
    CHECK_EQ(errno, ENOENT);
    int fd = files->open(O_RDWR | O_CREAT, 0600, &granted);
    CHECK(fd >= 0);
    CHECK_EQ(granted, O_RDWR);
    CHECK_EQ(write(fd, "From a@b Mon Nov  9 10:00:00 2026\n\nbody\n", 38), 38);
    CHECK((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
    close(fd);
    struct stat st;
    CHECK_EQ(stat(mailbox.c_str(), &st), 0);
    CHECK_EQ(st.st_uid, getuid());
    chmod(mailbox.c_str(), 0640);

    fd = files->open(O_RDONLY, 0600, &granted);
    CHECK(fd >= 0 && granted == O_RDONLY);
    CHECK(read_fd(fd).substr(0, 5) == std::string("From "));
    CHECK_EQ(write(fd, "x", 1), -1);
    close(fd);
    fd = files->open(O_RDWR, 0600, &granted);
    CHECK(fd >= 0 && granted == O_RDWR);
    close(fd);

    if (getuid() != 0) {
        chmod(mailbox.c_str(), 0400);
        fd = files->open(O_RDWR, 0600, &granted);      // not writable: read-only
        CHECK(fd >= 0 && granted == O_RDONLY);
        close(fd);
        fd = files->open(O_RDONLY, 0600, &granted);
        CHECK(fd >= 0 && granted == O_RDONLY);
        close(fd);
        chmod(mailbox.c_str(), 0640);
    }

    // the lock files: the whole text, a null byte included
    const char text[] = "0\0abc-1234";
    uint32_t age = 99;
    CHECK_EQ(files->lockTry(DTSP_LOCK_MAIL, text, 10, &age), DTSP_OK);
    CHECK(read_file(locks + ".lock") == std::string(std::string(text, 10)));
    CHECK_EQ(files->lockVerify(DTSP_LOCK_MAIL, text, 10), DTSP_OK);
    CHECK_EQ(files->lockVerify(DTSP_LOCK_MAIL, text, 9), (int)DTSP_NOT_OURS);
    CHECK_EQ(files->lockVerify(DTSP_LOCK_DTMAIL, text, 10), ENOENT);
    CHECK_EQ(files->lockTry(DTSP_LOCK_MAIL, "other", 5, &age), (int)DTSP_BUSY);
    CHECK(age <= 1);
    CHECK_EQ(files->lockRelease(DTSP_LOCK_MAIL, "other", 5), (int)DTSP_NOT_OURS);
    CHECK(read_file(locks + ".lock") == std::string(std::string(text, 10)));
    CHECK_EQ(files->lockTry(DTSP_LOCK_DTMAIL, "4711\n", 5, &age), DTSP_OK);
    CHECK_EQ(files->lockVerify(DTSP_LOCK_DTMAIL, "4712\n", 5), (int)DTSP_NOT_OURS);
    CHECK_EQ(files->lockVerify(DTSP_LOCK_DTMAIL, "4711", 4), (int)DTSP_NOT_OURS);
    CHECK_EQ(files->lockRelease(DTSP_LOCK_DTMAIL, "4711\n", 5), DTSP_OK);
    // a lock file that is longer than any buffer, with no line break, or has no number, is not ours and is
    // not read past the buffer; it stays
    const std::string odd[] = {std::string(3000, '7'), std::string(1023, '9'), std::string(129, '1'),
                               std::string(5000, ' '), std::string(), std::string("\n"), std::string("owner unknown\n")};
    for (size_t i = 0; i < sizeof(odd) / sizeof(odd[0]); i++) {
        write_file(locks + ".dtmail", odd[i], 0660);
        CHECK_EQ(files->lockVerify(DTSP_LOCK_DTMAIL, "4711\n", 5), (int)DTSP_NOT_OURS);
        CHECK_EQ(files->lockRelease(DTSP_LOCK_DTMAIL, "4711\n", 5), (int)DTSP_NOT_OURS);
        CHECK(read_file(locks + ".dtmail") == odd[i] || (odd[i].empty() && read_file(locks + ".dtmail").empty()));
    }
    CHECK_EQ(files->lockBreak(DTSP_LOCK_DTMAIL), DTSP_OK);
    CHECK(read_file(locks + ".dtmail") == std::string("<missing>"));
    CHECK_EQ(files->lockRelease(DTSP_LOCK_MAIL, text, 10), DTSP_OK);
    CHECK_EQ(files->lockRelease(DTSP_LOCK_MAIL, text, 10), ENOENT);
    // something that is not a file is not our lock either
    CHECK_EQ(mkdir((locks + ".lock").c_str(), 0700), 0);
    CHECK_EQ(files->lockVerify(DTSP_LOCK_MAIL, text, 10), (int)DTSP_NOT_OURS);
    CHECK_EQ(files->lockRelease(DTSP_LOCK_MAIL, text, 10), (int)DTSP_NOT_OURS);
    CHECK_EQ(rmdir((locks + ".lock").c_str()), 0);
    CHECK_EQ(files->lockBreak(DTSP_LOCK_MAIL), DTSP_OK);       // nothing to break is not an error
    CHECK_EQ(files->lockTry(DTSP_LOCK_MAIL, "first", 5, &age), DTSP_OK);
    CHECK_EQ(files->lockBreak(DTSP_LOCK_MAIL), DTSP_OK);       // whoever made it
    CHECK(read_file(locks + ".lock") == std::string("<missing>"));

    // the file that replaces the mailbox in a save
    int mailbox_fd = files->open(O_RDWR, 0600, &granted);
    CHECK(mailbox_fd >= 0);
    MailboxFiles::TemporaryStep step;
    int temp = files->createTemporary(mailbox_fd, &step);
    CHECK(temp >= 0);
    CHECK(strlen(files->temporaryName()) > 0);
    CHECK_EQ(fstat(temp, &st), 0);
    CHECK_EQ(st.st_mode & 07777, 0640);
    CHECK_EQ(st.st_uid, getuid());
    CHECK_EQ(write(temp, "new\n", 4), 4);
    CHECK(read_file(mailbox).substr(0, 5) == std::string("From "));     // not yet
    CHECK_EQ(files->commitTemporary(), 0);
    CHECK(read_file(mailbox) == std::string("new\n"));
    std::vector<std::string> names = names_in(listing_dir);
    for (size_t i = 0; i < names.size(); i++) CHECK(names[i].find(".tmp.") == std::string::npos);
    close(temp);
    close(mailbox_fd);

    mailbox_fd = files->open(O_RDWR, 0600, &granted);
    temp = files->createTemporary(mailbox_fd, &step);
    CHECK(temp >= 0);
    close(temp);
    files->discardTemporary();
    names = names_in(listing_dir);
    for (size_t i = 0; i < names.size(); i++) CHECK(names[i].find(".tmp.") == std::string::npos);
    CHECK(read_file(mailbox) == std::string("new\n"));
    close(mailbox_fd);
    return 0;
}

/* the mailbox of the user in the spool is handled by the helper */
static int the_users_mailbox_in_the_spool_is_handled_by_the_helper(void)
{
    mkdir(spool.c_str(), 0770);
    set_mode("real\n");
    std::string mailbox = spool + "/" + user;
    MailboxFiles *files = MailboxFiles::forMailbox(mailbox.c_str());
    CHECK_EQ(wrapper_starts(), 0);                        // chosen, but not started yet
    int result = contract(files, mailbox, mailbox, spool);
    delete files;
    CHECK_EQ(result, 0);
    CHECK_EQ(wrapper_starts(), 1);
    std::vector<std::string> names = names_in(spool);
    CHECK_EQ(names.size(), 1u);
    return 0;
}

/* a mailbox anywhere else is handled without a helper, with the same results */
static int a_mailbox_in_another_directory_is_handled_without_the_helper(void)
{
    mkdir(spool.c_str(), 0770);
    set_mode("real\n");
    std::string area = path("folders");
    CHECK_EQ(mkdir(area.c_str(), 0700), 0);
    std::string mailbox = area + "/" + user;                 // the name of the user, but not the spool
    MailboxFiles *files = MailboxFiles::forMailbox(mailbox.c_str());
    int result = contract(files, mailbox, mailbox, area);
    delete files;
    CHECK_EQ(result, 0);
    CHECK_EQ(wrapper_starts(), 0);
    return 0;
}

/* only a file of the name of the user in the spool directory is the user's mailbox */
static int the_users_mailbox_is_recognised_by_name_and_directory(void)
{
    mkdir(spool.c_str(), 0770);
    SpoolHelper client(wrapper.c_str(), spool.c_str(), user.c_str());
    CHECK(client.isMailbox((spool + "/" + user).c_str()));
    CHECK(!client.isMailbox((spool + "/other-user").c_str()));
    CHECK(!client.isMailbox((spool + "/" + user + ".lock").c_str()));
    CHECK(!client.isMailbox((spool + "/" + user + "/").c_str()));
    CHECK(!client.isMailbox((spool + "/" + user + "/inner").c_str()));
    CHECK(!client.isMailbox(user.c_str()));
    CHECK(!client.isMailbox(""));
    CHECK(!client.isMailbox("/"));
    CHECK(!client.isMailbox((dir + "/" + user).c_str()));
    CHECK(!client.isMailbox((dir + "/nonexistent/" + user).c_str()));
    CHECK(!client.isMailbox((spool + "/../nonexistent/" + user).c_str()));

    // a name the helper would refuse is no mailbox of the helper's
    std::string long_user(DTSP_USER_MAX + 1, 'u');
    SpoolHelper long_name(wrapper.c_str(), spool.c_str(), long_user.c_str());
    CHECK(!long_name.isMailbox((spool + "/" + long_user).c_str()));
    SpoolHelper longest(wrapper.c_str(), spool.c_str(), long_user.substr(1).c_str());
    CHECK(longest.isMailbox((spool + "/" + long_user.substr(1)).c_str()));

    // the same directory by another name is the same directory
    std::string link = path("spool-link");
    CHECK_EQ(symlink(spool.c_str(), link.c_str()), 0);
    CHECK(client.isMailbox((link + "/" + user).c_str()));
    std::string spool_name = spool.substr(spool.rfind('/') + 1);
    CHECK(client.isMailbox((spool + "/../" + spool_name + "/" + user).c_str()));
    CHECK(client.isMailbox((spool + "//" + user).c_str()));

    // a directory of another name that holds the spool's name is another directory
    CHECK_EQ(mkdir(path("elsewhere").c_str(), 0700), 0);
    CHECK(!client.isMailbox((path("elsewhere") + "/" + user).c_str()));
    char *own = client.mailboxPath();
    CHECK(own != NULL && std::string(own) == spool + "/" + user);
    free(own);
    return 0;
}

/* the connection and the descriptors the helper hands over are not inherited by programs the mailer starts */
static int descriptors_are_not_inherited_by_children(void)
{
    mkdir(spool.c_str(), 0770);
    set_mode("real\n");
    SpoolHelper *client = fresh_client();
    int fd;
    bool writable, created;
    CHECK_EQ(client->open(true, true, fd, writable, created), DTSP_OK);
    CHECK((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
    int temp;
    CHECK_EQ(client->createTemporary(temp), DTSP_OK);
    CHECK((fcntl(temp, F_GETFD) & FD_CLOEXEC) != 0);
    for (int other = 3; other < 256; other++) {
        int flags = fcntl(other, F_GETFD);
        if (flags != -1 && !(flags & FD_CLOEXEC)) {
            fprintf(stderr, "descriptor %d is inherited\n", other);
            return 1;
        }
    }
    close(temp);
    close(fd);
    CHECK_EQ(client->discardTemporary(), DTSP_OK);
    delete client;
    return 0;
}

/* stopping the client ends the helper, which removes the file of a save that was not finished */
static int stopping_the_client_ends_the_helper_and_its_temporary_file(void)
{
    mkdir(spool.c_str(), 0770);
    set_mode("real\n");
    write_file(spool + "/" + user, "x\n", 0660);
    SpoolHelper *client = fresh_client();
    int temp;
    CHECK_EQ(client->createTemporary(temp), DTSP_OK);
    close(temp);
    CHECK_EQ(names_in(spool).size(), 2u);
    client->stop();
    CHECK_EQ(client->lastExitStatus(), 0);
    CHECK_EQ(names_in(spool).size(), 1u);
    delete client;
    return 0;
}

static struct { const char *name; int (*run)(void); } cases[] = {
    {"truncated_replies_fail_cleanly", truncated_replies_fail_cleanly},
    {"oversize_and_wrong_replies_fail_cleanly", oversize_and_wrong_replies_fail_cleanly},
    {"replies_of_the_wrong_shape_fail_cleanly", replies_of_the_wrong_shape_fail_cleanly},
    {"a_bad_request_reply_ends_that_helper", a_bad_request_reply_ends_that_helper},
    {"a_helper_that_cannot_run_fails_the_call", a_helper_that_cannot_run_fails_the_call},
    {"a_helper_that_died_is_replaced", a_helper_that_died_is_replaced},
    {"the_users_mailbox_in_the_spool_is_handled_by_the_helper", the_users_mailbox_in_the_spool_is_handled_by_the_helper},
    {"a_mailbox_in_another_directory_is_handled_without_the_helper", a_mailbox_in_another_directory_is_handled_without_the_helper},
    {"the_users_mailbox_is_recognised_by_name_and_directory", the_users_mailbox_is_recognised_by_name_and_directory},
    {"descriptors_are_not_inherited_by_children", descriptors_are_not_inherited_by_children},
    {"stopping_the_client_ends_the_helper_and_its_temporary_file", stopping_the_client_ends_the_helper_and_its_temporary_file},
    {NULL, NULL}
};

int main(int argc, char **argv)
{
    if (argc < 2) {
        for (int i = 0; cases[i].name; i++) puts(cases[i].name);
        return 0;
    }
    const char *work = getenv("DTMAIL_TEST_DIR");
    const char *spool_dir = getenv("DTMAIL_TEST_SPOOL");
    const char *wrapper_path = getenv("DTMAIL_TEST_WRAPPER");
    const char *log = getenv("DTMAIL_TEST_WRAPPER_LOG");
    const char *mode = getenv("DTMAIL_TEST_MODE_FILE");
    const char *real = getenv("DTMAIL_TEST_REAL");
    if (!work || !spool_dir || !wrapper_path || !log || !mode || !real) { fprintf(stderr, "the test environment is not set\n"); return 2; }
    dir = work;
    spool = spool_dir;
    wrapper = wrapper_path;
    wrapper_log = log;
    mode_file = mode;
    real_helper = real;
    struct passwd *pw = getpwuid(getuid());
    if (!pw) return 2;
    user = pw->pw_name;
    for (int i = 0; cases[i].name; i++)
        if (!strcmp(argv[1], cases[i].name)) return cases[i].run();
    return 2;
}
