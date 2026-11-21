/*
 * CDE - Common Desktop Environment
 *
 * iTIP (RFC 5546) meeting invitation parser and dtcm handoff for dtmail.
 */

#include "ItipBanner.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static DtMailItipInvite g_cur_invite;
static char *g_cur_ics = NULL;
static size_t g_cur_ics_len = 0;

static void trim_crlf(char *s)
{
    size_t l = strlen(s);
    while (l > 0 && (s[l - 1] == '\r' || s[l - 1] == '\n' || s[l - 1] == ' ')) {
        s[--l] = '\0';
    }
}

static int parse_prop(const char *line, const char *prop_name, char *out, size_t out_len)
{
    size_t plen = strlen(prop_name);
    if (strncasecmp(line, prop_name, plen) != 0) return 0;
    if (line[plen] != ':' && line[plen] != ';') return 0;
    const char *colon = strchr(line + plen, ':');
    if (!colon) return 0;
    snprintf(out, out_len, "%s", colon + 1);
    trim_crlf(out);
    return 1;
}

int dtmail_itip_parse(const char *raw, size_t len, DtMailItipInvite *out)
{
    if (!raw || len == 0 || !out) return -1;
    memset(out, 0, sizeof(*out));

    /* Find BEGIN:VCALENDAR */
    const char *start = strstr(raw, "BEGIN:VCALENDAR");
    if (!start) return -1;
    const char *end = strstr(start, "END:VCALENDAR");
    if (!end) return -1;
    end += strlen("END:VCALENDAR");
    if ((size_t)(end - raw) > len) return -1;

    int in_vevent = 0;
    const char *p = start;
    while (p < end) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        size_t llen = nl ? (size_t)(nl - p) : (size_t)(end - p);
        char line[512];
        if (llen >= sizeof(line)) llen = sizeof(line) - 1;
        memcpy(line, p, llen);
        line[llen] = '\0';
        trim_crlf(line);

        if (strcasecmp(line, "BEGIN:VEVENT") == 0) {
            in_vevent = 1;
        } else if (strcasecmp(line, "END:VEVENT") == 0) {
            in_vevent = 0;
        } else if (!in_vevent) {
            (void)parse_prop(line, "METHOD", out->method, sizeof(out->method));
        } else {
            (void)parse_prop(line, "SUMMARY", out->summary, sizeof(out->summary));
            (void)parse_prop(line, "DTSTART", out->dtstart, sizeof(out->dtstart));
            (void)parse_prop(line, "DTEND", out->dtend, sizeof(out->dtend));
            (void)parse_prop(line, "LOCATION", out->location, sizeof(out->location));
            (void)parse_prop(line, "ORGANIZER", out->organizer, sizeof(out->organizer));
            (void)parse_prop(line, "UID", out->uid, sizeof(out->uid));
        }
        p = nl ? nl + 1 : end;
    }

    if (out->summary[0] || out->dtstart[0]) {
        out->valid = 1;
        return 0;
    }
    return -1;
}

int dtmail_itip_open_in_dtcm(const char *ics_raw, size_t ics_len,
                             char *out_saved_path, size_t out_path_len)
{
    if (!ics_raw || ics_len == 0) return -1;
    const char *home = getenv("HOME");
    if (!home || !home[0]) return -1;

    char dir1[1024], dir2[1024], path[1024];
    snprintf(dir1, sizeof(dir1), "%s/.dt", home);
    snprintf(dir2, sizeof(dir2), "%s/.dt/calendar", home);
    (void)mkdir(dir1, 0700);
    (void)mkdir(dir2, 0700);
    snprintf(path, sizeof(path), "%s/incoming_invite.ics", dir2);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    (void)fchmod(fd, 0600);
    size_t written = 0;
    while (written < ics_len) {
        ssize_t n = write(fd, ics_raw + written, ics_len - written);
        if (n <= 0) { close(fd); return -1; }
        written += (size_t)n;
    }
    close(fd);

    if (out_saved_path && out_path_len > 0) {
        snprintf(out_saved_path, out_path_len, "%s", path);
    }

    if (getenv("DTMAIL_NO_LAUNCH_DTCM")) {
        return 0;
    }

    pid_t pid = fork();
    if (pid == 0) {
        execlp("/usr/dt/bin/dtcm", "dtcm", (char *)NULL);
        execlp("dtcm", "dtcm", (char *)NULL);
        _exit(127);
    }
    return (pid > 0) ? 0 : -1;
}

void dtmail_itip_set_current(const char *raw, size_t len)
{
    free(g_cur_ics);
    g_cur_ics = NULL;
    g_cur_ics_len = 0;
    memset(&g_cur_invite, 0, sizeof(g_cur_invite));

    if (!raw || len == 0) return;
    if (dtmail_itip_parse(raw, len, &g_cur_invite) == 0 && g_cur_invite.valid) {
        const char *start = strstr(raw, "BEGIN:VCALENDAR");
        const char *end = start ? strstr(start, "END:VCALENDAR") : NULL;
        if (start && end) {
            size_t ilen = (size_t)(end + strlen("END:VCALENDAR") - start);
            g_cur_ics = (char *)malloc(ilen + 1);
            if (g_cur_ics) {
                memcpy(g_cur_ics, start, ilen);
                g_cur_ics[ilen] = '\0';
                g_cur_ics_len = ilen;
            }
        }
    }
}

int dtmail_itip_has_current(void)
{
    return (g_cur_invite.valid && g_cur_ics && g_cur_ics_len > 0) ? 1 : 0;
}

const DtMailItipInvite *dtmail_itip_get_current(void)
{
    return dtmail_itip_has_current() ? &g_cur_invite : NULL;
}

int dtmail_itip_open_current_in_dtcm(void)
{
    if (!dtmail_itip_has_current()) return -1;
    return dtmail_itip_open_in_dtcm(g_cur_ics, g_cur_ics_len, NULL, 0);
}
