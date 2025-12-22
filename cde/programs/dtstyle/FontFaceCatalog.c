#include "FontFaceCatalog.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <ctype.h>

#define FACE_CATALOG_PATTERN "-*-*-*-*-*-*-*-*-*-*-*-*-*-*"
#define FACE_CATALOG_LIMIT   12000

typedef struct {
    int size;
    char *raw;
} VariantSizeEntry;

typedef struct {
    char *registry;
    char *encoding;
    XmString label;
    char *raw;
    VariantSizeEntry *sizes;
    int sizeCount;
    int sizeCapacity;
} VariantCharsetEntry;

typedef struct {
    char *key;
    char *labelText;
    XmString label;
    FontDescriptor *base;
    Boolean scalable;
    VariantCharsetEntry *charsets;
    int charsetCount;
    int charsetCapacity;
} VariantEntry;

typedef struct {
    char *familyName;
    XmString label;
    VariantEntry **variants;
    int variantCount;
    int variantCapacity;
} FamilyEntry;

struct FontFaceCatalog {
    FamilyEntry *families;
    int familyCount;
    VariantEntry *variantPool;
    int variantCount;
};

static bool
should_include_weight(const char *value)
{
    if (!value || *value == '\0')
        return false;
    if (strcasecmp(value, "medium") == 0 || strcasecmp(value, "normal") == 0)
        return false;
    return true;
}

static bool
should_include_slant(const char *value)
{
    if (!value || *value == '\0')
        return false;
    if (strcasecmp(value, "r") == 0 || strcasecmp(value, "n") == 0)
        return false;
    return true;
}

static bool
should_include_style(const char *value)
{
    if (!value || *value == '\0')
        return false;
    if (strcmp(value, "*") == 0)
        return false;
    return true;
}

static void
append_label_part(char *buffer, size_t buffer_size, const char *part)
{
    if (!part || *part == '\0')
        return;
    size_t current = strlen(buffer);
    if (current >= buffer_size - 1)
        return;
    if (current > 0)
        strncat(buffer, " ", buffer_size - current - 1);
    strncat(buffer, part, buffer_size - current - 1);
}

static char *
build_variant_label_text(const FontDescriptor *desc, Boolean scalable)
{
    char label[256] = {0};
    if (desc && desc->family && *desc->family)
        strncpy(label, desc->family, sizeof(label) - 1);
    else
        strncpy(label, "unknown", sizeof(label) - 1);

    if (desc) {
        if (should_include_weight(desc->weight))
            append_label_part(label, sizeof(label), desc->weight);
        if (should_include_slant(desc->slant))
            append_label_part(label, sizeof(label), desc->slant);
        if (should_include_style(desc->addStyle))
            append_label_part(label, sizeof(label), desc->addStyle);
        if (should_include_style(desc->setWidth))
            append_label_part(label, sizeof(label), desc->setWidth);
        if (should_include_style(desc->fields[10]))
            append_label_part(label, sizeof(label), desc->fields[10]); /* spacing */
    }

    if (scalable) {
        size_t len = strlen(label);
        if (len < sizeof(label) - 12)
            strncat(label, " (scalable)", sizeof(label) - len - 1);
    }

    return strdup(label);
}

static const char *
field_or_empty(const char *value)
{
    return value ? value : "";
}

static char *
build_variant_key(const FontDescriptor *desc)
{
    if (!desc)
        return strdup("");

    const char *foundry = field_or_empty(desc->foundry);
    const char *family = field_or_empty(desc->family);
    const char *weight = field_or_empty(desc->weight);
    const char *slant = field_or_empty(desc->slant);
    const char *setWidth = field_or_empty(desc->setWidth);
    const char *addStyle = field_or_empty(desc->addStyle);
    const char *spacing = field_or_empty(desc->fields[10]);

    size_t needed = strlen(foundry) + strlen(family) + strlen(weight) + strlen(slant) +
            strlen(setWidth) + strlen(addStyle) + strlen(spacing) + 10;
    char *key = calloc(1, needed);
    if (!key)
        return NULL;

    snprintf(key, needed, "%s|%s|%s|%s|%s|%s|%s",
             foundry, family, weight, slant, setWidth, addStyle, spacing);
    return key;
}

static VariantEntry *
find_variant(VariantEntry *variants, int count, const char *key)
{
    if (!variants || count <= 0 || !key)
        return NULL;
    for (int i = 0; i < count; i++) {
        if (variants[i].key && strcasecmp(variants[i].key, key) == 0)
            return &variants[i];
    }
    return NULL;
}

static VariantCharsetEntry *
find_charset(VariantEntry *variant, const char *registry, const char *encoding)
{
    if (!variant)
        return NULL;
    for (int i = 0; i < variant->charsetCount; i++) {
        VariantCharsetEntry *charset = &variant->charsets[i];
        if (registry && encoding &&
            charset->registry && charset->encoding &&
            strcasecmp(charset->registry, registry) == 0 &&
            strcasecmp(charset->encoding, encoding) == 0)
        {
            return charset;
        }
    }
    return NULL;
}

static VariantCharsetEntry *
add_charset(
        VariantEntry *variant,
        const char *registry,
        const char *encoding,
        const char *raw)
{
    if (!variant)
        return NULL;

    if (variant->charsetCount >= variant->charsetCapacity) {
        int newCap = variant->charsetCapacity ? variant->charsetCapacity * 2 : 4;
        VariantCharsetEntry *newEntries =
            realloc(variant->charsets, sizeof(VariantCharsetEntry) * newCap);
        if (!newEntries)
            return NULL;
        variant->charsets = newEntries;
        variant->charsetCapacity = newCap;
    }

    VariantCharsetEntry *entry = &variant->charsets[variant->charsetCount++];
    entry->registry = strdup(registry ? registry : "*");
    entry->encoding = strdup(encoding ? encoding : "*");
    entry->raw = raw ? strdup(raw) : NULL;
    entry->sizes = NULL;
    entry->sizeCount = 0;
    entry->sizeCapacity = 0;

    char label[64];
    if (entry->encoding && *entry->encoding)
        snprintf(label, sizeof(label), "%s", entry->encoding);
    else
        snprintf(label, sizeof(label), "%s/%s", entry->registry, entry->encoding);
    entry->label = XmStringCreateLocalized(label);
    return entry;
}

static void
variant_add_size(
        VariantCharsetEntry *charset,
        int size,
        const char *raw)
{
    if (!charset || size <= 0 || !raw)
        return;

    for (int i = 0; i < charset->sizeCount; i++) {
        if (charset->sizes[i].size == size)
            return;
    }

    if (charset->sizeCount >= charset->sizeCapacity) {
        int newCap = charset->sizeCapacity ? charset->sizeCapacity * 2 : 8;
        VariantSizeEntry *newSizes =
            realloc(charset->sizes, sizeof(VariantSizeEntry) * newCap);
        if (!newSizes)
            return;
        charset->sizes = newSizes;
        charset->sizeCapacity = newCap;
    }

    charset->sizes[charset->sizeCount].size = size;
    charset->sizes[charset->sizeCount].raw = strdup(raw);
    charset->sizeCount++;
}

static int
compare_families(const void *a, const void *b)
{
    const FamilyEntry *left = a;
    const FamilyEntry *right = b;
    if (!left || !right)
        return 0;
    const char *ln = left->familyName ? left->familyName : "";
    const char *rn = right->familyName ? right->familyName : "";
    return strcasecmp(ln, rn);
}

static int
compare_variants(const void *a, const void *b)
{
    const VariantEntry *left = *(const VariantEntry *const *)a;
    const VariantEntry *right = *(const VariantEntry *const *)b;
    if (!left || !right)
        return 0;
    const char *l = left->labelText ? left->labelText : "";
    const char *r = right->labelText ? right->labelText : "";
    return strcasecmp(l, r);
}

static int
compare_sizes(const void *a, const void *b)
{
    const VariantSizeEntry *left = a;
    const VariantSizeEntry *right = b;
    if (!left || !right)
        return 0;
    return (left->size > right->size) - (left->size < right->size);
}

static FamilyEntry *
find_family(FamilyEntry *families, int count, const char *name)
{
    if (!families || count <= 0 || !name)
        return NULL;
    for (int i = 0; i < count; i++) {
        if (families[i].familyName &&
            strcasecmp(families[i].familyName, name) == 0)
        {
            return &families[i];
        }
    }
    return NULL;
}

static VariantEntry *
variant_entry(const FontFaceCatalog *catalog, int familyIndex, int variantIndex)
{
    if (!catalog || familyIndex < 0 || familyIndex >= catalog->familyCount)
        return NULL;
    FamilyEntry *family = &catalog->families[familyIndex];
    if (variantIndex < 0 || variantIndex >= family->variantCount)
        return NULL;
    return family->variants[variantIndex];
}

static VariantCharsetEntry *
variant_charset(
        const FontFaceCatalog *catalog,
        int familyIndex,
        int variantIndex,
        int charsetIndex )
{
    VariantEntry *variant = variant_entry(catalog, familyIndex, variantIndex);
    if (!variant || charsetIndex < 0 || charsetIndex >= variant->charsetCount)
        return NULL;
    return &variant->charsets[charsetIndex];
}

static VariantSizeEntry *
variant_size_entry(
        VariantCharsetEntry *charset,
        int sizeValue )
{
    if (!charset)
        return NULL;
    VariantSizeEntry *best = NULL;
    int bestDiff = INT_MAX;
    for (int i = 0; i < charset->sizeCount; i++) {
        VariantSizeEntry *entry = &charset->sizes[i];
        int diff = abs(entry->size - sizeValue);
        if (!best || diff < bestDiff) {
            best = entry;
            bestDiff = diff;
        }
        if (diff == 0)
            break;
    }
    return best;
}

static void
add_alias_descriptor(
        VariantEntry *variants,
        int variantCount,
        const FontDescriptor *descriptor )
{
    if (!variants || variantCount <= 0 || !descriptor)
        return;

    char *key = build_variant_key(descriptor);
    if (!key)
        return;

    VariantEntry *variant = find_variant(variants, variantCount, key);
    free(key);
    if (!variant)
        return;

    const char *registry = field_or_empty(descriptor->registry);
    const char *encoding = field_or_empty(descriptor->encoding);
    VariantCharsetEntry *charset =
        find_charset(variant, registry, encoding);
    if (!charset)
        charset = add_charset(variant, registry, encoding, descriptor->raw);
    if (!charset)
        return;

    if (descriptor->scalable) {
        if (charset->raw)
            free(charset->raw);
        charset->raw = descriptor->raw ? strdup(descriptor->raw) : NULL;
        variant->scalable = True;
    } else {
        int size = descriptor->pixelSize > 0 ? descriptor->pixelSize : descriptor->pointSize;
        if (size > 0)
            variant_add_size(charset, size, descriptor->raw);
    }
}

static void
load_alias_file(
        VariantEntry *variants,
        int variantCount,
        const char *path )
{
    if (!variants || variantCount <= 0 || !path)
        return;

    FILE *fp = fopen(path, "r");
    if (!fp)
        return;

    char line[2048];
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p || *p == '!' || *p == '#')
            continue;

        while (*p && !isspace((unsigned char)*p))
            p++;
        if (!*p)
            continue;
        *p++ = '\0';

        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            continue;

        char *actual = p;
        char *end = actual + strlen(actual);
        while (end > actual && isspace((unsigned char)*(end - 1)))
            end--;
        *end = '\0';
        if (*actual == '\0')
            continue;

        FontDescriptor *desc = ParseXLFD(actual);
        if (desc) {
            add_alias_descriptor(variants, variantCount, desc);
            FreeFontDescriptor(desc);
        }
    }

    fclose(fp);
}

static void
load_alias_font_sizes(
        VariantEntry *variants,
        int variantCount,
        Display *display )
{
    if (!variants || variantCount <= 0 || !display)
        return;

    int pathCount = 0;
    char **paths = XGetFontPath(display, &pathCount);
    if (!paths)
        return;

    for (int i = 0; i < pathCount; i++) {
        const char *dir = paths[i];
        if (!dir || !*dir)
            continue;
        size_t len = strlen(dir);
        size_t need = len + strlen("/fonts.alias") + 2;
        char *aliasPath = (char *)malloc(need);
        if (!aliasPath)
            continue;
        if (len > 0 && dir[len - 1] == '/')
            snprintf(aliasPath, need, "%sfonts.alias", dir);
        else
            snprintf(aliasPath, need, "%s/fonts.alias", dir);
        load_alias_file(variants, variantCount, aliasPath);
        free(aliasPath);
    }

    XFreeFontPath(paths);
}


FontFaceCatalog *
FontFaceCatalogLoad(Display *display)
{
    if (!display)
        return NULL;

    int count = 0;
    char **font_names = XListFonts(display, FACE_CATALOG_PATTERN, FACE_CATALOG_LIMIT, &count);
    if (!font_names || count <= 0) {
        if (font_names)
            XFreeFontNames(font_names);
        return NULL;
    }

    VariantEntry *variantPool = calloc(count, sizeof(VariantEntry));
    if (!variantPool) {
        XFreeFontNames(font_names);
        return NULL;
    }

    int variantCount = 0;
    for (int i = 0; i < count; i++) {
        FontDescriptor *desc = ParseXLFD(font_names[i]);
        if (!desc)
            continue;

        char *key = build_variant_key(desc);
        if (!key) {
            FreeFontDescriptor(desc);
            continue;
        }

        VariantEntry *variant = find_variant(variantPool, variantCount, key);
        if (!variant) {
            variant = &variantPool[variantCount++];
            variant->key = key;
            variant->base = desc;
            variant->scalable = desc->scalable ? True : False;
            variant->labelText = build_variant_label_text(desc, variant->scalable);
            variant->label = XmStringCreateLocalized(
                    variant->labelText ? variant->labelText : "unknown");
            variant->charsets = NULL;
            variant->charsetCount = 0;
            variant->charsetCapacity = 0;
            desc = NULL;
        } else {
            free(key);
            if (desc->scalable)
                variant->scalable = True;

            /* Prefer a scalable base descriptor for later descriptor creation. */
            if (desc->scalable && variant->base && !variant->base->scalable) {
                FreeFontDescriptor(variant->base);
                variant->base = desc;
                desc = NULL;
            }
        }

        const char *registry = desc && desc->registry ? desc->registry : "*";
        const char *encoding = desc && desc->encoding ? desc->encoding : "*";
        VariantCharsetEntry *charset =
            find_charset(variant, registry, encoding);
        if (!charset)
            charset = add_charset(variant, registry, encoding, font_names[i]);

        if (desc && charset) {
            if (desc->scalable) {
                if (charset->raw)
                    free(charset->raw);
                charset->raw = strdup(font_names[i]);
            } else {
                int size = desc->pixelSize > 0 ? desc->pixelSize : desc->pointSize;
                variant_add_size(charset, size, font_names[i]);
            }
        }
        if (variant->scalable && charset && !charset->raw)
            charset->raw = strdup(font_names[i]);

        if (desc)
            FreeFontDescriptor(desc);

    }

    XFreeFontNames(font_names);
    load_alias_font_sizes(variantPool, variantCount, display);

    if (variantCount <= 0) {
        free(variantPool);
        return NULL;
    }

    for (int i = 0; i < variantCount; i++) {
        VariantEntry *variant = &variantPool[i];
        for (int c = 0; c < variant->charsetCount; c++) {
            VariantCharsetEntry *charset = &variant->charsets[c];
            if (charset->sizeCount > 1)
                qsort(charset->sizes, charset->sizeCount, sizeof(VariantSizeEntry), compare_sizes);
        }
    }

    FamilyEntry *families = calloc(variantCount, sizeof(FamilyEntry));
    if (!families) {
        for (int i = 0; i < variantCount; i++) {
            VariantEntry *variant = &variantPool[i];
            free(variant->key);
            free(variant->labelText);
            if (variant->label)
                XmStringFree(variant->label);
            FreeFontDescriptor(variant->base);
            for (int c = 0; c < variant->charsetCount; c++) {
                VariantCharsetEntry *charset = &variant->charsets[c];
                if (charset->label)
                    XmStringFree(charset->label);
                free(charset->registry);
                free(charset->encoding);
                free(charset->raw);
                for (int s = 0; s < charset->sizeCount; s++)
                    free(charset->sizes[s].raw);
                free(charset->sizes);
            }
            free(variant->charsets);
        }
        free(variantPool);
        return NULL;
    }

    int familyCount = 0;
    for (int i = 0; i < variantCount; i++) {
        VariantEntry *variant = &variantPool[i];
        const char *family = variant->base && variant->base->family ? variant->base->family : "unknown";
        FamilyEntry *entry = find_family(families, familyCount, family);
        if (!entry) {
            entry = &families[familyCount++];
            entry->familyName = strdup(family);
            entry->label = XmStringCreateLocalized(entry->familyName);
            entry->variantCapacity = 4;
            entry->variants = calloc(entry->variantCapacity, sizeof(VariantEntry *));
        }
        if (entry->variantCount >= entry->variantCapacity) {
            int newCap = entry->variantCapacity * 2;
            VariantEntry **newArr = realloc(entry->variants, sizeof(VariantEntry *) * newCap);
            if (!newArr)
                continue;
            entry->variants = newArr;
            entry->variantCapacity = newCap;
        }
        entry->variants[entry->variantCount++] = variant;
    }

    for (int i = 0; i < familyCount; i++) {
        FamilyEntry *entry = &families[i];
        if (entry->variantCount > 1)
            qsort(entry->variants, entry->variantCount, sizeof(VariantEntry *), compare_variants);
    }

    qsort(families, familyCount, sizeof(FamilyEntry), compare_families);

    FontFaceCatalog *catalog = calloc(1, sizeof(FontFaceCatalog));
    if (!catalog) {
        for (int i = 0; i < familyCount; i++) {
            FamilyEntry *entry = &families[i];
            free(entry->familyName);
            if (entry->label)
                XmStringFree(entry->label);
            free(entry->variants);
        }
        free(families);
        for (int i = 0; i < variantCount; i++) {
            VariantEntry *variant = &variantPool[i];
            free(variant->key);
            free(variant->labelText);
            if (variant->label)
                XmStringFree(variant->label);
            FreeFontDescriptor(variant->base);
            for (int c = 0; c < variant->charsetCount; c++) {
                VariantCharsetEntry *charset = &variant->charsets[c];
                if (charset->label)
                    XmStringFree(charset->label);
                free(charset->registry);
                free(charset->encoding);
                free(charset->raw);
                for (int s = 0; s < charset->sizeCount; s++)
                    free(charset->sizes[s].raw);
                free(charset->sizes);
            }
            free(variant->charsets);
        }
        free(variantPool);
        return NULL;
    }

    catalog->families = families;
    catalog->familyCount = familyCount;
    catalog->variantPool = variantPool;
    catalog->variantCount = variantCount;
    return catalog;
}

static void
free_variant_entry(VariantEntry *variant)
{
    if (!variant)
        return;
    free(variant->key);
    free(variant->labelText);
    if (variant->label)
        XmStringFree(variant->label);
    FreeFontDescriptor(variant->base);
    for (int c = 0; c < variant->charsetCount; c++) {
        VariantCharsetEntry *charset = &variant->charsets[c];
        if (charset->label)
            XmStringFree(charset->label);
        free(charset->registry);
        free(charset->encoding);
        free(charset->raw);
        for (int s = 0; s < charset->sizeCount; s++)
            free(charset->sizes[s].raw);
        free(charset->sizes);
    }
    free(variant->charsets);
}

void
FontFaceCatalogFree(FontFaceCatalog *catalog)
{
    if (!catalog)
        return;

    for (int i = 0; i < catalog->familyCount; i++) {
        FamilyEntry *family = &catalog->families[i];
        free(family->familyName);
        if (family->label)
            XmStringFree(family->label);
        free(family->variants);
    }
    free(catalog->families);

    for (int i = 0; i < catalog->variantCount; i++)
        free_variant_entry(&catalog->variantPool[i]);
    free(catalog->variantPool);

    free(catalog);
}

int
FontFaceCatalogFamilyCount(const FontFaceCatalog *catalog)
{
    return catalog ? catalog->familyCount : 0;
}

XmString
FontFaceCatalogFamilyLabel(const FontFaceCatalog *catalog, int familyIndex)
{
    if (!catalog || familyIndex < 0 || familyIndex >= catalog->familyCount)
        return NULL;
    return catalog->families[familyIndex].label;
}

int
FontFaceCatalogVariantCount(const FontFaceCatalog *catalog, int familyIndex)
{
    if (!catalog || familyIndex < 0 || familyIndex >= catalog->familyCount)
        return 0;
    return catalog->families[familyIndex].variantCount;
}

XmString
FontFaceCatalogVariantLabel(
        const FontFaceCatalog *catalog,
        int familyIndex,
        int variantIndex)
{
    VariantEntry *variant = variant_entry(catalog, familyIndex, variantIndex);
    return variant ? variant->label : NULL;
}

Boolean
FontFaceCatalogVariantIsScalable(
        const FontFaceCatalog *catalog,
        int familyIndex,
        int variantIndex)
{
    VariantEntry *variant = variant_entry(catalog, familyIndex, variantIndex);
    return variant ? variant->scalable : False;
}

const FontDescriptor *
FontFaceCatalogVariantBase(
        const FontFaceCatalog *catalog,
        int familyIndex,
        int variantIndex)
{
    VariantEntry *variant = variant_entry(catalog, familyIndex, variantIndex);
    return variant ? variant->base : NULL;
}

int
FontFaceCatalogVariantCharsetCount(
        const FontFaceCatalog *catalog,
        int familyIndex,
        int variantIndex)
{
    VariantEntry *variant = variant_entry(catalog, familyIndex, variantIndex);
    return variant ? variant->charsetCount : 0;
}

XmString
FontFaceCatalogVariantCharsetLabel(
        const FontFaceCatalog *catalog,
        int familyIndex,
        int variantIndex,
        int charsetIndex)
{
    VariantCharsetEntry *charset =
        variant_charset(catalog, familyIndex, variantIndex, charsetIndex);
    return charset ? charset->label : NULL;
}

int
FontFaceCatalogVariantCharsetSizeCount(
        const FontFaceCatalog *catalog,
        int familyIndex,
        int variantIndex,
        int charsetIndex)
{
    VariantCharsetEntry *charset =
        variant_charset(catalog, familyIndex, variantIndex, charsetIndex);
    return charset ? charset->sizeCount : 0;
}

int
FontFaceCatalogVariantCharsetSizeAt(
        const FontFaceCatalog *catalog,
        int familyIndex,
        int variantIndex,
        int charsetIndex,
        int sizePos)
{
    VariantCharsetEntry *charset =
        variant_charset(catalog, familyIndex, variantIndex, charsetIndex);
    if (!charset || sizePos < 0 || sizePos >= charset->sizeCount)
        return 0;
    return charset->sizes[sizePos].size;
}

int
FontFaceCatalogVariantCharsetNearestSize(
        const FontFaceCatalog *catalog,
        int familyIndex,
        int variantIndex,
        int charsetIndex,
        int desiredSize)
{
    VariantCharsetEntry *charset =
        variant_charset(catalog, familyIndex, variantIndex, charsetIndex);
    if (!charset || charset->sizeCount <= 0 || desiredSize <= 0)
        return desiredSize;

    VariantSizeEntry *best = NULL;
    int bestDiff = INT_MAX;
    for (int i = 0; i < charset->sizeCount; i++) {
        VariantSizeEntry *entry = &charset->sizes[i];
        int diff = abs(entry->size - desiredSize);
        if (!best || diff < bestDiff) {
            best = entry;
            bestDiff = diff;
        }
        if (diff == 0)
            break;
    }
    return best ? best->size : desiredSize;
}

FontDescriptor *
FontFaceCatalogCreateDescriptorForVariant(
        const FontFaceCatalog *catalog,
        int familyIndex,
        int variantIndex,
        int charsetIndex,
        int sizeValue)
{
    VariantEntry *variant = variant_entry(catalog, familyIndex, variantIndex);
    if (!variant)
        return NULL;

    VariantCharsetEntry *charset =
        variant_charset(catalog, familyIndex, variantIndex, charsetIndex);

    if (variant->scalable) {
        const char *raw = NULL;
        FontDescriptor *probe = NULL;
        if (charset && charset->raw)
            probe = ParseXLFD(charset->raw);
        if (probe && probe->scalable)
            raw = charset->raw;
        if (probe)
            FreeFontDescriptor(probe);
        if (!raw)
            raw = (variant->base ? variant->base->raw : NULL);
        FontDescriptor *desc = ParseXLFD(raw ? raw : "");
        if (!desc)
            return NULL;
        FontDescriptorSetSize(desc, sizeValue);
        return desc;
    }

    if (!charset)
        return NULL;

    if (charset->sizeCount <= 0)
        return NULL;

    int sizeToUse = sizeValue;
    VariantSizeEntry *sizeEntry = NULL;
    for (int i = 0; i < charset->sizeCount; i++) {
        if (charset->sizes[i].size == sizeValue) {
            sizeEntry = &charset->sizes[i];
            break;
        }
    }
    if (!sizeEntry)
        sizeEntry = variant_size_entry(charset, sizeValue);
    if (!sizeEntry)
        return NULL;

    return ParseXLFD(sizeEntry->raw);
}

int
FontFaceCatalogFindVariantForDescriptor(
        const FontFaceCatalog *catalog,
        const FontDescriptor *descriptor,
        int *outFamilyIndex,
        int *outVariantIndex,
        int *outCharsetIndex)
{
    if (!catalog || !descriptor)
        return -1;

    char *key = build_variant_key(descriptor);
    if (!key)
        return -1;

    int foundFamily = -1;
    int foundVariant = -1;
    int foundCharset = -1;

    for (int fi = 0; fi < catalog->familyCount; fi++) {
        FamilyEntry *family = &catalog->families[fi];
        for (int vi = 0; vi < family->variantCount; vi++) {
            VariantEntry *variant = family->variants[vi];
            if (!variant || !variant->key)
                continue;
            if (strcasecmp(variant->key, key) != 0)
                continue;

            foundFamily = fi;
            foundVariant = vi;
            for (int ci = 0; ci < variant->charsetCount; ci++) {
                VariantCharsetEntry *charset = &variant->charsets[ci];
                const char *reg = descriptor->registry ? descriptor->registry : "*";
                const char *enc = descriptor->encoding ? descriptor->encoding : "*";
                if (charset->registry && charset->encoding &&
                    strcasecmp(charset->registry, reg) == 0 &&
                    strcasecmp(charset->encoding, enc) == 0)
                {
                    foundCharset = ci;
                    break;
                }
            }
            if (foundCharset < 0 && variant->charsetCount > 0)
                foundCharset = 0;
            break;
        }
        if (foundFamily >= 0)
            break;
    }

    free(key);
    if (foundFamily >= 0 && outFamilyIndex)
        *outFamilyIndex = foundFamily;
    if (foundVariant >= 0 && outVariantIndex)
        *outVariantIndex = foundVariant;
    if (foundCharset >= 0 && outCharsetIndex)
        *outCharsetIndex = foundCharset;

    return (foundFamily >= 0 && foundVariant >= 0) ? 0 : -1;
}
