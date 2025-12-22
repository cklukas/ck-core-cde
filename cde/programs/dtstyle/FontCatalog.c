#include "FontCatalog.h"

#include <Xm/Xm.h>
#include <X11/Xlib.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define FONT_CATALOG_PATTERN "-*-*-*-*-*-*-*-*-*-*-*-*-*-*"
#define FONT_CATALOG_LIMIT   2048

typedef struct {
    FontDescriptor *descriptor;
    XmString label;
} FontCatalogEntry;

typedef struct {
    char *name;
    XmString label;
    FontCatalogEntry **styles;
    int styleCount;
    int styleCapacity;
} FontCatalogFamily;

struct FontCatalog {
    FontCatalogEntry *entries;
    int count;
    FontCatalogFamily *families;
    int familyCount;
};

static int
compare_entries(
        const void *a,
        const void *b )
{
    const FontCatalogEntry *left = a;
    const FontCatalogEntry *right = b;

    const char *left_family = left && left->descriptor && left->descriptor->family ?
            left->descriptor->family : "unknown";
    const char *right_family = right && right->descriptor && right->descriptor->family ?
            right->descriptor->family : "unknown";
    int family_cmp = strcasecmp(left_family, right_family);
    if (family_cmp != 0)
        return family_cmp;

    const char *left_label = left && left->descriptor ?
            (left->descriptor->displayName ? left->descriptor->displayName : left->descriptor->raw)
            : "";
    const char *right_label = right && right->descriptor ?
            (right->descriptor->displayName ? right->descriptor->displayName : right->descriptor->raw)
            : "";

    return strcasecmp(left_label, right_label);
}

FontCatalog *
FontCatalogLoad(
        Display *display )
{
    if (!display)
        return NULL;

    FontCatalog *catalog = calloc(1, sizeof(FontCatalog));
    if (!catalog)
        return NULL;

    int count = 0;
    char **font_names = XListFonts(display, FONT_CATALOG_PATTERN,
                                   FONT_CATALOG_LIMIT, &count);
    if (!font_names || count == 0) {
        if (font_names)
            XFreeFontNames(font_names);
        FontCatalogFree(catalog);
        return NULL;
    }

    catalog->entries = calloc(count, sizeof(FontCatalogEntry));
    if (!catalog->entries) {
        XFreeFontNames(font_names);
        FontCatalogFree(catalog);
        return NULL;
    }

    catalog->families = calloc(count, sizeof(FontCatalogFamily));
    if (!catalog->families) {
        XFreeFontNames(font_names);
        FontCatalogFree(catalog);
        return NULL;
    }

    int added = 0;
    int i, j;
    for (i = 0; i < count; i++) {
        FontDescriptor *descriptor = ParseXLFD(font_names[i]);
        if (!descriptor)
            continue;

        const char *raw = descriptor->raw;
        if (!raw) {
            raw = font_names[i];
        }

        bool duplicate = false;
        for (j = 0; j < added; j++) {
            const char *existing = catalog->entries[j].descriptor ?
                catalog->entries[j].descriptor->raw : NULL;
            if (existing && raw && strcmp(existing, raw) == 0) {
                duplicate = true;
                break;
            }
        }

        if (duplicate) {
            FreeFontDescriptor(descriptor);
            continue;
        }

        catalog->entries[added].descriptor = descriptor;
        const char *label_text = descriptor->displayName ? descriptor->displayName : raw;
        catalog->entries[added].label = XmStringCreateLocalized(label_text ? label_text : "unknown");
        added++;
    }

    XFreeFontNames(font_names);

    if (added == 0) {
        FontCatalogFree(catalog);
        return NULL;
    }

    catalog->count = added;
    qsort(catalog->entries, catalog->count, sizeof(FontCatalogEntry), compare_entries);

    /* Build family groups once entries are sorted by family */
    int family_count = 0;
    FontCatalogFamily *current_family = NULL;
    for (i = 0; i < catalog->count; i++) {
        FontDescriptor *desc = catalog->entries[i].descriptor;
        const char *family_name = desc && desc->family ? desc->family : "unknown";

        if (!current_family || strcasecmp(current_family->name, family_name) != 0) {
            current_family = &catalog->families[family_count++];
            current_family->name = strdup(family_name);
            current_family->label = XmStringCreateLocalized(current_family->name);
            current_family->styleCapacity = 4;
            current_family->styles = calloc(current_family->styleCapacity,
                                            sizeof(FontCatalogEntry *));
            current_family->styleCount = 0;
        }

        if (!current_family)
            continue;

        if (current_family->styleCount >= current_family->styleCapacity) {
            current_family->styleCapacity *= 2;
            FontCatalogEntry **new_array = realloc(
                    current_family->styles,
                    sizeof(FontCatalogEntry *) * current_family->styleCapacity);
            if (new_array)
                current_family->styles = new_array;
        }

        current_family->styles[current_family->styleCount++] = &catalog->entries[i];
    }

    catalog->familyCount = family_count;
    return catalog;
}

void
FontCatalogFree(
        FontCatalog *catalog )
{
    if (!catalog)
        return;

    int i;
    for (i = 0; i < catalog->count; i++) {
        FreeFontDescriptor(catalog->entries[i].descriptor);
        if (catalog->entries[i].label)
            XmStringFree(catalog->entries[i].label);
    }
    free(catalog->entries);
    for (i = 0; i < catalog->familyCount; i++) {
        FontCatalogFamily *family = &catalog->families[i];
        free(family->name);
        if (family->label)
            XmStringFree(family->label);
        free(family->styles);
    }
    free(catalog->families);
    free(catalog);
}

int
FontCatalogCount(
        const FontCatalog *catalog )
{
    return catalog ? catalog->count : 0;
}

FontDescriptor *
FontCatalogDescriptor(
        const FontCatalog *catalog,
        int index )
{
    if (!catalog || index < 0 || index >= catalog->count)
        return NULL;
    return catalog->entries[index].descriptor;
}

XmString
FontCatalogLabel(
        const FontCatalog *catalog,
        int index )
{
    if (!catalog || index < 0 || index >= catalog->count)
        return NULL;
    return catalog->entries[index].label;
}

int
FontCatalogFamilyCount(
        const FontCatalog *catalog )
{
    return catalog ? catalog->familyCount : 0;
}

XmString
FontCatalogFamilyLabel(
        const FontCatalog *catalog,
        int index )
{
    if (!catalog || index < 0 || index >= catalog->familyCount)
        return NULL;
    return catalog->families[index].label;
}

int
FontCatalogStyleCount(
        const FontCatalog *catalog,
        int familyIndex )
{
    if (!catalog || familyIndex < 0 || familyIndex >= catalog->familyCount)
        return 0;
    return catalog->families[familyIndex].styleCount;
}

const char *
FontCatalogStyleLabel(
        const FontCatalog *catalog,
        int familyIndex,
        int styleIndex )
{
    if (!catalog || familyIndex < 0 || familyIndex >= catalog->familyCount)
        return NULL;
    FontCatalogFamily *family = &catalog->families[familyIndex];
    if (styleIndex < 0 || styleIndex >= family->styleCount)
        return NULL;
    FontDescriptor *desc = family->styles[styleIndex]->descriptor;
    if (!desc)
        return NULL;
    return desc->displayName ? desc->displayName : desc->raw;
}

FontDescriptor *
FontCatalogStyleDescriptor(
        const FontCatalog *catalog,
        int familyIndex,
        int styleIndex )
{
    if (!catalog || familyIndex < 0 || familyIndex >= catalog->familyCount)
        return NULL;
    FontCatalogFamily *family = &catalog->families[familyIndex];
    if (styleIndex < 0 || styleIndex >= family->styleCount)
        return NULL;
    return family->styles[styleIndex]->descriptor;
}

int
FontCatalogIndicesForRaw(
        const FontCatalog *catalog,
        const char *raw,
        int *familyIndex,
        int *styleIndex )
{
    if (!catalog || !raw)
        return 0;

    int i, j;
    for (i = 0; i < catalog->familyCount; i++) {
        FontCatalogFamily *family = &catalog->families[i];
        for (j = 0; j < family->styleCount; j++) {
            FontDescriptor *descriptor = family->styles[j]->descriptor;
            if (descriptor && descriptor->raw && strcmp(descriptor->raw, raw) == 0) {
                if (familyIndex)
                    *familyIndex = i;
                if (styleIndex)
                    *styleIndex = j;
                return 1;
            }
        }
    }
    return 0;
}
