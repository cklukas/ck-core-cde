#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define NO_MESSAGE_CATALOG 1
char *XeProgName = "msglog-test";

char *Xestrdup(const char *s) { return strdup(s); }
int Xestrcmp(const char *a, const char *b) { return strcmp(a, b); }
char *Xestrstr(const char *a, const char *b) { return (char *)strstr(a, b); }
char *Xestrchr(const char *s, int c) { return (char *)strchr(s, c); }
size_t Xestrlen(const char *s) { return strlen(s); }

#include "../DtUtil2/MsgLog.c"
#include "../DtUtil2/UErrNoBMS.c"

static char *
slurp_file(const char *path, size_t *len_out)
{
    struct stat st;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return NULL;
    }
    size_t sz = (size_t)st.st_size;
    char *buf = malloc(sz + 1);
    assert(buf != NULL);
    size_t got = 0;
    while (got < sz) {
        ssize_t nr = read(fd, buf + got, sz - got);
        if (nr <= 0)
            break;
        got += (size_t)nr;
    }
    close(fd);
    buf[got] = '\0';
    if (len_out)
        *len_out = got;
    return buf;
}

static void
reset_env(void)
{
    unsetenv("CDE_DEBUG");
    unsetenv("DT_ERRORLOG_LEVEL");
    unsetenv("DT_ERRORLOG_TRIM");
    unsetenv("DT_ERRORLOG_MAX_BYTES");
    unsetenv("DT_ERRORLOG_MAX_SIZE");
    unsetenv("DT_ERRORLOG_DEDUP");
}

int
main(int argc, char **argv)
{
    assert(argc == 2);
    const char *base_dir = argv[1];
    char home[MAXPATHLEN];
    char dt_dir[MAXPATHLEN];
    char errlog[MAXPATHLEN];
    char conf_path[MAXPATHLEN];

    /* 1. Default level filtering & UErrNoBMS severity mapping */
    snprintf(home, sizeof(home), "%s/h1", base_dir);
    snprintf(dt_dir, sizeof(dt_dir), "%s/.dt", home);
    snprintf(errlog, sizeof(errlog), "%s/errorlog", dt_dir);
    mkdir(home, 0755);
    mkdir(dt_dir, 0755);
    setenv("HOME", home, 1);
    reset_env();

    DtMsgLogMessage("testprog", DtMsgLogDebug, "hidden-debug-1\n");
    _DtSimpleError("testprog", DtIgnore, NULL, "%s", "hidden-dtignore-1\n");
    _DtSimpleError("testprog", DtInformation, NULL, "%s", "visible-dtinfo-1\n");
    _DtSimpleError("testprog", DtWarning, NULL, "%s", "visible-dtwarn-1\n");

    size_t sz = 0;
    char *content = slurp_file(errlog, &sz);
    assert(content != NULL);
    assert(strstr(content, "hidden-debug-1") == NULL);
    assert(strstr(content, "hidden-dtignore-1") == NULL);
    assert(strstr(content, "*** INFORMATION(0): testprog:") != NULL);
    assert(strstr(content, "visible-dtinfo-1") != NULL);
    assert(strstr(content, "*** WARNING(3): testprog:") != NULL);
    assert(strstr(content, "visible-dtwarn-1") != NULL);
    free(content);

    setenv("DT_ERRORLOG_LEVEL", "debug", 1);
    DtMsgLogMessage("testprog", DtMsgLogDebug, "shown-debug-2\n");
    content = slurp_file(errlog, &sz);
    assert(content != NULL);
    assert(strstr(content, "*** DEBUG(2): testprog:") != NULL);
    assert(strstr(content, "shown-debug-2") != NULL);
    free(content);

    /* 2. Automatic trimming enabled by default (tested with 8 KiB cap) */
    snprintf(home, sizeof(home), "%s/h2", base_dir);
    snprintf(dt_dir, sizeof(dt_dir), "%s/.dt", home);
    snprintf(errlog, sizeof(errlog), "%s/errorlog", dt_dir);
    mkdir(home, 0755);
    mkdir(dt_dir, 0755);
    setenv("HOME", home, 1);
    reset_env();
    setenv("DT_ERRORLOG_MAX_BYTES", "8192", 1);

    DtMsgLogMessage("trimtest", DtMsgLogWarning, "entry-0000\n");
    struct stat st_initial;
    assert(stat(errlog, &st_initial) == 0);

    for (int i = 1; i <= 260; i++) {
        DtMsgLogMessage("trimtest", DtMsgLogWarning,
                        "distinct-record-%04d-payload-0123456789abcdef\n", i);
    }

    struct stat st_trimmed;
    assert(stat(errlog, &st_trimmed) == 0);
    assert(st_trimmed.st_ino == st_initial.st_ino);
    assert(st_trimmed.st_size > 0 && st_trimmed.st_size <= 8192);
    content = slurp_file(errlog, &sz);
    assert(content != NULL);
    assert(strstr(content, "Trimmed errorlog") != NULL);
    assert(strstr(content, "distinct-record-0001-") == NULL);
    assert(strstr(content, "distinct-record-0260-") != NULL);
    free(content);

    /* 3. Disabling automatic trimming via DT_ERRORLOG_TRIM=0 and errorlog.conf */
    snprintf(home, sizeof(home), "%s/h3", base_dir);
    snprintf(dt_dir, sizeof(dt_dir), "%s/.dt", home);
    snprintf(errlog, sizeof(errlog), "%s/errorlog", dt_dir);
    snprintf(conf_path, sizeof(conf_path), "%s/errorlog.conf", dt_dir);
    mkdir(home, 0755);
    mkdir(dt_dir, 0755);
    FILE *cf = fopen(conf_path, "w");
    assert(cf != NULL);
    fputs("trim = false\nmax_bytes = 8192\n", cf);
    fclose(cf);

    setenv("HOME", home, 1);
    reset_env();

    for (int i = 1; i <= 150; i++) {
        DtMsgLogMessage("notrim", DtMsgLogWarning,
                        "untrimmed-record-%04d-payload-0123456789abcdef\n", i);
    }
    struct stat st_untrimmed;
    assert(stat(errlog, &st_untrimmed) == 0);
    assert(st_untrimmed.st_size > 8192);
    content = slurp_file(errlog, &sz);
    assert(content != NULL);
    assert(strstr(content, "Trimmed errorlog") == NULL);
    assert(strstr(content, "untrimmed-record-0001-") != NULL);
    assert(strstr(content, "untrimmed-record-0150-") != NULL);
    free(content);

    /* 4. Tight-loop burst deduplication */
    snprintf(home, sizeof(home), "%s/h4", base_dir);
    snprintf(dt_dir, sizeof(dt_dir), "%s/.dt", home);
    snprintf(errlog, sizeof(errlog), "%s/errorlog", dt_dir);
    mkdir(home, 0755);
    mkdir(dt_dir, 0755);
    setenv("HOME", home, 1);
    reset_env();

    for (int i = 0; i < 1000; i++) {
        DtMsgLogMessage("flooder", DtMsgLogWarning, "identical-spam-line\n");
    }
    DtMsgLogMessage("flooder", DtMsgLogError, "after-flood-marker\n");

    struct stat st_dedup;
    assert(stat(errlog, &st_dedup) == 0);
    assert(st_dedup.st_size < 4096);
    content = slurp_file(errlog, &sz);
    assert(content != NULL);
    assert(strstr(content, "[suppressed ") != NULL);
    assert(strstr(content, "after-flood-marker") != NULL);
    free(content);

    printf("PASS check-msglog-trim\n");
    return 0;
}
