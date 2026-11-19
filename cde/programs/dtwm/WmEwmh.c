/*
 * CDE - Common Desktop Environment
 *
 * (c) Copyright 1993-2012 The Open Group
 * (c) Copyright 2012-2022 CDE Project contributors, see
 * CONTRIBUTORS for details
 *
 * These libraries and programs are free software; you can
 * redistribute them and/or modify them under the terms of the GNU
 * Lesser General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * These libraries and programs are distributed in the hope that
 * they will be useful, but WITHOUT ANY WARRANTY; without even the
 * implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE. See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with these libraries and programs; if not, write
 * to the Free Software Foundation, Inc., 51 Franklin Street, Fifth
 * Floor, Boston, MA 02110-1301 USA
 */

#include <ctype.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "WmGlobal.h"
#include "WmEwmh.h"
#include "WmEvent.h"
#include "WmFunction.h"
#include "WmKeyFocus.h"
#include "WmMultiHead.h"
#include "WmProperty.h"
#include "WmWinState.h"
#include "WmWrkspace.h"
#include <X11/XKBlib.h>
#include <X11/keysym.h>

static Boolean g_superTapPending = False;
static Time g_superTapPressTime = 0;

/*WP-13: Hardware keys, 3D bevel OSD, and XInput2 3-finger swipe gestures */
#define DTWM_X11_OSD_W 336
#define DTWM_X11_OSD_H 84

static Atom g_xaDtOsdState = None;
static Atom g_xaDtWmSwipe = None;
static Atom g_xaDtWmRegrabKeys = None;
static Window g_osdWin = None;
static Pixmap g_osdPixmap = None;
static WmScreenData *g_osdSD = nullptr;
static XtIntervalId g_osdTimer = 0;
static char g_osdLastKind[32] = "volume";
static int g_osdLastLevel = 0;
static int g_osdLastMuted = 0;
static char g_osdLastLabel[128] = "";

static int g_xiOpcode = -1;
static Boolean g_xiSwipeActive = False;
static int g_xiSwipeFingers = 0;
static double g_xiSwipeDx = 0.0;
static double g_xiSwipeDy = 0.0;

/* WP-20: X11 AccessX (XKB), Visual Bell, Reduced Motion & Magnifier state */
static int g_xkbEventBase = -1;
static Atom g_xaDtA11yState = None;
static Atom g_xaCkMagnifierZoom = None;
static Boolean g_x11StickyKeys = False;
static Boolean g_x11StickyTwoKeysOff = True;
static unsigned int g_x11LatchedMods = 0;
static unsigned int g_x11LockedMods = 0;
static Boolean g_x11SlowKeys = False;
static int g_x11SlowKeysMs = 300;
static Boolean g_x11BounceKeys = False;
static int g_x11BounceKeysMs = 300;
static int g_x11VbellMode = 0; /* 0=off, 1=window, 2=screen */
static unsigned int g_x11VbellCount = 0;
static Boolean g_x11ReducedMotion = False;
static Boolean g_savedAnimateMinValid = False;
static Boolean g_savedAnimateMin = False;
static int g_x11ConfiguredZoomPct = 200;
static int g_x11ZoomPct = 100;
static Window g_vbellWin = None;
static WmScreenData *g_vbellSD = nullptr;
static XtIntervalId g_vbellTimer = 0;

typedef struct {
    const char *keysym_name;
    const char *media_action;
    const char *exec_cmd;
    const char *tile_dir;
    unsigned int req_mods;
} WmHwKeyBinding;

static const WmHwKeyBinding g_hwKeyBindings[] = {
    { "XF86AudioRaiseVolume",  "vol-up",         nullptr, nullptr, 0 },
    { "XF86AudioLowerVolume",  "vol-down",       nullptr, nullptr, 0 },
    { "XF86AudioMute",         "vol-mute",       nullptr, nullptr, 0 },
    { "XF86AudioMicMute",      "mic-mute",       nullptr, nullptr, 0 },
    { "XF86MonBrightnessUp",   "bright-up",      nullptr, nullptr, 0 },
    { "XF86MonBrightnessDown", "bright-down",    nullptr, nullptr, 0 },
    { "XF86KbdBrightnessUp",   "kbd-bright-up",  nullptr, nullptr, 0 },
    { "XF86KbdBrightnessDown", "kbd-bright-down",nullptr, nullptr, 0 },
    { "XF86AudioPlay",         "media-play",     nullptr, nullptr, 0 },
    { "XF86AudioPause",        "media-pause",    nullptr, nullptr, 0 },
    { "XF86AudioNext",         "media-next",     nullptr, nullptr, 0 },
    { "XF86AudioPrev",         "media-prev",     nullptr, nullptr, 0 },
    { "XF86AudioStop",         "media-stop",     nullptr, nullptr, 0 },
    { "Print",                 nullptr,          "ck-grab", nullptr, 0 },
    { "Print",                 nullptr,          "ck-grab --region --clipboard", nullptr, ShiftMask },
    { "l",                     nullptr,          "dtaction LockDisplay", nullptr, Mod4Mask },
    { "Left",                  nullptr,          nullptr, "left",  Mod4Mask },
    { "Right",                 nullptr,          nullptr, "right", Mod4Mask },
    { "Up",                    nullptr,          nullptr, "up",    Mod4Mask },
    { "Down",                  nullptr,          nullptr, "down",  Mod4Mask },
    { "equal",                 nullptr,          "ck-magnifier --in", nullptr, Mod4Mask },
    { "equal",                 nullptr,          "ck-magnifier --in", nullptr, Mod4Mask | ShiftMask },
    { "KP_Add",                nullptr,          "ck-magnifier --in", nullptr, Mod4Mask },
    { "minus",                 nullptr,          "ck-magnifier --out", nullptr, Mod4Mask },
    { "minus",                 nullptr,          "ck-magnifier --out", nullptr, Mod4Mask | ShiftMask },
    { "KP_Subtract",           nullptr,          "ck-magnifier --out", nullptr, Mod4Mask },
    { "0",                     nullptr,          "ck-magnifier --reset", nullptr, Mod4Mask },
    { "KP_0",                  nullptr,          "ck-magnifier --reset", nullptr, Mod4Mask },
};

static int
IgnoreXGrabError (Display *dpy, XErrorEvent *err)
{
    (void)dpy;
    (void)err;
    return 0;
}

static void
PublishX11OsdState (WmScreenData *pSD, int visible)
{
    char state[256];
    if (!pSD || !g_xaDtOsdState)
	return;
    snprintf (state, sizeof(state),
	      "visible=%d;kind=%s;level=%d;muted=%d;label=%s",
	      visible ? 1 : 0, g_osdLastKind, g_osdLastLevel,
	      g_osdLastMuted, g_osdLastLabel);
    XChangeProperty (DISPLAY, pSD->rootWindow, g_xaDtOsdState, XA_STRING, 8,
		     PropModeReplace, (unsigned char *)state, (int)strlen(state));
}

static void
OsdHideTimeoutCB (XtPointer client_data, XtIntervalId *id)
{
    WmScreenData *pSD = (WmScreenData *)client_data;
    (void)id;
    g_osdTimer = 0;
    if (g_osdWin != None)
	XUnmapWindow (DISPLAY, g_osdWin);
    if (pSD)
	PublishX11OsdState (pSD, 0);
    XFlush (DISPLAY);
}

static void
DrawX11BevelRect (Display *dpy, Drawable d, GC gcTop, GC gcBot,
		  int x, int y, int w, int h, int thick, Boolean sunken)
{
    GC tl = sunken ? gcBot : gcTop;
    GC br = sunken ? gcTop : gcBot;
    int i;
    for (i = 0; i < thick; ++i)
    {
	XDrawLine (dpy, d, tl, x + i, y + i, x + w - 1 - i, y + i);
	XDrawLine (dpy, d, tl, x + i, y + i, x + i, y + h - 1 - i);
	XDrawLine (dpy, d, br, x + i, y + h - 1 - i, x + w - 1 - i, y + h - 1 - i);
	XDrawLine (dpy, d, br, x + w - 1 - i, y + i, x + w - 1 - i, y + h - 1 - i);
    }
}

static void
DrawX11OsdIcon (Display *dpy, Drawable d, GC fgGC, GC hiGC,
		const char *kind, int level, int muted,
		int ix, int iy, int iw, int ih)
{
    int cx = ix + iw / 2;
    int cy = iy + ih / 2;

    if (kind && strcmp (kind, "mic") == 0)
    {
	XFillRectangle (dpy, d, fgGC, cx - 5, cy - 12, 10, 15);
	XDrawLine (dpy, d, fgGC, cx - 9, cy - 2, cx - 9, cy + 5);
	XDrawLine (dpy, d, fgGC, cx + 9, cy - 2, cx + 9, cy + 5);
	XDrawLine (dpy, d, fgGC, cx - 9, cy + 5, cx + 9, cy + 5);
	XDrawLine (dpy, d, fgGC, cx, cy + 5, cx, cy + 12);
	XDrawLine (dpy, d, fgGC, cx - 7, cy + 12, cx + 7, cy + 12);
	if (muted)
	    XDrawLine (dpy, d, hiGC, ix + 8, iy + 8, ix + iw - 8, iy + ih - 8);
    }
    else if (kind && (strcmp (kind, "brightness") == 0 ||
		      strcmp (kind, "kbd-brightness") == 0))
    {
	int r;
	XFillArc (dpy, d, hiGC, cx - 7, cy - 7, 14, 14, 0, 360 * 64);
	for (r = 0; r < 8; ++r)
	{
	    double a = (double)r * (3.14159265358979323846 / 4.0);
	    int x1 = cx + (int)lround (cos (a) * 10.0);
	    int y1 = cy + (int)lround (sin (a) * 10.0);
	    int x2 = cx + (int)lround (cos (a) * 15.0);
	    int y2 = cy + (int)lround (sin (a) * 15.0);
	    XDrawLine (dpy, d, fgGC, x1, y1, x2, y2);
	}
    }
    else if (kind && strcmp (kind, "media") == 0)
    {
	XPoint pts[3];
	pts[0].x = (short)(cx - 6); pts[0].y = (short)(cy - 10);
	pts[1].x = (short)(cx - 6); pts[1].y = (short)(cy + 10);
	pts[2].x = (short)(cx + 10); pts[2].y = (short)cy;
	XFillPolygon (dpy, d, hiGC, pts, 3, Convex, CoordModeOrigin);
    }
    else
    {
	XPoint cone[4];
	XFillRectangle (dpy, d, fgGC, cx - 13, cy - 5, 6, 10);
	cone[0].x = (short)(cx - 7); cone[0].y = (short)(cy - 5);
	cone[1].x = (short)(cx + 1); cone[1].y = (short)(cy - 12);
	cone[2].x = (short)(cx + 1); cone[2].y = (short)(cy + 12);
	cone[3].x = (short)(cx - 7); cone[3].y = (short)(cy + 5);
	XFillPolygon (dpy, d, fgGC, cone, 4, Convex, CoordModeOrigin);
	if (muted || level <= 0)
	{
	    XDrawLine (dpy, d, hiGC, cx + 5, cy - 6, cx + 13, cy + 6);
	    XDrawLine (dpy, d, hiGC, cx + 13, cy - 6, cx + 5, cy + 6);
	}
	else
	{
	    XDrawArc (dpy, d, hiGC, cx + 1, cy - 7, 8, 14, -60 * 64, 120 * 64);
	    if (level >= 50)
		XDrawArc (dpy, d, hiGC, cx + 3, cy - 12, 13, 24, -60 * 64, 120 * 64);
	}
    }
}

static void
ShowX11HardwareOsd (WmScreenData *pSD, const char *kind, int level,
		    int muted, const char *label)
{
    int sw, sh, ox, oy;
    GC gcBg, gcFg, gcTop, gcBot, gcBar, gcWell;
    XGCValues gcv;
    Colormap cmap;
    XColor exact, emerald;
    Boolean hasGauge = True;

    if (!pSD)
	return;
    if (level < 0)
    {
	hasGauge = False;
	level = 0;
    }
    if (level > 100)
	level = 100;

    snprintf (g_osdLastKind, sizeof(g_osdLastKind), "%s", kind ? kind : "volume");
    g_osdLastLevel = level;
    g_osdLastMuted = muted ? 1 : 0;
    snprintf (g_osdLastLabel, sizeof(g_osdLastLabel), "%s", label ? label : "");

    sw = DisplayWidth (DISPLAY, pSD->screen);
    sh = DisplayHeight (DISPLAY, pSD->screen);
    ox = (sw - DTWM_X11_OSD_W) / 2;
    oy = sh - DTWM_X11_OSD_H - 84;
    if (ox < 8) ox = 8;
    if (oy < 8) oy = 8;

    if (g_osdWin != None && g_osdSD != pSD)
    {
	if (g_osdPixmap != None)
	{
	    XFreePixmap (DISPLAY, g_osdPixmap);
	    g_osdPixmap = None;
	}
	XDestroyWindow (DISPLAY, g_osdWin);
	g_osdWin = None;
    }
    g_osdSD = pSD;

    if (g_osdWin == None)
    {
	XSetWindowAttributes xswa;
	memset (&xswa, 0, sizeof(xswa));
	xswa.override_redirect = True;
	xswa.save_under = True;
	xswa.background_pixel = pSD->feedbackAppearance.background;
	xswa.border_pixel = pSD->feedbackAppearance.bottomShadowColor;
	g_osdWin = XCreateWindow (DISPLAY, pSD->rootWindow,
				  ox, oy, DTWM_X11_OSD_W, DTWM_X11_OSD_H, 0,
				  DefaultDepth (DISPLAY, pSD->screen),
				  InputOutput,
				  DefaultVisual (DISPLAY, pSD->screen),
				  CWOverrideRedirect | CWSaveUnder | CWBackPixel | CWBorderPixel,
				  &xswa);
    }
    else
    {
	XMoveResizeWindow (DISPLAY, g_osdWin, ox, oy, DTWM_X11_OSD_W, DTWM_X11_OSD_H);
    }

    if (g_osdPixmap == None)
    {
	g_osdPixmap = XCreatePixmap (DISPLAY, g_osdWin,
				     DTWM_X11_OSD_W, DTWM_X11_OSD_H,
				     (unsigned int)DefaultDepth (DISPLAY, pSD->screen));
    }

    memset (&gcv, 0, sizeof(gcv));
    gcv.foreground = pSD->feedbackAppearance.background;
    gcBg = XCreateGC (DISPLAY, g_osdPixmap, GCForeground, &gcv);
    gcv.foreground = pSD->feedbackAppearance.foreground;
    if (pSD->feedbackAppearance.font)
	gcv.font = pSD->feedbackAppearance.font->fid;
    gcFg = XCreateGC (DISPLAY, g_osdPixmap,
		      GCForeground | (pSD->feedbackAppearance.font ? GCFont : 0), &gcv);
    gcv.foreground = pSD->feedbackAppearance.topShadowColor;
    gcTop = XCreateGC (DISPLAY, g_osdPixmap, GCForeground, &gcv);
    gcv.foreground = pSD->feedbackAppearance.bottomShadowColor;
    gcBot = XCreateGC (DISPLAY, g_osdPixmap, GCForeground, &gcv);

    cmap = DefaultColormap (DISPLAY, pSD->screen);
    if (!muted && XAllocNamedColor (DISPLAY, cmap, "#2ecc71", &emerald, &exact))
	gcv.foreground = emerald.pixel;
    else
	gcv.foreground = muted ? pSD->feedbackAppearance.bottomShadowColor
			       : pSD->feedbackAppearance.activeBackground;
    gcBar = XCreateGC (DISPLAY, g_osdPixmap, GCForeground, &gcv);

    if (XAllocNamedColor (DISPLAY, cmap, "#1d2428", &emerald, &exact))
	gcv.foreground = emerald.pixel;
    else
	gcv.foreground = pSD->feedbackAppearance.bottomShadowColor;
    gcWell = XCreateGC (DISPLAY, g_osdPixmap, GCForeground, &gcv);

    XFillRectangle (DISPLAY, g_osdPixmap, gcBg, 0, 0, DTWM_X11_OSD_W, DTWM_X11_OSD_H);
    DrawX11BevelRect (DISPLAY, g_osdPixmap, gcTop, gcBot,
		      0, 0, DTWM_X11_OSD_W, DTWM_X11_OSD_H, 2, False);
    DrawX11BevelRect (DISPLAY, g_osdPixmap, gcTop, gcBot,
		      6, 6, DTWM_X11_OSD_W - 12, DTWM_X11_OSD_H - 12, 1, True);

    /* Left sunken icon well */
    XFillRectangle (DISPLAY, g_osdPixmap, gcWell, 16, 18, 48, 48);
    DrawX11BevelRect (DISPLAY, g_osdPixmap, gcTop, gcBot, 16, 18, 48, 48, 2, True);
    DrawX11OsdIcon (DISPLAY, g_osdPixmap, gcTop, gcBar,
		    g_osdLastKind, level, muted, 16, 18, 48, 48);

    /* Label text */
    if (g_osdLastLabel[0])
    {
	if (pSD->feedbackAppearance.fontList)
	{
	    XmString xs = XmStringCreateLocalized (g_osdLastLabel);
	    if (xs)
	    {
		XmStringDraw (DISPLAY, g_osdPixmap, pSD->feedbackAppearance.fontList,
			      xs, gcFg, 76, hasGauge ? 20 : 34,
			      DTWM_X11_OSD_W - 92, XmALIGNMENT_BEGINNING,
			      XmSTRING_DIRECTION_L_TO_R, nullptr);
		XmStringFree (xs);
	    }
	}
	else
	{
	    XDrawString (DISPLAY, g_osdPixmap, gcFg, 76, hasGauge ? 34 : 46,
			 g_osdLastLabel, (int)strlen(g_osdLastLabel));
	}
    }

    /* 10-segment sunken LED bar */
    if (hasGauge)
    {
	int bx = 76, by = 46, bw = DTWM_X11_OSD_W - 92, bh = 20;
	int segs = 10;
	int gap = 3;
	int innerW = bw - 6;
	int segW = (innerW - (segs - 1) * gap) / segs;
	int lit = (level * segs + 5) / 100;
	int s;
	if (level > 0 && lit == 0) lit = 1;
	XFillRectangle (DISPLAY, g_osdPixmap, gcWell, bx, by, bw, bh);
	DrawX11BevelRect (DISPLAY, g_osdPixmap, gcTop, gcBot, bx, by, bw, bh, 2, True);
	for (s = 0; s < segs; ++s)
	{
	    int sx = bx + 3 + s * (segW + gap);
	    if (s < lit)
		XFillRectangle (DISPLAY, g_osdPixmap, gcBar, sx, by + 3, segW, bh - 6);
	}
    }

    XFreeGC (DISPLAY, gcBg);
    XFreeGC (DISPLAY, gcFg);
    XFreeGC (DISPLAY, gcTop);
    XFreeGC (DISPLAY, gcBot);
    XFreeGC (DISPLAY, gcBar);
    XFreeGC (DISPLAY, gcWell);

    XSetWindowBackgroundPixmap (DISPLAY, g_osdWin, g_osdPixmap);
    XMapRaised (DISPLAY, g_osdWin);
    XClearWindow (DISPLAY, g_osdWin);
    PublishX11OsdState (pSD, 1);
    XFlush (DISPLAY);

    if (g_osdTimer)
	XtRemoveTimeOut (g_osdTimer);
    g_osdTimer = XtAppAddTimeOut (wmGD.mwmAppContext, 1600, OsdHideTimeoutCB, pSD);
}

static Boolean
InvokeX11MediaCtlAndOsd (WmScreenData *pSD, const char *action)
{
    const char *candidates[] = {
	"/usr/dt/bin/dtwm-media-ctl",
	CDE_INSTALLATION_TOP "/bin/dtwm-media-ctl",
	"dtwm-media-ctl",
	nullptr
    };
    char cmd[512];
    char line[256];
    FILE *fp = nullptr;
    int i;

    for (i = 0; candidates[i]; ++i)
    {
	if (candidates[i][0] == '/' && access (candidates[i], X_OK) != 0)
	    continue;
	snprintf (cmd, sizeof(cmd), "%s %s 2>/dev/null", candidates[i], action);
	fp = popen (cmd, "r");
	if (fp)
	    break;
    }
    if (!fp)
	return False;

    line[0] = '\0';
    if (fgets (line, sizeof(line), fp) == nullptr)
	line[0] = '\0';
    pclose (fp);

    if (strncmp (line, "OSD|", 4) == 0)
    {
	char *nl = strchr (line, '\n');
	char *saveptr = nullptr;
	char *tok_osd, *tok_kind, *tok_lvl, *tok_mute, *tok_lbl;
	if (nl) *nl = '\0';
	tok_osd = strtok_r (line, "|", &saveptr);
	tok_kind = strtok_r (nullptr, "|", &saveptr);
	tok_lvl = strtok_r (nullptr, "|", &saveptr);
	tok_mute = strtok_r (nullptr, "|", &saveptr);
	tok_lbl = strtok_r (nullptr, "", &saveptr);
	(void)tok_osd;
	if (tok_kind && tok_lvl && tok_mute)
	{
	    ShowX11HardwareOsd (pSD, tok_kind, atoi (tok_lvl), atoi (tok_mute),
				tok_lbl ? tok_lbl : tok_kind);
	    return True;
	}
    }
    return False;
}

static WmScreenData *
FindScreenForRoot (Window root)
{
    int scr;
    for (scr = 0; scr < wmGD.numScreens; ++scr)
    {
	if (wmGD.Screens[scr].managed && wmGD.Screens[scr].rootWindow == root)
	    return &wmGD.Screens[scr];
    }
    return ACTIVE_PSD ? ACTIVE_PSD : &wmGD.Screens[0];
}

static void
SwitchX11WorkspaceRelative (WmScreenData *pSD, int delta)
{
    int idx = 0, i, target;
    if (!pSD || pSD->numWorkspaces <= 1 || !pSD->pActiveWS)
	return;
    for (i = 0; i < pSD->numWorkspaces; ++i)
    {
	if (&pSD->pWS[i] == pSD->pActiveWS)
	{
	    idx = i;
	    break;
	}
    }
    target = (idx + delta) % pSD->numWorkspaces;
    if (target < 0)
	target += pSD->numWorkspaces;
    if (target != idx)
	ChangeToWorkspace (&pSD->pWS[target]);
}

static void
GrabEwmhHardwareKeys (WmScreenData *pSD)
{
    int (*oldHandler)(Display *, XErrorEvent *);
    size_t i;
    static const unsigned int lockVariants[4] = {
	0, LockMask, Mod2Mask, LockMask | Mod2Mask
    };

    if (!pSD || !pSD->managed)
	return;

    oldHandler = XSetErrorHandler (IgnoreXGrabError);
    for (i = 0; i < sizeof(g_hwKeyBindings) / sizeof(g_hwKeyBindings[0]); ++i)
    {
	KeySym ks = XStringToKeysym (g_hwKeyBindings[i].keysym_name);
	KeyCode kc;
	int v;
	if (ks == NoSymbol)
	    continue;
	kc = XKeysymToKeycode (DISPLAY, ks);
	if (kc == 0)
	    continue;
	for (v = 0; v < 4; ++v)
	{
	    XGrabKey (DISPLAY, kc, g_hwKeyBindings[i].req_mods | lockVariants[v],
		      pSD->rootWindow, True, GrabModeAsync, GrabModeAsync);
	}
    }
    XSync (DISPLAY, False);
    XSetErrorHandler (oldHandler);
}

static void
SetupXInput2TouchpadGestures (WmScreenData *pSD)
{
    typedef int (*pfn_XIQueryVersion)(Display *, int *, int *);
    typedef int (*pfn_XISelectEvents)(Display *, Window, void *, int);
    struct LocalXIEventMask {
	int deviceid;
	int mask_len;
	unsigned char *mask;
    } evmask;
    unsigned char maskBytes[8];
    void *xiLib;
    pfn_XIQueryVersion fnQueryVersion;
    pfn_XISelectEvents fnSelectEvents;
    int evBase = 0, errBase = 0;
    int maj = 2, min = 4;

    if (!pSD || !pSD->managed)
	return;
    if (!XQueryExtension (DISPLAY, "XInputExtension", &g_xiOpcode, &evBase, &errBase))
    {
	g_xiOpcode = -1;
	return;
    }

    xiLib = dlopen ("libXi.so.6", RTLD_LAZY | RTLD_LOCAL);
    if (!xiLib)
	return;
    fnQueryVersion = (pfn_XIQueryVersion)dlsym (xiLib, "XIQueryVersion");
    fnSelectEvents = (pfn_XISelectEvents)dlsym (xiLib, "XISelectEvents");
    if (!fnQueryVersion || !fnSelectEvents)
	return;
    if (fnQueryVersion (DISPLAY, &maj, &min) != Success)
	return;
    if (maj < 2 || (maj == 2 && min < 4))
	return;

    /* XI_GestureSwipeBegin=27, XI_GestureSwipeUpdate=28, XI_GestureSwipeEnd=29 */
    memset (maskBytes, 0, sizeof(maskBytes));
    maskBytes[27 >> 3] |= (unsigned char)(1u << (27 & 7));
    maskBytes[28 >> 3] |= (unsigned char)(1u << (28 & 7));
    maskBytes[29 >> 3] |= (unsigned char)(1u << (29 & 7));
    evmask.deviceid = 1; /* XIAllMasterDevices */
    evmask.mask_len = (int)sizeof(maskBytes);
    evmask.mask = maskBytes;
    fnSelectEvents (DISPLAY, pSD->rootWindow, &evmask, 1);
}

static void ProcessNetWmStateAbove (ClientData *pCD, long action);
static void ProcessNetWmStateBelow (ClientData *pCD, long action);

static void ProcessNetWmStateMaximized (ClientData *pCD, long action)
{
    int newState;

    switch (action)
    {
	case _NET_WM_STATE_REMOVE:
	    if (pCD->clientState != MAXIMIZED_STATE) return;
	    newState = NORMAL_STATE;
	    break;
	case _NET_WM_STATE_ADD:
	    if (pCD->clientState == MAXIMIZED_STATE) return;
	    newState = MAXIMIZED_STATE;
	    break;
	case _NET_WM_STATE_TOGGLE:
	    newState = pCD->clientState == MAXIMIZED_STATE ?
		NORMAL_STATE : MAXIMIZED_STATE;
	    break;
	default:
	    return;
    }

    SetClientState (pCD, newState, GetTimestamp ());
}

static void ProcessNetWmStateFullscreen (ClientData *pCD, long action)
{
    Boolean fullscreen = pCD->fullscreen;

    switch (action)
    {
	case _NET_WM_STATE_REMOVE:
	    if (!fullscreen) return;
	    fullscreen = False;
	    break;
	case _NET_WM_STATE_ADD:
	    if (fullscreen) return;
	    fullscreen = True;
	    break;
	case _NET_WM_STATE_TOGGLE:
	    fullscreen = !fullscreen;
	    break;
	default:
	    return;
    }

    pCD->fullscreen = False;
    SetClientState (pCD, NORMAL_STATE, GetTimestamp ());

    if (fullscreen) {
	pCD->fullscreen = True;
	SetClientState (pCD, MAXIMIZED_STATE, GetTimestamp ());
    }
}

static void ProcessNetWmStateAbove (ClientData *pCD, long action)
{
    if (action == _NET_WM_STATE_TOGGLE)
	action = pCD == pCD->pSD->topClient ? _NET_WM_STATE_REMOVE :
					      _NET_WM_STATE_ADD;

    switch (action)
    {
	case _NET_WM_STATE_REMOVE:
	    if (pCD->pSD->topClient == pCD) pCD->pSD->topClient = nullptr;
	    break;
	case _NET_WM_STATE_ADD:
	    pCD->pSD->topClient = pCD;
	    ProcessNetWmStateBelow (pCD, _NET_WM_STATE_REMOVE);
	    Do_Raise (pCD, nullptr, STACK_NORMAL);
	    break;
	default:
	    return;
    }

    UpdateNetWmState (pCD->client, &wmGD.xa__NET_WM_STATE_ABOVE, 1, action);
}

static void ProcessNetWmStateBelow (ClientData *pCD, long action)
{
    if (action == _NET_WM_STATE_TOGGLE)
	action = pCD == pCD->pSD->bottomClient ? _NET_WM_STATE_REMOVE :
						 _NET_WM_STATE_ADD;

    switch (action)
    {
	case _NET_WM_STATE_REMOVE:
	    if (pCD->pSD->bottomClient == pCD) pCD->pSD->bottomClient = nullptr;
	    break;
	case _NET_WM_STATE_ADD:
	    pCD->pSD->bottomClient = pCD;
	    ProcessNetWmStateAbove (pCD, _NET_WM_STATE_REMOVE);
	    Do_Lower (pCD, nullptr, STACK_NORMAL);
	    break;
	default:
	    return;
    }

    UpdateNetWmState (pCD->client, &wmGD.xa__NET_WM_STATE_BELOW, 1, action);
}

/**
 * @brief Processes the _NET_WM_FULLSCREEN_MONITORS protocol.
 *
 * @param pCD
 * @param top
 * @param bottom
 * @param left
 * @param right
 */
void ProcessNetWmFullscreenMonitors (ClientData *pCD,
    int top, int bottom, int left, int right)
{
    WmHeadInfo_t *pHeadInfo;

    pCD->fullscreenAuto = True;

    if (!(pHeadInfo = GetHeadInfoById (top))) return;
    pCD->fullscreenY = pHeadInfo->y_org;
    free(pHeadInfo);

    if (!(pHeadInfo = GetHeadInfoById (bottom))) return;
    pCD->fullscreenHeight = top == bottom ? pHeadInfo->height :
	    pHeadInfo->y_org + pHeadInfo->height;
    free(pHeadInfo);

    if (!(pHeadInfo = GetHeadInfoById (left))) return;
    pCD->fullscreenX = pHeadInfo->x_org;
    free(pHeadInfo);

    if (!(pHeadInfo = GetHeadInfoById (right))) return;
    pCD->fullscreenWidth = left == right ? pHeadInfo->width :
	    pHeadInfo->x_org + pHeadInfo->width;
    free(pHeadInfo);

    pCD->fullscreenAuto = False;
}

/**
 * @brief Processes the _NET_WM_STATE client message.
 *
 * @param pCD
 * @param action
 * @param firstProperty
 * @param secondProperty
 */
void ProcessNetWmState (ClientData *pCD, long action,
    Atom firstProperty, Atom secondProperty)
{
    if (pCD->clientState & UNSEEN_STATE) return;

    if (firstProperty  == wmGD.xa__NET_WM_STATE_MAXIMIZED_VERT &&
	secondProperty == wmGD.xa__NET_WM_STATE_MAXIMIZED_HORZ ||
	firstProperty  == wmGD.xa__NET_WM_STATE_MAXIMIZED_HORZ &&
	secondProperty == wmGD.xa__NET_WM_STATE_MAXIMIZED_VERT)
	ProcessNetWmStateMaximized (pCD, action);
    else if (firstProperty  == wmGD.xa__NET_WM_STATE_FULLSCREEN ||
	     secondProperty == wmGD.xa__NET_WM_STATE_FULLSCREEN)
	ProcessNetWmStateFullscreen (pCD, action);
    else if (firstProperty  == wmGD.xa__NET_WM_STATE_ABOVE ||
	     secondProperty == wmGD.xa__NET_WM_STATE_ABOVE)
	ProcessNetWmStateAbove (pCD, action);
    else if (firstProperty  == wmGD.xa__NET_WM_STATE_BELOW ||
	     secondProperty == wmGD.xa__NET_WM_STATE_BELOW)
	ProcessNetWmStateBelow (pCD, action);

    if (!ClientInWorkspace (ACTIVE_WS, pCD))
	SetClientState (pCD, pCD->clientState | UNSEEN_STATE, GetTimestamp ());
}

void ProcessNetActiveWindow (ClientData *pCD, Time timestamp)
{
    XEvent ev;
    Time t;

    if (!pCD || !pCD->pSD) return;
    t = timestamp ? timestamp : GetTimestamp ();

    if (!ClientInWorkspace (pCD->pSD->pActiveWS, pCD))
    {
	int ws;
	for (ws = 0; ws < pCD->pSD->numWorkspaces; ++ws)
	{
	    if (ClientInWorkspace (&pCD->pSD->pWS[ws], pCD))
	    {
		ChangeToWorkspace (&pCD->pSD->pWS[ws]);
		break;
	    }
	}
    }

    memset (&ev, 0, sizeof(ev));
    ev.xbutton.time = t;
    F_Normalize_And_Raise (nullptr, pCD, &ev);
    Do_Focus_Key (pCD, t, ALWAYS_SET_FOCUS);
}

void UpdateEwmhClientList (WmScreenData *pSD)
{
    ClientListEntry *pEntry;
    Window *wins = nullptr;
    int cap = 0;
    int count = 0;

    if (!pSD || !pSD->managed || !wmGD.xa__NET_CLIENT_LIST) return;

    cap = (int)pSD->clientCounter + 8;
    if (cap < 16) cap = 16;
    wins = (Window *)calloc ((size_t)cap, sizeof(Window));
    if (!wins) return;

    for (pEntry = pSD->clientList; pEntry; pEntry = pEntry->nextSibling)
    {
	ClientData *pCD = pEntry->pCD;
	int i;
	Boolean dup = False;

	if (!pCD || !pCD->client) continue;
	if (pCD->clientFlags & CLIENT_DESTROYED) continue;
	if (pSD->taskSwitchWin && pCD->client == pSD->taskSwitchWin) continue;

	for (i = 0; i < count; ++i)
	{
	    if (wins[i] == pCD->client)
	    {
		dup = True;
		break;
	    }
	}
	if (dup) continue;

	if (count >= cap)
	{
	    Window *grown;
	    cap *= 2;
	    grown = (Window *)realloc (wins, (size_t)cap * sizeof(Window));
	    if (!grown) break;
	    wins = grown;
	}
	wins[count++] = pCD->client;
    }

    XChangeProperty (DISPLAY, pSD->rootWindow, wmGD.xa__NET_CLIENT_LIST,
		     XA_WINDOW, 32, PropModeReplace,
		     (unsigned char *)wins, count);
    XChangeProperty (DISPLAY, pSD->rootWindow, wmGD.xa__NET_CLIENT_LIST_STACKING,
		     XA_WINDOW, 32, PropModeReplace,
		     (unsigned char *)wins, count);
    free (wins);
}

void UpdateEwmhActiveWindow (WmScreenData *pSD, ClientData *pCD)
{
    Window activeWin = None;
    if (!pSD || !pSD->managed || !wmGD.xa__NET_ACTIVE_WINDOW) return;
    if (pCD && pCD->client) activeWin = pCD->client;
    XChangeProperty (DISPLAY, pSD->rootWindow, wmGD.xa__NET_ACTIVE_WINDOW,
		     XA_WINDOW, 32, PropModeReplace,
		     (unsigned char *)&activeWin, 1);
}

void CancelEwmhSuperTap (void)
{
    g_superTapPending = False;
}

Boolean HandleEwmhSuperKey (XKeyEvent *keyEvent, Boolean isPress)
{
    KeySym ks;
    if (!keyEvent) return False;
    ks = XkbKeycodeToKeysym (DISPLAY, (KeyCode)keyEvent->keycode, 0, 0);

    if (isPress && ks != NoSymbol)
    {
	unsigned int cleanMods = keyEvent->state & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask);
	size_t i;
	for (i = 0; i < sizeof(g_hwKeyBindings) / sizeof(g_hwKeyBindings[0]); ++i)
	{
	    KeySym targetKs = XStringToKeysym (g_hwKeyBindings[i].keysym_name);
	    if (targetKs != NoSymbol && ks == targetKs &&
		cleanMods == g_hwKeyBindings[i].req_mods)
	    {
		WmScreenData *pSD = FindScreenForRoot (keyEvent->root);
		g_superTapPending = False;
		if (g_hwKeyBindings[i].media_action)
		{
		    InvokeX11MediaCtlAndOsd (pSD, g_hwKeyBindings[i].media_action);
		    return True;
		}
		if (g_hwKeyBindings[i].exec_cmd)
		{
		    XEvent ev;
		    memset (&ev, 0, sizeof(ev));
		    ev.xkey = *keyEvent;
		    F_Exec ((char *)g_hwKeyBindings[i].exec_cmd, nullptr, &ev);
		    return True;
		}
		if (g_hwKeyBindings[i].tile_dir)
		{
		    XEvent ev;
		    memset (&ev, 0, sizeof(ev));
		    ev.xkey = *keyEvent;
		    F_Tile_Window ((String)g_hwKeyBindings[i].tile_dir, wmGD.keyboardFocus, &ev);
		    return True;
		}
	    }
	}
    }

    if (ks != XK_Super_L && ks != XK_Super_R)
    {
	if (isPress) g_superTapPending = False;
	return False;
    }

    if (isPress)
    {
	unsigned int mods = keyEvent->state & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask);
	if (mods == 0)
	{
	    g_superTapPending = True;
	    g_superTapPressTime = keyEvent->time;
	}
	else
	{
	    g_superTapPending = False;
	}
	return False;
    }
    else
    {
	if (g_superTapPending)
	{
	    Time elapsed = keyEvent->time >= g_superTapPressTime
		? (keyEvent->time - g_superTapPressTime) : 0;
	    g_superTapPending = False;
	    if (elapsed <= 600 && !g_x11StickyKeys)
	    {
		XEvent ev;
		memset (&ev, 0, sizeof(ev));
		ev.xkey = *keyEvent;
		F_Exec ("ck-launch --toggle", nullptr, &ev);
		return True;
	    }
	}
    }
    return False;
}

void HandleEwmhRootClientMessage (XClientMessageEvent *cme)
{
    if (!cme)
	return;
    if (g_xaDtWmSwipe != None && cme->message_type == g_xaDtWmSwipe)
    {
	WmScreenData *pSD = FindScreenForRoot (cme->window);
	long fingers = cme->data.l[0];
	long dx = cme->data.l[1];
	long dy = cme->data.l[2];
	long absDx = dx < 0 ? -dx : dx;
	long absDy = dy < 0 ? -dy : dy;
	if (fingers == 3 && absDx >= 40 && absDx > (absDy * 3) / 2)
	    SwitchX11WorkspaceRelative (pSD, dx < 0 ? +1 : -1);
	return;
    }
    if (g_xaDtWmRegrabKeys != None && cme->message_type == g_xaDtWmRegrabKeys)
    {
	WmScreenData *pSD = FindScreenForRoot (cme->window);
	GrabEwmhHardwareKeys (pSD);
	return;
    }
}

void HandleEwmhGenericEvent (XEvent *event)
{
    XGenericEventCookie *cookie = event ? &event->xcookie : nullptr;
    struct LocalXIGestureSwipeEvent {
	int type;
	unsigned long serial;
	Bool send_event;
	Display *display;
	int extension;
	int evtype;
	Time time;
	int deviceid;
	int sourceid;
	int detail;
	Window root;
	Window event;
	Window child;
	double root_x;
	double root_y;
	double event_x;
	double event_y;
	double delta_x;
	double delta_y;
	double delta_unaccel_x;
	double delta_unaccel_y;
	int flags;
    };

    if (!cookie || g_xiOpcode < 0 || cookie->extension != g_xiOpcode)
	return;
    if (cookie->evtype != 27 && cookie->evtype != 28 && cookie->evtype != 29)
	return;
    if (!XGetEventData (DISPLAY, cookie))
	return;
    if (cookie->data)
    {
	struct LocalXIGestureSwipeEvent *sev =
	    (struct LocalXIGestureSwipeEvent *)cookie->data;
	if (cookie->evtype == 27) /* XI_GestureSwipeBegin */
	{
	    g_xiSwipeActive = (sev->detail == 3) ? True : False;
	    g_xiSwipeFingers = sev->detail;
	    g_xiSwipeDx = 0.0;
	    g_xiSwipeDy = 0.0;
	}
	else if (cookie->evtype == 28 && g_xiSwipeActive) /* XI_GestureSwipeUpdate */
	{
	    g_xiSwipeDx += sev->delta_x;
	    g_xiSwipeDy += sev->delta_y;
	}
	else if (cookie->evtype == 29) /* XI_GestureSwipeEnd */
	{
	    Boolean cancelled = (sev->flags & 1) ? True : False;
	    if (g_xiSwipeActive && g_xiSwipeFingers == 3 && !cancelled &&
		fabs (g_xiSwipeDx) >= 40.0 &&
		fabs (g_xiSwipeDx) > fabs (g_xiSwipeDy) * 1.5)
	    {
		WmScreenData *pSD = FindScreenForRoot (sev->root);
		SwitchX11WorkspaceRelative (pSD, g_xiSwipeDx < 0.0 ? +1 : -1);
	    }
	    g_xiSwipeActive = False;
	    g_xiSwipeFingers = 0;
	    g_xiSwipeDx = 0.0;
	    g_xiSwipeDy = 0.0;
	}
    }
    XFreeEventData (DISPLAY, cookie);
}

/* --- WP-20: X11 AccessX (XKB), Visual Bell, Reduced Motion & Magnifier --- */

static const char *
X11VbellModeStr (int mode)
{
    if (mode == 1) return "window";
    if (mode == 2) return "screen";
    return "off";
}

static int
ParseX11VbellMode (const char *s)
{
    if (!s) return 0;
    if (strcasecmp (s, "window") == 0 || strcasecmp (s, "active") == 0 || strcmp (s, "1") == 0)
	return 1;
    if (strcasecmp (s, "screen") == 0 || strcasecmp (s, "fullscreen") == 0 || strcmp (s, "2") == 0)
	return 2;
    return 0;
}

static Boolean
ParseX11BoolStr (const char *s, Boolean fallback)
{
    if (!s || !*s) return fallback;
    if (strcmp (s, "1") == 0 || strcasecmp (s, "true") == 0 ||
	strcasecmp (s, "yes") == 0 || strcasecmp (s, "on") == 0)
	return True;
    if (strcmp (s, "0") == 0 || strcasecmp (s, "false") == 0 ||
	strcasecmp (s, "no") == 0 || strcasecmp (s, "off") == 0)
	return False;
    return fallback;
}

static void
SyncX11XkbModifierMasks (void)
{
    XkbStateRec st;
    if (g_xkbEventBase < 0)
	return;
    memset (&st, 0, sizeof(st));
    if (XkbGetState (DISPLAY, XkbUseCoreKbd, &st) == Success)
    {
	g_x11LatchedMods = (unsigned int)st.latched_mods;
	g_x11LockedMods = (unsigned int)(st.locked_mods & ~LockMask);
    }
}

static void
PublishX11AccessibilityState (WmScreenData *pSD)
{
    char state[512];
    int n;

    if (!pSD)
	pSD = ACTIVE_PSD ? ACTIVE_PSD : &wmGD.Screens[0];
    if (!pSD || !pSD->managed)
	return;
    if (g_xaDtA11yState == None)
	g_xaDtA11yState = XInternAtom (DISPLAY, "_DT_ACCESSIBILITY_STATE", False);
    if (g_xaDtA11yState == None)
	return;

    SyncX11XkbModifierMasks ();
    n = snprintf (
	state, sizeof(state),
	"sticky=%d latched=%u locked=%u slow=%d slow_ms=%d bounce=%d bounce_ms=%d "
	"vbell=%s vbell_count=%u reduced_motion=%d zoom_pct=%d",
	g_x11StickyKeys ? 1 : 0,
	g_x11LatchedMods,
	g_x11LockedMods,
	g_x11SlowKeys ? 1 : 0,
	g_x11SlowKeysMs,
	g_x11BounceKeys ? 1 : 0,
	g_x11BounceKeysMs,
	X11VbellModeStr (g_x11VbellMode),
	g_x11VbellCount,
	g_x11ReducedMotion ? 1 : 0,
	g_x11ZoomPct);
    if (n > 0)
    {
	XChangeProperty (DISPLAY, pSD->rootWindow, g_xaDtA11yState, XA_STRING, 8,
			 PropModeReplace, (unsigned char *)state, n);
	XFlush (DISPLAY);
    }
}

static void
ApplyX11ReducedMotion (WmScreenData *pSD, Boolean reduced)
{
    int scr;
    (void)pSD;
    if (!g_savedAnimateMinValid && wmGD.numScreens > 0)
    {
	g_savedAnimateMin = wmGD.Screens[0].animateMinimizeRestore;
	g_savedAnimateMinValid = True;
    }
    g_x11ReducedMotion = reduced;
    for (scr = 0; scr < wmGD.numScreens; ++scr)
    {
	if (!wmGD.Screens[scr].managed)
	    continue;
	wmGD.Screens[scr].animateMinimizeRestore =
	    reduced ? False : g_savedAnimateMin;
    }
}

static void
ApplyX11XkbControls (void)
{
    XkbDescPtr xkb;
    if (g_xkbEventBase < 0)
	return;
    xkb = XkbGetMap (DISPLAY, 0, XkbUseCoreKbd);
    if (!xkb)
	return;
    if (XkbGetControls (DISPLAY, XkbAllControlsMask, xkb) == Success && xkb->ctrls)
    {
	unsigned long mask =
	    XkbStickyKeysMask | XkbSlowKeysMask | XkbBounceKeysMask |
	    XkbControlsEnabledMask | XkbAccessXFeedbackMask;
	if (g_x11StickyKeys)
	    xkb->ctrls->enabled_ctrls |= XkbStickyKeysMask;
	else
	    xkb->ctrls->enabled_ctrls &= ~XkbStickyKeysMask;

	if (g_x11SlowKeys)
	    xkb->ctrls->enabled_ctrls |= XkbSlowKeysMask;
	else
	    xkb->ctrls->enabled_ctrls &= ~XkbSlowKeysMask;

	if (g_x11BounceKeys)
	    xkb->ctrls->enabled_ctrls |= XkbBounceKeysMask;
	else
	    xkb->ctrls->enabled_ctrls &= ~XkbBounceKeysMask;

	if (g_x11SlowKeysMs < 50) g_x11SlowKeysMs = 50;
	if (g_x11SlowKeysMs > 2000) g_x11SlowKeysMs = 2000;
	if (g_x11BounceKeysMs < 50) g_x11BounceKeysMs = 50;
	if (g_x11BounceKeysMs > 2000) g_x11BounceKeysMs = 2000;

	xkb->ctrls->slow_keys_delay = (unsigned short)g_x11SlowKeysMs;
	xkb->ctrls->debounce_delay = (unsigned short)g_x11BounceKeysMs;
	if (g_x11StickyTwoKeysOff)
	    xkb->ctrls->ax_options |= XkbAX_TwoKeysMask;
	else
	    xkb->ctrls->ax_options &= ~XkbAX_TwoKeysMask;
	xkb->ctrls->ax_options |= XkbAX_LatchToLockMask;
	xkb->ctrls->ax_options &= ~XkbAX_FBOptionsMask;

	XkbSetControls (DISPLAY, mask, xkb);
	if (!g_x11StickyKeys)
	{
	    XkbLatchModifiers (DISPLAY, XkbUseCoreKbd, 0xff, 0);
	    XkbLockModifiers (DISPLAY, XkbUseCoreKbd,
			      ShiftMask | ControlMask | Mod1Mask | Mod4Mask, 0);
	}
	XSync (DISPLAY, False);
    }
    XkbFreeKeyboard (xkb, XkbAllComponentsMask, True);
}

static void
VbellHideTimeoutCB (XtPointer client_data, XtIntervalId *id)
{
    (void)client_data;
    (void)id;
    g_vbellTimer = 0;
    if (g_vbellWin != None)
    {
	XUnmapWindow (DISPLAY, g_vbellWin);
	XFlush (DISPLAY);
    }
}

static void
TriggerX11VisualBell (WmScreenData *pSD)
{
    int fx = 0, fy = 0, fw = 0, fh = 0;
    unsigned long flashPixel;
    XColor exact, warm;

    if (!pSD)
	pSD = ACTIVE_PSD ? ACTIVE_PSD : &wmGD.Screens[0];
    if (!pSD || !pSD->managed || g_x11VbellMode == 0)
	return;

    if (g_x11VbellMode == 1 && wmGD.keyboardFocus &&
	!(wmGD.keyboardFocus->clientFlags & CLIENT_DESTROYED))
    {
	ClientData *pCD = wmGD.keyboardFocus;
	Window target = pCD->clientFrameWin ? pCD->clientFrameWin : pCD->client;
	Window rootRet = None, childRet = None;
	int rx = 0, ry = 0;
	unsigned int uw = 0, uh = 0, bw = 0, dep = 0;
	if (target != None &&
	    XGetGeometry (DISPLAY, target, &rootRet, &rx, &ry, &uw, &uh, &bw, &dep) &&
	    uw > 0 && uh > 0)
	{
	    int tx = rx, ty = ry;
	    if (XTranslateCoordinates (DISPLAY, target, pSD->rootWindow, 0, 0, &tx, &ty, &childRet))
	    {
		fx = tx;
		fy = ty;
	    }
	    else
	    {
		fx = rx;
		fy = ry;
	    }
	    fw = (int)uw;
	    fh = (int)uh;
	}
    }
    if (fw <= 0 || fh <= 0)
    {
	fx = 0;
	fy = 0;
	fw = DisplayWidth (DISPLAY, pSD->screen);
	fh = DisplayHeight (DISPLAY, pSD->screen);
    }

    flashPixel = WhitePixel (DISPLAY, pSD->screen);
    if (XAllocNamedColor (DISPLAY, DefaultColormap (DISPLAY, pSD->screen),
			  "#fff5c8", &warm, &exact))
    {
	flashPixel = warm.pixel;
    }

    if (g_vbellWin != None && g_vbellSD != pSD)
    {
	XDestroyWindow (DISPLAY, g_vbellWin);
	g_vbellWin = None;
    }
    g_vbellSD = pSD;

    if (g_vbellWin == None)
    {
	XSetWindowAttributes xswa;
	memset (&xswa, 0, sizeof(xswa));
	xswa.override_redirect = True;
	xswa.save_under = True;
	xswa.background_pixel = flashPixel;
	g_vbellWin = XCreateWindow (
	    DISPLAY, pSD->rootWindow, fx, fy, (unsigned int)fw, (unsigned int)fh, 0,
	    DefaultDepth (DISPLAY, pSD->screen), InputOutput,
	    DefaultVisual (DISPLAY, pSD->screen),
	    CWOverrideRedirect | CWSaveUnder | CWBackPixel, &xswa);
    }
    else
    {
	XSetWindowBackground (DISPLAY, g_vbellWin, flashPixel);
	XMoveResizeWindow (DISPLAY, g_vbellWin, fx, fy, (unsigned int)fw, (unsigned int)fh);
    }

    g_x11VbellCount++;
    XMapRaised (DISPLAY, g_vbellWin);
    XClearWindow (DISPLAY, g_vbellWin);
    PublishX11AccessibilityState (pSD);
    XFlush (DISPLAY);

    if (g_vbellTimer)
	XtRemoveTimeOut (g_vbellTimer);
    g_vbellTimer = XtAppAddTimeOut (wmGD.mwmAppContext, 120, VbellHideTimeoutCB, pSD);
}

static void
SpawnCkMagnifierCmd (WmScreenData *pSD, const char *args)
{
    char cmd[256];
    XEvent ev;
    (void)pSD;
    snprintf (cmd, sizeof(cmd), "ck-magnifier %s", args);
    memset (&ev, 0, sizeof(ev));
    F_Exec (cmd, nullptr, &ev);
}

static int
ReadCkMagnifierRootZoom (WmScreenData *pSD)
{
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nitems = 0, bytesAfter = 0;
    unsigned char *prop = nullptr;
    int val = -1;

    if (!pSD)
	return -1;
    if (g_xaCkMagnifierZoom == None)
	g_xaCkMagnifierZoom = XInternAtom (DISPLAY, "_CK_MAGNIFIER_ZOOM", False);
    if (g_xaCkMagnifierZoom == None)
	return -1;
    if (XGetWindowProperty (DISPLAY, pSD->rootWindow, g_xaCkMagnifierZoom,
			    0, 1, False, XA_CARDINAL,
			    &actualType, &actualFormat, &nitems, &bytesAfter, &prop) == Success &&
	prop)
    {
	if (actualType == XA_CARDINAL && actualFormat == 32 && nitems >= 1)
	    val = (int)(*(unsigned long *)prop);
	XFree (prop);
    }
    return val;
}

static void
LoadX11AccessibilityConf (WmScreenData *pSD)
{
    const char *home = getenv ("HOME");
    char path[1024];
    char line[512];
    FILE *fp;
    Boolean magEnabled = False;
    int cfgZoom = g_x11ConfiguredZoomPct;

    if (!home || !home[0])
	return;
    snprintf (path, sizeof(path), "%s/.dt/accessibility.conf", home);
    fp = fopen (path, "r");
    if (!fp)
	return;

    while (fgets (line, sizeof(line), fp))
    {
	char *p = line;
	char *nl, *eq, *key, *val;
	size_t klen;

	while (*p && isspace ((unsigned char)*p)) p++;
	if (*p == '\0' || *p == '#' || *p == '!') continue;
	nl = strpbrk (p, "\r\n");
	if (nl) *nl = '\0';
	eq = strchr (p, '=');
	if (!eq) continue;
	*eq = '\0';
	key = p;
	val = eq + 1;
	while (*val && isspace ((unsigned char)*val)) val++;
	klen = strlen (key);
	while (klen > 0 && isspace ((unsigned char)key[klen - 1]))
	    key[--klen] = '\0';

	if (strcasecmp (key, "sticky_keys") == 0 || strcasecmp (key, "sticky") == 0)
	    g_x11StickyKeys = ParseX11BoolStr (val, g_x11StickyKeys);
	else if (strcasecmp (key, "sticky_two_keys_off") == 0 || strcasecmp (key, "two_keys_off") == 0)
	    g_x11StickyTwoKeysOff = ParseX11BoolStr (val, g_x11StickyTwoKeysOff);
	else if (strcasecmp (key, "slow_keys") == 0 || strcasecmp (key, "slow") == 0)
	    g_x11SlowKeys = ParseX11BoolStr (val, g_x11SlowKeys);
	else if (strcasecmp (key, "slow_keys_ms") == 0 || strcasecmp (key, "slow_ms") == 0)
	    g_x11SlowKeysMs = atoi (val);
	else if (strcasecmp (key, "bounce_keys") == 0 || strcasecmp (key, "bounce") == 0)
	    g_x11BounceKeys = ParseX11BoolStr (val, g_x11BounceKeys);
	else if (strcasecmp (key, "bounce_keys_ms") == 0 || strcasecmp (key, "bounce_ms") == 0)
	    g_x11BounceKeysMs = atoi (val);
	else if (strcasecmp (key, "visual_bell") == 0 || strcasecmp (key, "vbell") == 0)
	    g_x11VbellMode = ParseX11VbellMode (val);
	else if (strcasecmp (key, "reduced_motion") == 0)
	    ApplyX11ReducedMotion (pSD, ParseX11BoolStr (val, g_x11ReducedMotion));
	else if (strcasecmp (key, "magnifier") == 0)
	    magEnabled = ParseX11BoolStr (val, False);
	else if (strcasecmp (key, "zoom_pct") == 0)
	{
	    int z = atoi (val);
	    if (z >= 125 && z <= 400)
		cfgZoom = z;
	}
    }
    fclose (fp);

    g_x11ConfiguredZoomPct = cfgZoom;
    g_x11ZoomPct = magEnabled ? cfgZoom : 100;
    ApplyX11XkbControls ();
    if (magEnabled && g_x11ZoomPct > 100)
    {
	char zarg[64];
	snprintf (zarg, sizeof(zarg), "--zoom %d", g_x11ZoomPct);
	SpawnCkMagnifierCmd (pSD, zarg);
    }
}

static void
RingX11AudibleBell (const XkbBellNotifyEvent *bellEv)
{
    static uint64_t lastBellMs = 0;
    struct timespec ts = {0, 0};
    uint64_t nowMs;
    const char *dis = getenv ("DTWM_DISABLE_AUDIBLE_BELL");
    const char *home;
    Boolean haveConf = False;
    char cmd[256];
    XEvent ev;

    if (dis && strcmp (dis, "1") == 0)
	return;
    if (bellEv && bellEv->percent == 0)
	return;

    clock_gettime (CLOCK_MONOTONIC, &ts);
    nowMs = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
    if (nowMs - lastBellMs < 75ULL)
	return;
    lastBellMs = nowMs;

    home = getenv ("HOME");
    if (home && home[0])
    {
	char confPath[1024];
	snprintf (confPath, sizeof(confPath), "%s/.dt/beep.conf", home);
	haveConf = (access (confPath, R_OK) == 0) ? True : False;
    }

    if (haveConf || !bellEv)
    {
	snprintf (cmd, sizeof(cmd), "ck-sound --bell");
    }
    else
    {
	int pct = bellEv->percent <= 100 ? (int)bellEv->percent : 50;
	int pitch = (bellEv->pitch >= 50 && bellEv->pitch <= 9000) ? (int)bellEv->pitch : 400;
	int dur = (bellEv->duration >= 40 && bellEv->duration <= 2500) ? (int)bellEv->duration : 100;
	snprintf (cmd, sizeof(cmd), "ck-sound --bell %d %d %d", pct, pitch, dur);
    }
    memset (&ev, 0, sizeof(ev));
    F_Exec (cmd, nullptr, &ev);
}

Boolean
HandleEwmhXkbEvent (XEvent *event)
{
    XkbEvent *xkbev;
    WmScreenData *pSD;

    if (!event || g_xkbEventBase < 0 || event->type != g_xkbEventBase)
	return False;

    xkbev = (XkbEvent *)event;
    pSD = ACTIVE_PSD ? ACTIVE_PSD : &wmGD.Screens[0];

    if (xkbev->any.xkb_type == XkbMapNotify ||
	xkbev->any.xkb_type == XkbNewKeyboardNotify)
    {
	XkbRefreshKeyboardMapping ((XkbMapNotifyEvent *)xkbev);
	GrabEwmhHardwareKeys (pSD);
	return True;
    }
    if (xkbev->any.xkb_type == XkbBellNotify)
    {
	TriggerX11VisualBell (pSD);
	RingX11AudibleBell (&xkbev->bell);
	return True;
    }
    if (xkbev->any.xkb_type == XkbStateNotify)
    {
	g_x11LatchedMods = (unsigned int)xkbev->state.latched_mods;
	g_x11LockedMods = (unsigned int)(xkbev->state.locked_mods & ~LockMask);
	PublishX11AccessibilityState (pSD);
	return True;
    }
    if (xkbev->any.xkb_type == XkbControlsNotify)
    {
	g_x11StickyKeys = (xkbev->ctrls.enabled_ctrls & XkbStickyKeysMask) ? True : False;
	g_x11SlowKeys = (xkbev->ctrls.enabled_ctrls & XkbSlowKeysMask) ? True : False;
	g_x11BounceKeys = (xkbev->ctrls.enabled_ctrls & XkbBounceKeysMask) ? True : False;
	PublishX11AccessibilityState (pSD);
	return True;
    }
    return True;
}

Boolean
HandleEwmhAccessibilityRequest (WmScreenData *pSD, const char *req)
{
    const char *cmd = req;
    if (!cmd)
	return False;
    while (*cmd && isspace ((unsigned char)*cmd))
	cmd++;

    if (strncmp (cmd, "f.visual_bell", 13) == 0 &&
	(cmd[13] == '\0' || isspace ((unsigned char)cmd[13])))
    {
	TriggerX11VisualBell (pSD);
	return True;
    }

    if (strncmp (cmd, "f.reload_accessibility", 22) == 0 &&
	(cmd[22] == '\0' || isspace ((unsigned char)cmd[22])))
    {
	LoadX11AccessibilityConf (pSD);
	PublishX11AccessibilityState (pSD);
	return True;
    }

    if (strncmp (cmd, "f.magnifier_zoom", 16) == 0 &&
	(cmd[16] == '\0' || isspace ((unsigned char)cmd[16])))
    {
	const char *arg = cmd + 16;
	while (*arg && isspace ((unsigned char)*arg)) arg++;
	if (strcasecmp (arg, "in") == 0)
	    SpawnCkMagnifierCmd (pSD, "--in");
	else if (strcasecmp (arg, "out") == 0)
	    SpawnCkMagnifierCmd (pSD, "--out");
	else if (strcasecmp (arg, "reset") == 0)
	    SpawnCkMagnifierCmd (pSD, "--reset");
	else if (*arg)
	{
	    int z = atoi (arg);
	    char zarg[64];
	    if (z < 125) z = 100;
	    if (z > 400) z = 400;
	    snprintf (zarg, sizeof(zarg), "--zoom %d", z);
	    SpawnCkMagnifierCmd (pSD, zarg);
	}
	return True;
    }

    if (strncmp (cmd, "f.set_accessibility", 19) == 0 &&
	(cmd[19] == '\0' || isspace ((unsigned char)cmd[19])))
    {
	char copy[512];
	char *saveptr = nullptr;
	char *tok;
	Boolean touchXkb = False;
	Boolean sawMagFlag = False;
	Boolean magFlagVal = False;
	Boolean sawZoom = False;
	int zoomVal = g_x11ZoomPct;

	snprintf (copy, sizeof(copy), "%s", cmd + 19);
	for (tok = strtok_r (copy, " \t\r\n", &saveptr); tok;
	     tok = strtok_r (nullptr, " \t\r\n", &saveptr))
	{
	    char *eq = strchr (tok, '=');
	    const char *k, *v;
	    if (!eq) continue;
	    *eq = '\0';
	    k = tok;
	    v = eq + 1;

	    if (strcasecmp (k, "sticky") == 0 || strcasecmp (k, "sticky_keys") == 0)
	    {
		g_x11StickyKeys = ParseX11BoolStr (v, g_x11StickyKeys);
		touchXkb = True;
	    }
	    else if (strcasecmp (k, "two_keys_off") == 0 || strcasecmp (k, "sticky_two_keys_off") == 0)
	    {
		g_x11StickyTwoKeysOff = ParseX11BoolStr (v, g_x11StickyTwoKeysOff);
		touchXkb = True;
	    }
	    else if (strcasecmp (k, "clear_latches") == 0 && ParseX11BoolStr (v, False))
	    {
		XkbLatchModifiers (DISPLAY, XkbUseCoreKbd, 0xff, 0);
		XkbLockModifiers (DISPLAY, XkbUseCoreKbd,
				  ShiftMask | ControlMask | Mod1Mask | Mod4Mask, 0);
	    }
	    else if (strcasecmp (k, "slow") == 0 || strcasecmp (k, "slow_keys") == 0)
	    {
		g_x11SlowKeys = ParseX11BoolStr (v, g_x11SlowKeys);
		touchXkb = True;
	    }
	    else if (strcasecmp (k, "slow_ms") == 0 || strcasecmp (k, "slow_keys_ms") == 0)
	    {
		g_x11SlowKeysMs = atoi (v);
		touchXkb = True;
	    }
	    else if (strcasecmp (k, "bounce") == 0 || strcasecmp (k, "bounce_keys") == 0)
	    {
		g_x11BounceKeys = ParseX11BoolStr (v, g_x11BounceKeys);
		touchXkb = True;
	    }
	    else if (strcasecmp (k, "bounce_ms") == 0 || strcasecmp (k, "bounce_keys_ms") == 0)
	    {
		g_x11BounceKeysMs = atoi (v);
		touchXkb = True;
	    }
	    else if (strcasecmp (k, "vbell") == 0 || strcasecmp (k, "visual_bell") == 0)
	    {
		g_x11VbellMode = ParseX11VbellMode (v);
	    }
	    else if (strcasecmp (k, "reduced_motion") == 0)
	    {
		ApplyX11ReducedMotion (pSD, ParseX11BoolStr (v, g_x11ReducedMotion));
	    }
	    else if (strcasecmp (k, "magnifier") == 0)
	    {
		sawMagFlag = True;
		magFlagVal = ParseX11BoolStr (v, False);
	    }
	    else if (strcasecmp (k, "zoom_pct") == 0)
	    {
		sawZoom = True;
		zoomVal = atoi (v);
	    }
	}

	if (touchXkb)
	    ApplyX11XkbControls ();

	if (sawMagFlag)
	{
	    int targetZoom;
	    if (sawZoom && zoomVal >= 125 && zoomVal <= 400)
		g_x11ConfiguredZoomPct = zoomVal;
	    targetZoom = magFlagVal ? g_x11ConfiguredZoomPct : 100;
	    g_x11ZoomPct = targetZoom;
	    if (ReadCkMagnifierRootZoom (pSD) != targetZoom)
	    {
		if (targetZoom <= 100)
		    SpawnCkMagnifierCmd (pSD, "--reset");
		else
		{
		    char zarg[64];
		    snprintf (zarg, sizeof(zarg), "--zoom %d", targetZoom);
		    SpawnCkMagnifierCmd (pSD, zarg);
		}
	    }
	}
	else if (sawZoom)
	{
	    if (zoomVal < 125) zoomVal = 100;
	    if (zoomVal > 400) zoomVal = 400;
	    g_x11ZoomPct = zoomVal;
	}

	PublishX11AccessibilityState (pSD);
	return True;
    }

    return False;
}

/**
* @brief Sets up the window manager handling of the EWMH.
*/
void SetupWmEwmh (void)
{
    int scr;

    enum {
	XA_UTF8_STRING,
	XA__NET_SUPPORTED,
	XA__NET_SUPPORTING_WM_CHECK,
	XA__NET_WM_NAME,
	XA__NET_WM_ICON_NAME,
	XA__NET_WM_VISIBLE_NAME,
    XA__NET_WM_VISIBLE_ICON_NAME,
    XA__NET_WM_PID,
    XA__NET_WM_ICON_GEOMETRY,
    XA__NET_WM_FULLSCREEN_MONITORS,
	XA__NET_WM_STATE,
	XA__NET_WM_STATE_MAXIMIZED_VERT,
	XA__NET_WM_STATE_MAXIMIZED_HORZ,
	XA__NET_WM_STATE_FULLSCREEN,
	XA__NET_WM_STATE_ABOVE,
	XA__NET_WM_STATE_BELOW,
	XA__NET_CLIENT_LIST,
	XA__NET_CLIENT_LIST_STACKING,
	XA__NET_ACTIVE_WINDOW
    };

    static char *atom_names[] = {
	"UTF8_STRING",
	_XA__NET_SUPPORTED,
	_XA__NET_SUPPORTING_WM_CHECK,
	_XA__NET_WM_NAME,
	_XA__NET_WM_ICON_NAME,
	_XA__NET_WM_VISIBLE_NAME,
		_XA__NET_WM_VISIBLE_ICON_NAME,
	_XA__NET_WM_PID,
	_XA__NET_WM_ICON_GEOMETRY,
	_XA__NET_WM_FULLSCREEN_MONITORS,
	_XA__NET_WM_STATE,
	_XA__NET_WM_STATE_MAXIMIZED_VERT,
	_XA__NET_WM_STATE_MAXIMIZED_HORZ,
	_XA__NET_WM_STATE_FULLSCREEN,
	_XA__NET_WM_STATE_ABOVE,
	_XA__NET_WM_STATE_BELOW,
	_XA__NET_CLIENT_LIST,
	_XA__NET_CLIENT_LIST_STACKING,
	_XA__NET_ACTIVE_WINDOW
    };

    Atom atoms[XtNumber(atom_names) + 1];

    XInternAtoms(DISPLAY, atom_names, XtNumber(atom_names), False, atoms);

    wmGD.xa_UTF8_STRING = atoms[XA_UTF8_STRING];
    wmGD.xa__NET_WM_NAME = atoms[XA__NET_WM_NAME];
    wmGD.xa__NET_WM_ICON_NAME = atoms[XA__NET_WM_ICON_NAME];
    wmGD.xa__NET_WM_VISIBLE_NAME = atoms[XA__NET_WM_VISIBLE_NAME];
	wmGD.xa__NET_WM_VISIBLE_ICON_NAME = atoms[XA__NET_WM_VISIBLE_ICON_NAME];
	wmGD.xa__NET_WM_PID = atoms[XA__NET_WM_PID];
    wmGD.xa__NET_WM_ICON_GEOMETRY = atoms[XA__NET_WM_ICON_GEOMETRY];
    wmGD.xa__NET_WM_FULLSCREEN_MONITORS = atoms[XA__NET_WM_FULLSCREEN_MONITORS];
    wmGD.xa__NET_WM_STATE = atoms[XA__NET_WM_STATE];
    wmGD.xa__NET_WM_STATE_MAXIMIZED_VERT =
	atoms[XA__NET_WM_STATE_MAXIMIZED_VERT];
    wmGD.xa__NET_WM_STATE_MAXIMIZED_HORZ =
	atoms[XA__NET_WM_STATE_MAXIMIZED_HORZ];
    wmGD.xa__NET_WM_STATE_FULLSCREEN = atoms[XA__NET_WM_STATE_FULLSCREEN];
    wmGD.xa__NET_WM_STATE_ABOVE = atoms[XA__NET_WM_STATE_ABOVE];
    wmGD.xa__NET_WM_STATE_BELOW = atoms[XA__NET_WM_STATE_BELOW];
    wmGD.xa__NET_CLIENT_LIST = atoms[XA__NET_CLIENT_LIST];
    wmGD.xa__NET_CLIENT_LIST_STACKING = atoms[XA__NET_CLIENT_LIST_STACKING];
    wmGD.xa__NET_ACTIVE_WINDOW = atoms[XA__NET_ACTIVE_WINDOW];

    for (scr = 0; scr < wmGD.numScreens; ++scr)
    {
	Window childWindow;
	WmScreenData *pSD;
	KeyCode kcL, kcR;

	pSD = &(wmGD.Screens[scr]);

	if (!pSD->managed) continue;

	childWindow = pSD->wmWorkspaceWin;

	XChangeProperty(DISPLAY, childWindow, atoms[XA__NET_WM_NAME],
			atoms[XA_UTF8_STRING], 8, PropModeReplace,
			(unsigned char *)DT_WM_RESOURCE_NAME, 5);

	XChangeProperty(DISPLAY, childWindow,
			atoms[XA__NET_SUPPORTING_WM_CHECK], XA_WINDOW, 32,
			PropModeReplace, (unsigned char *)&childWindow, 1);

	XChangeProperty(DISPLAY, wmGD.Screens[scr].rootWindow,
			atoms[XA__NET_SUPPORTING_WM_CHECK], XA_WINDOW, 32,
			PropModeReplace, (unsigned char *)&childWindow, 1);

	XChangeProperty(DISPLAY, wmGD.Screens[scr].rootWindow,
			atoms[XA__NET_SUPPORTED], XA_ATOM, 32, PropModeReplace,
			(unsigned char *)&atoms[XA__NET_SUPPORTING_WM_CHECK],
			17);

	UpdateEwmhClientList (pSD);
	UpdateEwmhActiveWindow (pSD, wmGD.keyboardFocus);

	/* Passive async grab on Super_L / Super_R so bare Super tap opens ck-launch */
	{
	    int (*oldHandler)(Display *, XErrorEvent *) = XSetErrorHandler (IgnoreXGrabError);
	    kcL = XKeysymToKeycode (DISPLAY, XK_Super_L);
	    if (kcL != 0)
	    {
		XGrabKey (DISPLAY, kcL, 0, pSD->rootWindow, True, GrabModeAsync, GrabModeAsync);
		XGrabKey (DISPLAY, kcL, LockMask, pSD->rootWindow, True, GrabModeAsync, GrabModeAsync);
		XGrabKey (DISPLAY, kcL, Mod2Mask, pSD->rootWindow, True, GrabModeAsync, GrabModeAsync);
		XGrabKey (DISPLAY, kcL, LockMask | Mod2Mask, pSD->rootWindow, True, GrabModeAsync, GrabModeAsync);
	    }
	    kcR = XKeysymToKeycode (DISPLAY, XK_Super_R);
	    if (kcR != 0)
	    {
		XGrabKey (DISPLAY, kcR, 0, pSD->rootWindow, True, GrabModeAsync, GrabModeAsync);
		XGrabKey (DISPLAY, kcR, LockMask, pSD->rootWindow, True, GrabModeAsync, GrabModeAsync);
		XGrabKey (DISPLAY, kcR, Mod2Mask, pSD->rootWindow, True, GrabModeAsync, GrabModeAsync);
		XGrabKey (DISPLAY, kcR, LockMask | Mod2Mask, pSD->rootWindow, True, GrabModeAsync, GrabModeAsync);
	    }
	    XSync (DISPLAY, False);
	    XSetErrorHandler (oldHandler);
	}

	g_xaDtOsdState = XInternAtom (DISPLAY, "_DT_OSD_STATE", False);
	g_xaDtWmSwipe = XInternAtom (DISPLAY, "_DT_WM_SWIPE", False);
	g_xaDtWmRegrabKeys = XInternAtom (DISPLAY, "_DT_WM_REGRAB_KEYS", False);
	g_xaDtA11yState = XInternAtom (DISPLAY, "_DT_ACCESSIBILITY_STATE", False);
	g_xaCkMagnifierZoom = XInternAtom (DISPLAY, "_CK_MAGNIFIER_ZOOM", False);
	if (scr == 0)
	{
	    int xkbOp = 0, xkbErr = 0, xkbMaj = XkbMajorVersion, xkbMin = XkbMinorVersion;
	    if (XkbQueryExtension (DISPLAY, &xkbOp, &g_xkbEventBase, &xkbErr, &xkbMaj, &xkbMin))
	    {
		unsigned long xkbMask =
		    XkbNewKeyboardNotifyMask | XkbMapNotifyMask |
		    XkbBellNotifyMask | XkbStateNotifyMask | XkbControlsNotifyMask;
		XkbSelectEvents (DISPLAY, XkbUseCoreKbd, xkbMask, xkbMask);
	    }
	    else
	    {
		g_xkbEventBase = -1;
	    }
	    LoadX11AccessibilityConf (pSD);
	}
	GrabEwmhHardwareKeys (pSD);
	SetupXInput2TouchpadGestures (pSD);
	PublishX11OsdState (pSD, 0);
	PublishX11AccessibilityState (pSD);
    }
}
