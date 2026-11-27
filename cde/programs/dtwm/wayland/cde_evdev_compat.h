/*
 * CDE - Common Desktop Environment
 * cde_evdev_compat.h: Portable evdev button and key constants across Linux
 * (<linux/input-event-codes.h>) and FreeBSD / BSD evdev-proto
 * (<dev/evdev/input-event-codes.h>).
 */

#ifndef CDE_EVDEV_COMPAT_H
#define CDE_EVDEV_COMPAT_H

#if defined(__has_include)
#if __has_include(<linux/input-event-codes.h>)
#include <linux/input-event-codes.h>
#elif __has_include(<dev/evdev/input-event-codes.h>)
#include <dev/evdev/input-event-codes.h>
#endif
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <dev/evdev/input-event-codes.h>
#else
#include <linux/input-event-codes.h>
#endif

#ifndef BTN_LEFT
#define BTN_LEFT 0x110
#endif
#ifndef BTN_RIGHT
#define BTN_RIGHT 0x111
#endif
#ifndef BTN_MIDDLE
#define BTN_MIDDLE 0x112
#endif
#ifndef BTN_SIDE
#define BTN_SIDE 0x113
#endif
#ifndef BTN_EXTRA
#define BTN_EXTRA 0x114
#endif

#endif /* CDE_EVDEV_COMPAT_H */
