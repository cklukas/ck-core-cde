/*
 * Fuzz target: the window property readers of dtwm (dtwm/WmProperty.c).
 *
 * run-fuzz.py extracts GetNormalHints, ProcessWmProtocols, ProcessMwmMessages,
 * GetWMState, GetMwmHints, GetMwmInfo, ProcessWmColormapWindows and
 * GetUtf8String verbatim.  The readers see the reply of XGetWindowProperty,
 * which a client controls completely, so the stand-in for it is driven by the
 * fuzz input and keeps the promises of Xlib: it never returns more items than
 * were asked for, returns format 32 items as an array of long, ends the data
 * with a NUL byte, and answers a property of another type than the requested
 * one with no items and no data beyond that NUL.
 *
 * Input layout:
 *   byte 0   reader: 0 GetWMState, 1 GetMwmInfo, 2 GetUtf8String, 3 GetMwmHints,
 *            4 GetNormalHints, 5 ProcessWmColormapWindows, 6 ProcessWmProtocols,
 *            7 ProcessMwmMessages (modulo 8)
 *   byte 1   bit 0-1: 0 success, else a failing call; bit 2: the property has
 *            the type the reader asks for; bit 3-4: format 8, 16, 32 or none;
 *            bit 5: the window has no such property (HasProperty); bit 6: one
 *            byte per item of format 32 instead of all eight; bit 7: window
 *            lookups find a colormap
 *   byte 2-3 item count the property holds (little endian)
 *   byte 4   bytes left after the part read
 *   rest     the data of the property
 */
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

typedef struct { int x, y; } AspectRatio;
#include "WmICCC.h"
typedef struct PropMwmInfo PropMwmInfo;
typedef struct PropMwmHints PropMwmHints;
#define PROP_MWM_INFO_ELEMENTS 2     /* from the Motif header MwmUtil.h */
#define PROP_MWM_HINTS_ELEMENTS 5
#define PROTOCOL_WM_SAVE_YOURSELF	(1L << 0)
#define PROTOCOL_WM_DELETE_WINDOW	(1L << 1)
#define PROTOCOL_WM_TAKE_FOCUS		(1L << 2)
#define PROTOCOL_MWM_MESSAGES		(1L << 3)
#define PROTOCOL_MWM_OFFSET		(1L << 4)
#define MAX_CLIENT_PROTOCOL_COUNT	40
#define MAX_COLORMAP_WINDOWS_COUNT	40
#define MAX_MWM_MESSAGES_COUNT		40
#define DISPLAY ((Display *)0)
#define GETMESSAGE(a, b, c) (c)

static struct {
    Atom xa_WM_STATE, xa_MWM_INFO, xa_UTF8_STRING, xa_WM_COLORMAP_WINDOWS, xa_MWM_HINTS, xa_WM_PROTOCOLS,
         xa_MWM_MESSAGES, xa_WM_SAVE_YOURSELF, xa_WM_TAKE_FOCUS, xa_WM_DELETE_WINDOW, xa_MWM_OFFSET;
    XContext windowContextType;
} wmGD = {11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 0};

typedef struct {
    Window client, *cmapWindows;
    Colormap clientColormap, *clientCmapList;
    int clientCmapCount, clientCmapIndex, *clientCmapFlags;
    Atom *clientProtocols;
    int clientProtocolCount;
    unsigned long protocolFlags;
    long *mwmMessages;
    int mwmMessagesCount;
} ClientData;

static SizeHints sizeHints;

/* the fuzz input */
static uint8_t flags, count_lo, count_hi, left_after;
static const uint8_t *payload;
static size_t payload_size;

static void Warning(char *s) { (void)s; }
static int HasProperty(ClientData *c, Atom a) { (void)c; (void)a; return !(flags & 0x20); }
static void *XtMalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) abort(); return p; }
static void *XtCalloc(size_t n, size_t s) { void *p = calloc(n ? n : 1, s ? s : 1); if (!p) abort(); return p; }
static void XtFree(char *p) { free(p); }
static Colormap FindColormap(ClientData *c, Window w) { (void)c; return (flags & 0x80) ? w + 100 : None; }
static void ResetColormapData(ClientData *c, Window *w, int n) { (void)c; (void)w; (void)n; }
int XFindContext(Display *d, XID w, XContext c, XPointer *p) { (void)d; (void)w; (void)c; (void)p; return 0; }
int XChangeWindowAttributes(Display *d, Window w, unsigned long m, XSetWindowAttributes *a)
{ (void)d; (void)w; (void)m; (void)a; return 0; }
Status XGetWindowAttributes(Display *d, Window w, XWindowAttributes *a)
{ (void)d; (void)w; memset(a, 0, sizeof *a); a->colormap = 7; return 1; }
int XFree(void *p) { free(p); return 0; }

/* Xlib: reply of a request for LONG_LENGTH 32-bit units of property PROP */
int XGetWindowProperty(Display *d, Window w, Atom p, long o, long l, Bool del, Atom req, Atom *t, int *f,
                       unsigned long *n, unsigned long *left, unsigned char **data)
{
    static const int formats[4] = {8, 16, 32, 0};
    (void)d; (void)w; (void)p; (void)o; (void)del;
    if (flags & 3) return BadWindow;                      /* the call fails, nothing is returned */

    int format = formats[(flags >> 3) & 3];
    unsigned long have = (unsigned long)count_lo | ((unsigned long)count_hi << 8);
    *left = left_after;
    if (format == 0) {                                     /* no such property */
        *t = None; *f = 0; *n = 0; *data = NULL;
        return Success;
    }
    Atom type = req;
    if (!(flags & 4)) type = req == AnyPropertyType ? XA_STRING : req + 1;
    *t = type; *f = format;
    if (req != AnyPropertyType && type != req) {           /* wrong type: nothing read */
        *n = 0;
        *left = have * (format / 8);
        *data = calloc(1, 1);
        return Success;
    }
    unsigned long max = (unsigned long)l * 4 / (format / 8);
    unsigned long n_items = have < max ? have : max;
    size_t unit = format == 32 ? sizeof(long) : format / 8;
    uint8_t *buf = calloc(n_items * unit + 1, 1);
    for (unsigned long i = 0; i < n_items; i++) {
        if (format == 8) {
            buf[i] = i < payload_size ? payload[i] : 0;
        } else if (format == 16) {
            unsigned short v = 0;
            for (int b = 0; b < 2; b++) v |= (unsigned short)((2 * i + b < payload_size ? payload[2 * i + b] : 0) << (8 * b));
            memcpy(buf + 2 * i, &v, 2);
        } else {
            unsigned long v = 0;
            if (flags & 0x40) {
                v = i < payload_size ? payload[i] : 0;
            } else {
                for (int b = 0; b < 8; b++) v |= (unsigned long)((8 * i + b < payload_size ? payload[8 * i + b] : 0)) << (8 * b);
            }
            memcpy(buf + 8 * i, &v, 8);
        }
    }
    *n = n_items;
    *data = buf;
    return Success;
}

#include FUZZ_PRODUCTION_H

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 5) return 0;
    int reader = data[0] & 7;
    flags = data[1]; count_lo = data[2]; count_hi = data[3]; left_after = data[4];
    payload = data + 5; payload_size = size - 5;

    ClientData client;
    memset(&client, 0, sizeof client);
    client.client = 1;
    client.clientColormap = 101;

    void *p;
    switch (reader) {
    case 0: p = GetWMState(1); if (p) XFree(p); break;
    case 1: p = GetMwmInfo(1); if (p) XFree(p); break;
    case 2: p = GetUtf8String(NULL, 1, 99); if (p) { (void)strlen(p); XFree(p); } break;
    case 3: p = GetMwmHints(&client); if (p) XFree(p); break;
    case 4: (void)GetNormalHints(&client); break;
    case 5:
        ProcessWmColormapWindows(&client);
        XtFree((char *)client.cmapWindows);
        XtFree((char *)client.clientCmapList);
        XtFree((char *)client.clientCmapFlags);
        break;
    case 6:
        ProcessWmProtocols(&client);
        XtFree((char *)client.clientProtocols);
        break;
    case 7:
        ProcessMwmMessages(&client);
        XtFree((char *)client.mwmMessages);
        break;
    }
    return 0;
}
