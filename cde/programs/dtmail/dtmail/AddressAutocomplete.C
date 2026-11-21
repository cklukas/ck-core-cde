/*
 * CDE - Common Desktop Environment
 *
 * Sender & recipient autocomplete contact store (~/.dt/dtmail_contacts, 0600).
 */

#include "AddressAutocomplete.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static DtMailContact g_contacts[DTMAIL_MAX_CONTACTS];
static int g_ncontacts = 0;
static int g_contacts_inited = 0;

static int str_contains_ci(const char *hay, const char *needle)
{
    if (!needle || !needle[0]) return 1;
    if (!hay) return 0;
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) return 1;
    }
    return 0;
}

int dtmail_contacts_default_path(char *out, size_t out_len)
{
    const char *home = getenv("HOME");
    if (!home || !home[0] || !out || out_len < 32) return -1;
    int n = snprintf(out, out_len, "%s/.dt/dtmail_contacts", home);
    return (n > 0 && (size_t)n < out_len) ? 0 : -1;
}

void dtmail_contacts_init(void)
{
    if (g_contacts_inited) return;
    g_contacts_inited = 1;
    g_ncontacts = 0;
    char path[1024];
    if (dtmail_contacts_default_path(path, sizeof(path)) == 0) {
        (void)dtmail_contacts_load_file(path);
    }
}

void dtmail_contacts_shutdown(void)
{
    g_ncontacts = 0;
    g_contacts_inited = 0;
}

int dtmail_contacts_count(void)
{
    return g_ncontacts;
}

int dtmail_contacts_add(const char *name, const char *email)
{
    if (!email || !email[0] || !strchr(email, '@')) return -1;
    for (int i = 0; i < g_ncontacts; i++) {
        if (strcasecmp(g_contacts[i].email, email) == 0) {
            if (name && name[0] && !g_contacts[i].name[0]) {
                snprintf(g_contacts[i].name, sizeof(g_contacts[i].name), "%s", name);
            }
            return 0;
        }
    }
    if (g_ncontacts >= DTMAIL_MAX_CONTACTS) return -1;
    DtMailContact *c = &g_contacts[g_ncontacts++];
    memset(c, 0, sizeof(*c));
    if (name) snprintf(c->name, sizeof(c->name), "%s", name);
    snprintf(c->email, sizeof(c->email), "%s", email);
    return 0;
}

int dtmail_contacts_harvest_header(const char *header_val)
{
    if (!header_val || !header_val[0]) return 0;
    char buf[2048];
    snprintf(buf, sizeof(buf), "%s", header_val);
    int added = 0;
    char *sp = NULL;
    for (char *tok = strtok_r(buf, ",;", &sp); tok; tok = strtok_r(NULL, ",;", &sp)) {
        while (*tok == ' ' || *tok == '\t') tok++;
        char name[128] = "";
        char email[256] = "";
        char *lt = strchr(tok, '<');
        char *gt = lt ? strchr(lt + 1, '>') : NULL;
        if (lt && gt) {
            *lt = '\0';
            *gt = '\0';
            char *nm = tok;
            while (*nm == ' ' || *nm == '"') nm++;
            size_t nl = strlen(nm);
            while (nl > 0 && (nm[nl - 1] == ' ' || nm[nl - 1] == '"')) nm[--nl] = '\0';
            snprintf(name, sizeof(name), "%s", nm);
            snprintf(email, sizeof(email), "%s", lt + 1);
        } else {
            size_t el = strlen(tok);
            while (el > 0 && isspace((unsigned char)tok[el - 1])) tok[--el] = '\0';
            snprintf(email, sizeof(email), "%s", tok);
        }
        if (dtmail_contacts_add(name, email) == 0) added++;
    }
    return added;
}

int dtmail_contacts_load_file(const char *path)
{
    if (!path) return -1;
    g_ncontacts = 0;
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    char line[512];
    while (fgets(line, sizeof(line), fp) && g_ncontacts < DTMAIL_MAX_CONTACTS) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        if (!line[0] || line[0] == '#') continue;
        char *tab = strchr(line, '\t');
        if (tab) {
            *tab = '\0';
            (void)dtmail_contacts_add(line, tab + 1);
        } else {
            (void)dtmail_contacts_add("", line);
        }
    }
    fclose(fp);
    return 0;
}

int dtmail_contacts_save_file(const char *path)
{
    if (!path || !path[0]) return -1;
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash && slash > dir) {
        *slash = '\0';
        (void)mkdir(dir, 0700);
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    (void)fchmod(fd, 0600);
    FILE *fp = fdopen(fd, "w");
    if (!fp) { close(fd); return -1; }
    for (int i = 0; i < g_ncontacts; i++) {
        fprintf(fp, "%s\t%s\n", g_contacts[i].name, g_contacts[i].email);
    }
    fclose(fp);
    return 0;
}

int dtmail_contacts_match(const char *prefix, char results[][256], int max_results)
{
    if (!prefix || !prefix[0] || !results || max_results <= 0) return 0;
    int found = 0;
    for (int i = 0; i < g_ncontacts && found < max_results; i++) {
        if (str_contains_ci(g_contacts[i].email, prefix) ||
            str_contains_ci(g_contacts[i].name, prefix)) {
            if (g_contacts[i].name[0]) {
                snprintf(results[found], 256, "%s <%s>",
                         g_contacts[i].name, g_contacts[i].email);
            } else {
                snprintf(results[found], 256, "%s", g_contacts[i].email);
            }
            found++;
        }
    }
    return found;
}
