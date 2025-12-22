#include "ActionIconCache.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdarg.h>
#include <time.h>

#include <Xm/Xm.h>
#include <X11/Intrinsic.h>

#include <Dt/DtsDb.h>
#include <Dt/DtsMM.h>
#include <Dt/ActionDb.h>

#define ACTION_ICON_CACHE_FILE ".dt/action-icon-cache"
#define ACTION_ICON_CACHE_MAX_ENTRIES 256
#define ACTION_ICON_CACHE_LOG_ENV "DTWM_ACTION_ICON_CACHE_LOG"

typedef struct {
    char *command;
    char *icon;
} ActionIconEntry;

typedef struct {
    char *command;
    char *action;
} ActionCacheEntry;

typedef struct {
    char **items;
    int count;
    int capacity;
} StringList;

static ActionIconEntry *actionEntries = NULL;
static int actionEntryCount = 0;
static int actionEntryCapacity = 0;
static Boolean actionEntriesLoaded = False;

static ActionCacheEntry *cacheEntries = NULL;
static int cacheEntryCount = 0;
static int cacheEntryCapacity = 0;
static Boolean cacheLoaded = False;
static Boolean cacheDirty = False;
static char *cacheFilePath = NULL;
static char *homePath = NULL;

static const char *GetHomeDir(void);
static const char *GetCacheFilePath(void);
static Boolean EnsureCacheDir(void);
static void LoadActionDatabase(void);
static void FreeActionEntries(void);
static Boolean LoadActionDatabaseFromInMemoryDb(void);
static Boolean LoadActionDatabaseFromMmDb(void);
static void LoadActionDatabaseFromFiles(void);
static void LoadActionDatabaseFromDir(const char *dirPath);
static void ParseActionFile(const char *filePath);
static void AddOrUpdateActionEntry(const char *command, const char *icon, const char *source);
static void LoadPersistentCache(void);
static char *DeriveActionCommand(const char *execString);
static Boolean CommandsMatch(const char *actionCmd, const char *windowCmd);
static const char *FindInEntries(const ActionIconEntry *entries, int count,
                                  const char *command);
static Boolean AddCacheEntryInternal(const char *command, const char *action,
                                     Boolean markDirty);
static void SaveCacheFile(void);
static void FreeCacheEntry(ActionCacheEntry *entry);
static const char *FindCachedAction(const char *command);
static void DescribeChildList(const char *childList,
                              char *snippet,
                              size_t snippetSize,
                              int *count);
static void ActionIconCacheLog(const char *fmt, ...);
static void StringListInit(StringList *list);
static void StringListFree(StringList *list);
static Boolean StringListAdd(StringList *list, char *value);
static Boolean StringListAddDup(StringList *list, const char *value, size_t len);
static void ParseCommandList(const char *source, StringList *list);

static const char *
GetHomeDir(void)
{
    if (homePath)
        return homePath;

    const char *value = getenv("HOME");
    if (!value || value[0] == '\0') {
        struct passwd *user = getpwuid(getuid());
        if (user)
            value = user->pw_dir;
    }

    if (!value)
        return NULL;

    homePath = strdup(value);
    return homePath;
}

static const char *
GetCacheFilePath(void)
{
    if (cacheFilePath)
        return cacheFilePath;

    const char *home = GetHomeDir();
    if (!home || home[0] == '\0')
        return NULL;

    char path[PATH_MAX];
    int len = snprintf(path, sizeof(path), "%s/%s", home, ACTION_ICON_CACHE_FILE);
    if (len < 0 || len >= (int)sizeof(path))
        return NULL;

    cacheFilePath = strdup(path);
    return cacheFilePath;
}

static void
DescribeChildList(const char *childList, char *snippet, size_t snippetSize,
                  int *count)
{
    if (count)
        *count = 0;

    if (!snippet || snippetSize == 0)
        return;

    snippet[0] = '\0';

    if (!childList || childList[0] == '\0')
        return;

    size_t sniplen = 0;
    const char *cur = childList;
    int idx = 0;

    while (*cur)
    {
        const char *end = cur;
        while (*end && *end != '\n')
            ++end;

        if (idx == 0)
        {
            size_t len = (size_t)(end - cur);
            size_t copy = len < snippetSize - 1 ? len : snippetSize - 1;
            memcpy(snippet, cur, copy);
            snippet[copy] = '\0';
        }

        ++idx;

        if (*end == '\0')
            break;

        cur = end + 1;
    }

    if (count)
        *count = idx;
}

static void
ActionIconCacheLog(const char *fmt, ...)
{
    if (!fmt)
        return;

    const char *logPath = getenv(ACTION_ICON_CACHE_LOG_ENV);
    if (!logPath || logPath[0] == '\0')
        return;

    FILE *fp = fopen(logPath, "a");
    if (!fp)
        return;

    static Boolean wroteHeader = False;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char timestr[32];
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", &tm);

    fprintf(fp, "%s ", timestr);

    if (!wroteHeader)
    {
        wroteHeader = True;
        char exePath[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
        if (len > 0)
        {
            exePath[len] = '\0';
            fprintf(fp, "pid=%ld exe=%s ", (long)getpid(), exePath);
        }
        else
        {
            fprintf(fp, "pid=%ld exe=(unknown) ", (long)getpid());
        }
    }

    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);

    fputc('\n', fp);
    fclose(fp);
}

static void
StringListInit(StringList *list)
{
    if (!list)
        return;
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void
StringListFree(StringList *list)
{
    if (!list)
        return;

    for (int i = 0; i < list->count; ++i)
        free(list->items[i]);

    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static Boolean
StringListAdd(StringList *list, char *value)
{
    if (!list || !value)
        return False;

    if (list->count >= list->capacity)
    {
        int newCap = list->capacity ? list->capacity * 2 : 8;
        char **newItems = (char **)realloc(list->items,
                                           newCap * sizeof(char *));
        if (!newItems)
            return False;

        list->items = newItems;
        list->capacity = newCap;
    }

    list->items[list->count++] = value;
    return True;
}

static Boolean
StringListAddDup(StringList *list, const char *value, size_t len)
{
    if (!list || !value || len == 0)
        return False;

    char *copy = (char *)malloc(len + 1);
    if (!copy)
        return False;

    memcpy(copy, value, len);
    copy[len] = '\0';

    if (!StringListAdd(list, copy))
    {
        free(copy);
        return False;
    }

    return True;
}

static void
ParseCommandList(const char *source, StringList *list)
{
    if (!list)
        return;

    StringListInit(list);
    if (!source)
        return;

    const char *cur = source;
    while (cur && *cur)
    {
        const char *end = strchr(cur, '\n');
        size_t len = end ? (size_t)(end - cur) : strlen(cur);

        if (len > 0)
            StringListAddDup(list, cur, len);

        if (!end)
            break;

        cur = end + 1;
    }
}

static Boolean
EnsureCacheDir(void)
{
    const char *home = GetHomeDir();
    if (!home || home[0] == '\0')
        return False;

    char dir[PATH_MAX];
    int len = snprintf(dir, sizeof(dir), "%s/.dt", home);
    if (len < 0 || len >= (int)sizeof(dir))
        return False;

    struct stat st;
    if (stat(dir, &st) != 0) {
        if (errno != ENOENT)
            return False;
        if (mkdir(dir, S_IRWXU) != 0 && errno != EEXIST)
            return False;
    }
    else if (!S_ISDIR(st.st_mode)) {
        return False;
    }

    return True;
}

static char *
DeriveActionCommand(const char *execString)
{
    if (!execString)
        return NULL;

    size_t len = strlen(execString);
    char *buffer = (char *)malloc(len + 1);
    if (!buffer)
        return NULL;

    size_t dst = 0;
    const char *src = execString;

    while (*src && dst < len)
    {
        if (*src == '%')
        {
            ++src;
            while (*src && *src != '%')
                ++src;
            if (*src == '%')
                ++src;
            continue;
        }

        if (*src == '\\' && (src[1] == '\n' || src[1] == '\r'))
        {
            ++src;
            continue;
        }

        buffer[dst++] = *src++;
    }

    buffer[dst] = '\0';

    char *start = buffer;
    while (*start && isspace((unsigned char)*start))
        ++start;

    if (*start == '\0')
    {
        free(buffer);
        return NULL;
    }

    char *end = start;
    while (*end && !isspace((unsigned char)*end))
        ++end;
    *end = '\0';

    char *result = strdup(start);
    free(buffer);
    return result;
}

static Boolean
CommandsMatch(const char *actionCmd, const char *windowCmd)
{
    if (!actionCmd || !windowCmd)
        return False;

    if (strcmp(actionCmd, windowCmd) == 0)
        return True;

    const char *actionBase = strrchr(actionCmd, '/');
    const char *windowBase = strrchr(windowCmd, '/');
    const char *actionName = actionBase ? actionBase + 1 : actionCmd;
    const char *windowName = windowBase ? windowBase + 1 : windowCmd;

    if (strcmp(actionName, windowName) == 0)
        return True;

    if (*actionName && *windowName &&
        (strcasestr(actionName, windowName) != NULL ||
         strcasestr(windowName, actionName) != NULL))
    {
        return True;
    }

    return False;
}

static const char *
FindInEntries(const ActionIconEntry *entries, int count, const char *command)
{
    if (!entries || !command)
        return NULL;

    for (int i = 0; i < count; ++i)
    {
        if (CommandsMatch(entries[i].command, command))
            return entries[i].icon;
    }

    return NULL;
}

static const char *
FindCachedAction(const char *command)
{
    if (!cacheEntries || !command)
        return NULL;

    for (int i = 0; i < cacheEntryCount; ++i)
    {
        if (CommandsMatch(cacheEntries[i].command, command))
            return cacheEntries[i].action;
    }

    return NULL;
}

static void
LoadActionDatabase(void)
{
    if (actionEntriesLoaded && actionEntryCount > 0)
        return;

    if (actionEntriesLoaded && actionEntryCount == 0)
    {
        ActionIconCacheLog("Action DB previously loaded empty; retrying");
        actionEntriesLoaded = False;
    }

    if (LoadActionDatabaseFromInMemoryDb() || LoadActionDatabaseFromMmDb())
    {
        actionEntriesLoaded = True;
        ActionIconCacheLog("Loaded action DB entries=%d", actionEntryCount);
        return;
    }

    ActionIconCacheLog("Action DB unavailable, falling back to parsing .dt files");
    LoadActionDatabaseFromFiles();
    actionEntriesLoaded = True;
    ActionIconCacheLog("Loaded action DB entries=%d (file scan)", actionEntryCount);
}

static void
FreeActionEntries(void)
{
    for (int i = 0; i < actionEntryCount; ++i)
    {
        free(actionEntries[i].command);
        free(actionEntries[i].icon);
    }
    free(actionEntries);
    actionEntries = NULL;
    actionEntryCount = 0;
    actionEntryCapacity = 0;
}

static Boolean
LoadActionDatabaseFromInMemoryDb(void)
{
    DtDtsDbDatabase *db = _DtDtsDbGet(_DtACTION_NAME);
    if (!db || db->recordCount <= 0)
    {
        ActionIconCacheLog("In-memory action DB not ready: db=%s recordCount=%d",
                           db ? "yes" : "no",
                           db ? db->recordCount : -1);
        return False;
    }

    ActionIconCacheLog("Loading in-memory action DB recordCount=%d", db->recordCount);

    FreeActionEntries();

    actionEntries = (ActionIconEntry *)calloc((size_t)db->recordCount, sizeof(ActionIconEntry));
    if (!actionEntries)
    {
        ActionIconCacheLog("Failed to allocate action entry list count=%d", db->recordCount);
        return False;
    }
    actionEntryCapacity = db->recordCount;

    for (int i = 0; i < db->recordCount; ++i)
    {
        DtDtsDbRecord *rec = db->recordList[i];
        if (!rec)
        {
            ActionIconCacheLog("Skipping NULL action record index=%d", i);
            continue;
        }

        DtDtsDbField *typeField = _DtDtsDbGetField(rec, _DtACTION_TYPE);
        if (!typeField)
        {
            ActionIconCacheLog("Skipping action record index=%d because TYPE field is missing", i);
            continue;
        }
        if (strcmp(typeField->fieldValue, _DtACTION_COMMAND) != 0)
            continue;

        DtDtsDbField *execField = _DtDtsDbGetField(rec, _DtACTION_EXEC_STRING);
        DtDtsDbField *iconField = _DtDtsDbGetField(rec, _DtACTION_ICON);
        DtDtsDbField *instanceIconField = _DtDtsDbGetField(rec, _DtACTION_INSTANCE_ICON);

        const char *iconValue = NULL;
        if (iconField && iconField->fieldValue && iconField->fieldValue[0])
            iconValue = iconField->fieldValue;
        else if (instanceIconField && instanceIconField->fieldValue && instanceIconField->fieldValue[0])
            iconValue = instanceIconField->fieldValue;

        if (!execField || !execField->fieldValue || !iconValue)
            continue;

        char *command = DeriveActionCommand(execField->fieldValue);
        if (!command)
            continue;

        AddOrUpdateActionEntry(command, iconValue, "in-memory");
        free(command);
    }

    return actionEntryCount > 0;
}

static Boolean
LoadActionDatabaseFromMmDb(void)
{
    DtDtsMMDatabase *db = _DtDtsMMGet(_DtACTION_NAME);
    if (!db || db->recordCount <= 0)
    {
        ActionIconCacheLog("MM action DB not ready: db=%s recordCount=%d",
                           db ? "yes" : "no",
                           db ? db->recordCount : -1);
        return False;
    }

    ActionIconCacheLog("Loading MM action DB recordCount=%d", db->recordCount);

    FreeActionEntries();

    actionEntryCapacity = db->recordCount;
    actionEntries = (ActionIconEntry *)calloc((size_t)actionEntryCapacity, sizeof(ActionIconEntry));
    if (!actionEntries)
    {
        ActionIconCacheLog("Failed to allocate MM action entry list count=%d", db->recordCount);
        actionEntryCapacity = 0;
        return False;
    }

    DtDtsMMRecord *recordList = (DtDtsMMRecord *)_DtDtsMMGetPtr(db->recordList);
    for (int i = 0; i < db->recordCount; ++i)
    {
        DtDtsMMRecord *rec = &recordList[i];

        const char *typeValue = _DtDtsMMGetFieldByName(rec, _DtACTION_TYPE);
        if (!typeValue || strcmp(typeValue, _DtACTION_COMMAND) != 0)
            continue;

        const char *execValue = _DtDtsMMGetFieldByName(rec, _DtACTION_EXEC_STRING);
        if (!execValue || !execValue[0])
            continue;

        const char *iconValue = _DtDtsMMGetFieldByName(rec, _DtACTION_ICON);
        if (!iconValue || !iconValue[0])
            iconValue = _DtDtsMMGetFieldByName(rec, _DtACTION_INSTANCE_ICON);

        if (!iconValue || !iconValue[0])
            continue;

        char *command = DeriveActionCommand(execValue);
        if (!command)
            continue;

        AddOrUpdateActionEntry(command, iconValue, "mmdb");
        free(command);
    }

    return actionEntryCount > 0;
}

static void
AddOrUpdateActionEntry(const char *command, const char *icon, const char *source)
{
    if (!command || !icon)
        return;

    for (int i = 0; i < actionEntryCount; ++i)
    {
        if (strcmp(actionEntries[i].command, command) == 0)
        {
            if (strcmp(actionEntries[i].icon, icon) != 0)
            {
                free(actionEntries[i].icon);
                actionEntries[i].icon = strdup(icon);
                ActionIconCacheLog("Updated action entry command=%s icon=%s source=%s",
                                   command, icon, source ? source : "(unknown)");
            }
            return;
        }
    }

    if (actionEntryCount >= actionEntryCapacity)
    {
        int newCapacity = actionEntryCapacity ? actionEntryCapacity * 2 : 64;
        ActionIconEntry *newEntries =
            (ActionIconEntry *)realloc(actionEntries,
                                       (size_t)newCapacity * sizeof(ActionIconEntry));
        if (!newEntries)
        {
            ActionIconCacheLog("Failed to grow action entry list capacity=%d", newCapacity);
            return;
        }
        actionEntries = newEntries;
        actionEntryCapacity = newCapacity;
    }

    actionEntries[actionEntryCount].command = strdup(command);
    actionEntries[actionEntryCount].icon = strdup(icon);
    if (!actionEntries[actionEntryCount].command || !actionEntries[actionEntryCount].icon)
    {
        free(actionEntries[actionEntryCount].command);
        free(actionEntries[actionEntryCount].icon);
        actionEntries[actionEntryCount].command = NULL;
        actionEntries[actionEntryCount].icon = NULL;
        return;
    }

    ActionIconCacheLog("Added action entry command=%s icon=%s source=%s",
                       command, icon, source ? source : "(unknown)");
    actionEntryCount++;
}

static void
LoadActionDatabaseFromFiles(void)
{
    free(actionEntries);
    actionEntries = NULL;
    actionEntryCount = 0;
    actionEntryCapacity = 0;

    const char *home = GetHomeDir();
    const char *lang = getenv("LANG");

    char path[PATH_MAX];

    if (home && home[0])
    {
        snprintf(path, sizeof(path), "%s/.dt/types", home);
        LoadActionDatabaseFromDir(path);

        if (lang && lang[0])
        {
            snprintf(path, sizeof(path), "%s/.dt/types/%s", home, lang);
            LoadActionDatabaseFromDir(path);
        }
    }

    if (lang && lang[0])
    {
        snprintf(path, sizeof(path), "/etc/dt/appconfig/types/%s", lang);
        LoadActionDatabaseFromDir(path);

        snprintf(path, sizeof(path), "/usr/dt/appconfig/types/%s", lang);
        LoadActionDatabaseFromDir(path);

        snprintf(path, sizeof(path), "/usr/local/CDE/appconfig/types/%s", lang);
        LoadActionDatabaseFromDir(path);
    }

    LoadActionDatabaseFromDir("/etc/dt/appconfig/types/C");
    LoadActionDatabaseFromDir("/usr/dt/appconfig/types/C");
    LoadActionDatabaseFromDir("/usr/local/CDE/appconfig/types/C");
}

static void
LoadActionDatabaseFromDir(const char *dirPath)
{
    if (!dirPath || dirPath[0] == '\0')
        return;

    DIR *dir = opendir(dirPath);
    if (!dir)
        return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL)
    {
        const char *name = ent->d_name;
        size_t len = strlen(name);
        if (len < 4)
            continue;
        if (strcmp(name + len - 3, ".dt") != 0)
            continue;

        char filePath[PATH_MAX];
        int n = snprintf(filePath, sizeof(filePath), "%s/%s", dirPath, name);
        if (n < 0 || n >= (int)sizeof(filePath))
            continue;

        struct stat st;
        if (stat(filePath, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        ParseActionFile(filePath);
    }

    closedir(dir);
}

static void
ParseActionFile(const char *filePath)
{
    if (!filePath)
        return;

    FILE *fp = fopen(filePath, "r");
    if (!fp)
        return;

    char line[4096];
    char actionName[256];
    actionName[0] = '\0';
    int braceDepth = 0;
    Boolean inAction = False;
    char typeValue[64];
    typeValue[0] = '\0';
    char execValue[2048];
    execValue[0] = '\0';
    char iconValue[256];
    iconValue[0] = '\0';
    char instanceIconValue[256];
    instanceIconValue[0] = '\0';

    while (fgets(line, sizeof(line), fp))
    {
        char *cur = line;
        while (*cur && isspace((unsigned char)*cur))
            ++cur;

        if (*cur == '\0' || *cur == '#')
            continue;

        if (!inAction)
        {
            if (strncmp(cur, "ACTION", 6) == 0 && isspace((unsigned char)cur[6]))
            {
                cur += 6;
                while (*cur && isspace((unsigned char)*cur))
                    ++cur;

                size_t nameLen = 0;
                while (cur[nameLen] && !isspace((unsigned char)cur[nameLen]) && cur[nameLen] != '{' &&
                       nameLen + 1 < sizeof(actionName))
                {
                    ++nameLen;
                }
                memcpy(actionName, cur, nameLen);
                actionName[nameLen] = '\0';

                inAction = True;
                braceDepth = 0;
                typeValue[0] = '\0';
                execValue[0] = '\0';
                iconValue[0] = '\0';
                instanceIconValue[0] = '\0';
            }
        }

        if (!inAction)
            continue;

        for (char *p = cur; *p; ++p)
        {
            if (*p == '{')
                braceDepth++;
            else if (*p == '}')
                braceDepth--;
        }

        while (*cur && isspace((unsigned char)*cur))
            ++cur;

        if (strncmp(cur, "TYPE", 4) == 0 && isspace((unsigned char)cur[4]))
        {
            cur += 4;
            while (*cur && isspace((unsigned char)*cur))
                ++cur;
            sscanf(cur, "%63s", typeValue);
        }
        else if (strncmp(cur, "EXEC_STRING", 11) == 0 && isspace((unsigned char)cur[11]))
        {
            cur += 11;
            while (*cur && isspace((unsigned char)*cur))
                ++cur;
            snprintf(execValue, sizeof(execValue), "%s", cur);
            char *nl = strpbrk(execValue, "\r\n");
            if (nl)
                *nl = '\0';
        }
        else if (strncmp(cur, "ICON", 4) == 0 && isspace((unsigned char)cur[4]))
        {
            cur += 4;
            while (*cur && isspace((unsigned char)*cur))
                ++cur;
            sscanf(cur, "%255s", iconValue);
        }
        else if (strncmp(cur, "INSTANCE_ICON", 13) == 0 &&
                 isspace((unsigned char)cur[13]))
        {
            cur += 13;
            while (*cur && isspace((unsigned char)*cur))
                ++cur;
            sscanf(cur, "%255s", instanceIconValue);
        }

        if (braceDepth <= 0 && strchr(cur, '}') != NULL)
        {
            const char *icon = iconValue[0] ? iconValue : (instanceIconValue[0] ? instanceIconValue : NULL);
            if (execValue[0] && icon && icon[0])
            {
                if (typeValue[0] == '\0' || strcmp(typeValue, _DtACTION_COMMAND) == 0)
                {
                    char *command = DeriveActionCommand(execValue);
                    if (command)
                    {
                        AddOrUpdateActionEntry(command, icon, filePath);
                        free(command);
                    }
                }
            }

            inAction = False;
            braceDepth = 0;
            actionName[0] = '\0';
        }
    }

    fclose(fp);
}

static void
FreeCacheEntry(ActionCacheEntry *entry)
{
    if (!entry)
        return;
    free(entry->command);
    free(entry->action);
    entry->command = NULL;
    entry->action = NULL;
}

static Boolean
AddCacheEntryInternal(const char *command, const char *action, Boolean markDirty)
{
    if (!command || !action)
        return False;

    for (int i = 0; i < cacheEntryCount; ++i)
    {
        if (strcmp(cacheEntries[i].command, command) == 0)
        {
            if (strcmp(cacheEntries[i].action, action) == 0)
                return False;

            free(cacheEntries[i].action);
            cacheEntries[i].action = strdup(action);
            if (markDirty)
                cacheDirty = True;
            return True;
        }
    }

    if (cacheEntryCount >= cacheEntryCapacity)
    {
        int newCapacity = cacheEntryCapacity ? cacheEntryCapacity * 2 : 8;
        if (newCapacity < cacheEntryCount + 1)
            newCapacity = cacheEntryCount + 1;

        ActionCacheEntry *newEntries = (ActionCacheEntry *)realloc(cacheEntries,
                                                                  newCapacity * sizeof(ActionCacheEntry));
        if (!newEntries)
            return False;

        cacheEntries = newEntries;
        cacheEntryCapacity = newCapacity;
    }

    cacheEntries[cacheEntryCount].command = strdup(command);
    cacheEntries[cacheEntryCount].action = strdup(action);
    if (!cacheEntries[cacheEntryCount].command || !cacheEntries[cacheEntryCount].action)
    {
        FreeCacheEntry(&cacheEntries[cacheEntryCount]);
        return False;
    }

    cacheEntryCount++;
    if (markDirty)
        cacheDirty = True;

    if (cacheEntryCount > ACTION_ICON_CACHE_MAX_ENTRIES)
    {
        FreeCacheEntry(&cacheEntries[0]);
        memmove(cacheEntries, cacheEntries + 1,
                (size_t)(cacheEntryCount - 1) * sizeof(ActionCacheEntry));
        cacheEntryCount--;
    }

    return True;
}

static void
LoadPersistentCache(void)
{
    if (cacheLoaded)
        return;

    cacheLoaded = True;

    const char *path = GetCacheFilePath();
    if (!path)
        return;

    FILE *fp = fopen(path, "r");
    if (!fp)
        return;

    char line[PATH_MAX * 2];
    while (fgets(line, sizeof(line), fp))
    {
        char *newline = strchr(line, '\n');
        if (newline)
            *newline = '\0';

        char *tab = strchr(line, '\t');
        if (!tab)
            continue;

        *tab = '\0';
        char *command = line;
        char *action = tab + 1;
        if (*command && *action)
        {
            if (AddCacheEntryInternal(command, action, False))
                ActionIconCacheLog("Loaded cache entry child=%s action=%s",
                                   command, action);
        }
    }

    fclose(fp);
}

static void
SaveCacheFile(void)
{
    if (!cacheDirty || cacheEntryCount == 0)
        return;

    if (!EnsureCacheDir())
        return;

    const char *path = GetCacheFilePath();
    if (!path)
        return;

    char tmpPath[PATH_MAX];
    int len = snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
    if (len < 0 || len >= (int)sizeof(tmpPath))
        return;

    FILE *fp = fopen(tmpPath, "w");
    if (!fp)
        return;

    ActionIconCacheLog("Saving cache entries=%d temp=%s", cacheEntryCount, tmpPath);

    for (int i = 0; i < cacheEntryCount; ++i)
    {
        if (fprintf(fp, "%s\t%s\n", cacheEntries[i].command,
                    cacheEntries[i].action) < 0)
        {
            fclose(fp);
            remove(tmpPath);
            return;
        }
    }

    fclose(fp);

    if (rename(tmpPath, path) != 0)
    {
        remove(tmpPath);
        ActionIconCacheLog("Failed to rename cache tmp=%s", tmpPath);
        return;
    }

    ActionIconCacheLog("Saved cache entries=%d path=%s", cacheEntryCount, path);

    chmod(path, S_IRUSR | S_IWUSR);
    cacheDirty = False;
}

const char *
ActionIconFind(const char *command)
{
    if (!command)
        return NULL;

    if (!actionEntriesLoaded || actionEntryCount == 0)
        LoadActionDatabase();

    if (!cacheLoaded)
        LoadPersistentCache();

    const char *cachedAction = FindCachedAction(command);
    if (cachedAction)
    {
        const char *icon = FindInEntries(actionEntries, actionEntryCount, cachedAction);
        if (icon)
            return icon;
        ActionIconCacheLog("Cached action %s for command=%s has no icon",
                           cachedAction, command);
    }

    return FindInEntries(actionEntries, actionEntryCount, command);
}

Boolean
ActionIconCacheUpdate(const char *actionCommand, const char *childCommandList)
{
    if (!actionCommand || !childCommandList)
        return False;

    if (!actionEntriesLoaded || actionEntryCount == 0)
        LoadActionDatabase();

    if (!cacheLoaded)
        LoadPersistentCache();

    char *actionKeyBuf = DeriveActionCommand(actionCommand);
    const char *actionKey = actionKeyBuf ? actionKeyBuf : actionCommand;

    char snippet[128];
    int childCount = 0;
    DescribeChildList(childCommandList, snippet, sizeof(snippet), &childCount);
    ActionIconCacheLog("Update request action=%s key=%s children=%d first=%s",
                       actionCommand,
                       actionKey,
                       childCount,
                       snippet[0] ? snippet : "(none)");

    StringList children;
    ParseCommandList(childCommandList, &children);

    const char *icon = ActionIconFind(actionKey);
    if (!icon)
    {
        for (int i = 0; i < children.count && !icon; ++i)
        {
            char *childKeyBuf = DeriveActionCommand(children.items[i]);
            const char *childKey = childKeyBuf ? childKeyBuf : children.items[i];
            icon = ActionIconFind(childKey);
            free(childKeyBuf);
        }
    }

    if (!icon)
    {
        ActionIconCacheLog("No icon found for action=%s, caching child mappings anyway",
                           actionKey);
    }
    else
    {
        ActionIconCacheLog("Resolved icon=%s for action=%s", icon,
                           actionKey);
    }

    Boolean updated = False;
    for (int i = 0; i < children.count; ++i)
    {
        char *childKeyBuf = DeriveActionCommand(children.items[i]);
        const char *childKey = childKeyBuf ? childKeyBuf : children.items[i];

        if (AddCacheEntryInternal(childKey, actionKey, False))
        {
            updated = True;
            ActionIconCacheLog("Cached child=%s -> action=%s icon=%s",
                               childKey, actionKey,
                               icon ? icon : "(none)");
        }
        else
        {
            ActionIconCacheLog("Cache already contains child=%s -> action=%s",
                               childKey, actionKey);
        }

        free(childKeyBuf);
    }

    StringListFree(&children);
    free(actionKeyBuf);

    if (updated)
    {
        cacheDirty = True;
        SaveCacheFile();
    }

    return updated;
}
