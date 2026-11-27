#ifndef DTMAIL_HTML_READABLE_H
#define DTMAIL_HTML_READABLE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Converts an HTML email body into clean, readable plain text (caller frees
 * the returned buffer with free()). Strips <script>/<style>/<head>/comments,
 * formats headings, paragraphs, lists, tables, links, and decodes entities. */
char *dtmail_html_to_readable_text(const char *html, size_t html_len);

/* Produces a strict CSP-sandboxed HTML document (caller frees with free()):
 *  - Allowlists safe formatting tags & attributes only
 *  - Strips <script>, <iframe>, <object>, <embed>, <svg>, <form>, <base>, <meta>,
 *    all on* event handlers, srcdoc, CSS url()/expression()/@import, and
 *    javascript:/vbscript:/file:/data:text/html URIs (including entity-encoded)
 *  - Blocks remote http(s):// images & tracking pixels when allow_remote_images == 0
 *    and reports the blocked count in *out_blocked_remote_count (if non-NULL). */
char *dtmail_sanitize_html_sandboxed(const char *html,
                                     size_t html_len,
                                     int allow_remote_images,
                                     int *out_blocked_remote_count);

/* Sanitizes html via dtmail_sanitize_html_sandboxed(..., allow_remote_images=0),
 * writes it to a 0600 temporary .html file, and launches ck-browser
 * (or configured $BROWSER / xdg-open fallback) without blocking.
 * Returns 0 on launch success, -1 on error. */
int dtmail_open_html_in_browser(const char *html, size_t html_len);

#ifdef __cplusplus
}
#endif

#endif /* DTMAIL_HTML_READABLE_H */
