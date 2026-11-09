/* Validate a complete .trashinfo record before it can authorize an import. */
#include "XdgTrashInfo.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void DtfileFreeXdgTrashInfo(DtfileXdgTrashInfo *result)
{
    if (!result) return;
    free(result->path);
    free(result->deletion_date);
    memset(result, 0, sizeof(*result));
}

/* A final complete line need not have a newline. Never parse a fragment of
 * an oversized line, or accept EOF caused by a stream error as completion.
 */
static int ReadInfoLine(FILE *stream, char *line, size_t *total)
{
    size_t length = 0;
    int ch;
    while ((ch = fgetc(stream)) != EOF) {
        if (*total == DTFILE_XDG_INFO_FILE_LIMIT)
            return DTFILE_XDG_INFO_TOO_LARGE;
        ++*total;
        if (ch == '\0')
            return DTFILE_XDG_INFO_INVALID;
        if (ch == '\n')
            break;
        if (length == DTFILE_XDG_INFO_LINE_LIMIT)
            return DTFILE_XDG_INFO_TOO_LARGE;
        line[length++] = (char)ch;
    }
    if (ferror(stream))
        return DTFILE_XDG_INFO_IO_ERROR;
    if (ch == EOF && length == 0)
        return 0;
    if (length && line[length - 1] == '\r')
        --length;
    line[length] = '\0';
    return DTFILE_XDG_INFO_OK;
}

static int HexDigit(unsigned char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static int DecodeInfoPath(const char *value, const char *base, char **result)
{
    size_t length = strlen(value), used = 0;
    char *path = malloc(length + 1);
    if (!path)
        return DTFILE_XDG_INFO_NO_MEMORY;
    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = (unsigned char)value[i];
        if (ch == '%') {
            int high, low;
            if (length - i < 3 || (high = HexDigit((unsigned char)value[i + 1])) < 0 ||
                (low = HexDigit((unsigned char)value[i + 2])) < 0) {
                free(path);
                return DTFILE_XDG_INFO_INVALID;
            }
            ch = (unsigned char)((high << 4) | low);
            i += 2;
        }
        if (ch == '\0') {
            free(path);
            return DTFILE_XDG_INFO_INVALID;
        }
        path[used++] = (char)ch;
    }
    path[used] = '\0';
    if (used == 0) {
        free(path);
        return DTFILE_XDG_INFO_INVALID;
    }
    if (path[0] == '/') {
        *result = path;
        return DTFILE_XDG_INFO_OK;
    }
    for (const char *part = path; *part;) {
        const char *end = strchr(part, '/');
        size_t count = end ? (size_t)(end - part) : strlen(part);
        if (count == 2 && part[0] == '.' && part[1] == '.') {
            free(path);
            return DTFILE_XDG_INFO_INVALID;
        }
        if (!end) break;
        part = end + 1;
    }
    if (!base || base[0] != '/') {
        free(path);
        return DTFILE_XDG_INFO_INVALID;
    }
    size_t base_length = strnlen(base, DTFILE_XDG_INFO_LINE_LIMIT + 1);
    size_t separator = base_length && base[base_length - 1] != '/';
    if (base_length > DTFILE_XDG_INFO_LINE_LIMIT ||
        base_length > SIZE_MAX - used - separator - 1 ||
        base_length + used + separator > DTFILE_XDG_INFO_LINE_LIMIT) {
        free(path);
        return DTFILE_XDG_INFO_TOO_LARGE;
    }
    char *joined = malloc(base_length + used + separator + 1);
    if (!joined) {
        free(path);
        return DTFILE_XDG_INFO_NO_MEMORY;
    }
    memcpy(joined, base, base_length);
    if (separator) joined[base_length] = '/';
    memcpy(joined + base_length + separator, path, used + 1);
    free(path);
    *result = joined;
    return DTFILE_XDG_INFO_OK;
}

static int ValidDeletionDate(const char *text)
{
    static const unsigned char days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (strlen(text) != 19 || text[4] != '-' || text[7] != '-' ||
        text[10] != 'T' || text[13] != ':' || text[16] != ':')
        return 0;
    for (size_t i = 0; i < 19; ++i) {
        if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16) continue;
        if (text[i] < '0' || text[i] > '9') return 0;
    }
    unsigned int year = (unsigned int)(text[0]-'0')*1000 +
                        (unsigned int)(text[1]-'0')*100 +
                        (unsigned int)(text[2]-'0')*10 + (unsigned int)(text[3]-'0');
    unsigned int month = (unsigned int)(text[5]-'0')*10 + (unsigned int)(text[6]-'0');
    unsigned int day = (unsigned int)(text[8]-'0')*10 + (unsigned int)(text[9]-'0');
    unsigned int hour = (unsigned int)(text[11]-'0')*10 + (unsigned int)(text[12]-'0');
    unsigned int minute = (unsigned int)(text[14]-'0')*10 + (unsigned int)(text[15]-'0');
    unsigned int second = (unsigned int)(text[17]-'0')*10 + (unsigned int)(text[18]-'0');
    if (month < 1 || month > 12 || day < 1 || hour > 23 || minute > 59 || second > 60)
        return 0;
    unsigned int maximum = days[month - 1];
    if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))
        ++maximum;
    return day <= maximum;
}

int DtfileReadXdgTrashInfo(FILE *stream, const char *original_base,
                         DtfileXdgTrashInfo *result)
{
    size_t total = 0;
    int status, path_seen = 0, date_seen = 0;
    char *line;
    if (!result) return DTFILE_XDG_INFO_INVALID;
    memset(result, 0, sizeof(*result));
    if (!stream) return DTFILE_XDG_INFO_INVALID;
    line = malloc(DTFILE_XDG_INFO_LINE_LIMIT + 1);
    if (!line)
        return DTFILE_XDG_INFO_NO_MEMORY;
    status = ReadInfoLine(stream, line, &total);
    if (status != DTFILE_XDG_INFO_OK)
        goto failed;
    if (strcmp(line, "[Trash Info]") != 0) {
        status = DTFILE_XDG_INFO_INVALID;
        goto failed;
    }
    while ((status = ReadInfoLine(stream, line, &total)) == DTFILE_XDG_INFO_OK) {
        char *equals = strchr(line, '=');
        char *key_end, *value;
        size_t key_length;
        if (!equals) continue;
        key_end = equals;
        while (key_end > line && (key_end[-1] == ' ' || key_end[-1] == '\t'))
            --key_end;
        key_length = (size_t)(key_end - line);
        value = equals + 1;
        while (*value == ' ' || *value == '\t') ++value;
        /* Desktop Entry formatting around '=' is not filename data. Percent
         * escapes are decoded afterward, preserving an encoded leading space.
         */
        if (!path_seen && key_length == 4 && memcmp(line, "Path", 4) == 0) {
            path_seen = 1;
            status = DecodeInfoPath(value, original_base, &result->path);
            if (status != DTFILE_XDG_INFO_OK) goto failed;
        } else if (!date_seen && key_length == 12 && memcmp(line, "DeletionDate", 12) == 0) {
            date_seen = 1;
            if (!ValidDeletionDate(value)) {
                status = DTFILE_XDG_INFO_INVALID;
                goto failed;
            }
            result->deletion_date = strdup(value);
            if (!result->deletion_date) {
                status = DTFILE_XDG_INFO_NO_MEMORY;
                goto failed;
            }
        }
    }
    if (status == 0 && path_seen && date_seen) {
        free(line);
        return DTFILE_XDG_INFO_OK;
    }
failed:
    free(line);
    DtfileFreeXdgTrashInfo(result);
    return status == 0 ? DTFILE_XDG_INFO_INVALID : status;
}
