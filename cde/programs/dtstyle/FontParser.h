#ifndef _FONT_PARSER_H
#define _FONT_PARSER_H

#include <stdbool.h>

typedef struct {
    char *fields[14];
    char *foundry;
    char *family;
    char *weight;
    char *slant;
    char *setWidth;
    char *addStyle;
    char *registry;
    char *encoding;
    char *raw;
    char *displayName;
    int pointSize;
    int pixelSize;
    bool scalable;
} FontDescriptor;

/* Parse an XLFD string and return a descriptor owned by the caller. */
FontDescriptor *ParseXLFD(const char *xlfd);

/* Free a descriptor returned by ParseXLFD. */
void FreeFontDescriptor(FontDescriptor *descriptor);

/* Update the size fields stored in the descriptor. */
void FontDescriptorSetSize(FontDescriptor *descriptor, int size);

/* Update pixel size field stored in the descriptor. */
void FontDescriptorSetPixelSize(FontDescriptor *descriptor, int pixelSize);

/* Set XLFD resolution fields (decisions left to caller). */
void FontDescriptorSetResolution(FontDescriptor *descriptor, int resX, int resY);

/* Duplicate an existing descriptor. */
FontDescriptor *DuplicateFontDescriptor(const FontDescriptor *source);

#endif /* _FONT_PARSER_H */
