/*
 * CDE - Common Desktop Environment
 *
 * Pure C/C++ HTML-to-readable-text renderer (zero WebKit/GTK dependencies,
 * zero remote tracking fetches) + ck-browser handoff for dtmail.
 */

#include <ctype.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "HtmlReadable.h"

static char *
request_wl_activation_token_dl(const char *app_id)
{
    typedef unsigned char (*XmWlReqTokFn)(void *, unsigned long, const char *, char **);
    XmWlReqTokFn fn = (XmWlReqTokFn)dlsym(RTLD_DEFAULT, "XmWaylandRequestActivationToken");
    if (!fn)
        return NULL;
    char *tok = NULL;
    (void)fn(NULL, 0UL, app_id, &tok);
    return tok;
}

static void
free_wl_activation_token_dl(char *tok)
{
    if (!tok)
        return;
    typedef void (*XtFreeFn)(char *);
    XtFreeFn fn = (XtFreeFn)dlsym(RTLD_DEFAULT, "XtFree");
    if (fn)
        fn(tok);
}

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

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} RawBuf;

static void
rb_append(RawBuf *b, const char *s, size_t n)
{
    if (!b || !s || n == 0) return;
    if (b->len + n + 1 > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 2048;
        while (ncap < b->len + n + 1)
            ncap *= 2;
        if (ncap > 32 * 1024 * 1024) return; /* 32 MiB cap */
        char *nd = (char *)realloc(b->data, ncap);
        if (!nd) return;
        b->data = nd;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void
rb_puts(RawBuf *b, const char *s)
{
    if (s) rb_append(b, s, strlen(s));
}

static void
rb_puts_attr_escaped(RawBuf *b, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"') rb_puts(b, "&quot;");
        else if (c == '\'') rb_puts(b, "&#39;");
        else if (c == '<') rb_puts(b, "&lt;");
        else if (c == '>') rb_puts(b, "&gt;");
        else if (c >= 0x20 || c == '\t' || c == '\n') rb_append(b, (const char *)&c, 1);
    }
}

/* Decode HTML character references and strip ASCII control/whitespace bytes
 * to inspect canonical URI schemes (defeating java&#x09;script: obfuscation). */
static void
canonicalize_for_scheme_check(const char *raw, size_t len, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    size_t o = 0;
    size_t i = 0;
    while (i < len && o + 1 < out_len) {
        unsigned char c = (unsigned char)raw[i];
        if (c == '&') {
            size_t semi = 0;
            for (size_t k = i + 1; k < len && k < i + 14; k++) {
                if (raw[k] == ';') { semi = k; break; }
                if (!isalnum((unsigned char)raw[k]) && raw[k] != '#') break;
            }
            if (semi > i + 1) {
                char ent[16];
                size_t el = semi - (i + 1);
                if (el >= sizeof(ent)) el = sizeof(ent) - 1;
                memcpy(ent, raw + i + 1, el);
                ent[el] = '\0';
                unsigned long cp = 0;
                if (ent[0] == '#') {
                    cp = (ent[1] == 'x' || ent[1] == 'X')
                        ? strtoul(ent + 2, NULL, 16)
                        : strtoul(ent + 1, NULL, 10);
                } else if (strcasecmp(ent, "colon") == 0) {
                    cp = ':';
                } else if (strcasecmp(ent, "tab") == 0 || strcasecmp(ent, "newline") == 0) {
                    cp = ' ';
                }
                i = semi + 1;
                if (cp > 0x20 && cp < 0x7f)
                    out[o++] = (char)tolower((int)cp);
                continue;
            }
            /* Also handle numeric character references missing trailing ';' (e.g. &#106avascript:) */
            if (i + 2 < len && raw[i + 1] == '#') {
                size_t k = i + 2;
                int hex = 0;
                if (k < len && (raw[k] == 'x' || raw[k] == 'X')) { hex = 1; k++; }
                size_t ds = k;
                while (k < len && (hex ? isxdigit((unsigned char)raw[k]) : isdigit((unsigned char)raw[k])))
                    k++;
                if (k > ds) {
                    char num[16];
                    size_t nl = k - ds;
                    if (nl >= sizeof(num)) nl = sizeof(num) - 1;
                    memcpy(num, raw + ds, nl);
                    num[nl] = '\0';
                    unsigned long cp = strtoul(num, NULL, hex ? 16 : 10);
                    i = k;
                    if (cp > 0x20 && cp < 0x7f)
                        out[o++] = (char)tolower((int)cp);
                    continue;
                }
            }
        }
        i++;
        if (c <= 0x20 || c == 0x7f)
            continue;
        out[o++] = (char)tolower(c);
    }
    out[o] = '\0';
}

static int
is_dropped_subtree_tag(const char *name)
{
    static const char *const kDrop[] = {
        "script", "iframe", "object", "embed", "applet",
        "svg", "math", "template", "noscript", "head", "title",
        "style", "textarea", "select", "button", "audio", "video", NULL
    };
    for (int i = 0; kDrop[i]; i++) {
        if (strcmp(name, kDrop[i]) == 0)
            return 1;
    }
    return 0;
}

static int
is_allowed_html_tag(const char *name)
{
    static const char *const kAllow[] = {
        "div", "p", "span", "a", "img",
        "table", "thead", "tbody", "tfoot", "tr", "th", "td", "caption",
        "h1", "h2", "h3", "h4", "h5", "h6",
        "ul", "ol", "li", "dl", "dt", "dd",
        "blockquote", "pre", "code", "kbd", "samp",
        "b", "strong", "i", "em", "u", "s", "del", "ins", "mark",
        "br", "hr", "section", "article", "header", "footer", "aside",
        "center", "font", "small", "sub", "sup", "figure", "figcaption",
        NULL
    };
    for (int i = 0; kAllow[i]; i++) {
        if (strcmp(name, kAllow[i]) == 0)
            return 1;
    }
    return 0;
}

static int
is_void_html_tag(const char *name)
{
    return (strcmp(name, "br") == 0 ||
            strcmp(name, "hr") == 0 ||
            strcmp(name, "img") == 0);
}

static int
is_safe_inline_style(const char *val, size_t len, int *out_had_remote_url)
{
    char canon[512];
    canonicalize_for_scheme_check(val, len, canon, sizeof(canon));
    if (strstr(canon, "url(") != NULL) {
        if (out_had_remote_url &&
            (strstr(canon, "url(http") != NULL ||
             strstr(canon, "url(\"http") != NULL ||
             strstr(canon, "url('http") != NULL ||
             strstr(canon, "url(//") != NULL)) {
            *out_had_remote_url = 1;
        }
        return 0;
    }
    if (strstr(canon, "expression(") != NULL ||
        strstr(canon, "@import") != NULL ||
        strstr(canon, "javascript:") != NULL ||
        strstr(canon, "vbscript:") != NULL ||
        strstr(canon, "behavior:") != NULL ||
        strstr(canon, "-moz-binding") != NULL) {
        return 0;
    }
    return 1;
}

char *
dtmail_sanitize_html_sandboxed(const char *html,
                               size_t html_len,
                               int allow_remote_images,
                               int *out_blocked_remote_count)
{
    int blocked_remote = 0;
    if (out_blocked_remote_count)
        *out_blocked_remote_count = 0;

    RawBuf body = { NULL, 0, 0 };
    rb_puts(&body, "");
    if (!body.data) {
        body.data = (char *)calloc(1, 1);
        body.cap = 1;
    }

    char drop_tag[32] = "";
    int drop_depth = 0;

    size_t i = 0;
    while (html && i < html_len) {
        if (html[i] == '\0') {
            i++;
            continue;
        }
        if (html[i] != '<') {
            if (drop_depth == 0) {
                /* Copy text, escaping stray '>' */
                if (html[i] == '>')
                    rb_puts(&body, "&gt;");
                else
                    rb_append(&body, html + i, 1);
            }
            i++;
            continue;
        }

        /* HTML comment <!-- ... --> */
        if (i + 4 <= html_len && strncmp(html + i, "<!--", 4) == 0) {
            size_t k = i + 4;
            while (k + 3 <= html_len) {
                if (html[k] == '-' && html[k + 1] == '-' && html[k + 2] == '>') {
                    k += 3;
                    break;
                }
                k++;
            }
            i = (k + 3 <= html_len || (k >= 3 && html[k - 1] == '>')) ? k : html_len;
            continue;
        }

        /* <!DOCTYPE ...> or CDATA / processing instruction */
        if (i + 2 <= html_len && (html[i + 1] == '!' || html[i + 1] == '?')) {
            size_t j = i + 2;
            while (j < html_len && html[j] != '>')
                j++;
            i = (j < html_len) ? (j + 1) : html_len;
            continue;
        }

        /* Locate closing '>' respecting attribute quotes */
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
        if (j > html_len || html[j - 1] != '>') {
            /* Unclosed trailing '<...' — escape '<' safely rather than emitting broken tag */
            if (drop_depth == 0)
                rb_puts(&body, "&lt;");
            i++;
            continue;
        }

        size_t tstart = i + 1;
        size_t tend = j - 1;
        i = j;

        while (tstart < tend && isspace((unsigned char)html[tstart]))
            tstart++;
        int is_close = 0;
        if (tstart < tend && html[tstart] == '/') {
            is_close = 1;
            tstart++;
            while (tstart < tend && isspace((unsigned char)html[tstart]))
                tstart++;
        }

        char tname[32];
        size_t nlen = 0;
        while (tstart + nlen < tend && nlen + 1 < sizeof(tname) &&
               isalnum((unsigned char)html[tstart + nlen])) {
            tname[nlen] = (char)tolower((unsigned char)html[tstart + nlen]);
            nlen++;
        }
        tname[nlen] = '\0';
        if (nlen == 0)
            continue;

        if (drop_depth > 0) {
            if (strcmp(tname, drop_tag) == 0) {
                if (is_close) {
                    drop_depth--;
                    if (drop_depth == 0)
                        drop_tag[0] = '\0';
                } else {
                    drop_depth++;
                }
            }
            continue;
        }

        if (!is_close && is_dropped_subtree_tag(tname)) {
            /* Check if self-closing <script ... /> */
            size_t trim_end = tend;
            while (trim_end > tstart && isspace((unsigned char)html[trim_end - 1]))
                trim_end--;
            if (!(trim_end > tstart && html[trim_end - 1] == '/')) {
                drop_depth = 1;
                snprintf(drop_tag, sizeof(drop_tag), "%s", tname);
            }
            continue;
        }

        if (!is_allowed_html_tag(tname))
            continue;

        if (is_close) {
            if (!is_void_html_tag(tname)) {
                rb_puts(&body, "</");
                rb_puts(&body, tname);
                rb_puts(&body, ">");
            }
            continue;
        }

        /* Parse attributes on allowed opening tag */
        size_t ap = tstart + nlen;
        int img_remote_blocked = 0;
        char img_alt[128] = "";
        int has_safe_href = 0;

        RawBuf attr_buf = { NULL, 0, 0 };
        while (ap < tend) {
            while (ap < tend && (isspace((unsigned char)html[ap]) || html[ap] == '/'))
                ap++;
            if (ap >= tend)
                break;

            size_t an_start = ap;
            while (ap < tend && !isspace((unsigned char)html[ap]) &&
                   html[ap] != '=' && html[ap] != '/') {
                ap++;
            }
            size_t an_len = ap - an_start;
            if (an_len == 0) {
                ap++;
                continue;
            }
            char aname[32];
            size_t al = an_len < sizeof(aname) - 1 ? an_len : sizeof(aname) - 1;
            for (size_t u = 0; u < al; u++)
                aname[u] = (char)tolower((unsigned char)html[an_start + u]);
            aname[al] = '\0';

            while (ap < tend && isspace((unsigned char)html[ap]))
                ap++;

            const char *aval = "";
            size_t aval_len = 0;
            if (ap < tend && html[ap] == '=') {
                ap++;
                while (ap < tend && isspace((unsigned char)html[ap]))
                    ap++;
                if (ap < tend && (html[ap] == '"' || html[ap] == '\'')) {
                    char aq = html[ap++];
                    size_t vs = ap;
                    while (ap < tend && html[ap] != aq)
                        ap++;
                    aval = html + vs;
                    aval_len = ap - vs;
                    if (ap < tend && html[ap] == aq)
                        ap++;
                } else {
                    size_t vs = ap;
                    while (ap < tend && !isspace((unsigned char)html[ap]))
                        ap++;
                    aval = html + vs;
                    aval_len = ap - vs;
                }
            }

            /* Never allow any on* event handler */
            if (aname[0] == 'o' && aname[1] == 'n')
                continue;

            if (strcmp(aname, "srcset") == 0 || strcmp(aname, "background") == 0) {
                if (!allow_remote_images && aval_len > 0) {
                    char cchk[256];
                    canonicalize_for_scheme_check(aval, aval_len, cchk, sizeof(cchk));
                    if (strstr(cchk, "http://") || strstr(cchk, "https://") || strncmp(cchk, "//", 2) == 0)
                        blocked_remote++;
                }
                continue;
            }

            if (strcmp(aname, "href") == 0 && strcmp(tname, "a") == 0) {
                char cchk[256];
                canonicalize_for_scheme_check(aval, aval_len, cchk, sizeof(cchk));
                if (strncmp(cchk, "https://", 8) == 0 ||
                    strncmp(cchk, "http://", 7) == 0 ||
                    strncmp(cchk, "mailto:", 7) == 0 ||
                    cchk[0] == '#') {
                    rb_puts(&attr_buf, " href=\"");
                    rb_puts_attr_escaped(&attr_buf, aval, aval_len);
                    rb_puts(&attr_buf, "\"");
                    has_safe_href = 1;
                }
                continue;
            }

            if (strcmp(aname, "src") == 0 && strcmp(tname, "img") == 0) {
                char cchk[256];
                canonicalize_for_scheme_check(aval, aval_len, cchk, sizeof(cchk));
                int is_remote = (strncmp(cchk, "https://", 8) == 0 ||
                                 strncmp(cchk, "http://", 7) == 0 ||
                                 strncmp(cchk, "//", 2) == 0);
                int is_safe_inline = (strncmp(cchk, "cid:", 4) == 0 ||
                                      strncmp(cchk, "data:image/png;", 15) == 0 ||
                                      strncmp(cchk, "data:image/jpeg;", 16) == 0 ||
                                      strncmp(cchk, "data:image/gif;", 15) == 0 ||
                                      strncmp(cchk, "data:image/webp;", 16) == 0);
                if (is_remote && !allow_remote_images) {
                    blocked_remote++;
                    img_remote_blocked = 1;
                } else if (is_safe_inline || (is_remote && allow_remote_images)) {
                    rb_puts(&attr_buf, " src=\"");
                    rb_puts_attr_escaped(&attr_buf, aval, aval_len);
                    rb_puts(&attr_buf, "\"");
                }
                continue;
            }

            if (strcmp(aname, "style") == 0) {
                int had_remote = 0;
                if (is_safe_inline_style(aval, aval_len, &had_remote)) {
                    rb_puts(&attr_buf, " style=\"");
                    rb_puts_attr_escaped(&attr_buf, aval, aval_len);
                    rb_puts(&attr_buf, "\"");
                } else if (had_remote && !allow_remote_images) {
                    blocked_remote++;
                }
                continue;
            }

            if (strcmp(aname, "alt") == 0) {
                size_t cl = aval_len < sizeof(img_alt) - 1 ? aval_len : sizeof(img_alt) - 1;
                memcpy(img_alt, aval, cl);
                img_alt[cl] = '\0';
            }

            static const char *const kSafeAttrs[] = {
                "alt", "title", "colspan", "rowspan", "align", "valign",
                "width", "height", "border", "cellpadding", "cellspacing",
                "bgcolor", "color", "dir", "lang", NULL
            };
            int ok_attr = 0;
            for (int sa = 0; kSafeAttrs[sa]; sa++) {
                if (strcmp(aname, kSafeAttrs[sa]) == 0) {
                    ok_attr = 1;
                    break;
                }
            }
            if (ok_attr) {
                rb_puts(&attr_buf, " ");
                rb_puts(&attr_buf, aname);
                rb_puts(&attr_buf, "=\"");
                rb_puts_attr_escaped(&attr_buf, aval, aval_len);
                rb_puts(&attr_buf, "\"");
            }
        }

        if (strcmp(tname, "img") == 0 && img_remote_blocked) {
            rb_puts(&body, "<span class=\"dtmail-blocked-img\">[Remote image blocked");
            if (img_alt[0]) {
                rb_puts(&body, ": ");
                rb_puts_attr_escaped(&body, img_alt, strlen(img_alt));
            }
            rb_puts(&body, "]</span>");
            free(attr_buf.data);
            continue;
        }

        rb_puts(&body, "<");
        rb_puts(&body, tname);
        if (attr_buf.data && attr_buf.len > 0)
            rb_append(&body, attr_buf.data, attr_buf.len);
        if (strcmp(tname, "a") == 0 && has_safe_href)
            rb_puts(&body, " rel=\"noopener noreferrer\" target=\"_blank\"");
        rb_puts(&body, ">");
        free(attr_buf.data);
    }

    if (out_blocked_remote_count)
        *out_blocked_remote_count = blocked_remote;

    RawBuf doc = { NULL, 0, 0 };
    rb_puts(&doc,
        "<!DOCTYPE html>\n"
        "<html><head>\n"
        "<meta charset=\"utf-8\">\n"
        "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; ");
    if (allow_remote_images)
        rb_puts(&doc, "img-src cid: data: https: http:; ");
    else
        rb_puts(&doc, "img-src cid: data:; ");
    rb_puts(&doc,
        "style-src 'unsafe-inline'; form-action 'none'; base-uri 'none'\">\n"
        "<title>CK Core Mail — Sandboxed HTML Message</title>\n"
        "<style>"
        "body{font-family:sans-serif;line-height:1.45;margin:16px;color:#1b1f23;background:#ffffff;}"
        ".dtmail-sandbox-banner{background:#fff8dc;border:1px solid #d4b106;padding:8px 12px;"
        "margin-bottom:14px;font-size:13px;color:#5c4700;}"
        ".dtmail-blocked-img{display:inline-block;padding:2px 6px;border:1px dashed #888;"
        "font-size:12px;color:#555;background:#f6f8fa;}"
        "blockquote{border-left:3px solid #ccc;margin-left:0;padding-left:12px;color:#444;}"
        "table{border-collapse:collapse;}td,th{padding:4px 8px;}"
        "</style>\n"
        "</head><body>\n");

    if (blocked_remote > 0 && !allow_remote_images) {
        char banner[256];
        snprintf(banner, sizeof(banner),
                 "<div class=\"dtmail-sandbox-banner\">Sandboxed HTML view: %d remote image(s)/tracker(s) blocked for privacy.</div>\n",
                 blocked_remote);
        rb_puts(&doc, banner);
    }

    if (body.data && body.len > 0)
        rb_append(&doc, body.data, body.len);
    rb_puts(&doc, "\n</body></html>\n");
    free(body.data);
    return doc.data;
}

int
dtmail_open_html_in_browser(const char *html, size_t html_len)
{
    if (!html || html_len == 0)
        return -1;

    char *safe_html = dtmail_sanitize_html_sandboxed(html, html_len, 0, NULL);
    if (!safe_html)
        return -1;
    size_t safe_len = strlen(safe_html);

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
    if (fd < 0) {
        free(safe_html);
        return -1;
    }
    (void)fchmod(fd, 0600);

    size_t written = 0;
    while (written < safe_len) {
        ssize_t n = write(fd, safe_html + written, safe_len - written);
        if (n <= 0) {
            close(fd);
            unlink(tmppath);
            free(safe_html);
            return -1;
        }
        written += (size_t)n;
    }
    close(fd);
    free(safe_html);

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

    char *wl_tok = request_wl_activation_token_dl("ck-browser");

    pid_t pid = fork();
    if (pid < 0) {
        free_wl_activation_token_dl(wl_tok);
        return -1;
    }
    if (pid == 0) {
        if (wl_tok && wl_tok[0]) {
            setenv("XDG_ACTIVATION_TOKEN", wl_tok, 1);
            setenv("DESKTOP_STARTUP_ID", wl_tok, 1);
        }
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
    free_wl_activation_token_dl(wl_tok);
    return 0;
}
