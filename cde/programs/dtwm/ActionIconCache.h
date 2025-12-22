#ifndef _ACTION_ICON_CACHE_H
#define _ACTION_ICON_CACHE_H

#include <X11/Intrinsic.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const char *ActionIconFind(const char *command);
extern Boolean ActionIconCacheUpdate(const char *actionCommand,
                                     const char *childCommandList);

#ifdef __cplusplus
}
#endif

#endif /* _ACTION_ICON_CACHE_H */
