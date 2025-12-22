#ifndef DTSTYLE_FONT_FACE_CATALOG_H
#define DTSTYLE_FONT_FACE_CATALOG_H

#include <Xm/Xm.h>
#include <X11/Xlib.h>

#include "FontParser.h"

typedef struct FontFaceCatalog FontFaceCatalog;

FontFaceCatalog *FontFaceCatalogLoad(Display *display);
void FontFaceCatalogFree(FontFaceCatalog *catalog);

int FontFaceCatalogFamilyCount(const FontFaceCatalog *catalog);
XmString FontFaceCatalogFamilyLabel(const FontFaceCatalog *catalog, int familyIndex);

int FontFaceCatalogVariantCount(const FontFaceCatalog *catalog, int familyIndex);
XmString FontFaceCatalogVariantLabel(
        const FontFaceCatalog *catalog,
        int familyIndex,
        int variantIndex);
Boolean FontFaceCatalogVariantIsScalable(
        const FontFaceCatalog *catalog,
        int familyIndex,
        int variantIndex);
const FontDescriptor *FontFaceCatalogVariantBase(
        const FontFaceCatalog *catalog,
        int familyIndex,
        int variantIndex);

int FontFaceCatalogVariantCharsetCount(
        const FontFaceCatalog *catalog,
        int familyIndex,
        int variantIndex);
XmString FontFaceCatalogVariantCharsetLabel(
        const FontFaceCatalog *catalog,
        int familyIndex,
        int variantIndex,
        int charsetIndex);

int FontFaceCatalogVariantCharsetSizeCount(
        const FontFaceCatalog *catalog,
        int familyIndex,
        int variantIndex,
        int charsetIndex);
int FontFaceCatalogVariantCharsetSizeAt(
        const FontFaceCatalog *catalog,
        int familyIndex,
        int variantIndex,
        int charsetIndex,
        int sizePos);
int FontFaceCatalogVariantCharsetNearestSize(
        const FontFaceCatalog *catalog,
        int familyIndex,
        int variantIndex,
        int charsetIndex,
        int desiredSize);

FontDescriptor *
FontFaceCatalogCreateDescriptorForVariant(
        const FontFaceCatalog *catalog,
        int familyIndex,
        int variantIndex,
        int charsetIndex,
        int sizeValue);

int FontFaceCatalogFindVariantForDescriptor(
        const FontFaceCatalog *catalog,
        const FontDescriptor *descriptor,
        int *outFamilyIndex,
        int *outVariantIndex,
        int *outCharsetIndex);

#endif /* DTSTYLE_FONT_FACE_CATALOG_H */
