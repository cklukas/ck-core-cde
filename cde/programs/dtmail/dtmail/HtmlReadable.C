/*
 * CDE - Common Desktop Environment
 *
 * Pure C/C++ HTML-to-readable-text renderer (zero WebKit/GTK dependencies,
 * zero remote tracking fetches) + ck-browser handoff for dtmail.
 */

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "HtmlReadable.h"

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
    int    nl_run;
    int    last_space;
} TextOut;

static void
to_reserve(TextOut *t, size_t extra)
{
    if (t->len + extra + 1 <= t->cap)
        return;
    size_t ncap = t->cap ? t->cap * 2 : 1024;
    while (ncap < t->len + extra + 1)
        ncap *= 2;
    char *nd = (char *)realloc(t->data, ncap);
    if (!nd)
        return;
    t->data = nd;
    t->cap = ncap;
}

static void
to_putc(TextOut *t, char ch)
{
    if (ch == '\r')
        return;
    if (ch == '\n') {
        if (t->len == 0 || t->nl_run >= 2)
            return;
        /* Strip trailing spaces before newline */
        while (t->len > 0 && t->data[t->len - 1] == ' ')
            t->len--;
        to_reserve(t, 1);
        if (!t->data) return;
        t->data[t->len++] = '\n';
        t->data[t->len] = '\0';
        t->nl_run++;
        t->last_space = 1;
        return;
    }
    if (ch == ' ' || ch == '\t') {
        if (t->last_space || t->len == 0)
            return;
        ch = ' ';
        t->last_space = 1;
    } else {
        t->last_space = 0;
        t->nl_run = 0;
    }
    to_reserve(t, 1);
    if (!t->data) return;
    t->data[t->len++] = ch;
    t->data[t->len] = '\0';
}

static void
to_puts(TextOut *t, const char *s)
{
    if (!s) return;
    for (; *s; s++)
        to_putc(t, *s);
}

static void
to_utf8(TextOut *t, unsigned int cp)
{
    if (cp == 0 || cp > 0x10FFFF)
        return;
    if (cp == 0xA0) {
        to_putc(t, ' ');
        return;
    }
    char buf[5];
    int n = 0;
    if (cp < 0x80) {
        buf[n++] = (char)cp;
    } else if (cp < 0x800) {
        buf[n++] = (char)(0xC0 | (cp >> 6));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        buf[n++] = (char)(0xE0 | (cp >> 12));
        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        buf[n++] = (char)(0xF0 | (cp >> 18));
        buf[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    }
    buf[n] = '\0';
    to_puts(t, buf);
}

static size_t
decode_entity(const char *s, size_t rem, TextOut *out)
{
    /* s[0] == '&' */
    size_t semi = 0;
    size_t max_scan = rem < 16 ? rem : 16;
    for (size_t i = 1; i < max_scan; i++) {
        if (s[i] == ';') {
            semi = i;
            break;
        }
        if (!isalnum((unsigned char)s[i]) && s[i] != '#')
            break;
    }
    if (semi == 0) {
        to_putc(out, '&');
        return 1;
    }

    char ent[16];
    size_t elen = semi - 1;
    if (elen >= sizeof(ent)) elen = sizeof(ent) - 1;
    memcpy(ent, s + 1, elen);
    ent[elen] = '\0';

    if (ent[0] == '#') {
        unsigned long cp = 0;
        if (ent[1] == 'x' || ent[1] == 'X')
            cp = strtoul(ent + 2, NULL, 16);
        else
            cp = strtoul(ent + 1, NULL, 10);
        if (cp > 0)
            to_utf8(out, (unsigned int)cp);
        return semi + 1;
    }

    static const struct { const char *name; const char *repl; } kEntities[] = {
        { "amp",    "&"  },
        { "lt",     "<"  },
        { "gt",     ">"  },
        { "quot",   "\"" },
        { "apos",   "'"  },
        { "nbsp",   " "  },
        { "mdash",  "--" },
        { "ndash",  "-"  },
        { "hellip", "..." },
        { "copy",   "(c)" },
        { "reg",    "(R)" },
        { "trade",  "(TM)" },
        { "bull",   "*"  },
        { "euro",   "EUR" },
        { "lsquo",  "'"  },
        { "rsquo",  "'"  },
        { "ldquo",  "\"" },
        { "rdquo",  "\"" },
        { NULL,     NULL }
    };
    for (int i = 0; kEntities[i].name; i++) {
        if (strcasecmp(ent, kEntities[i].name) == 0) {
            to_puts(out, kEntities[i].repl);
            return semi + 1;
        }
    }
    to_putc(out, '&');
    return 1;
}

static void
extract_href(const char *tag_body, size_t len, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    out[0] = '\0';
    for (size_t i = 0; i + 5 < len; i++) {
        if (strncasecmp(tag_body + i, "href=", 5) == 0 &&
            (i == 0 || isspace((unsigned char)tag_body[i - 1]))) {
            size_t p = i + 5;
            char quote = 0;
            if (p < len && (tag_body[p] == '"' || tag_body[p] == '\'')) {
                quote = tag_body[p++];
            }
            size_t o = 0;
            while (p < len && o + 1 < out_len) {
                char c = tag_body[p++];
                if (quote && c == quote) break;
                if (!quote && isspace((unsigned char)c)) break;
                out[o++] = c;
            }
            out[o] = '\0';
            /* Only keep safe http/https/mailto URLs */
            if (strncasecmp(out, "https://", 8) != 0 &&
                strncasecmp(out, "http://", 7) != 0 &&
                strncasecmp(out, "mailto:", 7) != 0) {
                out[0] = '\0';
            }
            return;
        }
    }
}

char *
dtmail_html_to_readable_text(const char *html, size_t html_len)
{
    TextOut out;
    memset(&out, 0, sizeof(out));
    to_reserve(&out, 256);
    if (!out.data) return NULL;
    out.data[0] = '\0';

    if (!html || html_len == 0)
        return out.data;

    int skip_depth = 0;
    char skip_tag[16] = "";
    int in_blockquote = 0;
    char active_href[512] = "";
    size_t href_text_start = 0;

    size_t i = 0;
    while (i < html_len) {
        if (html[i] == '<') {
            /* Comment <!-- ... --> */
            if (i + 4 <= html_len && strncmp(html + i, "<!--", 4) == 0) {
                const char *endc = NULL;
                for (size_t k = i + 4; k + 3 <= html_len; k++) {
                    if (html[k] == '-' && html[k + 1] == '-' && html[k + 2] == '>') {
                        endc = html + k + 3;
                        break;
                    }
                }
                i = endc ? (size_t)(endc - html) : html_len;
                continue;
            }

            /* Find closing '>' (respecting quoted attribute values) */
            size_t j = i + 1;
            char q = 0;
            while (j < html_len) {
                char c = html[j++];
                if (q) {
                    if (c == q) q = 0;
                } else if (c == '"' || c == '\'') {
                    q = c;
                } else if (c == '>') {
                    break;
                }
            }
            size_t tag_start = i + 1;
            size_t tag_end = (j > i + 1 && html[j - 1] == '>') ? (j - 1) : j;
            i = j;

            while (tag_start < tag_end && isspace((unsigned char)html[tag_start]))
                tag_start++;
            int is_close = 0;
            if (tag_start < tag_end && html[tag_start] == '/') {
                is_close = 1;
                tag_start++;
            }
            char name[32];
            size_t nlen = 0;
            while (tag_start + nlen < tag_end && nlen + 1 < sizeof(name) &&
                   (isalnum((unsigned char)html[tag_start + nlen]) || html[tag_start + nlen] == '!')) {
                name[nlen] = (char)tolower((unsigned char)html[tag_start + nlen]);
                nlen++;
            }
            name[nlen] = '\0';

            if (skip_depth > 0) {
                if (is_close && strcmp(name, skip_tag) == 0) {
                    skip_depth = 0;
                    skip_tag[0] = '\0';
                }
                continue;
            }

            if (!is_close && (strcmp(name, "script") == 0 ||
                              strcmp(name, "style") == 0 ||
                              strcmp(name, "head") == 0 ||
                              strcmp(name, "noscript") == 0)) {
                skip_depth = 1;
                snprintf(skip_tag, sizeof(skip_tag), "%s", name);
                continue;
            }

            if (strcmp(name, "br") == 0) {
                to_putc(&out, '\n');
                if (in_blockquote) to_puts(&out, "> ");
            } else if (strcmp(name, "p") == 0 || strcmp(name, "div") == 0 ||
                       strcmp(name, "tr") == 0 || strcmp(name, "section") == 0 ||
                       strcmp(name, "article") == 0) {
                to_putc(&out, '\n');
                if (!is_close && strcmp(name, "p") == 0)
                    to_putc(&out, '\n');
                if (!is_close && in_blockquote)
                    to_puts(&out, "> ");
            } else if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == '\0') {
                to_putc(&out, '\n');
                if (!is_close) {
                    to_putc(&out, '\n');
                    to_puts(&out, "## ");
                } else {
                    to_putc(&out, '\n');
                }
            } else if (strcmp(name, "li") == 0) {
                if (!is_close) {
                    to_putc(&out, '\n');
                    to_puts(&out, "  * ");
                } else {
                    to_putc(&out, '\n');
                }
            } else if (strcmp(name, "hr") == 0) {
                to_putc(&out, '\n');
                to_puts(&out, "----------------------------------------");
                to_putc(&out, '\n');
            } else if (strcmp(name, "blockquote") == 0) {
                if (!is_close) {
                    in_blockquote++;
                    to_putc(&out, '\n');
                    to_puts(&out, "> ");
                } else if (in_blockquote > 0) {
                    in_blockquote--;
                    to_putc(&out, '\n');
                }
            } else if (strcmp(name, "td") == 0 || strcmp(name, "th") == 0) {
                if (is_close)
                    to_puts(&out, "  | ");
            } else if (strcmp(name, "a") == 0) {
                if (!is_close) {
                    extract_href(html + tag_start + nlen,
                                 tag_end > tag_start + nlen ? tag_end - (tag_start + nlen) : 0,
                                 active_href, sizeof(active_href));
                    href_text_start = out.len;
                } else if (active_href[0]) {
                    /* Append [URL] unless the link text already contains the URL */
                    int dup = 0;
                    if (out.data && out.len > href_text_start) {
                        if (strstr(out.data + href_text_start, active_href) != NULL)
                            dup = 1;
                    }
                    if (!dup) {
                        to_puts(&out, " [");
                        to_puts(&out, active_href);
                        to_putc(&out, ']');
                    }
                    active_href[0] = '\0';
                }
            }
            continue;
        }

        if (skip_depth > 0) {
            i++;
            continue;
        }

        if (html[i] == '&') {
            i += decode_entity(html + i, html_len - i, &out);
        } else if (html[i] == '\n' || html[i] == '\r' || html[i] == '\t') {
            to_putc(&out, ' ');
            i++;
        } else {
            to_putc(&out, html[i]);
            i++;
        }
    }

    while (out.len > 0 && (out.data[out.len - 1] == '\n' || out.data[out.len - 1] == ' '))
        out.data[--out.len] = '\0';
    return out.data;
}

int
dtmail_open_html_in_browser(const char *html, size_t html_len)
{
    if (!html || html_len == 0)
        return -1;

    const char *tmpdir = getenv("TMPDIR");
    char dir_buf[1024];
    if (!tmpdir || !tmpdir[0]) {
        const char *home = getenv("HOME");
        if (home && home[0]) {
            snprintf(dir_buf, sizeof(dir_buf), "%s/.dt/tmp", home);
            (void)mkdir(dir_buf, 0700);
            tmpdir = dir_buf;
        } else {
            tmpdir = "/tmp";
        }
    }

    char tmppath[1280];
    snprintf(tmppath, sizeof(tmppath), "%s/dtmail-html-XXXXXX.html", tmpdir);
    int fd = mkstemps(tmppath, 5);
    if (fd < 0)
        return -1;
    (void)fchmod(fd, 0600);

    size_t written = 0;
    while (written < html_len) {
        ssize_t n = write(fd, html + written, html_len - written);
        if (n <= 0) {
            close(fd);
            unlink(tmppath);
            return -1;
        }
        written += (size_t)n;
    }
    close(fd);

    /* Preferred browser order: /usr/dt/bin/ck-browser -> ck-browser -> $BROWSER -> xdg-open */
    const char *browser = NULL;
    if (access("/usr/dt/bin/ck-browser", X_OK) == 0)
        browser = "/usr/dt/bin/ck-browser";
    else {
        const char *env_b = getenv("BROWSER");
        if (env_b && env_b[0])
            browser = env_b;
        else
            browser = "ck-browser";
    }

    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        char *argv[] = { (char *)browser, tmppath, NULL };
        execvp(browser, argv);
        /* Fallback to xdg-open if ck-browser wasn't in PATH */
        char *fb[] = { (char *)"xdg-open", tmppath, NULL };
        execvp("xdg-open", fb);
        _exit(127);
    }
    return 0;
}
