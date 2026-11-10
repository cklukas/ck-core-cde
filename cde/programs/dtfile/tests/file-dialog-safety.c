#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int Boolean;
typedef int Tt_status;

#define True 1
#define False 0
#define MAX_PATH 32
#define XtFree free

static const char *resolved_value;
static int resolver_calls;
static char *home_host_name = "local";

static char *
ResolveLocalPathName(char *host, char *path, char *file, char *home,
                     Tt_status *status)
{
   (void)host;
   (void)path;
   (void)file;
   (void)home;
   ++resolver_calls;
   *status = 0;
   return resolved_value == NULL ? NULL : strdup(resolved_value);
}

#include "file-dialog-production.inc"

#define CHECK(condition) do { \
   if (!(condition)) { \
      fprintf(stderr, "FAIL %s:%d: %s\n", name, __LINE__, #condition); \
      return 1; \
   } \
} while (0)

static int
run_case(const char *name)
{
   char path[16] = "unchanged";
   char host[16] = "host-old";
   char directory[16] = "directory-old";
   char file[16] = "file-old";

   resolver_calls = 0;
   resolved_value = "/resolved/path";
   if (strcmp(name, "join-root") == 0)
   {
      CHECK(FileDialogBuildPath(path, sizeof(path), "/", "item"));
      CHECK(strcmp(path, "/item") == 0);
   }
   else if (strcmp(name, "join-directory") == 0)
   {
      CHECK(FileDialogBuildPath(path, sizeof(path), "/dir", "item"));
      CHECK(strcmp(path, "/dir/item") == 0);
   }
   else if (strcmp(name, "join-exact") == 0)
   {
      CHECK(FileDialogBuildPath(path, 4, "a", "b"));
      CHECK(strcmp(path, "a/b") == 0);
   }
   else if (strcmp(name, "join-long") == 0)
   {
      CHECK(!FileDialogBuildPath(path, 3, "a", "b"));
      CHECK(strcmp(path, "unchanged") == 0);
   }
   else if (strcmp(name, "join-null") == 0)
   {
      CHECK(!FileDialogBuildPath(path, sizeof(path), NULL, "b"));
      CHECK(!FileDialogBuildPath(path, sizeof(path), "a", NULL));
   }
   else if (strcmp(name, "target-relative") == 0)
   {
      CHECK(GetToPath(path, sizeof(path), "item", "host", "/dir"));
      CHECK(strcmp(path, resolved_value) == 0 && resolver_calls == 1);
   }
   else if (strcmp(name, "target-absolute") == 0)
   {
      CHECK(GetToPath(path, sizeof(path), "/item", "host", "/dir"));
      CHECK(strcmp(path, resolved_value) == 0 && resolver_calls == 1);
   }
   else if (strcmp(name, "target-join-long") == 0)
   {
      CHECK(!GetToPath(path, 4, "item", "host", "/dir"));
      CHECK(resolver_calls == 0);
   }
   else if (strcmp(name, "target-resolved-long") == 0)
   {
      resolved_value = "/resolved/path/is/too/long";
      CHECK(!GetToPath(path, sizeof(path), "item", "host", "/dir"));
      CHECK(resolver_calls == 1);
   }
   else if (strcmp(name, "target-resolve-error") == 0)
   {
      resolved_value = NULL;
      CHECK(!GetToPath(path, sizeof(path), "item", "host", "/dir"));
      CHECK(resolver_calls == 1);
   }
   else if (strcmp(name, "split-absolute-root") == 0)
   {
      CHECK(GetTarget("host", "/ignored", "/item", host, sizeof(host),
                      directory, sizeof(directory), file, sizeof(file)));
      CHECK(strcmp(host, "host") == 0 && strcmp(directory, "/") == 0 &&
            strcmp(file, "item") == 0);
   }
   else if (strcmp(name, "split-relative-root") == 0)
   {
      CHECK(GetTarget("host", "/", "item", host, sizeof(host),
                      directory, sizeof(directory), file, sizeof(file)));
      CHECK(strcmp(directory, "/") == 0 && strcmp(file, "item") == 0);
   }
   else if (strcmp(name, "split-relative-directory") == 0)
   {
      CHECK(GetTarget("host", "/dir", "item", host, sizeof(host),
                      directory, sizeof(directory), file, sizeof(file)));
      CHECK(strcmp(directory, "/dir") == 0 && strcmp(file, "item") == 0);
   }
   else if (strcmp(name, "split-nested-absolute") == 0)
   {
      CHECK(GetTarget("host", "/ignored", "/one/two", host, sizeof(host),
                      directory, sizeof(directory), file, sizeof(file)));
      CHECK(strcmp(directory, "/one") == 0 && strcmp(file, "two") == 0);
   }
   else if (strcmp(name, "split-host-long") == 0)
   {
      errno = 0;
      CHECK(!GetTarget("host-too-long", "/", "item", host, 8,
                       directory, sizeof(directory), file, sizeof(file)));
      CHECK(errno == ENAMETOOLONG && strcmp(host, "host-old") == 0 &&
            strcmp(directory, "directory-old") == 0 &&
            strcmp(file, "file-old") == 0);
   }
   else if (strcmp(name, "split-path-long") == 0)
   {
      errno = 0;
      CHECK(!GetTarget("host", "/012345678901234567890123456789", "item",
                       host, sizeof(host), directory, sizeof(directory),
                       file, sizeof(file)));
      CHECK(errno == ENAMETOOLONG && strcmp(directory, "directory-old") == 0);
   }
   else if (strcmp(name, "split-trailing-slash") == 0)
   {
      errno = 0;
      CHECK(!GetTarget("host", "/ignored", "/dir/", host, sizeof(host),
                       directory, sizeof(directory), file, sizeof(file)));
      CHECK(errno == EINVAL && strcmp(directory, "directory-old") == 0);
   }
   else if (strcmp(name, "split-file-capacity") == 0)
   {
      CHECK(GetTarget("host", "/", "123456789012345", host, sizeof(host),
                      directory, sizeof(directory), file, sizeof(file)));
      CHECK(strcmp(file, "123456789012345") == 0);
      strcpy(file, "file-old");
      errno = 0;
      CHECK(!GetTarget("host", "/", "1234567890123456", host, sizeof(host),
                       directory, sizeof(directory), file, sizeof(file)));
      CHECK(errno == ENAMETOOLONG && strcmp(file, "file-old") == 0);
   }
   else
      return 2;

   printf("PASS %s\n", name);
   return 0;
}

int
main(int argc, char **argv)
{
   return argc == 2 ? run_case(argv[1]) : 2;
}
