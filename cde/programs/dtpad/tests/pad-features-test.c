#define _DEFAULT_SOURCE
#include "padFeatures.h"
#include "padSyntax.h"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    char cwd[PATH_MAX];
    assert(getcwd(cwd, sizeof(cwd)) != NULL);

    char recent_store[PATH_MAX];
    char autosave_dir[PATH_MAX];
    snprintf(recent_store, sizeof(recent_store), "%s/dtpad.recent", cwd);
    snprintf(autosave_dir, sizeof(autosave_dir), "%s/autosave", cwd);

    /* 1. Recent files MRU ordering + deduplication + cap at DTPAD_MAX_RECENT */
    char f1[PATH_MAX], f2[PATH_MAX], f3[PATH_MAX];
    snprintf(f1, sizeof(f1), "%s/alpha.txt", cwd);
    snprintf(f2, sizeof(f2), "%s/beta.txt", cwd);
    snprintf(f3, sizeof(f3), "%s/gamma.txt", cwd);

    FILE *fp = fopen(f1, "w"); assert(fp); fprintf(fp, "one\n"); fclose(fp);
    fp = fopen(f2, "w"); assert(fp); fprintf(fp, "two\n"); fclose(fp);
    fp = fopen(f3, "w"); assert(fp); fprintf(fp, "three\n"); fclose(fp);

    assert(dtpad_recent_record(recent_store, f1) == 0);
    assert(dtpad_recent_record(recent_store, f2) == 0);
    assert(dtpad_recent_record(recent_store, f3) == 0);
    /* Re-opening alpha moves it to top without duplicating */
    assert(dtpad_recent_record(recent_store, f1) == 0);

    char loaded[DTPAD_MAX_RECENT][PATH_MAX];
    int cnt = dtpad_recent_load(recent_store, loaded);
    assert(cnt == 3);
    assert(strstr(loaded[0], "alpha.txt") != NULL);
    assert(strstr(loaded[1], "gamma.txt") != NULL);
    assert(strstr(loaded[2], "beta.txt") != NULL);

    /* 2. Named-file crash recovery from dead child PID */
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        char swap_out[PATH_MAX];
        const char *unsaved = "Recovered line 1\nRecovered line 2\n";
        int rc = dtpad_autosave_write(autosave_dir, f2, (int)getpid(), 1,
                                      unsaved, strlen(unsaved), swap_out, sizeof(swap_out));
        _exit(rc == 0 ? 0 : 2);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    char *rec_body = NULL;
    size_t rec_len = 0;
    char rec_swap[PATH_MAX] = "";
    assert(dtpad_autosave_find_recovery(autosave_dir, f2, &rec_body, &rec_len,
                                        rec_swap, sizeof(rec_swap)) == 0);
    assert(rec_body != NULL);
    assert(strcmp(rec_body, "Recovered line 1\nRecovered line 2\n") == 0);
    free(rec_body);

    /* Normal save removes autosave swap file */
    assert(dtpad_autosave_remove_for_path(autosave_dir, f2) == 0);
    assert(dtpad_autosave_find_recovery(autosave_dir, f2, &rec_body, &rec_len,
                                        rec_swap, sizeof(rec_swap)) == -1);

    /* 3. Untitled crash recovery from dead child PID */
    pid_t child2 = fork();
    assert(child2 >= 0);
    if (child2 == 0) {
        char swap_out[PATH_MAX];
        const char *scratch = "Unsaved scratch notes before SIGKILL";
        (void)dtpad_autosave_write(autosave_dir, NULL, (int)getpid(), 1,
                                   scratch, strlen(scratch), swap_out, sizeof(swap_out));
        raise(SIGKILL);
        _exit(1);
    }
    assert(waitpid(child2, &status, 0) == child2);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);

    rec_body = NULL;
    rec_len = 0;
    assert(dtpad_autosave_find_recovery(autosave_dir, NULL, &rec_body, &rec_len,
                                        rec_swap, sizeof(rec_swap)) == 0);
    assert(rec_body != NULL);
    assert(strcmp(rec_body, "Unsaved scratch notes before SIGKILL") == 0);
    free(rec_body);
    assert(dtpad_autosave_remove_file(rec_swap) == 0);

    /* 4. Multi-language syntax detection and UTF-8 scalar lexing */
    assert(dtpad_syntax_detect("/tmp/demo.c", NULL) == DTPAD_SYNTAX_C);
    assert(dtpad_syntax_detect("/tmp/config.json", NULL) == DTPAD_SYNTAX_JSON);
    assert(dtpad_syntax_detect("/tmp/app.ts", NULL) == DTPAD_SYNTAX_JS);
    assert(dtpad_syntax_detect("/tmp/script", "#!/usr/bin/env bash\necho hi\n") == DTPAD_SYNTAX_SHELL);
    assert(dtpad_syntax_detect("/tmp/tool", "#!/usr/bin/env python3\nx = 1\n") == DTPAD_SYNTAX_PYTHON);
    assert(dtpad_syntax_detect("/tmp/Makefile.am", NULL) == DTPAD_SYNTAX_MAKEFILE);
    assert(dtpad_syntax_detect("/tmp/notes.md", NULL) == DTPAD_SYNTAX_MARKDOWN);
    assert(dtpad_syntax_detect("/tmp/query.sql", NULL) == DTPAD_SYNTAX_SQL);
    assert(dtpad_syntax_detect("/tmp/fix.patch", NULL) == DTPAD_SYNTAX_DIFF);

    /* C lexing with UTF-8 comment before keyword */
    {
        const char *c_src = "/* café */\nint main(void) {\n    return 42;\n}\n";
        DtpadSyntaxSpan *spans = NULL;
        size_t n = dtpad_syntax_lex(DTPAD_SYNTAX_C, c_src, strlen(c_src), &spans);
        assert(n >= 5 && spans != NULL);
        // The leading block comment has 10 Unicode scalars (even though 'é' is 2 UTF-8 bytes)
        assert(spans[0].kind == DTPAD_TOK_COMMENT && spans[0].start_pos == 0 && spans[0].end_pos == 10);
        /* Next line starts at scalar 11: "int" is [11, 14) */
        assert(spans[1].kind == DTPAD_TOK_TYPE && spans[1].start_pos == 11 && spans[1].end_pos == 14);
        assert(spans[2].kind == DTPAD_TOK_FUNCTION && spans[2].start_pos == 15 && spans[2].end_pos == 19);
        free(spans);
    }

    /* JSON lexing distinguishes keys from string values, numbers, and booleans */
    {
        const char *j_src = "{\"name\": \"cde\", \"port\": 8080, \"ok\": true}\n";
        DtpadSyntaxSpan *spans = NULL;
        size_t n = dtpad_syntax_lex(DTPAD_SYNTAX_JSON, j_src, strlen(j_src), &spans);
        assert(n == 6 && spans != NULL);
        assert(spans[0].kind == DTPAD_TOK_KEY);
        assert(spans[1].kind == DTPAD_TOK_STRING);
        assert(spans[2].kind == DTPAD_TOK_KEY);
        assert(spans[3].kind == DTPAD_TOK_NUMBER);
        assert(spans[4].kind == DTPAD_TOK_KEY);
        assert(spans[5].kind == DTPAD_TOK_KEYWORD);
        free(spans);
    }

    /* Shell lexing handles shebang, variables, keywords, and strings */
    {
        const char *sh_src = "#!/bin/sh\nFOO=\"bar\"\nif [ -n \"$FOO\" ]; then\n  exit 0\nfi\n";
        DtpadSyntaxSpan *spans = NULL;
        size_t n = dtpad_syntax_lex(DTPAD_SYNTAX_SHELL, sh_src, strlen(sh_src), &spans);
        assert(n >= 5 && spans != NULL);
        assert(spans[0].kind == DTPAD_TOK_PREPROC); /* shebang */
        assert(spans[1].kind == DTPAD_TOK_KEY);     /* FOO */
        assert(spans[2].kind == DTPAD_TOK_STRING);  /* "bar" */
        free(spans);
    }

    printf("pad-features-test: all checks passed!\n");
    return 0;
}
