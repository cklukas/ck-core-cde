/*
 * Regression tests for what libtt does with the environment and with the
 * programs it starts: the settings that name a program, a library or a file
 * (TTSESSION_CMD, TT_TRACE_SCRIPT, LD_LIBRARY_PATH, TTAUTHORITY, the TTPATH
 * family), the privileges of a process that runs for the owner of a program
 * file, and the execution of commands without a shell.
 *
 * check-environment.py compiles the production sources these tests exercise
 * next to this program, with the sanitizers, and links them in front of the
 * built libtt, so the code that ships is what is tested.  A process with
 * other effective ids than real ones is simulated by the program's
 * __wrap_geteuid() and __wrap_getegid() (see the link line in
 * check-environment.py); the case that needs a real set-user-ID process runs
 * when the program is started by the superuser (see ttsession_drops_privileges).
 * Each case runs in its own process (environment-test CASE).
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "tt_options.h"
#include "util/tt_global_env.h"
#include "util/tt_string.h"
#include "util/tt_secure_env.h"
#include "util/tt_spawn.h"
#include "util/tt_trace.h"
#include "util/tt_path.h"
#include "util/tt_ldpath.h"
#include "mp/mp_auth_functions.h"

/* the one place that needs the members of a session the tests drive */
#define private public
#define protected public
#include "mp/mp_c_session.h"
#undef private
#undef protected

static int failures;

#define CHECK(cond) \
  do { \
    if(!(cond)) { \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
      failures++; \
    } \
  } while(0)

/* ---- a process with other effective ids than real ones --------------- */

extern "C" uid_t __real_geteuid(void);
extern "C" gid_t __real_getegid(void);

static enum { PLAIN, EUID_DIFFERS, EGID_DIFFERS } fake_ids = PLAIN;
static pid_t fake_pid;

/*
 * Only the process of the case is privileged: the programs it starts see the
 * real ids, as a child that has given up its privileges would.
 */
extern "C" uid_t
__wrap_geteuid(void)
{
  if(fake_ids == EUID_DIFFERS && getpid() == fake_pid)
    return getuid() + 1;
  return __real_geteuid();
}

extern "C" gid_t
__wrap_getegid(void)
{
  if(fake_ids == EGID_DIFFERS && getpid() == fake_pid)
    return getgid() + 1;
  return __real_getegid();
}

static void
become_privileged(void)
{
  fake_ids = EUID_DIFFERS;
  fake_pid = getpid();
}

/* ---- fixtures ---------------------------------------------------------- */

static std::string casedir;

static void
setup(void)
{
  const char *base = getenv("TT_ENV_TEST_DIR");
  char path[PATH_MAX];

  snprintf(path, sizeof(path), "%s/case-XXXXXX", base ? base : ".");
  if(!mkdtemp(path)) {
    perror("mkdtemp");
    exit(2);
  }
  casedir = path;
  chmod(path, 0755);
  /* nothing from the environment of the run leaks into a case */
  unsetenv("TTSESSION_CMD");
  unsetenv("SUN_TTSESSION_CMD");
  unsetenv("TT_TRACE_SCRIPT");
  unsetenv("TTAUTHORITY");
  unsetenv("LD_LIBRARY_PATH");
  unsetenv("TTPATH");
}

static std::string
path_of(const char *name)
{
  return casedir + "/" + name;
}

static bool
exists(const std::string &path)
{
  struct stat st;
  return lstat(path.c_str(), &st) == 0;
}

static void
write_file(const std::string &path, const std::string &text, mode_t mode = 0644)
{
  FILE *f = fopen(path.c_str(), "w");
  if(!f) {
    perror(path.c_str());
    exit(2);
  }
  fputs(text.c_str(), f);
  fclose(f);
  chmod(path.c_str(), mode);
}

static std::string
read_file(const std::string &path)
{
  std::string text;
  FILE *f = fopen(path.c_str(), "r");
  char buf[512];
  size_t n;

  if(!f)
    return text;
  while((n = fread(buf, 1, sizeof(buf), f)) > 0)
    text.append(buf, n);
  fclose(f);
  return text;
}

/* the NUL separated words of a file */
/* whether PATH, which may be empty or missing, lies under the case directory */
static bool
in_casedir(const char *path)
{
  return path != 0 && strncmp(path, casedir.c_str(), casedir.size()) == 0;
}

static std::vector<std::string>
words_of(const std::string &text)
{
  std::vector<std::string> words;
  size_t start = 0;

  for(size_t i = 0; i < text.size(); i++)
    if(text[i] == '\0') {
      words.push_back(text.substr(start, i - start));
      start = i + 1;
    }
  return words;
}

/*
 * A program that records its arguments and ids in the files named by
 * FAKE_OUT: the arguments NUL separated, "real effective" ids in FAKE_OUT.ids.
 * (-p: a shell that is not told otherwise gives up its effective ids.)
 */
static void
install_fake(const char *name)
{
  write_file(path_of(name),
	     "#!/bin/sh -p\n"
	     "printf '%s\\0' \"$@\" > \"$FAKE_OUT\"\n"
	     "echo \"$(id -ru) $(id -u) $(id -rg) $(id -g)\" > \"$FAKE_OUT.ids\"\n"
	     "exit ${FAKE_EXIT:-0}\n", 0755);
}

static void
use_fakes_from_casedir(void)
{
  std::string path = casedir;
  const char *old = getenv("PATH");

  if(old) {
    path += ":";
    path += old;
  }
  setenv("PATH", path.c_str(), 1);
  setenv("FAKE_OUT", path_of("fake.out").c_str(), 1);
}

static std::vector<std::string>
fake_arguments(void)
{
  return words_of(read_file(path_of("fake.out")));
}

/* ---- the privilege test --------------------------------------------------- */

static void
test_privileged_predicate(void)
{
  setup();
  CHECK(!_tt_privileged_process());
  fake_ids = EUID_DIFFERS;
  fake_pid = getpid();
  CHECK(_tt_privileged_process());
  fake_ids = EGID_DIFFERS;
  CHECK(_tt_privileged_process());
  fake_pid = getpid() + 1;		/* some other process */
  CHECK(!_tt_privileged_process());
}

static void
test_secure_getenv(void)
{
  setup();
  setenv("TT_ENV_TEST_VAR", "value", 1);
  CHECK(_tt_secure_getenv("TT_ENV_TEST_VAR") != 0);
  CHECK(!strcmp(_tt_secure_getenv("TT_ENV_TEST_VAR"), "value"));
  CHECK(_tt_secure_getenv("TT_ENV_TEST_UNSET") == 0);
  CHECK(_tt_secure_getenv(0) == 0);
  become_privileged();
  CHECK(_tt_secure_getenv("TT_ENV_TEST_VAR") == 0);
  fake_ids = EGID_DIFFERS;
  CHECK(_tt_secure_getenv("TT_ENV_TEST_VAR") == 0);
}

static void
test_secure_home(void)
{
  setup();
  struct passwd *pw = getpwuid(getuid());
  std::string pw_dir = pw ? pw->pw_dir : "";

  setenv("HOME", "/nonexistent/environment-home", 1);
  CHECK(!strcmp(_tt_secure_home(), "/nonexistent/environment-home"));
  become_privileged();
  const char *home = _tt_secure_home();
  CHECK(home == 0 || pw_dir == home);
  CHECK(home == 0 || strcmp(home, "/nonexistent/environment-home") != 0);
}

/* ---- running programs ----------------------------------------------------- */

static void
test_argv_words(void)
{
  _Tt_argv argv;

  CHECK(argv.add_words("  ttsession  -X\t--flag\n x ") == TRUE);
  CHECK(argv.count() == 4);
  CHECK(!strcmp(argv.vector()[0], "ttsession"));
  CHECK(!strcmp(argv.vector()[1], "-X"));
  CHECK(!strcmp(argv.vector()[2], "--flag"));
  CHECK(!strcmp(argv.vector()[3], "x"));
  CHECK(argv.vector()[4] == 0);
  CHECK(argv.add("a b") == TRUE);
  CHECK(!strcmp((const char *)argv.text(), "ttsession -X --flag x a b"));

  _Tt_argv empty;
  CHECK(empty.add_words("   \t\n") == TRUE);
  CHECK(empty.count() == 0);
  CHECK(empty.add_words(0) == TRUE);
  CHECK(empty.count() == 0);
}

static void
test_run_arguments_reach_the_program_unchanged(void)
{
  setup();
  install_fake("fakecmd");
  use_fakes_from_casedir();
  std::string marker = path_of("injected");
  std::string tricky[] = {
    "a b", "x;touch " + marker, "$(touch " + marker + ")", "`touch " + marker + "`",
    "*", "~", "", "-n", "a\nb", "'quoted'", "\"dq\"", "a|b", "a>" + marker,
  };
  _Tt_argv argv;
  CHECK(argv.add("fakecmd"));
  for(const std::string &a : tricky)
    CHECK(argv.add(a.c_str()));
  int status = _tt_run(argv);
  CHECK(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  std::vector<std::string> got = fake_arguments();
  CHECK(got.size() == sizeof(tricky) / sizeof(tricky[0]));
  for(size_t i = 0; i < got.size() && i < sizeof(tricky) / sizeof(tricky[0]); i++)
    CHECK(got[i] == tricky[i]);
  CHECK(!exists(marker));
}

static void
test_run_status_and_directory(void)
{
  setup();
  install_fake("fakecmd");
  use_fakes_from_casedir();
  mkdir(path_of("sub").c_str(), 0755);

  _Tt_argv missing;
  CHECK(missing.add("tt-environment-test-no-such-program"));
  int status = _tt_run(missing);
  CHECK(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 127);

  _Tt_argv failing;
  CHECK(failing.add("fakecmd"));
  setenv("FAKE_EXIT", "7", 1);
  status = _tt_run(failing);
  CHECK(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 7);
  unsetenv("FAKE_EXIT");

  _Tt_argv where;
  CHECK(where.add("sh") && where.add("-c") && where.add("pwd > \"$1\"") &&
	where.add("sh") && where.add(path_of("pwd.out").c_str()));
  status = _tt_run(where, path_of("sub").c_str());
  CHECK(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  std::string cwd = read_file(path_of("pwd.out"));
  CHECK(cwd.find("/sub\n") != std::string::npos);

  CHECK(_tt_run(_Tt_argv()) == -1);
  _Tt_argv bad_dir;
  CHECK(bad_dir.add("true"));
  status = _tt_run(bad_dir, path_of("no-such-directory").c_str());
  CHECK(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 127);
}

static _Tt_argv *
shell(const std::string &script, const std::string &arg = "")
{
  _Tt_argv *argv = new _Tt_argv;
  argv->add("sh");
  argv->add("-c");
  argv->add(script.c_str());
  argv->add("sh");
  if(arg.size())
    argv->add(arg.c_str());
  return argv;
}

static void
test_run_pipeline(void)
{
  setup();
  mkdir(path_of("from").c_str(), 0755);
  mkdir(path_of("to").c_str(), 0755);
  std::string out = path_of("pipe.out");

  _Tt_argv *producer = shell("pwd; echo payload");
  _Tt_argv *consumer = shell("cat > \"$1\"; pwd >> \"$1\"", out);
  int status = _tt_run_pipeline(path_of("from").c_str(), *producer,
				path_of("to").c_str(), *consumer);
  CHECK(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  std::string got = read_file(out);
  CHECK(got.find("/from\npayload\n") != std::string::npos);
  CHECK(got.find("/to\n") != std::string::npos);

  /* the status of the consumer, or else the one of the producer */
  delete producer;
  delete consumer;
  producer = shell("echo x; exit 3");
  consumer = shell("cat > /dev/null");
  status = _tt_run_pipeline(0, *producer, 0, *consumer);
  CHECK(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 3);
  delete consumer;
  consumer = shell("cat > /dev/null; exit 4");
  status = _tt_run_pipeline(0, *producer, 0, *consumer);
  CHECK(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 4);
  delete producer;
  delete consumer;
}

static void
test_popen_read(void)
{
  setup();
  _Tt_argv echo;
  CHECK(echo.add("echo") && echo.add("a b;c"));
  int pid = 0;
  FILE *f = _tt_popen_read(echo, &pid);
  CHECK(f != 0);
  if(f) {
    char line[64] = "";
    CHECK(fgets(line, sizeof(line), f) != 0);
    CHECK(!strcmp(line, "a b;c\n"));
    int status = _tt_pclose_read(f, pid);
    CHECK(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }
  _Tt_argv *failing = shell("echo partial; exit 5");
  f = _tt_popen_read(*failing, &pid);
  CHECK(f != 0);
  if(f) {
    char line[64];
    while(fgets(line, sizeof(line), f))
      ;
    int status = _tt_pclose_read(f, pid);
    CHECK(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 5);
  }
  delete failing;
}

/* ---- starting ttsession ------------------------------------------------- */

static int
start_ttsession(const char *display, std::string &shown)
{
  _Tt_c_session session;
  _Tt_string command;

  session._displayname = display;
  int status = session.startup_ttsession(command);
  shown = (const char *)command;
  return status;
}

static void
test_ttsession_standard_command(void)
{
  setup();
  install_fake("ttsession");
  use_fakes_from_casedir();
  std::string shown;
  int status = start_ttsession(":7.0", shown);
  CHECK(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  std::vector<std::string> got = fake_arguments();
  CHECK(got.size() == 3);
  if(got.size() == 3) {
    CHECK(got[0] == "-s");
    CHECK(got[1] == "-d");
    CHECK(got[2] == ":7.0");
  }
  CHECK(shown == "ttsession -s -d :7.0");
}

static void
test_ttsession_command_from_environment(void)
{
  setup();
  install_fake("fakecmd");
  use_fakes_from_casedir();
  setenv("TTSESSION_CMD", "fakecmd -X  --verbose", 1);
  std::string shown;
  int status = start_ttsession(":7.0", shown);
  CHECK(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  std::vector<std::string> got = fake_arguments();
  CHECK(got.size() == 4);
  if(got.size() == 4) {
    CHECK(got[0] == "-X");
    CHECK(got[1] == "--verbose");
    CHECK(got[2] == "-d");
    CHECK(got[3] == ":7.0");
  }
  CHECK(shown == "fakecmd -X --verbose -d :7.0");

  /* the old name is used when the new one is not set */
  unsetenv("TTSESSION_CMD");
  setenv("SUN_TTSESSION_CMD", "fakecmd -Y", 1);
  unlink(path_of("fake.out").c_str());
  status = start_ttsession(":8.0", shown);
  CHECK(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  got = fake_arguments();
  CHECK(got.size() == 3 && got[0] == "-Y" && got[2] == ":8.0");
}

static void
test_ttsession_command_is_not_a_shell_line(void)
{
  setup();
  install_fake("fakecmd");
  use_fakes_from_casedir();
  std::string marker = path_of("injected");
  std::string redirect = path_of("redirected");
  setenv("TTSESSION_CMD",
	 ("fakecmd >" + redirect + "; touch " + marker + " $(touch " + marker + ")").c_str(), 1);
  std::string shown;
  int status = start_ttsession(":7.0", shown);
  CHECK(status != -1 && WIFEXITED(status));
  CHECK(!exists(marker));
  CHECK(!exists(redirect));
  /* the words went to the program as they stand */
  std::vector<std::string> got = fake_arguments();
  CHECK(got.size() == 7);
  if(got.size() == 7) {
    CHECK(got[0] == ">" + redirect + ";");
    CHECK(got[1] == "touch");
    CHECK(got[5] == "-d");
  }
}

static void
test_ttsession_display_is_one_argument(void)
{
  setup();
  install_fake("fakecmd");
  use_fakes_from_casedir();
  std::string marker = path_of("injected");
  setenv("TTSESSION_CMD", "fakecmd", 1);
  std::string display = "host:0;touch " + marker;
  std::string shown;
  int status = start_ttsession(display.c_str(), shown);
  CHECK(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  CHECK(!exists(marker));
  std::vector<std::string> got = fake_arguments();
  CHECK(got.size() == 2 && got[0] == "-d" && got[1] == display);
}

static void
test_ttsession_environment_ignored_when_privileged(void)
{
  setup();
  install_fake("ttsession");
  install_fake("fakecmd");
  use_fakes_from_casedir();
  std::string marker = path_of("ran-fakecmd");
  /* fakecmd would leave this behind; the standard command does not */
  write_file(path_of("fakecmd"),
	     "#!/bin/sh\ntouch \"$FAKE_MARK\"\nexit 0\n", 0755);
  setenv("FAKE_MARK", marker.c_str(), 1);
  setenv("TTSESSION_CMD", "fakecmd", 1);
  setenv("SUN_TTSESSION_CMD", "fakecmd", 1);
  become_privileged();
  std::string shown;
  int status = start_ttsession(":7.0", shown);
  CHECK(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  CHECK(!exists(marker));
  std::vector<std::string> got = fake_arguments();
  CHECK(got.size() == 3 && got[0] == "-s");
  CHECK(shown == "ttsession -s -d :7.0");
}

/*
 * With the superuser's help: a process whose real user is another one than
 * its effective user, as a set-user-ID program is, starts ttsession.  The
 * program gives up the privileges before it executes anything.  Started as an
 * ordinary user this case has nothing to do.  TT_ENV_TEST_REAL_UID and
 * TT_ENV_TEST_REAL_GID name the ids the process continues with as its real
 * ids.
 */
static void
test_ttsession_drops_privileges(void)
{
  if(__real_geteuid() != 0 || !getenv("TT_ENV_TEST_REAL_UID") ||
     !getenv("TT_ENV_TEST_REAL_GID")) {
    printf("skipped: needs the superuser and TT_ENV_TEST_REAL_UID/GID\n");
    return;
  }
  uid_t ruid = (uid_t)atol(getenv("TT_ENV_TEST_REAL_UID"));
  gid_t rgid = (gid_t)atol(getenv("TT_ENV_TEST_REAL_GID"));

  setup();
  install_fake("ttsession");
  use_fakes_from_casedir();
  chown(casedir.c_str(), ruid, rgid);
  chown(path_of("ttsession").c_str(), ruid, rgid);
  /* the ids of a set-user-ID program: real user, effective root */
  if(setresgid(rgid, 0, 0) != 0 || setresuid(ruid, 0, 0) != 0) {
    perror("setresuid");
    exit(2);
  }
  CHECK(_tt_privileged_process());
  setenv("FAKE_OUT", path_of("fake.out").c_str(), 1);
  std::string shown;
  int status = start_ttsession(":7.0", shown);
  CHECK(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  /* "real effective" ids of the program the child executed */
  char want[64];
  snprintf(want, sizeof(want), "%u %u %u %u\n", (unsigned)ruid, (unsigned)ruid,
	   (unsigned)rgid, (unsigned)rgid);
  CHECK(read_file(path_of("fake.out.ids")) == want);
}

/* ---- trace scripts ------------------------------------------------------ */

static void
test_trace_script_from_environment(void)
{
  setup();
  std::string sink = path_of("trace.out");
  setenv("TT_TRACE_SCRIPT", ("functions all\n> " + sink + "\n").c_str(), 1);
  _Tt_trace::fini();
  int on = _Tt_trace::init(0);
  CHECK(on == 1);
  CHECK(exists(sink));
  _Tt_trace::fini();
}

static void
test_trace_script_file_from_environment(void)
{
  setup();
  std::string sink = path_of("trace.out");
  std::string script = path_of("trace.script");
  write_file(script, "functions all\n> " + sink + "\n");
  setenv("TT_TRACE_SCRIPT", script.c_str(), 1);
  _Tt_trace::fini();
  CHECK(_Tt_trace::init(0) == 1);
  CHECK(exists(sink));
  _Tt_trace::fini();
}

static void
test_trace_script_ignored_when_privileged(void)
{
  setup();
  std::string sink = path_of("trace.out");
  std::string script = path_of("trace.script");
  write_file(script, "functions all\n> " + sink + "\n");
  become_privileged();
  setenv("TT_TRACE_SCRIPT", script.c_str(), 1);
  /* tracing that is not asked for by the program stays off ... */
  _Tt_trace::fini();
  CHECK(_Tt_trace::init(0) == 0);
  CHECK(!exists(sink));
  /* ... and the script is not read when the program asks for it */
  _Tt_trace::fini();
  _Tt_trace::init(1);
  CHECK(!exists(sink));
  _Tt_trace::fini();
  /* the script given as text */
  setenv("TT_TRACE_SCRIPT", ("functions all\n> " + sink + "\n").c_str(), 1);
  CHECK(_Tt_trace::init(0) == 0);
  _Tt_trace::init(1);
  CHECK(!exists(sink));
  _Tt_trace::fini();
}

static void
test_trace_script_file_missing(void)
{
  setup();
  setenv("TT_TRACE_SCRIPT", path_of("no-such-script").c_str(), 1);
  _Tt_trace::fini();
  CHECK(_Tt_trace::init(0) == 0);
  _Tt_trace::fini();
}

/* ---- files and libraries the environment names ------------------------ */

static void
test_ldpath_from_environment(void)
{
  setup();
  std::string lib = path_of("libX11.so");
  write_file(lib, "not a library\n");
  setenv("LD_LIBRARY_PATH", casedir.c_str(), 1);
  _Tt_string found;
  CHECK(tt_ldpath("libX11.so", found));
  CHECK(found == lib.c_str());
}

static void
test_ldpath_ignored_when_privileged(void)
{
  setup();
  std::string lib = path_of("libX11.so");
  write_file(lib, "not a library\n");
  setenv("LD_LIBRARY_PATH", casedir.c_str(), 1);
  become_privileged();
  _Tt_string found;
  if(tt_ldpath("libX11.so", found))
    CHECK(!in_casedir((const char *)found));
}

static void
test_auth_file_name(void)
{
  setup();
  setenv("TTAUTHORITY", path_of("authority").c_str(), 1);
  setenv("HOME", casedir.c_str(), 1);
  CHECK(!strcmp(_tt_AuthFileName(), path_of("authority").c_str()));
  unsetenv("TTAUTHORITY");
  CHECK(!strcmp(_tt_AuthFileName(), path_of(".TTauthority").c_str()));
}

static void
test_auth_file_name_when_privileged(void)
{
  setup();
  struct passwd *pw = getpwuid(getuid());
  std::string pw_dir = pw ? pw->pw_dir : "";

  setenv("TTAUTHORITY", path_of("authority").c_str(), 1);
  setenv("HOME", casedir.c_str(), 1);
  become_privileged();
  char *name = _tt_AuthFileName();
  CHECK(name == 0 || strcmp(name, path_of("authority").c_str()) != 0);
  CHECK(!in_casedir(name));
  CHECK(name == 0 || pw_dir + "/.TTauthority" == name);
}

static void
test_user_path_from_environment(void)
{
  setup();
  std::string file = path_of("types.xdr");
  write_file(file, "types\n");
  setenv("TT_ENV_TEST_FILE", file.c_str(), 1);
  _Tt_string found = _tt_user_path("types.xdr", "TT_ENV_TEST_FILE", FALSE);
  CHECK(found == file.c_str());

  /* the directories of TTPATH come before the standard ones */
  unsetenv("TT_ENV_TEST_FILE");
  mkdir(path_of("user").c_str(), 0755);
  write_file(path_of("user/other.xdr"), "types\n");
  setenv("TTPATH", path_of("user").c_str(), 1);
  found = _tt_user_path("other.xdr", "TT_ENV_TEST_FILE", FALSE);
  CHECK(found == path_of("user/other.xdr").c_str());
}

static void
test_user_path_ignored_when_privileged(void)
{
  setup();
  std::string file = path_of("types.xdr");
  write_file(file, "types\n");
  mkdir(path_of("user").c_str(), 0755);
  write_file(path_of("user/other.xdr"), "types\n");
  setenv("TT_ENV_TEST_FILE", file.c_str(), 1);
  setenv("TTPATH", path_of("user").c_str(), 1);
  setenv("HOME", casedir.c_str(), 1);
  mkdir(path_of(".tt").c_str(), 0755);
  write_file(path_of(".tt/home.xdr"), "types\n");
  become_privileged();
  _Tt_string found = _tt_user_path("types.xdr", "TT_ENV_TEST_FILE", FALSE);
  CHECK(!in_casedir((const char *)found));
  found = _tt_user_path("other.xdr", "TT_ENV_TEST_FILE", FALSE);
  CHECK(!in_casedir((const char *)found));
  found = _tt_user_path("home.xdr", "TT_ENV_TEST_FILE", FALSE);
  CHECK(!in_casedir((const char *)found));
}

/* ---- the table of cases ---------------------------------------------------- */

struct Case {
  std::string name;
  void (*run)(void);
};

int
main(int argc, char **argv)
{
  std::vector<Case> cases;
  auto add = [&](const char *name, void (*run)(void)) {
    cases.push_back(Case{name, run});
  };

  add("privileged_predicate", test_privileged_predicate);
  add("secure_getenv", test_secure_getenv);
  add("secure_home", test_secure_home);
  add("argv_words", test_argv_words);
  add("run_arguments_unchanged", test_run_arguments_reach_the_program_unchanged);
  add("run_status_and_directory", test_run_status_and_directory);
  add("run_pipeline", test_run_pipeline);
  add("popen_read", test_popen_read);
  add("ttsession_standard_command", test_ttsession_standard_command);
  add("ttsession_command_from_environment", test_ttsession_command_from_environment);
  add("ttsession_command_is_not_a_shell_line", test_ttsession_command_is_not_a_shell_line);
  add("ttsession_display_is_one_argument", test_ttsession_display_is_one_argument);
  add("ttsession_environment_ignored_when_privileged",
      test_ttsession_environment_ignored_when_privileged);
  add("ttsession_drops_privileges", test_ttsession_drops_privileges);
  add("trace_script_from_environment", test_trace_script_from_environment);
  add("trace_script_file_from_environment", test_trace_script_file_from_environment);
  add("trace_script_ignored_when_privileged", test_trace_script_ignored_when_privileged);
  add("trace_script_file_missing", test_trace_script_file_missing);
  add("ldpath_from_environment", test_ldpath_from_environment);
  add("ldpath_ignored_when_privileged", test_ldpath_ignored_when_privileged);
  add("auth_file_name", test_auth_file_name);
  add("auth_file_name_when_privileged", test_auth_file_name_when_privileged);
  add("user_path_from_environment", test_user_path_from_environment);
  add("user_path_ignored_when_privileged", test_user_path_ignored_when_privileged);

  if(argc == 2 && !strcmp(argv[1], "--list")) {
    for(const Case &c : cases)
      printf("%s\n", c.name.c_str());
    return 0;
  }
  if(argc != 2) {
    fprintf(stderr, "usage: %s --list | CASE\n", argv[0]);
    return 2;
  }
  for(const Case &c : cases) {
    if(c.name != argv[1])
      continue;
    c.run();
    return failures ? 1 : 0;
  }
  fprintf(stderr, "unknown case %s\n", argv[1]);
  return 2;
}
