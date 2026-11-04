#!/usr/bin/env python3
"""Feed crafted session files and settings to dtsession's restore code.

The production code of SmRestore.c that reads a saved session -- the quoting
parser (GetSmartString), the per-screen command and hint buffers, the loop that
sorts the lines of a CDE 1.0 client file into them, and RestoreSettings with its
font path, button, key and modifier maps -- is cut out of the source and
compiled, under AddressSanitizer and UndefinedBehaviorSanitizer, with stand-ins
for Xlib.  The input is what a damaged or hostile session file or settings
resource can hold: quotes nested deeper than the parser's table, commands and
hints longer than the buffers start out, options without an argument, lists
that outgrow their arrays and counts that do not match their lists.  Every
scenario runs in its own process.

DTSESSION_SOURCE names another copy of the sources to test, for example an
older revision in which the client file is parsed inside RestoreClients().
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

source = Path(os.environ.get("DTSESSION_SOURCE") or Path(__file__).resolve().parents[1])
text = (source / "SmRestore.c").read_text()
header = (source / "Sm.h").read_text()


def function(signature):
    start = text.index(signature)
    # the functions of this file end with a closing brace in the first column
    return text[start:text.index("\n}", start) + 3] + "\n"


def typedef(name):
    match = re.search(r"typedef struct\n\{(?:(?!typedef)[^}])*\} " + name + ";", header)
    assert match, name
    return match.group(0) + "\n"


types = typedef(r"SessionSettings, \*SessionSettingsPtr") + typedef("SettingsSet")

parsers = "".join(function(signature) for signature in (
    "static unsigned int \nPeekAhead(", "static unsigned char * \nGetSmartString(", "void \nScanWhitespace(",
    "static unsigned char * \nGetNextLine(", "static int\nFillHintBuf(", "static int \nFillCmdBuf(",
    "static int \nFillRemoteBuf("))
if "EnsureBufferRoom" in text:
    parsers = function("static int\nEnsureBufferRoom(") + parsers
    parser = function("static int\nParseClientFile(")
else:
    # sources from before ParseClientFile existed: the loop sits inline in RestoreClients
    first = text.index("    while(GetNextLine() != NULL)\n    {\n\tlineP = line;")
    last = text.index("    /*\n     * All done with file so close it off")
    loop = text[first:last].replace("XDefaultScreen(smGD.display)", "defaultScreen")
    parser = '''static int ParseClientFile(unsigned char *remoteDisplay, unsigned char **hintsBuf, unsigned char **cmdBuf,
    unsigned int *maxHintsSize, unsigned int *actHintsSize, unsigned int *numHints,
    unsigned int *maxCmdSize, unsigned int *actCmdSize, unsigned int *numCmd,
    unsigned int *maxRemoteSize, int defaultScreen)
{
    unsigned char *lineP, *string;
    unsigned char *hostPtr = NULL, *cmdPtr = NULL, *hintPtr = NULL;
    int screenNum = 0, i;
    char *displayName = strdup("x");
''' + loop + '''    free(displayName);
    return 0;
}
'''

settings_start = text.index("static int \nRestoreSettings( void )")
settings = text[settings_start:text.index("\n}\n", settings_start) + 3]
settings = settings.replace("smGD.display->min_keycode", "8").replace("&(smGD.display->db)", "&fake_db")
settings = settings.replace("static int \nRestoreSettings( void )", "static int RestoreSettings( void )")

harness = r'''
#include <assert.h>
#include <limits.h>
#include <locale.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>
#include <X11/Intrinsic.h>

#define MAXLINE 1024
#define MAX_QUOTE_DEPTH 10
#define MAX_SCREENS_SAVED 10
#define SM_MALLOC(n) malloc(n)
#define SM_REALLOC(p, n) realloc(p, n)
#define SM_FREE(p) free(p)
#define DtError 1
#define GETMESSAGE(set, n, str) (str)

''' + types + r'''
static struct {
    char *fontDirs, *autoRepeats, *buttonMap, *keySyms, *modSyms, settingPath[MAXPATHLEN + 1];
    void *display, *topLevelWid;
    int numSavedScreens;
} smGD;
static struct { Boolean querySettings; } smRes;
static struct { int cycleTimeout, lockTimeout, saverTimeout; } smSaverRes;
static SessionSettings smSettings;
static SettingsSet smToSet;
static struct { int smBellPercent, smBellPitch, smBellDuration; } audioVals;
static struct { int smKeyClickPercent, smGlobalAutoRepeat; } keyboardVals;
static struct { int smTimeout, smInterval, smPreferBlank, smAllowExp; } screenSaverVals;
static struct { char *cantMallocErrorString; } smNLS = { "malloc" };
static XrmDatabase fake_db;
void ScanWhitespace(unsigned char **);
#define DtSM_ASK_STATE 2
#define DtSM_VERBOSE_MODE 1
static int settingsResources[1];

static unsigned char fallBackLine[MAXLINE + 2];
static unsigned char *line;
static FILE *cfileP;
static int linec, fileSize, numRemoteExecs;
static unsigned char *parseP;
unsigned char *remoteBuf[MAX_SCREENS_SAVED];
unsigned int actRemoteSize[MAX_SCREENS_SAVED];

static int errors;
static void PrintErrnoError(int type, const char *message) { (void) type; (void) message; errors++; }
static void PrintError(int type, const char *message) { (void) type; (void) message; errors++; }
static char *SmNewString(char *s) { return s ? strdup(s) : strdup(""); }

/* Xlib */
static int font_count, mapping_calls, mapping_codes, modifier_calls, modifier_keys;
static char *font_dirs[1000];
#define XChangePointerControl fake_XChangePointerControl
#define XSetScreenSaver fake_XSetScreenSaver
#define XChangeKeyboardControl fake_XChangeKeyboardControl
#define XSetPointerMapping fake_XSetPointerMapping
#define XSetFontPath fake_XSetFontPath
#define XChangeKeyboardMapping fake_XChangeKeyboardMapping
#define XKeysymToKeycode fake_XKeysymToKeycode
#define XSetModifierMapping fake_XSetModifierMapping
#define XrmGetFileDatabase fake_XrmGetFileDatabase
#define XtGetApplicationResources fake_XtGetApplicationResources
#define XrmMergeDatabases fake_XrmMergeDatabases
static int XChangePointerControl(void *d, int a, int b, int c, int e, int f) { return 0; }
static int XSetScreenSaver(void *d, int a, int b, int c, int e) { return 0; }
static int XChangeKeyboardControl(void *d, unsigned long m, XKeyboardControl *c) { return 0; }
static int XSetPointerMapping(void *d, unsigned char *map, int n) { (void) map; return n; }
static int XSetFontPath(void *d, char **dirs, int n)
{
    int i;
    font_count = n;
    for (i = 0; i < n; i++) font_dirs[i] = dirs[i];
    return 0;
}
static int XChangeKeyboardMapping(void *d, int first, int per, KeySym *syms, int n)
{
    (void) d; (void) first; (void) syms;
    mapping_calls++; mapping_codes = n;
    assert(per > 0 && n > 0);
    return 0;
}
static KeyCode XKeysymToKeycode(void *d, KeySym s) { (void) d; return (KeyCode) (s & 0xff); }
static int XSetModifierMapping(void *d, XModifierKeymap *map)
{
    int i, sum = 0;
    (void) d;
    modifier_calls++; modifier_keys = map->max_keypermod;
    for (i = 0; i < 8 * map->max_keypermod; i++) sum += map->modifiermap[i];   /* reads what was written */
    return sum >= 0 ? 0 : 1;
}
static XrmDatabase XrmGetFileDatabase(const char *name) { (void) name; return NULL; }
static void XtGetApplicationResources(void *w, void *base, void *res, unsigned n, void *a, unsigned b) { }
static void XrmMergeDatabases(XrmDatabase a, XrmDatabase *b) { }


''' + parsers + parser + r'''

''' + settings + r'''

/* --- helpers --- */
static char *work;
static unsigned char *hintsBuf[MAX_SCREENS_SAVED], *cmdBuf[MAX_SCREENS_SAVED];
static unsigned int maxHints[MAX_SCREENS_SAVED], actHints[MAX_SCREENS_SAVED], numHints[MAX_SCREENS_SAVED];
static unsigned int maxCmd[MAX_SCREENS_SAVED], actCmd[MAX_SCREENS_SAVED], numCmd[MAX_SCREENS_SAVED], maxRemote[MAX_SCREENS_SAVED];

static int parse_file(const char *content)
{
    struct stat st;
    char path[MAXPATHLEN];
    FILE *f;
    unsigned char display[128] = "host:0";
    snprintf(path, sizeof path, "%s/dt.session", work);
    f = fopen(path, "w"); assert(f); fputs(content, f); fclose(f);
    stat(path, &st);
    fileSize = st.st_size < MAXLINE + 1 ? MAXLINE + 1 : (int) st.st_size;
    line = malloc(fileSize + 1);
    cfileP = fopen(path, "r");
    memset(hintsBuf, 0, sizeof hintsBuf); memset(cmdBuf, 0, sizeof cmdBuf); memset(remoteBuf, 0, sizeof remoteBuf);
    memset(maxHints, 0, sizeof maxHints); memset(actHints, 0, sizeof actHints); memset(numHints, 0, sizeof numHints);
    memset(maxCmd, 0, sizeof maxCmd); memset(actCmd, 0, sizeof actCmd); memset(numCmd, 0, sizeof numCmd);
    memset(maxRemote, 0, sizeof maxRemote); memset(actRemoteSize, 0, sizeof actRemoteSize);
    smGD.numSavedScreens = 2;
    linec = 0; numRemoteExecs = 0;
    return ParseClientFile(display, hintsBuf, cmdBuf, maxHints, actHints, numHints, maxCmd, actCmd, numCmd, maxRemote, 0);
}
static char *repeat(char c, size_t n) { char *s = malloc(n + 1); memset(s, c, n); s[n] = 0; return s; }

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "failed: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

/* --- the quoting parser --- */
static int quote_depth(void)
{
    char input[4096] = "\"", *p;
    int k;
    unsigned char *at;
    for (k = 2; k <= 14; k++) {
        memset(input + strlen(input), '\\', 2 * k - 3);
        strcat(input, "\"z");
    }
    strcat(input, "\" tail");
    p = strdup(input);
    at = (unsigned char *) p;
    while (GetSmartString(&at) != NULL) { }
    free(p);
    return 0;
}
static int quoting_basics(void)
{
    char input[] = "plain \"quoted words\" back\\ slash tail";
    unsigned char *at = (unsigned char *) input, *word;
    word = GetSmartString(&at); CHECK(word && !strcmp((char *) word, "plain"));
    word = GetSmartString(&at); CHECK(word && !strcmp((char *) word, "quoted words"));
    word = GetSmartString(&at); CHECK(word && !strcmp((char *) word, "back slash"));
    word = GetSmartString(&at); CHECK(word && !strcmp((char *) word, "tail"));
    CHECK(GetSmartString(&at) == NULL);
    return 0;
}

/* --- the client file --- */
static int long_commands(void)
{
    char *cmd = repeat('c', 9000), *hint = repeat('h', 12000), *remote = repeat('r', 7000), *input = malloc(60000);
    snprintf(input, 60000,
             "dtsmcmd -hints \"%s\" -screen 0\n"
             "dtsmcmd -cmd \"%s\" -screen 0\n"
             "dtsmcmd -cmd \"%s\" -screen 1\n"
             "dtsmcmd -host far -cmd \"%s -display x\" -screen 0\n"
             "dtsmcmd -hints \"%s\" -screen 0\n", hint, cmd, cmd, remote, hint);
    CHECK(parse_file(input) == 0);
    CHECK(strlen((char *) cmdBuf[0]) == 9001 && numCmd[0] == 1 && numCmd[1] == 1);
    CHECK(numHints[0] == 2 && strlen((char *) hintsBuf[0]) == 6 + 2 * 12001);
    CHECK(remoteBuf[0] != NULL && strstr((char *) remoteBuf[0], "far ") != NULL && strstr((char *) remoteBuf[0], "\"rrr") != NULL);
    return 0;
}
static int growth_boundaries(void)
{
    /* many commands of different lengths: the buffers are filled to the last byte before they grow */
    int length, count = 0;
    char *input = malloc(3000000), *at = input;
    for (length = 1; length < 700; length += 3) {
        at += sprintf(at, "dtsmcmd -cmd \"%s\" -screen 0\n", repeat('x', length));
        at += sprintf(at, "dtsmcmd -hints \"%s\" -screen 1\n", repeat('y', length + 1));
        count++;
    }
    CHECK(parse_file(input) == 0);
    CHECK(numCmd[0] == (unsigned) count && numHints[1] == (unsigned) count);
    CHECK(actCmd[0] == strlen((char *) cmdBuf[0]));
    return 0;
}
static int missing_arguments(void)
{
    CHECK(parse_file("dtsmcmd -screen\ndtsmcmd -host\ndtsmcmd -cmd\ndtsmcmd -hints\n"
                     "dtsmcmd -host far -cmd\ndtsmcmd -host far -cmd \"\"\ndtsmcmd -cmd \"\" -screen 99\n"
                     "dtsmcmd -cmd \"ok\" -screen -1\ndtsmcmd\n\"\\\n") == 0);
    return 0;
}
static int lines_stand_alone(void)
{
    /* the host of one line does not make the command of the next one remote */
    CHECK(parse_file("dtsmcmd -host far\ndtsmcmd -cmd \"local one\"\ndtsmcmd -hints \"h\"\nnot a command line\n") == 0);
    CHECK(numCmd[0] == 1 && remoteBuf[0] == NULL && strstr((char *) cmdBuf[0], "local one") != NULL);
    return 0;
}

/* --- the settings --- */
static void settings(char *fonts, char *buttons, char *keys, char *mods, int codes, int per, int maxmod)
{
    memset(&smSettings, 0, sizeof smSettings);
    smSettings.kbdState.key_click_percent = -1; smSettings.kbdState.bell_percent = -1;
    smSettings.kbdState.bell_pitch = -1; smSettings.kbdState.bell_duration = -1;
    smSettings.kbdState.global_auto_repeat = AutoRepeatModeOn;
    smSettings.accelNum = -1; smSettings.threshold = -1; smSettings.timeout = -1; smSettings.interval = -1;
    smSettings.fontDirs = fonts; smSettings.autoRepeats = ""; smSettings.buttonMap = buttons;
    smSettings.keySyms = keys; smSettings.modSyms = mods;
    smSettings.numKeyCode = codes; smSettings.keySymPerCode = per; smSettings.maxKeyPerMod = maxmod;
    smSaverRes.cycleTimeout = smSaverRes.lockTimeout = smSaverRes.saverTimeout = -1;
    smSettings.didQuery = 0; smRes.querySettings = 1;
    smGD.settingPath[0] = 0;
    font_count = mapping_calls = mapping_codes = modifier_calls = modifier_keys = 0;
    /* RestoreSettings copies the resource strings itself */
    smGD.fontDirs = smGD.autoRepeats = smGD.buttonMap = smGD.keySyms = smGD.modSyms = NULL;
}
static char *list(int n, int from, char *buffer)
{
    char *at = buffer;
    int i;
    *at = 0;
    for (i = 0; i < n; i++) at += sprintf(at, "%s%d", i ? "," : "", from + i);
    return buffer;
}
static int font_path(void)
{
    static char fonts[20000];
    int n, i;
    int counts[] = {1, 49, 50, 51, 52, 100, 101, 149, 150, 151, 400};
    for (i = 0; i < (int) (sizeof counts / sizeof counts[0]); i++) {
        char *at = fonts;
        n = counts[i];
        for (int j = 0; j < n; j++) at += sprintf(at, "%s/fonts/%d", j ? "," : "", j);
        settings(fonts, "", "", "", 0, 0, 0);
        CHECK(RestoreSettings() == 0);
        CHECK(font_count == n);
        CHECK(!strcmp(font_dirs[n - 1], (sprintf(fonts, "/fonts/%d", n - 1), fonts)));
    }
    settings("", "", "", "", 0, 0, 0);
    CHECK(RestoreSettings() == 0 && font_count == 0);
    return 0;
}
static int modifier_map(void)
{
    static char mods[600000];
    settings("", "", "", list(16, 100, mods), 0, 0, 2);
    CHECK(RestoreSettings() == 0 && modifier_calls == 1 && modifier_keys == 2);
    list(5000, 1, mods);
    settings("", "", "", mods, 0, 0, 255);                 /* the most the protocol allows */
    CHECK(RestoreSettings() == 0 && modifier_calls == 1 && modifier_keys == 255);
    settings("", "", "", mods, 0, 0, 700);                 /* more than the scratch buffer holds */
    CHECK(RestoreSettings() == 0 && modifier_calls == 0);
    settings("", "", "", mods, 0, 0, 100000);               /* more than any server can have */
    CHECK(RestoreSettings() == 0 && modifier_calls == 0);
    settings("", "", "", mods, 0, 0, -3);
    CHECK(RestoreSettings() == 0 && modifier_calls == 0);
    return 0;
}
static int key_map(void)
{
    static char keys[200000], mods[100];
    settings("", "", list(1000, 10, keys), "", 1000, 2, 0);
    CHECK(RestoreSettings() == 0 && mapping_calls == 1 && mapping_codes == 500);
    settings("", "", list(10000, 10, keys), "", 1000, 4, 0);
    CHECK(RestoreSettings() == 0 && mapping_calls == 1 && mapping_codes == 2500);
    settings("", "", list(10, 10, keys), "", 5, 0, 0);       /* no keysyms per code: nothing to divide by */
    CHECK(RestoreSettings() == 0 && mapping_calls == 0);
    settings("", "1,2,3", "", "", 5, 2, 0);                 /* no keysyms: the count of the buttons is not a count of codes */
    CHECK(RestoreSettings() == 0 && mapping_calls == 0);
    (void) mods;
    return 0;
}

static struct { const char *name; int (*run)(void); } scenarios[] = {
    {"quote_depth", quote_depth}, {"quoting_basics", quoting_basics},
    {"long_commands", long_commands}, {"growth_boundaries", growth_boundaries},
    {"missing_arguments", missing_arguments}, {"lines_stand_alone", lines_stand_alone},
    {"font_path", font_path}, {"modifier_map", modifier_map}, {"key_map", key_map},
    {NULL, NULL}
};

int main(int argc, char **argv)
{
    int i;
    setlocale(LC_ALL, "");
    if (argc < 3) { for (i = 0; scenarios[i].name; i++) puts(scenarios[i].name); return 0; }
    work = argv[2];
    for (i = 0; scenarios[i].name; i++)
        if (!strcmp(argv[1], scenarios[i].name)) return scenarios[i].run();
    return 2;
}
'''

cc = shlex.split(os.environ.get("CC", "gcc"))
failed = []
with tempfile.TemporaryDirectory(prefix="dtsession-parsers-", dir=os.environ.get("TMPDIR")) as scratch:
    scratch = Path(scratch)
    c = scratch / "parsers.c"
    c.write_text(harness)
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    binary = scratch / "parsers"
    build = cc + ["-std=gnu23", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-w", "-DMULTIBYTE", str(c), "-o", str(binary)]
    compiled = subprocess.run(build, capture_output=True, text=True)
    if compiled.returncode:
        sys.stderr.write(compiled.stderr[:6000])
        sys.exit("cannot compile the session parser harness")
    names = subprocess.run([str(binary)], capture_output=True, text=True, check=True, env=env).stdout.split()
    for locale in ("C.UTF-8", "C"):
        env["LC_ALL"] = locale
        for name in names:
            work = scratch / ("work-%s-%s" % (locale, name))
            work.mkdir()
            run = subprocess.run([str(binary), name, str(work)], capture_output=True, text=True, timeout=300, env=env)
            if run.returncode:
                failed.append("%s/%s" % (locale, name))
                lines = [l for l in run.stderr.strip().splitlines() if l.strip()]
                sys.stderr.write("%s/%s failed: %s\n" % (locale, name, lines[0] if lines else "exit %d" % run.returncode))
if failed:
    sys.exit("%d session parser scenarios failed: %s" % (len(failed), ", ".join(failed)))
print("Session parsers: %d scenarios passed in two locales" % len(names))
