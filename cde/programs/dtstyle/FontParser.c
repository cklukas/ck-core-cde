#include "FontParser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static char *duplicate_field(const char *value)
{
    if (!value || *value == '\0' || strcmp(value, "*") == 0)
        return NULL;
    return strdup(value);
}

static char *keep_field(const char *value)
{
    if (!value)
        return NULL;
    return strdup(value);
}

static int parse_size_field(const char *value)
{
    if (!value || *value == '\0' || *value == '*')
        return 0;
    char *endptr = NULL;
    long parsed = strtol(value, &endptr, 10);
    if (endptr == value)
        return 0;
    return (int) parsed;
}

static bool should_include_weight(const char *value)
{
    if (!value || *value == '\0')
        return false;
    if (strcasecmp(value, "medium") == 0 || strcasecmp(value, "normal") == 0)
        return false;
    return true;
}

static bool should_include_slant(const char *value)
{
    if (!value || *value == '\0')
        return false;
    if (strcasecmp(value, "r") == 0 || strcasecmp(value, "n") == 0)
        return false;
    return true;
}

static bool should_include_style(const char *value)
{
    if (!value || *value == '\0')
        return false;
    if (strcmp(value, "*") == 0)
        return false;
    return true;
}

static void append_label_part(char *buffer, size_t buffer_size, const char *part)
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

static char *build_display_name(const FontDescriptor *desc)
{
    char label[256] = {0};
    if (desc->family && *desc->family)
        strncpy(label, desc->family, sizeof(label) - 1);
    else
        strncpy(label, "unknown", sizeof(label) - 1);

    if (should_include_weight(desc->weight))
        append_label_part(label, sizeof(label), desc->weight);
    if (should_include_slant(desc->slant))
        append_label_part(label, sizeof(label), desc->slant);
    if (should_include_style(desc->addStyle))
        append_label_part(label, sizeof(label), desc->addStyle);

    size_t len = strlen(label);
    if (desc->scalable) {
        if (len < sizeof(label) - 12)
            strncat(label, " (scalable)", sizeof(label) - len - 1);
    } else if (desc->pixelSize > 0) {
        snprintf(label + len, sizeof(label) - len, " (%dpx)", desc->pixelSize);
    } else if (desc->pointSize > 0) {
        snprintf(label + len, sizeof(label) - len, " (%dpt)", desc->pointSize);
    }

    return strdup(label);
}

static void rebuild_raw(FontDescriptor *descriptor)
{
    if (!descriptor)
        return;

    char buf[1024] = {0};
    size_t offset = 0;
    for (int i = 0; i < 14; i++) {
        const char *field = descriptor->fields[i];
        if (!field || *field == '\0')
            field = "*";
        if (offset < sizeof(buf) - 1) {
            buf[offset++] = '-';
            size_t to_copy = strlen(field);
            if (offset + to_copy >= sizeof(buf))
                to_copy = sizeof(buf) - offset - 1;
            memcpy(buf + offset, field, to_copy);
            offset += to_copy;
        }
    }
    buf[offset] = '\0';
    free(descriptor->raw);
    descriptor->raw = strdup(buf);
}

static void refresh_display_name(FontDescriptor *descriptor)
{
    if (!descriptor)
        return;
    free(descriptor->displayName);
    descriptor->displayName = build_display_name(descriptor);
}

static void set_field_value(FontDescriptor *descriptor, int index, const char *value)
{
    if (!descriptor || index < 0 || index >= 14)
        return;
    free(descriptor->fields[index]);
    descriptor->fields[index] = value ? strdup(value) : NULL;
}

static void set_numeric_field(FontDescriptor *descriptor, int index, int value)
{
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%d", value);
    set_field_value(descriptor, index, buffer);
}

FontDescriptor *ParseXLFD(const char *xlfd)
{
    if (!xlfd)
        return NULL;

    char *copy = strdup(xlfd);
    if (!copy)
        return NULL;

    char *fields[14] = {0};
    char *p = copy;
    if (*p == '-')
        p++;
    int count = 0;
    while (count < 14 && p) {
        char *next = strchr(p, '-');
        if (next) {
            *next = '\0';
            fields[count++] = p;
            p = next + 1;
        } else {
            fields[count++] = p;
            p = NULL;
        }
    }

    if (count < 14) {
        free(copy);
        return NULL;
    }

    FontDescriptor *descriptor = calloc(1, sizeof(FontDescriptor));
    if (!descriptor) {
        free(copy);
        return NULL;
    }

    for (int i = 0; i < 14; i++)
        descriptor->fields[i] = NULL;

    descriptor->foundry = duplicate_field(fields[0]);
    descriptor->family = duplicate_field(fields[1]);
    descriptor->weight = duplicate_field(fields[2]);
    descriptor->slant = duplicate_field(fields[3]);
    descriptor->setWidth = duplicate_field(fields[4]);
    descriptor->addStyle = duplicate_field(fields[5]);
    descriptor->registry = duplicate_field(fields[12]);
    descriptor->encoding = duplicate_field(fields[13]);
    descriptor->pixelSize = parse_size_field(fields[6]);
    descriptor->pointSize = parse_size_field(fields[7]);
    descriptor->scalable = (descriptor->pixelSize == 0 || descriptor->pointSize == 0);
    descriptor->raw = strdup(xlfd);
    for (int i = 0; i < 14; i++)
        descriptor->fields[i] = keep_field(fields[i]);
    descriptor->displayName = build_display_name(descriptor);

    free(copy);
    return descriptor;
}

void FreeFontDescriptor(FontDescriptor *descriptor)
{
    if (!descriptor)
        return;

    free(descriptor->foundry);
    free(descriptor->family);
    free(descriptor->weight);
    free(descriptor->slant);
    free(descriptor->setWidth);
    free(descriptor->addStyle);
    free(descriptor->registry);
    free(descriptor->encoding);
    for (int i = 0; i < 14; i++) {
        free(descriptor->fields[i]);
    }
    free(descriptor->displayName);
    free(descriptor->raw);
    free(descriptor);
}

void FontDescriptorSetSize(FontDescriptor *descriptor, int size)
{
    if (!descriptor || size <= 0)
        return;

    if (descriptor->scalable) {
        descriptor->pointSize = size * 10;
        set_numeric_field(descriptor, 7, size * 10);
        descriptor->pixelSize = 0;
        set_numeric_field(descriptor, 6, 0);
    } else {
        descriptor->pixelSize = size;
        set_numeric_field(descriptor, 6, size);
    }
    rebuild_raw(descriptor);
    refresh_display_name(descriptor);
}

void FontDescriptorSetPixelSize(FontDescriptor *descriptor, int pixelSize)
{
    if (!descriptor || pixelSize <= 0)
        return;
    descriptor->pixelSize = pixelSize;
    set_numeric_field(descriptor, 6, pixelSize);
    rebuild_raw(descriptor);
    refresh_display_name(descriptor);
}

void FontDescriptorSetResolution(FontDescriptor *descriptor, int resX, int resY)
{
    if (!descriptor || resX <= 0 || resY <= 0)
        return;
    set_numeric_field(descriptor, 8, resX);
    set_numeric_field(descriptor, 9, resY);
    rebuild_raw(descriptor);
    refresh_display_name(descriptor);
}

FontDescriptor *
DuplicateFontDescriptor(
        const FontDescriptor *source )
{
    if (!source)
        return NULL;

    FontDescriptor *copy = calloc(1, sizeof(FontDescriptor));
    if (!copy)
        return NULL;

    for (int i = 0; i < 14; i++) {
        copy->fields[i] = keep_field(source->fields[i]);
    }

    copy->foundry = keep_field(source->foundry);
    copy->family = keep_field(source->family);
    copy->weight = keep_field(source->weight);
    copy->slant = keep_field(source->slant);
    copy->setWidth = keep_field(source->setWidth);
    copy->addStyle = keep_field(source->addStyle);
    copy->registry = keep_field(source->registry);
    copy->encoding = keep_field(source->encoding);
    copy->pixelSize = source->pixelSize;
    copy->pointSize = source->pointSize;
    copy->scalable = source->scalable;
    copy->raw = keep_field(source->raw);
    copy->displayName = keep_field(source->displayName);

    return copy;
}
