/*
 * Fuzz target: libDtHelp SGML/CCDF help-volume markup & tag parser
 * (DtHelp/CCDFUtil.c over DtHelp/bufio.c, FormatUtil.c, StringFuncs.c, CvString.c).
 *
 * Input layout:
 *   byte 0: bit 0 selects multibyte UTF-8 vs 1-byte locale; bits 1..3 select
 *           command/parameter flags.
 *   byte 1: count N (1..12) of parser operations to cycle through.
 *   bytes 2..2+N-1: operation selectors:
 *     0 _DtHelpCeGetCcdfCmd
 *     1 _DtHelpCeGetCcdfStrParam
 *     2 _DtHelpCeGetCcdfValueParam
 *     3 _DtHelpCeGetCcdfTopicCmd
 *     4 _DtHelpCeGetCcdfTopicAbbrev
 *     5 _DtHelpCeGetCcdfAbbrevCmd
 *     6 _DtHelpCeCheckNextCcdfCmd
 *     7 _DtHelpCeGetCcdfEndMark
 *     8 _DtHelpCeSkipToNextCcdfToken
 *     9 _DtHelpCeSkipCcdfAbbrev
 */
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include FUZZ_TYPES_H
#include FUZZ_PRODUCTION_H

struct source {
    const uint8_t *data;
    size_t size;
    size_t pos;
};

static int mem_read(BufFilePtr f)
{
    struct source *s = (struct source *)f->hidden;
    size_t left = s->size - s->pos;
    if (left == 0) {
        f->left = 0;
        return BUFFILEEOF;
    }
    size_t n = left < BUFFILESIZE ? left : BUFFILESIZE;
    memcpy(f->buffer, s->data + s->pos, n);
    s->pos += n;
    f->left = (int)n - 1;
    f->bufp = f->buffer + 1;
    return (unsigned char)f->buffer[0];
}

static int mem_close(BufFilePtr f, int do_close)
{
    (void)f;
    (void)do_close;
    return 1;
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc;
    (void)argv;
    if (!setlocale(LC_ALL, "C.UTF-8") && !setlocale(LC_ALL, "en_US.UTF-8"))
        setlocale(LC_ALL, "");
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 4)
        return 0;
    uint8_t flags = data[0];
    uint8_t nops = (data[1] % 12) + 1;
    if (size < 2u + nops)
        return 0;

    const uint8_t *ops = data + 2;
    struct source src = { data + 2 + nops, size - 2 - nops, 0 };
    BufFilePtr f = _DtHelpCeBufFileCreateRead((char *)&src, mem_read, NULL, mem_close);
    if (!f)
        return 0;

    int max_mb = (flags & 1) ? MB_CUR_MAX : 1;
    char in_buf[256];
    in_buf[0] = '\0';
    char *in_ptr = in_buf;
    int allowed = CCDF_ALL_CMDS;

    for (int step = 0; step < 128; step++) {
        uint8_t op = ops[step % nops] % 10;
        char *ret_str = NULL;
        char *ret_title = NULL;
        char *ret_cs = NULL;
        char *ret_abbrev = NULL;
        int ret_val = 0;

        switch (op) {
        case 0:
            (void)_DtHelpCeGetCcdfCmd(CCDF_PARAGRAPH_CMD, in_buf, &in_ptr, f,
                                      (int)sizeof(in_buf), allowed);
            break;
        case 1:
            (void)_DtHelpCeGetCcdfStrParam(f, in_buf, (int)sizeof(in_buf), max_mb,
                                           &in_ptr, (flags >> 1) & 1, (flags >> 2) & 1,
                                           (flags >> 3) & 1, (flags >> 4) & 1, &ret_str);
            free(ret_str);
            break;
        case 2:
            (void)_DtHelpCeGetCcdfValueParam(f, in_buf, (int)sizeof(in_buf),
                                             &in_ptr, (flags >> 1) & 1, max_mb, &ret_val);
            break;
        case 3:
            (void)_DtHelpCeGetCcdfTopicCmd(NULL, f, in_buf, &in_ptr,
                                           (int)sizeof(in_buf), max_mb, &ret_cs);
            free(ret_cs);
            break;
        case 4:
            (void)_DtHelpCeGetCcdfTopicAbbrev(NULL, f, in_buf, &in_ptr,
                                              (int)sizeof(in_buf), max_mb,
                                              &ret_title, &ret_cs, &ret_abbrev);
            free(ret_title);
            free(ret_cs);
            free(ret_abbrev);
            break;
        case 5:
            (void)_DtHelpCeGetCcdfAbbrevCmd(f, in_buf, (int)sizeof(in_buf),
                                            max_mb, &in_ptr, &ret_abbrev);
            free(ret_abbrev);
            break;
        case 6:
            (void)_DtHelpCeCheckNextCcdfCmd("pa", f, in_buf, (int)sizeof(in_buf),
                                            max_mb, &in_ptr);
            break;
        case 7:
            (void)_DtHelpCeGetCcdfEndMark(f, in_buf, &in_ptr, (int)sizeof(in_buf), max_mb);
            break;
        case 8:
            (void)_DtHelpCeSkipToNextCcdfToken(f, in_buf, (int)sizeof(in_buf),
                                               max_mb, &in_ptr, (flags >> 2) & 1);
            break;
        default:
            (void)_DtHelpCeSkipCcdfAbbrev(f, in_buf, &in_ptr, (int)sizeof(in_buf), max_mb);
            break;
        }

        if (src.pos >= src.size && f->left <= 0 && (!in_ptr || *in_ptr == '\0'))
            break;
    }

    (void)_DtHelpCeGetCcdfFontType("angle");
    free(f);
    return 0;
}
