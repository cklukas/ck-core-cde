/*
 * Fuzz target: the SDL token layer of DtHelp (help volume .sdl files),
 * DtHelp/UtilSDL.c, reading through the buffered reader of DtHelp/bufio.c.
 *
 * run-fuzz.py includes UtilSDL.c and bufio.c whole, and cuts
 * _DtHelpCeAddCharToBuf and _DtHelpCeAddStrToBuf (FormatUtil.c) and
 * _DtHelpCeStrNCaseCmpLatin1 (StringFuncs.c) out of their files verbatim.
 * The stand-ins are the functions that only the segment-building half of
 * UtilSDL.c calls (_DtHelpAllocateSegments, _DtHelpFmtFindBreak,
 * _DtHelpCeGetSdlAttributeList), which abort when reached, the element table
 * (a short list in the format of the table of FormatSDL.c), and a read
 * procedure that hands the input to BufFile like FdRawRead does for a file.
 *
 * Input layout: byte 0 bit 0: multibyte locale (UTF-8, max_mb 4);
 * bit 1: cdata_flag of _DtHelpCeReturnSdlElement; bits 2-3: the SdlOption type
 * of _DtHelpCeGetSdlCdata (literal, lined, dynamic, cdata); bit 4: nl_to_space;
 * bit 5: skip the data (string pointer NULL).  Byte 1 is the number N of
 * operation bytes that follow; the rest is the SDL text.  The operations are
 * run in turn, cycling, until the text ends or 256 calls were made:
 *   0 ReturnSdlElement   1 GetSdlAttribute (30 chars, 32 byte buffer)
 *   2 GetSdlCdata (the caller's loop: the string grows, is emptied after each
 *     result)   3 GetSdlAttributeCdata (unlimited)   4 GetSdlNumber
 *   5 GetSdlId   6 SkipCdata   7 FindSkipSdlElementEnd   8 MatchSdlElement
 *   9 GetSdlAttributeCdata (limited to DTD_NAMELEN), into a 65 byte buffer
 *
 * Environment: FUZZ_SKIP_OPS is a bit mask of operations (bit N = operation
 * N) that the harness does not call, so that a run can go on past an
 * operation whose bug was already reported.
 */
#include <assert.h>
#include <limits.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include FUZZ_TYPES_H
#include FUZZ_PRODUCTION_H

_DtCvSegment *_DtHelpAllocateSegments(int n) { (void)n; abort(); }
int _DtHelpFmtFindBreak(char *p, int mb, int *n) { (void)p; (void)mb; (void)n; abort(); }
const SDLAttribute *_DtHelpCeGetSdlAttributeList(void) { abort(); }

static const SDLElementInfo elements[] = {
    { "<alttext", "</alttext", 3, 3, SdlElementAltText },
    { "<anchor", nullptr, 4, 0, SdlElementAnchor },
    { "<block", "</block", 2, 3, SdlElementBlock },
    { "<cond", "</cond", 3, 4, SdlElementCond },
    { "<entry", "</entry", 3, 4, SdlElementEntry },
    { nullptr, nullptr, 0, 0, SdlElementNone },
};

struct source { const uint8_t *data; size_t size, pos; };

static int mem_read(BufFilePtr f)
{
    struct source *s = (struct source *)f->hidden;
    size_t left = s->size - s->pos;
    if (left == 0) { f->left = 0; return BUFFILEEOF; }
    size_t n = left < BUFFILESIZE ? left : BUFFILESIZE;
    memcpy(f->buffer, s->data + s->pos, n);
    s->pos += n;
    f->left = (int)n - 1;
    f->bufp = f->buffer + 1;
    return f->buffer[0];
}
static int mem_close(BufFilePtr f, int do_close) { (void)f; (void)do_close; return 1; }

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    if (!setlocale(LC_ALL, "C.UTF-8") && !setlocale(LC_ALL, "en_US.UTF-8"))
        setlocale(LC_ALL, "");
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 3) return 0;
    uint8_t flags = data[0], nops = data[1] % 12;
    if (nops == 0 || size < 2u + nops) return 0;
    const uint8_t *ops = data + 2;
    struct source src = { data + 2 + nops, size - 2 - nops, 0 };
    BufFilePtr f = _DtHelpCeBufFileCreateRead((char *)&src, mem_read, NULL, mem_close);
    if (!f) return 0;

    int max_mb = (flags & 1) ? MB_CUR_MAX : 1;
    _DtCvValue cdata_flag = (flags >> 1) & 1, nl_to_space = (flags >> 4) & 1;
    static const SdlOption types[4] = { SdlTypeLiteral, SdlTypeLined, SdlTypeDynamic, SdlTypeCdata };
    SdlOption type = types[(flags >> 2) & 3];
    _DtCvValue skip = (flags >> 5) & 1;
    _DtCvValue sp = 0, nl = 0, mb = 0;
    char *string = NULL;
    int str_max = 0;

    static unsigned long skip_ops = (unsigned long)-1;
    if (skip_ops == (unsigned long)-1) {
        const char *e = getenv("FUZZ_SKIP_OPS");
        skip_ops = e ? strtoul(e, NULL, 0) : 0;
    }

    for (int calls = 0; calls < 256; calls++) {
        if ((skip_ops >> (ops[calls % nops] % 10)) & 1)
            continue;
        char name[32], number[65], *value = NULL, nbc;
        enum SdlElement el; _DtCvValue end; char *d = NULL;
        int r;
        switch (ops[calls % nops] % 10) {
        case 0: r = _DtHelpCeReturnSdlElement(f, elements, cdata_flag, &el, &d, &end); free(d); break;
        case 1: r = _DtHelpCeGetSdlAttribute(f, 30, name); break;
        case 2:
            r = _DtHelpCeGetSdlCdata(f, type, max_mb, nl_to_space, &sp, &nl, &mb, &nbc,
                                     skip ? NULL : &string, &str_max);
            if (string) string[0] = '\0';
            break;
        case 3: r = _DtHelpCeGetSdlAttributeCdata(f, False, &value); free(value); break;
        case 4: r = _DtHelpCeGetSdlNumber(f, number); break;
        case 5: r = _DtHelpCeGetSdlId(f, &value); free(value); break;
        case 6: r = _DtHelpCeSkipCdata(f, calls & 1); break;
        case 7: r = _DtHelpCeFindSkipSdlElementEnd(f); break;
        case 8: r = _DtHelpCeMatchSdlElement(f, "block", 3); break;
        default:
            value = number; number[0] = 0;
            r = _DtHelpCeGetSdlAttributeCdata(f, True, &value);
            break;
        }
        if (r < 0 && (ops[calls % nops] % 10) != 2) {
            /* the real callers stop at an error; the harness reads on only after a result >= 0 */
        }
        if (src.pos >= src.size && f->left <= 0) break;
    }
    free(string);
    free(f);
    return 0;
}
