/* Freedesktop Trash metadata, independent of Xt and filesystem mutation. */
#ifndef DTFILE_XDG_TRASH_INFO_H
#define DTFILE_XDG_TRASH_INFO_H

#include <stdio.h>

#define DTFILE_XDG_INFO_LINE_LIMIT (64u * 1024u)
#define DTFILE_XDG_INFO_FILE_LIMIT (1024u * 1024u)

typedef struct {
    char *path;
    char *deletion_date;
} DtfileXdgTrashInfo;

enum {
    DTFILE_XDG_INFO_OK = 1,
    DTFILE_XDG_INFO_INVALID = -1,
    DTFILE_XDG_INFO_IO_ERROR = -2,
    DTFILE_XDG_INFO_NO_MEMORY = -3,
    DTFILE_XDG_INFO_TOO_LARGE = -4
};

/* Read the complete stream; the caller owns and closes it, and must check
 * close before accepting the result. Relative paths are resolved against the
 * absolute parent of the Trash directory, never the process working directory.
 * An absolute Path does not need original_base. Relative .. components are
 * invalid. Result strings use malloc/free; errors leave an empty result.
 * The result must be empty/uninitialized, not an existing owned result.
 * Lines and resolved paths are limited to 64 KiB, the complete file to 1 MiB.
 */
int DtfileReadXdgTrashInfo(FILE *stream, const char *original_base,
                         DtfileXdgTrashInfo *result);
void DtfileFreeXdgTrashInfo(DtfileXdgTrashInfo *result);

#endif
