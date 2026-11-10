#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int Boolean;
typedef int Tt_status;

#define True 1
#define False 0
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
