/*
 * FileManagerService.h - org.freedesktop.FileManager1 Session D-Bus Service
 *                        for CK-Core CDE File Manager (Wayland & X11, WP-16).
 */

#ifndef _Dtfile_FileManagerService_h
#define _Dtfile_FileManagerService_h

#include <X11/Intrinsic.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decode a "file:///..." URI (or plain absolute path) into `path_out`. */
Boolean DtfileDecodeFileUri(const char *uri, char *path_out, size_t path_sz);

/* Register org.freedesktop.FileManager1 on the session D-Bus and attach to Xt. */
void DtfileFileManagerServiceInit(Widget toplevel_widget);

/* Release D-Bus resources cleanly on exit. */
void DtfileFileManagerServiceShutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* _Dtfile_FileManagerService_h */
