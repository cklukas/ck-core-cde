/*
 * Minimal Shelf widget stub for dtfile.
 * This will be expanded with real shelf behavior later.
 */
#ifndef _DtShelf_h
#define _DtShelf_h

#include <Xm/Xm.h>

typedef struct
{
   int slot;
   char *path;
} ShelfSlot;

extern Widget _DtCreateShelf(
        Widget parent,
        char *name,
        ArgList args,
        Cardinal num_args);

extern void ShelfInit(void);
extern int ShelfGetSlotCount(void);
extern const ShelfSlot *ShelfGetSlots(int *count);
extern const char *ShelfGetPathForSlot(int slot);
extern const char *ShelfGetHomePath(void);
extern int ShelfNextEmptySlot(void);
extern void ShelfSetSlot(int slot, const char *path);
extern void ShelfClearSlot(int slot);
extern void ShelfClearSlotSkipIgnore(int slot);
extern Boolean ShelfIsIgnoredMount(const char *mount_point);
extern void ShelfIgnoreMount(const char *mount_point);
extern void ShelfUnignoreMount(const char *mount_point);
extern void ShelfLoadMounts(void);

#endif /* _DtShelf_h */
