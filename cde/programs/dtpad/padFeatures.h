#ifndef DTPAD_PAD_FEATURES_H
#define DTPAD_PAD_FEATURES_H

#include <limits.h>
#include <stddef.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define DTPAD_MAX_RECENT 10

const char *dtpad_default_recent_path(char *buf, size_t buf_len);
const char *dtpad_default_autosave_dir(char *buf, size_t buf_len);

int dtpad_recent_load(const char *store_path,
                      char out_paths[DTPAD_MAX_RECENT][PATH_MAX]);
int dtpad_recent_record(const char *store_path, const char *file_path);

void dtpad_autosave_slot_path(const char *autosave_dir,
                              const char *target_path,
                              int pid,
                              int tab_id,
                              char *out_path,
                              size_t out_len);

int dtpad_autosave_write(const char *autosave_dir,
                         const char *target_path,
                         int pid,
                         int tab_id,
                         const char *contents,
                         size_t len,
                         char *out_swap_path,
                         size_t swap_len);

int dtpad_autosave_remove_file(const char *swap_path);
int dtpad_autosave_remove_for_path(const char *autosave_dir,
                                   const char *target_path);

int dtpad_autosave_find_recovery(const char *autosave_dir,
                                 const char *target_path,
                                 char **out_contents,
                                 size_t *out_len,
                                 char *out_swap_path,
                                 size_t swap_len);

#endif /* DTPAD_PAD_FEATURES_H */
