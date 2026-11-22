/*
 * Verify _DtHelpXmFontListGetPropertyMax handles XmFONT_IS_FONT,
 * XmFONT_IS_FONTSET, and XmFONT_IS_XFT without unsafe XFontSet casts.
 */
/* @@PROLOGUE@@ */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char Boolean;
typedef unsigned short Dimension;
typedef unsigned long Atom;
typedef void *XtPointer;
typedef int Bool;

#define True 1
#define False 0
#define TRUE 1
#define FALSE 0

typedef enum {
    XmFONT_IS_FONT = 0,
    XmFONT_IS_FONTSET = 1,
    XmFONT_IS_XFT = 2
} XmFontType;

typedef struct {
    unsigned long ave_width;
    Bool has_prop;
} XFontStruct;

typedef struct FakeFontSet {
    uint32_t magic;
    int count;
    XFontStruct **fonts;
} *XFontSet;

typedef struct {
    uint64_t not_a_fontset_magic;
    int ascent;
    int descent;
} FakeXftFont;

typedef struct {
    XmFontType type;
    XtPointer data;
} FakeFontEntry;

typedef FakeFontEntry *XmFontListEntry;

typedef struct {
    FakeFontEntry *entries;
    size_t count;
    Dimension sample_10n_width;
} FakeFontList;

typedef FakeFontList *XmFontList;

typedef struct {
    XmFontList list;
    size_t idx;
} FakeFontContext;

typedef FakeFontContext *XmFontContext;
typedef char *XmString;

static int g_fonts_of_fontset_calls = 0;

static Boolean XmFontListInitFontContext(XmFontContext *ctx_out, XmFontList list)
{
    if (ctx_out == nullptr || list == nullptr || list->count == 0)
        return False;
    FakeFontContext *ctx = calloc(1, sizeof(*ctx));
    assert(ctx != nullptr);
    ctx->list = list;
    ctx->idx = 0;
    *ctx_out = ctx;
    return True;
}

static XmFontListEntry XmFontListNextEntry(XmFontContext ctx)
{
    if (ctx == nullptr || ctx->list == nullptr || ctx->idx >= ctx->list->count)
        return nullptr;
    return &ctx->list->entries[ctx->idx++];
}

static void XmFontListFreeFontContext(XmFontContext ctx)
{
    free(ctx);
}

static XtPointer XmFontListEntryGetFont(XmFontListEntry entry, XmFontType *type_return)
{
    assert(entry != nullptr);
    assert(type_return != nullptr);
    *type_return = entry->type;
    return entry->data;
}

static Bool XGetFontProperty(XFontStruct *fs, Atom atom, unsigned long *value_return)
{
    assert(fs != nullptr);
    assert(value_return != nullptr);
    (void)atom;
    if (!fs->has_prop)
        return FALSE;
    *value_return = fs->ave_width;
    return TRUE;
}

static int XFontsOfFontSet(XFontSet font_set, XFontStruct ***font_struct_list, char ***font_name_list)
{
    g_fonts_of_fontset_calls++;
    assert(font_set != nullptr);
    /* Catch any bogus cast of FakeXftFont* to XFontSet immediately */
    assert(font_set->magic == 0xF0975E70u);
    *font_struct_list = font_set->fonts;
    *font_name_list = nullptr;
    return font_set->count;
}

static XmString XmStringCreateLocalized(const char *text)
{
    assert(text != nullptr);
    char *dup = strdup(text);
    assert(dup != nullptr);
    return dup;
}

static Dimension XmStringWidth(XmFontList list, XmString str)
{
    assert(list != nullptr);
    assert(str != nullptr);
    assert(strcmp(str, "nnnnnnnnnn") == 0);
    return list->sample_10n_width;
}

static void XmStringFree(XmString str)
{
    free(str);
}

/* @@EXTRACTED@@ */

int main(void)
{
    unsigned long out = 73;

    /* 1. Null safety */
    assert(_DtHelpXmFontListGetPropertyMax(nullptr, 105, &out) == False);
    assert(out == 73);

    /* 2. XmFONT_IS_XFT entry must NOT call XFontsOfFontSet and must compute width via XmStringWidth */
    FakeXftFont xft = { .not_a_fontset_magic = 0x586674466f6e7400ULL, .ascent = 13, .descent = 4 };
    FakeFontEntry xft_entries[1] = {
        { .type = XmFONT_IS_XFT, .data = &xft }
    };
    FakeFontList xft_list = {
        .entries = xft_entries,
        .count = 1,
        .sample_10n_width = 84
    };
    g_fonts_of_fontset_calls = 0;
    out = 73;
    assert(_DtHelpXmFontListGetPropertyMax(&xft_list, 105, &out) == True);
    assert(g_fonts_of_fontset_calls == 0);
    assert(out == 84);

    /* 3. XmFONT_IS_FONT + XmFONT_IS_FONTSET max property resolution */
    XFontStruct f1 = { .ave_width = 70, .has_prop = TRUE };
    XFontStruct f2 = { .ave_width = 95, .has_prop = TRUE };
    XFontStruct *fs_arr[2] = { nullptr, &f2 };
    struct FakeFontSet fset = { .magic = 0xF0975E70u, .count = 2, .fonts = fs_arr };
    FakeFontEntry mixed_entries[2] = {
        { .type = XmFONT_IS_FONT, .data = &f1 },
        { .type = XmFONT_IS_FONTSET, .data = &fset }
    };
    FakeFontList mixed_list = {
        .entries = mixed_entries,
        .count = 2,
        .sample_10n_width = 80
    };
    out = 0;
    assert(_DtHelpXmFontListGetPropertyMax(&mixed_list, 105, &out) == True);
    assert(g_fonts_of_fontset_calls == 1);
    assert(out == 95);

    puts("PASS DtHelp Xft font-list property regression");
    return 0;
}
