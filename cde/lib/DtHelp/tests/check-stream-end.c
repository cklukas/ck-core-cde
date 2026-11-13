/* Harness for check-stream-end.py: the stream functions of Graphics.c must
 * return a defined failure value for a stream of unknown type, and still
 * read a buffer stream correctly. */
/* @@PROLOGUE@@ */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

enum _DtGrStreamType { _DtGrNONE, _DtGrBUFFER, _DtGrFILE };
typedef struct
{
    FILE *fileptr;
    char *filename;
    char *uncompressed_filename;
} _DtGrFile;
typedef struct
{
    const char *base;
    long       size;
    char       *current;
    char       *end;
} _DtGrBuffer;
typedef struct
{
    enum _DtGrStreamType  type;
    union
    {
	_DtGrFile    file;
	_DtGrBuffer  buffer;
    } source;
} _DtGrStream;

#define nullptr ((void *)0)
/* @@BODY@@ */
int main(void)
{
    static char data[] = "ab\ncd";
    _DtGrStream unknown;
    _DtGrStream buf;
    char out[16];

    memset(&unknown, 0, sizeof unknown);
    unknown.type = (enum _DtGrStreamType) 7;        /* neither none, buffer nor file */
    assert(_DtGrRead(out, 1, 4, &unknown) == 0);
    assert(_DtGrSeek(&unknown, 0, SEEK_SET) == -1);
    assert(_DtGrGetChar(&unknown) == EOF);
    assert(_DtGrGetString(out, sizeof out, &unknown) == NULL);

    memset(&buf, 0, sizeof buf);
    buf.type = _DtGrBUFFER;
    buf.source.buffer.base = data;
    buf.source.buffer.size = 5;
    buf.source.buffer.current = data;
    buf.source.buffer.end = data + 4;
    assert(_DtGrGetChar(&buf) == 'a');
    assert(_DtGrGetString(out, sizeof out, &buf) != NULL);
    assert(strcmp(out, "b\n") == 0);
    assert(_DtGrRead(out, 1, 2, &buf) == 2);
    assert(memcmp(out, "cd", 2) == 0);
    assert(_DtGrGetChar(&buf) == EOF);
    puts("stream end handling ok");
    return 0;
}
