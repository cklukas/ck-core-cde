/* Behavioral coverage for the production, UI-independent Trash reader. */
#include "TrashMetadata.h"
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static FILE *Input(const char *bytes, size_t length)
{
    const char *root = getenv("TMPDIR");
    assert(root && *root);
    char *path = malloc(strlen(root) + sizeof("/trash-metadata-XXXXXX"));
    assert(path);
    sprintf(path, "%s/trash-metadata-XXXXXX", root);
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *stream = fdopen(fd, "w+");
    assert(stream);
    assert(unlink(path) == 0);
    free(path);
    assert(fwrite(bytes, 1, length, stream) == length);
    rewind(stream);
    return stream;
}

static void Invalid(const char *bytes, size_t length)
{
    FILE *stream = Input(bytes, length);
    DtfileTrashMetadata result;
    assert(DtfileReadTrashMetadata(stream, &result) == DTFILE_TRASH_INVALID);
    assert(!result.external && !result.filename && !result.deletion_date);
    DtfileFreeTrashMetadata(&result);
    fclose(stream);
}

int main(void)
{
    const char *invalid[] = {
        "-1 3 /a one\n", "0 3 /a one\n", "2 -3 /a one\n",
        "184467440737095516160 3 /a one\n", "999999999 3 /a one\n",
        "2 3 /a on", "2 3 /a_one\n", "2 3 /a oneX\n",
        "2 2 /a ..\n", "2 1 /a .\n", "2 3 /a a/b\n",
        "2 3 /a one RecSize=-1\n", "2 3 /a one RecSize=12x\n",
        "2 3 /a one RecSize=18446744073709551616\n",
        "2 3 /a one RecSize=1 RecSize=2\n",
        "2 3 /a one DeletionDate=\n",
        "2 3 /a one DeletionDate=a DeletionDate=b\n",
        "2", "2 ", "2 3 ", "2 3 /a"
    };
    for (size_t i=0; i<sizeof(invalid)/sizeof(*invalid); ++i)
        Invalid(invalid[i], strlen(invalid[i]));
    const char nul_name[] = "2 3 /a o\0e\n";
    Invalid(nul_name, sizeof(nul_name)-1);
    const char nul_attribute[] = "2 3 /a one RecSize=1\0junk\n";
    Invalid(nul_attribute, sizeof(nul_attribute)-1);

    const char valid[] =
        "2 3 /a one DeletionDate=2026-11-02T17:00:00 RecSize=18446744073709551615\n"
        "4 3 /a\nb x\ny\n"
        "2 3 /c two RecSize=0\n"
        "2 3 /d end";
    FILE *stream = Input(valid, sizeof(valid)-1);
    DtfileTrashMetadata record;
    assert(DtfileReadTrashMetadata(stream, &record) == DTFILE_TRASH_RECORD);
    assert(strcmp(record.external,"/a")==0 && strcmp(record.filename,"one")==0);
    assert(strcmp(record.deletion_date,"2026-11-02T17:00:00")==0);
    assert(record.rec_size_valid && record.rec_size==ULLONG_MAX);
    DtfileFreeTrashMetadata(&record);
    assert(DtfileReadTrashMetadata(stream, &record) == DTFILE_TRASH_RECORD);
    assert(strcmp(record.external,"/a\nb")==0 && strcmp(record.filename,"x\ny")==0);
    DtfileFreeTrashMetadata(&record);
    assert(DtfileReadTrashMetadata(stream, &record) == DTFILE_TRASH_RECORD);
    assert(record.rec_size_valid && record.rec_size==0 && !record.deletion_date);
    DtfileFreeTrashMetadata(&record);
    assert(DtfileReadTrashMetadata(stream, &record) == DTFILE_TRASH_RECORD);
    assert(strcmp(record.filename,"end")==0 && !record.rec_size_valid);
    DtfileFreeTrashMetadata(&record);
    assert(DtfileReadTrashMetadata(stream, &record)==DTFILE_TRASH_END);
    fclose(stream);
    puts("Trash metadata: 23 malformed and four valid records passed");
    return 0;
}
