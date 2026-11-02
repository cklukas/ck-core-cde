/* Length-delimited Trash metadata, independent of the desktop UI. */
#ifndef DTFILE_TRASH_METADATA_H
#define DTFILE_TRASH_METADATA_H

#include <stdio.h>

typedef struct {
    char *external;
    char *filename;
    char *deletion_date;
    unsigned long long rec_size;
    int rec_size_valid;
} DtfileTrashMetadata;

enum {
    DTFILE_TRASH_RECORD = 1,
    DTFILE_TRASH_END = 0,
    DTFILE_TRASH_INVALID = -1,
    DTFILE_TRASH_IO_ERROR = -2,
    DTFILE_TRASH_NO_MEMORY = -3
};

/* Read one complete record. Successful strings belong to the result and must
 * be released with DtfileFreeTrashMetadata. On EOF/error the result is empty.
 * Stop on an error: a damaged record has no reliable resynchronization point.
 */
int DtfileReadTrashMetadata(FILE *stream, DtfileTrashMetadata *result);
void DtfileFreeTrashMetadata(DtfileTrashMetadata *record);

#endif
