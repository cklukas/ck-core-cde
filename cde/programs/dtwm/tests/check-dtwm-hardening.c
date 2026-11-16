/*
 * Standalone regression tests for dtwm input-path and bounds hardening:
 *   1. _DtWmParseFilenameExpand (WmParse.c):
 *      - "/prefix/$VAR" expansion at the end of a string does not overflow
 *        the heap buffer when strlen(prefix) >= strlen(VAR_VALUE) - strlen("$VAR")
 *      - "" (empty string) returns a NUL-terminated string even when malloc
 *        returns non-zeroed memory
 *   2. ParseSessionClientState / ParseSessionGeometry / ParseSessionWorkspaces /
 *      ParseSessionCommand / ParseSessionHost (WmResParse.c):
 *      - nullptr token argument does not crash
 *   3. StrToNum / ParseBtnEvent (WmResParse.c):
 *      - StrToNum(nullptr), StrToNum(""), StrToNum("0x") return (unsigned int)-1
 *      - ParseBtnEvent with button >= 6 on ButtonRelease does not read past
 *        buttonModifierMasks[6]
 *   4. EliminateEntries / ResolveDuplicates (DataBaseLoad.c):
 *      - EliminateEntries copies (*record_count - end - 1) records instead of
 *        reading 1 record past the end of record_data
 *      - ResolveDuplicates with ANY_CONTAINER_TYPE and locked=True compares
 *        against record_name instead of dereferencing cont_name == nullptr
 *   5. CreateWorkspace / RemoveClientFromWsList (WmWrkspace.c):
 *      - CreateWorkspace preserves pActiveWS and pLastWS indices across
 *        pSD->pWS reallocation (without dividing pointer difference by sizeof)
 *      - RemoveClientFromWsList does not decrement numClients when pCD is
 *        not present in pWS->ppClients
 *   6. ResCat (WmResource.c):
 *      - ResCat truncates 4 strings within sizeof(wmGD.tmpBuffer) without
 *        passing a negative count to strncat
 */

#define _GNU_SOURCE
#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int Boolean;
typedef char *String;
typedef unsigned int Cardinal;
typedef unsigned long Atom;
typedef unsigned long Window;
typedef unsigned long KeyCode;
typedef void *XtPointer;
typedef void *XmString;

#define True 1
#define False 0
#define TRUE 1
#define FALSE 0
#define XtNumber(arr) ((Cardinal)(sizeof(arr) / sizeof((arr)[0])))
#define XtNew(type) ((type *)XtMalloc(sizeof(type)))

static void *XtMalloc(size_t size)
{
    void *p = malloc(size ? size : 1);
    assert(p != nullptr);
    /* Fill with non-zero bytes to catch uninitialized NUL terminators. */
    memset(p, 0x5a, size ? size : 1);
    return p;
}

static void *XtCalloc(size_t count, size_t size)
{
    void *p = calloc(count ? count : 1, size ? size : 1);
    assert(p != nullptr);
    return p;
}

static void *XtRealloc(char *ptr, size_t size)
{
    void *p = realloc(ptr, size ? size : 1);
    assert(p != nullptr);
    return p;
}

static void XtFree(char *ptr)
{
    free(ptr);
}

static String XtNewString(const char *str)
{
    if (str == nullptr)
        return nullptr;
    size_t len = strlen(str) + 1;
    char *copy = (char *)XtMalloc(len);
    memcpy(copy, str, len);
    return copy;
}

static XmString XmStringCreateLocalized(char *text)
{
    return (XmString)XtNewString(text ? text : "");
}

static void XmStringFree(XmString s)
{
    XtFree((char *)s);
}

#define GETMESSAGE(set, num, dflt) (dflt)
static void Warning(char *msg)
{
    (void)msg;
}

/* Stand-ins for WmParse.c */
#define DTWM_CHAR_ENVIRONMENT '$'
#define DTWM_CHAR_DIRECTORY '/'

/* Stand-ins for WmResParse.c */
#define NORMAL_STATE 1
#define MINIMIZED_STATE 2
#define ButtonRelease 5

typedef struct {
    int flags;
    int clientX;
    int clientY;
    int clientWidth;
    int clientHeight;
} SessionGeom;

typedef struct {
    int clientState;
    SessionGeom *sessionGeom;
    String workspaces;
    int commandArgc;
    char **commandArgv;
    String clientMachine;
} DtSessionItem;

typedef struct _ClientData {
    int dummy;
} ClientData;

typedef struct _WmWorkspaceData {
    Atom id;
    String name;
    XmString title;
    ClientData **ppClients;
    int numClients;
} WmWorkspaceData;

typedef struct _WmScreenData {
    DtSessionItem *pDtSessionItems;
    int totalSessionItems;
    WmWorkspaceData *pWS;
    WmWorkspaceData *pActiveWS;
    WmWorkspaceData *pLastWS;
    int numWorkspaces;
    int numWsDataAllocated;
    Boolean useIconBox;
} WmScreenData;

#define WS_ALLOC_AMOUNT 2
#define WM_RES_WORKSPACE_LIST 1
#define WM_RES_WORKSPACE_COUNT 2
#define WM_RES_WORKSPACE_TITLE 4
#define DtWSM_REASON_ADD 1

static void SetWorkspaceInfoProperty(WmWorkspaceData *pWS) { (void)pWS; }
static void SetWorkspaceListProperty(WmScreenData *pSD) { (void)pSD; }
static void SaveWorkspaceResources(WmWorkspaceData *pWS, unsigned long flags)
{
    (void)pWS;
    (void)flags;
}
static void dtSendWorkspaceModifyNotification(WmScreenData *pSD, Atom id, int type)
{
    (void)pSD;
    (void)id;
    (void)type;
}
static void AddIconBoxForWorkspace(WmWorkspaceData *pWS) { (void)pWS; }
static void AddPersistentWindows(WmWorkspaceData *pWS) { (void)pWS; }
static void UpdatePresenceWorkspaces(WmScreenData *pSD) { (void)pSD; }

static int XParseGeometry(const char *str, int *x, int *y,
                          unsigned int *w, unsigned int *h)
{
    if (str == nullptr || *str == '\0')
        return 0;
    *x = 10;
    *y = 20;
    *w = 100;
    *h = 200;
    return 1;
}

static unsigned char *GetSmartSMString(unsigned char **linePP)
{
    if (linePP == nullptr || *linePP == nullptr || **linePP == '\0')
        return nullptr;
    unsigned char *s = *linePP;
    while (*s == ' ')
        s++;
    if (*s == '\0') {
        *linePP = s;
        return nullptr;
    }
    unsigned char *start = s;
    while (*s != '\0' && *s != ' ')
        s++;
    if (*s != '\0') {
        *s = '\0';
        s++;
    }
    *linePP = s;
    return start;
}

typedef struct {
    const char *name;
    unsigned int eventType;
    unsigned int button;
    Boolean fClick;
} EventTableEntry;

static EventTableEntry buttonEvents[] = {
    { "btn1up", ButtonRelease, 1, FALSE },
    { "btn8up", ButtonRelease, 8, FALSE },
    { nullptr, 0, 0, FALSE }
};

static unsigned int buttonModifierMasks[6] = {
    0, (1u << 8), (1u << 9), (1u << 10), (1u << 11), (1u << 12)
};

static Boolean ParseEvent(unsigned char **linePP, EventTableEntry *table,
                          unsigned int *eventType, unsigned int *button,
                          unsigned int *state, Boolean *fClick)
{
    (void)table;
    if (linePP == nullptr || *linePP == nullptr)
        return FALSE;
    if (strcmp((const char *)*linePP, "btn8up") == 0) {
        *eventType = ButtonRelease;
        *button = 8;
        *state = 0;
        *fClick = FALSE;
        return TRUE;
    }
    *eventType = ButtonRelease;
    *button = 1;
    *state = 0;
    *fClick = FALSE;
    return TRUE;
}

/* Stand-ins for DataBaseLoad.c */
#define ANY_CONTAINER_TYPE (-1)
typedef struct {
    void *parsed_value;
    char *string_value;
    int use_default;
} ElementValue;

typedef struct {
    ElementValue *element_values;
} RecordData;

static int removed_entries_count = 0;
static void RemoveEntry(RecordData *record, int record_type)
{
    (void)record;
    (void)record_type;
    removed_entries_count++;
}

/* Stand-ins for WmWrkspace.c & WmResource.c */
static unsigned char *GenerateWorkspaceName(WmScreenData *pSD, int wsnum)
{
    static char buf[32];
    (void)pSD;
    snprintf(buf, sizeof(buf), "ws%d", wsnum);
    return (unsigned char *)buf;
}

static void InitWmWorkspace(WmWorkspaceData *pWS, WmScreenData *pSD)
{
    (void)pSD;
    pWS->title = nullptr;
    pWS->ppClients = nullptr;
    pWS->numClients = 0;
}

#define MAXTMPBUFFER 256
#define MAXBUF 256
#define WM_RESOURCE_CLASS "Mwm"
#define DT_WM_RESOURCE_CLASS "Dtwm"
#define MwmBehavior 0

static struct {
    unsigned char tmpBuffer[MAXTMPBUFFER];
} wmGD;

#include DTWM_EXTRACTED_HEADER

static void test_filename_expand(void)
{
    /*
     * When a literal prefix precedes a trailing $VAR and
     * strlen(prefix) >= strlen(env_val) - strlen("$VAR"), the unfixed
     * _DtWmParseFilenameExpand did not advance pchO to pchEnv0 before
     * computing n = 1 + strlen(pchO), so nx > n was false and pchNew was
     * not reallocated before writing past the end of pchNew!
     */
    setenv("DTWM_TEST_EXPAND_VAR", "0123456789ABCDEF", 1);
    unsigned char input[] = "/very/long/prefix/path/$DTWM_TEST_EXPAND_VAR";
    unsigned char *expanded = _DtWmParseFilenameExpand(input);
    assert(expanded != nullptr);
    assert(strcmp((char *)expanded, "/very/long/prefix/path/0123456789ABCDEF") == 0);
    XtFree((char *)expanded);

    /* Empty string must be NUL-terminated even when XtMalloc returns 0x5a. */
    unsigned char empty[] = "";
    unsigned char *empty_expanded = _DtWmParseFilenameExpand(empty);
    assert(empty_expanded != nullptr);
    assert(strcmp((char *)empty_expanded, "") == 0);
    XtFree((char *)empty_expanded);
}

static void test_session_parsers_null_guard(void)
{
    DtSessionItem item;
    memset(&item, 0, sizeof(item));
    WmScreenData sd;
    memset(&sd, 0, sizeof(sd));
    sd.pDtSessionItems = &item;
    sd.totalSessionItems = 1;

    ParseSessionClientState(&sd, 0, nullptr);
    ParseSessionGeometry(&sd, 0, nullptr);
    ParseSessionWorkspaces(&sd, 0, nullptr);
    ParseSessionHost(&sd, 0, nullptr);
    unsigned char *null_cmd = nullptr;
    ParseSessionCommand(&sd, 0, &null_cmd);
    assert(item.sessionGeom == nullptr);
    assert(item.workspaces == nullptr);
    assert(item.clientMachine == nullptr);
    assert(item.commandArgv == nullptr);
}

static void test_strtonum_and_btn_event(void)
{
    assert(StrToNum(nullptr) == (unsigned int)-1);
    assert(StrToNum((unsigned char *)"") == (unsigned int)-1);
    assert(StrToNum((unsigned char *)"0x") == (unsigned int)-1);
    assert(StrToNum((unsigned char *)"42") == 42u);
    assert(StrToNum((unsigned char *)"0x1a") == 26u);

    unsigned char *line = (unsigned char *)"btn8up";
    unsigned int eventType = 0, button = 0, state = 0;
    Boolean fClick = FALSE;
    assert(ParseBtnEvent(&line, &eventType, &button, &state, &fClick) == TRUE);
    assert(button == 8u);
    assert(state == 0u);
}

static void test_database_load_bounds(void)
{
    /*
     * 1. EliminateEntries on a 3-element heap array: removing index 0 must
     *    memmove 2 records (indices 1 and 2), not 3 records (which reads 1
     *    RecordData past the heap allocation under ASan).
     */
    RecordData *records = (RecordData *)XtMalloc(3 * sizeof(RecordData));
    ElementValue ev0[2] = {{(void *)"r0", nullptr, 0}, {(void *)(intptr_t)0, nullptr, 0}};
    ElementValue ev1[2] = {{(void *)"r1", nullptr, 0}, {(void *)(intptr_t)0, nullptr, 0}};
    ElementValue ev2[2] = {{(void *)"r2", nullptr, 0}, {(void *)(intptr_t)0, nullptr, 0}};
    records[0].element_values = ev0;
    records[1].element_values = ev1;
    records[2].element_values = ev2;

    int record_count = 3;
    int i = 0;
    int count = 3;
    EliminateEntries(records, &record_count, 0, 0, &i, &count, 0);
    assert(record_count == 2);
    assert(records[0].element_values == ev1);
    assert(records[1].element_values == ev2);
    XtFree((char *)records);

    /*
     * 2. ResolveDuplicates with ANY_CONTAINER_TYPE and a locked record:
     *    cont_name is nullptr when container_type == ANY_CONTAINER_TYPE, so
     *    comparing against cont_name crashes in strcmp.
     */
    RecordData *dup_records = (RecordData *)XtMalloc(2 * sizeof(RecordData));
    ElementValue dev0[2] = {{(void *)"SubpanelA", nullptr, 0}, {(void *)(intptr_t)0, nullptr, 0}};
    ElementValue dev1[2] = {{(void *)"SubpanelA", nullptr, 0}, {(void *)(intptr_t)1, nullptr, 0}};
    dup_records[0].element_values = dev0;
    dup_records[1].element_values = dev1;
    int dup_count = 2;
    ResolveDuplicates(dup_records, &dup_count, 0, ANY_CONTAINER_TYPE, 0, 1, 0);
    assert(dup_count == 1);
    assert(dup_records[0].element_values == dev1);
    XtFree((char *)dup_records);
}

static void test_workspace_realloc_and_remove(void)
{
    WmScreenData sd;
    memset(&sd, 0, sizeof(sd));
    sd.numWsDataAllocated = 2;
    sd.numWorkspaces = 2;
    sd.pWS = (WmWorkspaceData *)XtCalloc(2, sizeof(WmWorkspaceData));
    sd.pWS[0].name = XtNewString("ws0");
    sd.pWS[1].name = XtNewString("ws1");
    sd.pActiveWS = &sd.pWS[1];
    sd.pLastWS = &sd.pWS[0];

    WmWorkspaceData *ws2 = CreateWorkspace(&sd, (unsigned char *)"Three");
    assert(ws2 == &sd.pWS[2]);
    assert(sd.numWorkspaces == 3);
    /* Active workspace index (1) and last workspace index (0) must be preserved! */
    assert(sd.pActiveWS == &sd.pWS[1]);
    assert(sd.pLastWS == &sd.pWS[0]);

    ClientData c1 = {1}, c2 = {2};
    ClientData *clients[2] = { &c1, nullptr };
    ws2->ppClients = clients;
    ws2->numClients = 1;

    /* Removing a client not in the workspace must not decrement numClients. */
    RemoveClientFromWsList(ws2, &c2);
    assert(ws2->numClients == 1);
    RemoveClientFromWsList(ws2, &c1);
    assert(ws2->numClients == 0);

    for (int i = 0; i < sd.numWorkspaces; i++) {
        XtFree(sd.pWS[i].name);
        XmStringFree(sd.pWS[i].title);
    }
    XtFree((char *)sd.pWS);
}

static void test_rescat_bounds(void)
{
    char s1[120], s2[120], s3[120], s4[120];
    memset(s1, 'a', sizeof(s1) - 1); s1[sizeof(s1) - 1] = '\0';
    memset(s2, 'b', sizeof(s2) - 1); s2[sizeof(s2) - 1] = '\0';
    memset(s3, 'c', sizeof(s3) - 1); s3[sizeof(s3) - 1] = '\0';
    memset(s4, 'd', sizeof(s4) - 1); s4[sizeof(s4) - 1] = '\0';

    String res = ResCat(s1, s2, s3, s4);
    assert(res == (String)wmGD.tmpBuffer);
    assert(strlen(res) == sizeof(wmGD.tmpBuffer) - 1);
}

int main(void)
{
    test_filename_expand();
    test_session_parsers_null_guard();
    test_strtonum_and_btn_event();
    test_database_load_bounds();
    test_workspace_realloc_and_remove();
    test_rescat_bounds();
    puts("check-dtwm-hardening: all tests passed");
    return 0;
}
