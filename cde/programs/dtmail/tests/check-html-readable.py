#!/usr/bin/env python3
"""ASan/UBSan unit tests for dtmail HtmlReadable (HTML-to-readable-text converter)."""

import os
import subprocess
import sys
import tempfile

C_TEST = r"""
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "HtmlReadable.h"

int main(void) {
    /* 1. Strips script/style/head/comments, formats headings, lists, links, entities */
    const char *html =
        "<html><head><title>Ignored</title><style>body{color:red}</style></head>"
        "<body>"
        "<!-- secret comment -->"
        "<script>alert('xss')</script>"
        "<h1>Release &amp; Notes</h1>"
        "<p>Hello &quot;World&quot; &mdash; check out "
        "<a href=\"https://example.com/docs\">our docs</a> and "
        "<a href=\"javascript:evil()\">bad link</a>.</p>"
        "<ul><li>Item &#49;</li><li>Item &#x32;</li></ul>"
        "<blockquote>Quoted line</blockquote>"
        "</body></html>";

    char *txt = dtmail_html_to_readable_text(html, strlen(html));
    assert(txt != NULL);
    assert(strstr(txt, "alert") == NULL);
    assert(strstr(txt, "color:red") == NULL);
    assert(strstr(txt, "secret comment") == NULL);
    assert(strstr(txt, "javascript:") == NULL);
    assert(strstr(txt, "## Release & Notes") != NULL);
    assert(strstr(txt, "\"World\" --") != NULL);
    assert(strstr(txt, "our docs [https://example.com/docs]") != NULL);
    assert(strstr(txt, "* Item 1") != NULL);
    assert(strstr(txt, "* Item 2") != NULL);
    assert(strstr(txt, "> Quoted line") != NULL);
    free(txt);
    printf("ok    html-sanitize-and-format\n");

    /* 2. Malformed/unclosed HTML does not crash or overrun under ASan */
    const char *bad = "<a href=\"https://x.test\" <div><b>unclosed <!-- never ends";
    char *txt2 = dtmail_html_to_readable_text(bad, strlen(bad));
    assert(txt2 != NULL);
    free(txt2);

    char *empty = dtmail_html_to_readable_text("", 0);
    assert(empty != NULL && empty[0] == '\0');
    free(empty);
    printf("ok    malformed-and-empty-html\n");
    return 0;
}
"""


def main() -> int:
    test_dir = os.path.dirname(os.path.abspath(__file__))
    dtmail_root = os.path.dirname(test_dir)
    dtmail_src = os.path.join(dtmail_root, "dtmail")
    cxx = os.environ.get("CXX", "clang++")

    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, "html-test.cc")
        exe = os.path.join(td, "html-test")
        with open(src, "w", encoding="utf-8") as f:
            f.write(C_TEST)
        subprocess.run(
            [cxx, "-O1", "-g", "-fsanitize=address,undefined",
             f"-I{dtmail_src}", src,
             os.path.join(dtmail_src, "HtmlReadable.C"),
             "-o", exe],
            check=True,
        )
        env = dict(os.environ)
        env["ASAN_OPTIONS"] = "detect_leaks=0"
        subprocess.run([exe], env=env, check=True)
    print("check-html-readable: all assertions passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
