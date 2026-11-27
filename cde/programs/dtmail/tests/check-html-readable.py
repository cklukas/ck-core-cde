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

    /* 3. Sandboxed HTML sanitizer strips XSS vectors, active tags, event handlers,
     *    obfuscated javascript: URIs, and blocks remote tracking pixels by default */
    const char *untrusted =
        "<html><head><meta http-equiv=\"refresh\" content=\"0;url=https://evil.test\">"
        "<base href=\"https://evil.test/\"></head><body>"
        "<script>window.fetch('https://evil.test/steal')</script>"
        "<iframe src=\"https://evil.test/frame\" srcdoc=\"<script>alert(1)</script>\"></iframe>"
        "<svg onload=\"alert(2)\"><script>alert(3)</script></svg>"
        "<form action=\"https://evil.test/phish\"><input name=\"pw\"></form>"
        "<div onclick=\"alert(4)\" style=\"background:url(https://tracker.test/bg.png)\">Hello <b>CDE</b></div>"
        "<a href=\"java&#x09;script:alert(5)\">Obfuscated JS 1</a>"
        "<a href=\"&#106avascript:alert(6)\">Obfuscated JS 2</a>"
        "<a href=\"data:text/html,<script>alert(7)</script>\">Data HTML</a>"
        "<a href=\"https://ck-core.example/safe\">Safe Link</a>"
        "<img src=\"https://tracker.test/pixel.gif\" onerror=\"alert(8)\" alt=\"TrackerLogo\">"
        "<img src=\"data:image/svg+xml;base64,PHN2Zz4=\" alt=\"BadSvg\">"
        "<img src=\"cid:inline1@mail\" alt=\"InlineImg\">"
        "</body></html>";

    int blocked = -1;
    char *san = dtmail_sanitize_html_sandboxed(untrusted, strlen(untrusted), 0, &blocked);
    assert(san != NULL);
    assert(blocked == 2); /* 1 CSS url(https://...) + 1 <img src="https://..."> */
    assert(strstr(san, "Content-Security-Policy") != NULL);
    assert(strstr(san, "default-src 'none'") != NULL);
    assert(strstr(san, "<script") == NULL);
    assert(strstr(san, "<iframe") == NULL);
    assert(strstr(san, "<svg") == NULL);
    assert(strstr(san, "<form") == NULL);
    assert(strstr(san, "<base") == NULL);
    assert(strstr(san, "http-equiv=\"refresh\"") == NULL);
    assert(strstr(san, "onclick") == NULL);
    assert(strstr(san, "onload") == NULL);
    assert(strstr(san, "onerror") == NULL);
    assert(strstr(san, "javascript:") == NULL);
    assert(strstr(san, "data:text/html") == NULL);
    assert(strstr(san, "image/svg+xml") == NULL);
    assert(strstr(san, "tracker.test") == NULL);
    assert(strstr(san, "[Remote image blocked: TrackerLogo]") != NULL);
    assert(strstr(san, "2 remote image(s)/tracker(s) blocked") != NULL);
    assert(strstr(san, "href=\"https://ck-core.example/safe\"") != NULL);
    assert(strstr(san, "rel=\"noopener noreferrer\"") != NULL);
    assert(strstr(san, "src=\"cid:inline1@mail\"") != NULL);
    free(san);
    printf("ok    html-sandbox-csp-and-tracker-blocking\n");

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
