/* Negative-control regression test: dtstyle's initialisation of style.home
 * must survive an unset HOME.
 *
 * The statements between "style.errDialog  = NULL;" and
 * "style.colorDialog = NULL;" are extracted verbatim from Main.c by
 * check-home-environment.py and compiled into init_style() below.
 *
 * Usage: check-home-environment unset | set PATH
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* @@PROLOGUE@@ */
static char *XtMalloc(unsigned size)
{
    char *p = malloc(size ? size : 1);
    if (p == NULL) { fputs("XtMalloc stub: out of memory\n", stderr); abort(); }
    return p;
}

static struct {
    char *home;
    void *display;
    unsigned long root;
    char *execName;
    void *errDialog;
    void *tmpXmStr;
} style;

static void init_style(char **argv)
{
    /* Same preceding statements as Main.c, with stubbed values. */
    style.display   = NULL;
    style.root      = 0;
    style.execName  = argv[0];
    style.home      = (char *) 0x1;   /* must be overwritten by the block */
    style.errDialog = (void *) 0x1;
    style.tmpXmStr  = (void *) 0x1;
/* @@EXTRACTED@@ */
}

/* @@BODY@@ */
static int fail(const char *msg)
{
    fprintf(stderr, "check-home-environment: %s\n", msg);
    return 1;
}

int main(int argc, char **argv)
{
    const char *expect;
    char *argv0[2] = { "dtstyle", NULL };
    (void) argv;

    if (argc == 2 && strcmp(argv[1], "unset") == 0) {
        unsetenv("HOME");
        expect = "";
    } else if (argc == 3 && strcmp(argv[1], "set") == 0) {
        if (setenv("HOME", argv[2], 1) != 0)
            return fail("setenv failed");
        expect = argv[2];
    } else {
        return fail("usage: unset | set PATH");
    }

    init_style(argv0);

    if (style.home == NULL || style.home == (char *) 0x1)
        return fail("style.home was not assigned an allocation");
    if (strcmp(style.home, expect) != 0)
        return fail("style.home does not equal the expected string");
    if (style.errDialog != NULL || style.tmpXmStr != NULL)
        return fail("errDialog/tmpXmStr were not reset to NULL");
    if (getenv("HOME") != NULL && style.home == getenv("HOME"))
        return fail("style.home aliases the environment instead of copying it");
    printf("ok: HOME %s -> style.home \"%s\"\n",
           argc == 2 ? "unset" : "set", style.home);
    free(style.home);
    return 0;
}
