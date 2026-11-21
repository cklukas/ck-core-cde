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

/* Writes html to a 0600 temporary .html file and launches ck-browser
 * (or configured $BROWSER / xdg-open fallback) without blocking.
 * Returns 0 on launch success, -1 on error. */
int dtmail_open_html_in_browser(const char *html, size_t html_len);

#ifdef __cplusplus
}
#endif

#endif /* DTMAIL_HTML_READABLE_H */
