#ifndef _FONT_CATALOG_H
#define _FONT_CATALOG_H

#include <Xm/Xm.h>
#include <X11/Xlib.h>

#include "FontParser.h"

typedef struct FontCatalog FontCatalog;

/* Build a catalog of fonts available from the display; caller owns the catalog. */
FontCatalog *FontCatalogLoad(Display *display);

/* Release the catalog and all owned resources. */
void FontCatalogFree(FontCatalog *catalog);

/* Query helpers for the catalog. */
int FontCatalogCount(const FontCatalog *catalog);
FontDescriptor *FontCatalogDescriptor(const FontCatalog *catalog, int index);
XmString FontCatalogLabel(const FontCatalog *catalog, int index);
XmString FontCatalogFamilyLabel(const FontCatalog *catalog, int index);
int FontCatalogFamilyCount(const FontCatalog *catalog);
int FontCatalogStyleCount(const FontCatalog *catalog, int familyIndex);
const char *FontCatalogStyleLabel(const FontCatalog *catalog, int familyIndex, int styleIndex);
FontDescriptor *FontCatalogStyleDescriptor(const FontCatalog *catalog, int familyIndex, int styleIndex);
int FontCatalogIndicesForRaw(const FontCatalog *catalog, const char *raw, int *familyIndex, int *styleIndex);

#endif /* _FONT_CATALOG_H */
