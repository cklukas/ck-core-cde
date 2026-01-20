/*
 * Minimal Shelf widget stub for dtfile.
 * This will be expanded with real shelf behavior later.
 */
#include <Xm/Form.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mntent.h>
#include <stdarg.h>

#include "Encaps.h"
#include "SharedProcs.h"
#include "Help.h"
#include "FileMgr.h"
#include "Desktop.h"
#include "Main.h"
#include "Shelf.h"

#define SHELF_FILE_NAME ".dtfile_shelf"

static Boolean shelf_loaded = False;
static ShelfSlot *shelf_slots = NULL;
static int shelf_slot_count = 0;
static char **ignored_mounts = NULL;
static int ignored_mount_count = 0;

static void
ShelfLog(const char *fmt, ...)
{
   va_list ap;

   if (getenv("DTFILE_SHELF_DEBUG") == NULL)
      return;

   fprintf(stderr, "dtfile-shelf: ");
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
   fflush(stderr);
}

const char *
ShelfGetHomePath(void)
{
   static char home_path[MAX_PATH];
   size_t len;

   if (home_path[0] != '\0')
      return home_path;

   if (users_home_dir[0] == '\0')
   {
      const char *env_home = getenv("HOME");
      if (env_home && *env_home)
      {
         strncpy(home_path, env_home, MAX_PATH - 1);
         home_path[MAX_PATH - 1] = '\0';
      }
      else
      {
         strncpy(home_path, "/", MAX_PATH - 1);
         home_path[MAX_PATH - 1] = '\0';
      }
   }
   else
   {
      strncpy(home_path, users_home_dir, MAX_PATH - 1);
      home_path[MAX_PATH - 1] = '\0';
   }

   len = strlen(home_path);
   if (len > 1 && home_path[len - 1] == '/')
      home_path[len - 1] = '\0';

   return home_path;
}

static const char *
ShelfConfigPath(void)
{
   static char config_path[MAX_PATH];
   const char *home_dir;

   if (config_path[0] != '\0')
      return config_path;

   home_dir = getenv("HOME");
   if (home_dir == NULL || *home_dir == '\0')
      home_dir = ShelfGetHomePath();

   snprintf(config_path, sizeof(config_path), "%s/.dt/%s",
            home_dir, SHELF_FILE_NAME);
   return config_path;
}

static void
ShelfEnsureConfigDir(void)
{
   const char *home_dir;
   char path[MAX_PATH];

   home_dir = getenv("HOME");
   if (home_dir == NULL || *home_dir == '\0')
      home_dir = ShelfGetHomePath();

   snprintf(path, sizeof(path), "%s/.dt", home_dir);
   if (mkdir(path, 0700) != 0 && errno != EEXIST)
      return;
}

static void
ShelfAddSlot(int slot, const char *path)
{
   int i;

   for (i = 0; i < shelf_slot_count; i++)
   {
      if (shelf_slots[i].slot == slot)
      {
         XtFree(shelf_slots[i].path);
         shelf_slots[i].path = XtNewString(path);
         return;
      }
   }

   shelf_slots = (ShelfSlot *)XtRealloc((char *)shelf_slots,
                                        sizeof(ShelfSlot) *
                                        (shelf_slot_count + 1));
   shelf_slots[shelf_slot_count].slot = slot;
   shelf_slots[shelf_slot_count].path = XtNewString(path);
   shelf_slot_count++;
}

static Boolean
ShelfIsLocalFsType(const char *fs_type)
{
   if (fs_type == NULL || *fs_type == '\0')
      return False;

   if (strcmp(fs_type, "nfs") == 0 ||
       strcmp(fs_type, "nfs4") == 0 ||
       strcmp(fs_type, "smbfs") == 0 ||
       strcmp(fs_type, "cifs") == 0 ||
       strcmp(fs_type, "sshfs") == 0 ||
       strcmp(fs_type, "fuse.sshfs") == 0)
      return False;

   if (strcmp(fs_type, "proc") == 0 ||
       strcmp(fs_type, "sysfs") == 0 ||
       strcmp(fs_type, "devtmpfs") == 0 ||
       strcmp(fs_type, "devpts") == 0 ||
       strcmp(fs_type, "tmpfs") == 0 ||
       strcmp(fs_type, "cgroup") == 0 ||
       strcmp(fs_type, "cgroup2") == 0 ||
       strcmp(fs_type, "overlay") == 0)
      return False;

   return True;
}

static Boolean
ShelfHasPrefix(const char *path, const char *prefix)
{
   size_t len;

   if (path == NULL || prefix == NULL)
      return False;

   len = strlen(prefix);
   return (strncmp(path, prefix, len) == 0);
}

static void
ShelfRemoveMissingMounts(void)
{
   int i;
   struct stat st;

   for (i = 0; i < shelf_slot_count; i++)
   {
      if (shelf_slots[i].path == NULL || shelf_slots[i].slot == 0)
         continue;

      if (stat(shelf_slots[i].path, &st) != 0)
      {
         XtFree(shelf_slots[i].path);
         shelf_slots[i].path = NULL;
      }
   }
}

static void
ShelfSave(void)
{
   const char *config_path;
   FILE *fp;
   int i;

   ShelfEnsureConfigDir();
   config_path = ShelfConfigPath();
   fp = fopen(config_path, "w");
   if (fp == NULL)
      return;

   fprintf(fp, "SHELFv1\n");
   for (i = 0; i < shelf_slot_count; i++)
   {
      if (shelf_slots[i].path)
         fprintf(fp, "SLOT %d %s\n", shelf_slots[i].slot, shelf_slots[i].path);
   }
   for (i = 0; i < ignored_mount_count; i++)
   {
      if (ignored_mounts[i])
         fprintf(fp, "IGNORE %s\n", ignored_mounts[i]);
   }

   fclose(fp);
}

static void
ShelfEnsureHomeSlot(void)
{
   const char *home_path;
   int i;

   for (i = 0; i < shelf_slot_count; i++)
   {
      if (shelf_slots[i].slot == 0)
         return;
   }

   home_path = ShelfGetHomePath();
   DPRINTF(("ShelfEnsureHomeSlot: slot 0 -> %s\n", home_path ? home_path : "(null)"));
   ShelfLog("ShelfEnsureHomeSlot: slot 0 -> %s\n",
            home_path ? home_path : "(null)");
   ShelfAddSlot(0, home_path);
}

static void
ShelfLoad(void)
{
   const char *config_path;
   FILE *fp;
   char line[MAX_PATH * 2];

   config_path = ShelfConfigPath();
   fp = fopen(config_path, "r");
   if (fp == NULL)
      return;

   while (fgets(line, sizeof(line), fp) != NULL)
   {
      char *cursor = line;
      char *newline = strchr(line, '\n');
      if (newline)
         *newline = '\0';

      if (strncmp(cursor, "SHELFv1", 7) == 0)
         continue;
      if (strncmp(cursor, "SLOT ", 5) == 0)
      {
         int slot = 0;
         char *path;

         cursor += 5;
         slot = (int)strtol(cursor, &path, 10);
         if (path == NULL)
            continue;
         while (*path == ' ')
            path++;
         if (*path == '\0')
            continue;
         ShelfAddSlot(slot, path);
         continue;
      }
      if (strncmp(cursor, "IGNORE ", 7) == 0)
      {
         char *path = cursor + 7;
         if (*path == '\0')
            continue;
         ShelfIgnoreMount(path);
         continue;
      }
   }

   fclose(fp);
}

Widget
_DtCreateShelf(
        Widget parent,
        char *name,
        ArgList args,
        Cardinal num_args)
{
   return XmCreateForm(parent, name, args, num_args);
}

void
ShelfInit(void)
{
   if (shelf_loaded)
      return;

   DPRINTF(("ShelfInit: loading shelf config\n"));
   ShelfLog("ShelfInit: loading shelf config\n");
   ShelfLoad();
   ShelfEnsureHomeSlot();
   ShelfSave();
   shelf_loaded = True;
}

void
ShelfLoadMounts(void)
{
   FILE *fp;
   struct mntent *ent;

   ShelfInit();

   DPRINTF(("ShelfLoadMounts: scanning mounts\n"));
   ShelfLog("ShelfLoadMounts: scanning mounts\n");
   fp = setmntent("/proc/mounts", "r");
   if (fp == NULL)
      fp = setmntent("/etc/mtab", "r");
   if (fp == NULL)
      return;

   while ((ent = getmntent(fp)) != NULL)
   {
      int slot;
      int i;
      Boolean exists = False;

      if (!ShelfIsLocalFsType(ent->mnt_type))
         continue;

      if (ShelfHasPrefix(ent->mnt_dir, "/sys/") ||
          ShelfHasPrefix(ent->mnt_dir, "/boot/"))
      {
         ShelfIgnoreMount(ent->mnt_dir);
         continue;
      }

      if (ShelfIsIgnoredMount(ent->mnt_dir))
         continue;

      if (ShelfGetPathForSlot(0) &&
          strcmp(ent->mnt_dir, ShelfGetPathForSlot(0)) == 0)
         continue;

      for (i = 0; i < shelf_slot_count; i++)
      {
         if (shelf_slots[i].path &&
             strcmp(shelf_slots[i].path, ent->mnt_dir) == 0)
         {
            exists = True;
            break;
         }
      }
      if (exists)
         continue;

      slot = ShelfNextEmptySlot();
      ShelfAddSlot(slot, ent->mnt_dir);
   }

   endmntent(fp);
   ShelfRemoveMissingMounts();
   ShelfSave();
}

int
ShelfGetSlotCount(void)
{
   return shelf_slot_count;
}

const ShelfSlot *
ShelfGetSlots(int *count)
{
   if (count)
      *count = shelf_slot_count;
   return shelf_slots;
}

const char *
ShelfGetPathForSlot(int slot)
{
   int i;

   for (i = 0; i < shelf_slot_count; i++)
   {
      if (shelf_slots[i].slot == slot)
         return shelf_slots[i].path;
   }

   return NULL;
}

int
ShelfNextEmptySlot(void)
{
   int slot = 1;

   while (ShelfGetPathForSlot(slot) != NULL)
      slot++;

   return slot;
}

void
ShelfSetSlot(int slot, const char *path)
{
   if (path == NULL || *path == '\0')
      return;

   if (slot == 0)
   {
      const char *home_path = ShelfGetHomePath();
      if (strcmp(path, home_path) != 0)
         return;
   }

   ShelfAddSlot(slot, path);
   DPRINTF(("ShelfSetSlot: slot %d -> %s\n", slot, path));
   ShelfLog("ShelfSetSlot: slot %d -> %s\n", slot, path);
   ShelfSave();
}

void
ShelfClearSlot(int slot)
{
   int i;

   for (i = 0; i < shelf_slot_count; i++)
   {
      if (shelf_slots[i].slot == slot)
      {
         if (shelf_slots[i].path)
         {
            if (slot != 0)
               ShelfIgnoreMount(shelf_slots[i].path);
            XtFree(shelf_slots[i].path);
         }
         shelf_slots[i].path = NULL;
         break;
      }
   }
   ShelfEnsureHomeSlot();
   ShelfSave();
}

Boolean
ShelfIsIgnoredMount(const char *mount_point)
{
   int i;

   if (mount_point == NULL || *mount_point == '\0')
      return False;

   for (i = 0; i < ignored_mount_count; i++)
   {
      if (ignored_mounts[i] && strcmp(ignored_mounts[i], mount_point) == 0)
         return True;
   }
   return False;
}

void
ShelfIgnoreMount(const char *mount_point)
{
   int i;

   if (mount_point == NULL || *mount_point == '\0')
      return;

   for (i = 0; i < ignored_mount_count; i++)
   {
      if (ignored_mounts[i] && strcmp(ignored_mounts[i], mount_point) == 0)
         return;
   }

   ignored_mounts = (char **)XtRealloc((char *)ignored_mounts,
                                       sizeof(char *) *
                                       (ignored_mount_count + 1));
   ignored_mounts[ignored_mount_count] = XtNewString(mount_point);
   ignored_mount_count++;

   for (i = 0; i < shelf_slot_count; i++)
   {
      if (shelf_slots[i].path &&
          strcmp(shelf_slots[i].path, mount_point) == 0 &&
          shelf_slots[i].slot != 0)
      {
         XtFree(shelf_slots[i].path);
         shelf_slots[i].path = NULL;
      }
   }
   ShelfSave();
}

void
ShelfUnignoreMount(const char *mount_point)
{
   int i;

   if (mount_point == NULL || *mount_point == '\0')
      return;

   for (i = 0; i < ignored_mount_count; i++)
   {
      if (ignored_mounts[i] && strcmp(ignored_mounts[i], mount_point) == 0)
      {
         int j;
         XtFree(ignored_mounts[i]);
         for (j = i; j < ignored_mount_count - 1; j++)
            ignored_mounts[j] = ignored_mounts[j + 1];
         ignored_mount_count--;
         break;
      }
   }
   ShelfSave();
}
