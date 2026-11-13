/*
 * Fuzz target: the dtsession client file (CDE 1.0 session file) parser.
 *
 * run-fuzz.py extracts the quoting parser (GetSmartString and its helpers),
 * the line reader, the buffer fillers and ParseClientFile verbatim from
 * dtsession/SmRestore.c.  The stand-ins here are the globals and error
 * reporters of the file.
 *
 * Input layout: byte 0 selects the entry point, the rest is the data.
 *   even  ParseClientFile reads the data as a client file (a memory stream
 *         stands in for the opened file, the line buffer is sized the way
 *         RestoreClients sizes it)
 *   odd   GetSmartString is applied to the data as one string until it
 *         returns no more words
 */
#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>

typedef unsigned char Boolean;
#define True 1
#define False 0

#define MAXLINE 1024
#define MAX_QUOTE_DEPTH 10
#define MAX_SCREENS_SAVED 10
#define SM_MALLOC(n) malloc(n)
#define SM_REALLOC(p, n) realloc(p, n)
#define SM_FREE(p) free(p)
#define DtError 1
#define GETMESSAGE(set, n, str) (str)
#define MULTIBYTE

#include FUZZ_TYPES_H

static struct {
    char *fontDirs, *autoRepeats, *buttonMap, *keySyms, *modSyms, settingPath[MAXPATHLEN + 1];
    void *display, *topLevelWid;
    int numSavedScreens;
} smGD;
static struct { char *cantMallocErrorString; } smNLS = { "malloc" };

static unsigned char fallBackLine[MAXLINE + 2];
static unsigned char *line;
static FILE *cfileP;
static int linec, fileSize, numRemoteExecs;
static unsigned char *parseP;
unsigned char *remoteBuf[MAX_SCREENS_SAVED];
unsigned int actRemoteSize[MAX_SCREENS_SAVED];

static void PrintErrnoError(int type, const char *message) { (void)type; (void)message; }
static void PrintError(int type, const char *message) { (void)type; (void)message; }
static char *SmNewString(char *s) { return s ? strdup(s) : strdup(""); }

void ScanWhitespace(unsigned char **);

#include FUZZ_PRODUCTION_H

static unsigned char *hintsBuf[MAX_SCREENS_SAVED], *cmdBuf[MAX_SCREENS_SAVED];
static unsigned int maxHints[MAX_SCREENS_SAVED], actHints[MAX_SCREENS_SAVED], numHints[MAX_SCREENS_SAVED];
static unsigned int maxCmd[MAX_SCREENS_SAVED], actCmd[MAX_SCREENS_SAVED], numCmd[MAX_SCREENS_SAVED];
static unsigned int maxRemote[MAX_SCREENS_SAVED];

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    if (!setlocale(LC_ALL, "en_US.UTF-8"))
        setlocale(LC_ALL, "");
    return 0;
}

static void release(unsigned char **bufs, int n)
{
    for (int i = 0; i < n; i++) { free(bufs[i]); bufs[i] = NULL; }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0) return 0;
    int mode = data[0] & 1;
    data++; size--;

    if (mode == 1) {
        unsigned char *copy = malloc(size + 1), *at;
        memcpy(copy, data, size);
        copy[size] = 0;
        at = copy;
        while (GetSmartString(&at) != NULL)
            ;
        free(copy);
        return 0;
    }

    unsigned char display[128] = "host:0";
    if (size == 0) return 0;
    fileSize = size < MAXLINE + 1 ? MAXLINE + 1 : (int)size;
    line = malloc(fileSize + 1);
    cfileP = fmemopen((void *)data, size, "r");
    memset(hintsBuf, 0, sizeof hintsBuf); memset(cmdBuf, 0, sizeof cmdBuf); memset(remoteBuf, 0, sizeof remoteBuf);
    memset(maxHints, 0, sizeof maxHints); memset(actHints, 0, sizeof actHints); memset(numHints, 0, sizeof numHints);
    memset(maxCmd, 0, sizeof maxCmd); memset(actCmd, 0, sizeof actCmd); memset(numCmd, 0, sizeof numCmd);
    memset(maxRemote, 0, sizeof maxRemote); memset(actRemoteSize, 0, sizeof actRemoteSize);
    smGD.numSavedScreens = 2;
    linec = 0; numRemoteExecs = 0;

    ParseClientFile(display, hintsBuf, cmdBuf, maxHints, actHints, numHints, maxCmd, actCmd, numCmd,
                    maxRemote, 0);

    fclose(cfileP);
    cfileP = NULL;
    free(line);
    release(hintsBuf, MAX_SCREENS_SAVED); release(cmdBuf, MAX_SCREENS_SAVED); release(remoteBuf, MAX_SCREENS_SAVED);
    return 0;
}
