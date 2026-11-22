/*
 * CDE - Common Desktop Environment
 *
 * Copyright (c) 1993-2012, The Open Group. All rights reserved.
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
/************************************<+>*************************************
 ****************************************************************************
 **
 **   File:        Mouse.c
 **
 **   Project:     DT 3.0
 **
 **   Description: Controls the Dtstyle Mouse dialog 
 **
 **
 **  (c) Copyright Hewlett-Packard Company, 1990.  
 **
 **
 **
 ****************************************************************************
 ************************************<+>*************************************/
/*$XConsortium: Mouse.c /main/5 1996/07/19 13:45:42 pascale $*/

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#include <Xm/Xm.h>
#include <Xm/Form.h>
#include <Xm/Frame.h>
#include <Xm/LabelG.h>
#include <Xm/MwmUtil.h>
#include <Xm/MessageB.h>
#include <Xm/PushB.h>
#include <Xm/PushBG.h>
#include <Xm/RowColumn.h>
#include <Xm/Scale.h>
#include <Xm/DrawingA.h>
#include <Xm/ToggleB.h>
#include <Xm/ToggleBG.h>
#include <Xm/VendorSEP.h>
#include <X11/Xutil.h>
#include <math.h>
#include <stdint.h>
#include <time.h>

#include <Dt/DialogBox.h>
#include <Dt/Message.h>
#include <Dt/SessionM.h>
#include <Dt/HourGlass.h>
#include <Dt/UserMsg.h>
#include <Dt/Wsm.h>
#include <Dt/Session.h>
#include <Dt/DtP.h>

#include <cde_config.h>
#include <X11/cursorfont.h>
#ifdef HAVE_XCURSOR
#include <X11/Xcursor/Xcursor.h>
#endif

#include "Help.h"
#include "Main.h"
#include "SaveRestore.h"

/*+++++++++++++++++++++++++++++++++++++++*/
/* include extern functions              */
/*+++++++++++++++++++++++++++++++++++++++*/
#include "Mouse.h"
#include "Protocol.h"

int _DtWmRestartNoConfirm (Display *display, Window root);


/*+++++++++++++++++++++++++++++++++++++++*/
/* Local #defines                        */
/*+++++++++++++++++++++++++++++++++++++++*/
#define ACCEL_MIN        0
#define ACCEL_MAX        10
#define THRESH_MIN       0
#define THRESH_MAX       15
#define THRESHDEFAULT    10
#define DCLICK_MIN       0
#define DCLICK_MAX       10
#define POINTER2B        0
#define POINTER3B        1
#define DCLICKDEFAULT    500
#define RIGHT_HANDED     0
#define LEFT_HANDED      1
#define TRANSFER         0
#define ADJUST           1


#define CURSOR_PREVIEW_COUNT 6
#define CURSOR_SIZE_CHOICE_COUNT 5
#define CURSOR_THEME_PREVIEW_ICON_SIZE 16

typedef struct {
    const char *name;
    unsigned int fontShape;
    int labelMsg;
    char *labelDefault;
} CursorPreviewInfo;

static const CursorPreviewInfo cursorPreviewInfo[CURSOR_PREVIEW_COUNT] = {
    { "left_ptr", XC_left_ptr, 26, "Pointer" },
    { "hand2", XC_hand2, 27, "Hand" },
    { "xterm", XC_xterm, 28, "Text" },
    { "watch", XC_watch, 29, "Hourglass" },
    { "question_arrow", XC_question_arrow, 30, "Help" },
    { "X_cursor", XC_X_cursor, 31, "X Cursor" },
};

static const int cursorSizeChoices[CURSOR_SIZE_CHOICE_COUNT] = {16, 24, 32, 48, 64};


#define LEFTMSG ((char *)GETMESSAGE(9, 1, "Left"))
#define RIGHTMSG ((char *)GETMESSAGE(9, 2, "Right"))
#define HANDMSG ((char *)GETMESSAGE(9, 3, "Handedness:"))
#define MIDDLEMSG ((char *)GETMESSAGE(9, 4, "Button 2:"))
#define MSG3  ((char *)GETMESSAGE(9, 10, "ERROR: setting pointer mapping\n"))
#define TRANSMSG ((char *)GETMESSAGE(9, 11, "Transfer"))
#define ADJMSG ((char *)GETMESSAGE(9, 12, "Adjust"))
#define WARN_MSG  ((char *)GETMESSAGE(9, 13, "The right and left mouse buttons will swap\n\
functions immediately after you click on OK.\n\
The `Test Double-Click' graphic in the Mouse\n\
window shows the current handedness by the \n\
positions of Button 1 and Button 3."))
#define MID_WARN_MSG_T  ((char *)GETMESSAGE(9, 14, "If you click on OK, mouse button 2 will be set\n\
to `transfer' (drag) at your next session."))
#define IMMEDIATE_MSG  ((char *)GETMESSAGE(9, 15, "The new double-click time will take effect\n\
as applications are restarted."))
#define  LATER_MSG  ((char *)GETMESSAGE(9, 16, "The new double-click time will take effect\n\
at your next session."))
#define MID_WARN_MSG_A  ((char *)GETMESSAGE(9, 19, "If you click on OK, mouse button 2 will be set\n\
to `adjust' at your next session. Button 2 may\n\
be used to adjust text and list selections.\n\
Transfer actions (drag) on text and lists will\n\
require the use of Button 1."))
#define MENUMSG ((char *)GETMESSAGE(9, 20, "Menu"))
#define CURSOR_SIZE_MSG ((char *)GETMESSAGE(9, 21, "Cursor Size"))
#define CURSOR_THEME_MSG ((char *)GETMESSAGE(9, 22, "Cursor Theme"))
#define CURSOR_PREVIEW_MSG ((char *)GETMESSAGE(9, 23, "Preview"))
#define CURSOR_PREVIEW_HINT_MSG ((char *)GETMESSAGE(9, 24, "Move pointer here to preview"))
#define RESTART_WM_MSG ((char *)GETMESSAGE(9, 25, "Restart Window Manager now (recommended)"))
#define TOUCHPAD_FRAME_MSG ((char *)GETMESSAGE(9, 32, "Touchpad"))
#define TP_TAP_MSG         ((char *)GETMESSAGE(9, 33, "Tap to Click"))
#define TP_NATURAL_MSG     ((char *)GETMESSAGE(9, 34, "Natural Scrolling"))
#define TP_CLICKFINGER_MSG ((char *)GETMESSAGE(9, 35, "Two-Finger Right-Click"))
#define TP_DWT_MSG         ((char *)GETMESSAGE(9, 36, "Disable While Typing"))
#define TP_DEXT_MSG        ((char *)GETMESSAGE(9, 37, "Disable With External Mouse"))
#define WHEEL_FRAME_MSG    ((char *)GETMESSAGE(9, 38, "Scroll Wheel"))
#define WHEEL_DIR_MSG      ((char *)GETMESSAGE(9, 39, "Direction:"))
#define WHEEL_STD_MSG      ((char *)GETMESSAGE(9, 40, "Standard"))
#define WHEEL_NAT_MSG      ((char *)GETMESSAGE(9, 41, "Natural"))
#define WHEEL_SMOOTH_MSG   ((char *)GETMESSAGE(9, 42, "Smooth Scrolling"))
#define WHEEL_SPEED_MSG    ((char *)GETMESSAGE(9, 43, "Speed:"))

#define B2	        "Dtms2B.bm"
#define B2_REV	        "Dtms2BR.bm"
#define B3	        "Dtms3B.bm"
#define B3_REV	        "Dtms3BR.bm"

static char *multiClickTimeString = "*multiClickTime:%d\n";
static char *enableBtn1TransferString = "*enableBtn1Transfer:%s\n";
static char *mouseString = "Mouse";

/*+++++++++++++++++++++++++++++++++++++++*/
/* Internal Functions                    */
/*+++++++++++++++++++++++++++++++++++++++*/

static Widget build_mouseDialog( 
                        Widget shell) ;
static void formLayoutCB( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void _DtmapCB_mouseDialog( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static int getValue( 
                        Widget w) ;
static void valueChangedCB( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void dclickVCCB( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void dclickTestCB( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void reverseToggleCB( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void midreverseToggleCB( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void warnToggleCB( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void midwarnToggleCB( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void cancelWarnCB( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void midcancelWarnCB( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void systemDefaultCB( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void ButtonCB( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;

static void cursorSizeChangedCB(
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void cursorThemeSelectCB(
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void cursorPreviewMapCB(
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void cursorPreviewExposeCB(
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;


/*+++++++++++++++++++++++++++++++++++++++*/
/* Internal Variables                    */
/*+++++++++++++++++++++++++++++++++++++++*/

typedef struct {
    Widget      pictFrame;
    Widget      pictRC;
    Widget      pictButton;
    Widget      pictLabel;
    Widget      handLabGad;
    Widget      rightToggle;
    Widget      leftToggle;
    Widget      middleLabGad;
    Widget      transferToggle;
    Widget      adjustToggle;
    Widget      systemDefault;
    Widget      accelLabGad;
    Widget      threshLabGad;
    Widget      dclickLabGad;
    Widget  	accelScale;
    Widget  	threshScale;
    Widget  	dclickScale;
    Widget      cursorSizeLabGad;
    Widget      cursorSizeRC;
    Widget      cursorSizeToggles[CURSOR_SIZE_CHOICE_COUNT];
    int         cursorSizeChoiceCount;
    int         cursorSizeChoiceValues[CURSOR_SIZE_CHOICE_COUNT];
    Widget      cursorThemeLabGad;
    Widget      cursorThemeOM;
    Widget      cursorThemeMenu;
    Widget      cursorPreviewFrame;
    Widget      cursorPreviewForm;
    Widget      cursorPreviewHint;
    Widget      cursorPreviewGrid;
    Widget      cursorPreviewCell[CURSOR_PREVIEW_COUNT];
    Widget      cursorPreviewArea[CURSOR_PREVIEW_COUNT];
    Widget      cursorPreviewLabel[CURSOR_PREVIEW_COUNT];
    Widget      dtwmRestartTG;
    Pixmap      pixmap;
    Pixmap      b3; 
    Pixmap      b3_rev; 
    Pixmap      b2; 
    Pixmap      b2_rev;
    Pixmap      pixmapSel; 
    Pixmap      b3Sel; 
    Pixmap      b3_revSel; 
    Pixmap      b2Sel; 
    Pixmap      b2_revSel;
    int         type;    
    int         numButtons;
    int         accel_numerator;
    int         accel_denominator;
    int         threshold;
    Boolean     dclickChanged;
    Boolean     btn2Changed;
    XtIntervalId     timeID;
    int         mydclick;
    Boolean     systemDefaultFlag;
    unsigned char map_return[256];
    unsigned char new_map_return[256];
    Boolean     origHanded;
    Boolean     handed;
    Boolean     function;
    Boolean     origFunction;
    Boolean     cursorChanged;
    int         origCursorSize;
    int         cursorSize;
    char       *origCursorTheme;
    char       *cursorTheme;
    char      **cursorThemes;
    Widget     *cursorThemeItems;
    Pixmap      *cursorThemePreviewPixmaps;
    int         cursorThemeCount;
    int         maxCursorSize;
    Cursor      cursorPreviewCursors[CURSOR_PREVIEW_COUNT];
    Pixmap      cursorPreviewPixmaps[CURSOR_PREVIEW_COUNT];
    int         cursorPreviewPixmapW[CURSOR_PREVIEW_COUNT];
    int         cursorPreviewPixmapH[CURSOR_PREVIEW_COUNT];
#ifdef HAVE_XCURSOR
    XcursorImages *cursorPreviewAnim[CURSOR_PREVIEW_COUNT];
    int         cursorPreviewAnimFrame[CURSOR_PREVIEW_COUNT];
    int         cursorPreviewCropX[CURSOR_PREVIEW_COUNT];
    int         cursorPreviewCropY[CURSOR_PREVIEW_COUNT];
    int         cursorPreviewCropW[CURSOR_PREVIEW_COUNT];
    int         cursorPreviewCropH[CURSOR_PREVIEW_COUNT];
    XtIntervalId cursorPreviewAnimTimer;
#endif
    Widget      scrollWheelFrame;
    Widget      scrollWheelFrameTitle;
    Widget      scrollWheelForm;
    Widget      wheelDirLabel;
    Widget      wheelDirRC;
    Widget      wheelStandardToggle;
    Widget      wheelNaturalToggle;
    Widget      wheelSmoothToggle;
    Widget      wheelSpeedLabel;
    Widget      wheelSpeedScale;
    Boolean     wheelNatural;
    int         wheelSpeed;
    Boolean     wheelSmooth;
    Boolean     origWheelNatural;
    int         origWheelSpeed;
    Boolean     origWheelSmooth;
    Boolean     wheelChanged;
    Widget      touchpadFrame;
    Widget      touchpadFrameTitle;
    Widget      touchpadForm;
    Widget      tpTapToggle;
    Widget      tpNaturalToggle;
    Widget      tpClickfingerToggle;
    Widget      tpDwtToggle;
    Widget      tpDextToggle;
    Boolean     tpTap;
    Boolean     tpNatural;
    Boolean     tpClickfinger;
    Boolean     tpDwt;
    Boolean     tpDext;
    Boolean     origTpTap;
    Boolean     origTpNatural;
    Boolean     origTpClickfinger;
    Boolean     origTpDwt;
    Boolean     origTpDext;
    Boolean     tpChanged;
} Mouse;

static Widget warnDialog;
static Widget midwarnDialog;

static Mouse mouse;
static saveRestore save = {FALSE, 0, };
static Boolean dclickSelectState = False;
static uint64_t lastPictBoxClickMonoMs = 0;
static int pictBoxClickCount = 0;

static Pixmap
LoadMouseBitmap(const char *name, Pixel fg, Pixel bg)
{
    Pixmap pix = XmGetPixmap(style.screen, (char *)name, fg, bg);
    if (pix == XmUNSPECIFIED_PIXMAP && name && name[0])
    {
        char fallback[PATH_MAX];
#ifdef CDE_INSTALLATION_TOP
        snprintf(fallback, sizeof(fallback), "%s/appconfig/icons/C/%s",
                 CDE_INSTALLATION_TOP, name);
        pix = XmGetPixmap(style.screen, fallback, fg, bg);
#endif
        if (pix == XmUNSPECIFIED_PIXMAP)
        {
            snprintf(fallback, sizeof(fallback), "/usr/dt/appconfig/icons/C/%s", name);
            pix = XmGetPixmap(style.screen, fallback, fg, bg);
        }
    }
    return pix;
}

static void
toggleDoubleClickTestVisual(void)
{
    static Boolean haveOrigColors = False;
    static Pixel origBg = 0;
    static Pixel origFg = 0;
    Arg color[4];
    int n;

    if (!haveOrigColors && mouse.pictLabel)
    {
        XtVaGetValues(mouse.pictLabel,
                      XmNbackground, &origBg,
                      XmNforeground, &origFg,
                      nullptr);
        haveOrigColors = True;
    }

    dclickSelectState = !dclickSelectState;
    if (!dclickSelectState)
    {
        Pixel restoreBg = haveOrigColors ? origBg : style.secBgCol;
        if (mouse.pictButton && mouse.pixmap != XmUNSPECIFIED_PIXMAP)
        {
            n = 0;
            XtSetArg(color[n], XmNlabelPixmap, mouse.pixmap); n++;
            XtSetValues(mouse.pictButton, color, n);
        }
        n = 0;
        XtSetArg(color[n], XmNbackground, restoreBg); n++;
        if (mouse.pictButton)
            XtSetValues(mouse.pictButton, color, n);
        if (mouse.pictRC)
            XtSetValues(mouse.pictRC, color, n);
        if (mouse.pictLabel)
        {
            if (haveOrigColors)
                XtSetArg(color[n], XmNforeground, origFg), n++;
            XtSetValues(mouse.pictLabel, color, n);
        }
    }
    else
    {
        Pixel restoreBg = haveOrigColors ? origBg : style.secBgCol;
        Pixel hiBg = (style.secBSCol != restoreBg) ? style.secBSCol : BlackPixelOfScreen(style.screen);
        Pixel hiFg = (style.secTSCol != hiBg) ? style.secTSCol : WhitePixelOfScreen(style.screen);

        if (mouse.pictButton && mouse.pixmapSel != XmUNSPECIFIED_PIXMAP)
        {
            n = 0;
            XtSetArg(color[n], XmNlabelPixmap, mouse.pixmapSel); n++;
            XtSetValues(mouse.pictButton, color, n);
        }
        n = 0;
        XtSetArg(color[n], XmNbackground, hiBg); n++;
        if (mouse.pictButton)
            XtSetValues(mouse.pictButton, color, n);
        if (mouse.pictRC)
            XtSetValues(mouse.pictRC, color, n);
        n = 0;
        XtSetArg(color[n], XmNbackground, hiBg); n++;
        XtSetArg(color[n], XmNforeground, hiFg); n++;
        if (mouse.pictLabel)
            XtSetValues(mouse.pictLabel, color, n);
    }
}

static void
pictBoxButtonPressEH(Widget w, XtPointer client_data, XEvent *event, Boolean *cont)
{
    struct timespec ts = {0};
    uint64_t nowMs;
    int mct;

    (void)w;
    (void)client_data;
    (void)cont;
    if (!event || event->type != ButtonPress)
        return;
    if (event->xbutton.button != Button1)
        return;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    nowMs = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
    mct = XtGetMultiClickTime(style.display);
    if (mct < 100)
        mct = DCLICKDEFAULT;
    if (lastPictBoxClickMonoMs != 0 &&
        nowMs >= lastPictBoxClickMonoMs &&
        (nowMs - lastPictBoxClickMonoMs) <= (uint64_t)mct)
    {
        pictBoxClickCount++;
    }
    else
    {
        pictBoxClickCount = 1;
    }
    lastPictBoxClickMonoMs = nowMs;
    if (pictBoxClickCount >= 2 && (pictBoxClickCount % 2) == 0)
    {
        toggleDoubleClickTestVisual();
    }
}

static void
EnsureXcursorPath(void)
{
    static Boolean pathSet = False;
    if (pathSet)
	return;

    char buf[4096];
    size_t pos = 0;
    const char *existing = getenv("XCURSOR_PATH");

    if (existing && *existing)
    {
	strncpy(buf, existing, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	pos = strlen(buf);
    }
    else
    {
	buf[0] = '\0';
	pos = 0;
    }

    const char *home = getenv("HOME");
    char temp[PATH_MAX];
    const char *candidates[3];
    int candCount = 0;

    if (home && *home)
    {
	snprintf(temp, sizeof(temp), "%s/.icons", home);
	candidates[candCount++] = XtNewString(temp);
	snprintf(temp, sizeof(temp), "%s/.local/share/icons", home);
	candidates[candCount++] = XtNewString(temp);
    }

    candidates[candCount++] = XtNewString("/usr/share/icons");

    for (int i = 0; i < candCount; i++)
    {
	const char *dir = candidates[i];
	if (!dir || !*dir)
	    continue;

	if (pos != 0 && buf[pos - 1] != ':')
	{
	    if (pos < sizeof(buf) - 1)
		buf[pos++] = ':';
	    buf[pos] = '\0';
	}

	size_t len = strlen(dir);
	if (pos + len >= sizeof(buf))
	    break;
	memcpy(buf + pos, dir, len);
	pos += len;
	buf[pos] = '\0';
    }

    for (int i = 0; i < candCount; i++)
    {
	if (candidates[i])
	    XtFree((char *)candidates[i]);
    }

    if (pos > 0)
	setenv("XCURSOR_PATH", buf, 1);

    pathSet = True;
}

static Boolean
DirectoryExists(const char *path)
{
    struct stat st;
    return (path && stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

static Boolean
ThemeAlreadyListed(const char *theme, char **themes, int count)
{
    int i;
    if (!theme)
	return True;
    for (i = 0; i < count; i++)
    {
	if (themes[i] && strcmp(themes[i], theme) == 0)
	    return True;
    }
    return False;
}

static void
AddTheme(char ***themes, Widget **items, int *count, int *capacity, const char *theme)
{
    if (!theme || !theme[0] || ThemeAlreadyListed(theme, *themes, *count))
	return;

    if (*count >= *capacity)
    {
	int newCap = (*capacity <= 0) ? 16 : (*capacity * 2);
	*themes = (char **)XtRealloc((char *)*themes, (Cardinal)(newCap * sizeof(char *)));
	*items = (Widget *)XtRealloc((char *)*items, (Cardinal)(newCap * sizeof(Widget)));
	*capacity = newCap;
    }

    (*themes)[*count] = XtNewString(theme);
    (*items)[*count] = (Widget)nullptr;
    (*count)++;
}

static void
ScanThemeDir(const char *baseDir, char ***themes, Widget **items, int *count, int *capacity)
{
    DIR *dir;
    struct dirent *entry;

    if (!DirectoryExists(baseDir))
	return;

    dir = opendir(baseDir);
    if (!dir)
	return;

    while ((entry = readdir(dir)) != nullptr)
    {
	char path[1024];
	char cursorsPath[1060];

	if (entry->d_name[0] == '\0' || entry->d_name[0] == '.')
	    continue;

	snprintf(path, sizeof(path), "%s/%s", baseDir, entry->d_name);
	snprintf(cursorsPath, sizeof(cursorsPath), "%s/%s/cursors", baseDir, entry->d_name);

	if (DirectoryExists(path) && DirectoryExists(cursorsPath))
	{
	    AddTheme(themes, items, count, capacity, entry->d_name);
	}
    }

    closedir(dir);
}

static void
RefreshXrmDatabaseFromRoot(void)
{
    const char *rmString;
    XrmDatabase newDb;
    XrmDatabase oldDb;

    rmString = XResourceManagerString(style.display);
    if (!rmString || !rmString[0])
	return;

    newDb = XrmGetStringDatabase(rmString);
    if (!newDb)
	return;

    oldDb = XrmGetDatabase(style.display);
    XrmSetDatabase(style.display, newDb);
    if (oldDb)
	XrmDestroyDatabase(oldDb);
}

static int
IgnoreXiError(Display *dpy, XErrorEvent *err)
{
    (void)dpy;
    (void)err;
    return 0;
}

static Boolean
ParseBoolValue(const char *val, Boolean fallback)
{
    if (!val || !val[0])
	return fallback;
    while (*val == ' ' || *val == '\t')
	++val;
    if (*val == '1' || *val == 't' || *val == 'T' ||
	*val == 'y' || *val == 'Y' || strncasecmp(val, "on", 2) == 0)
	return True;
    if (*val == '0' || *val == 'f' || *val == 'F' ||
	*val == 'n' || *val == 'N' || strncasecmp(val, "off", 3) == 0)
	return False;
    return fallback;
}

static void
LoadTouchpadResources(Boolean *tapOut, Boolean *natOut, Boolean *cfOut,
		      Boolean *dwtOut, Boolean *dextOut)
{
    Boolean tap = True, nat = False, cf = True, dwt = True, dext = False;
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nitems = 0, bytesAfter = 0;
    unsigned char *prop = nullptr;
    XrmDatabase db = nullptr;
    Boolean freeDb = False;

    if (style.display && style.root != None &&
	XGetWindowProperty(style.display, style.root, XA_RESOURCE_MANAGER,
			   0, 65536, False, XA_STRING, &actualType,
			   &actualFormat, &nitems, &bytesAfter, &prop) == Success &&
	prop && actualFormat == 8)
    {
	db = XrmGetStringDatabase((const char *)prop);
	freeDb = (db != nullptr);
    }
    if (prop)
	XFree(prop);
    if (!db && style.display)
	db = XrmGetDatabase(style.display);

    if (db)
    {
	char *typeRet = nullptr;
	XrmValue val = { 0, nullptr };
	if (XrmGetResource(db, "dtstyle.touchpadTapToClick",
			   "Dtstyle.TouchpadTapToClick", &typeRet, &val) && val.addr)
	    tap = ParseBoolValue(val.addr, tap);
	if (XrmGetResource(db, "dtstyle.touchpadNaturalScroll",
			   "Dtstyle.TouchpadNaturalScroll", &typeRet, &val) && val.addr)
	    nat = ParseBoolValue(val.addr, nat);
	if (XrmGetResource(db, "dtstyle.touchpadClickfinger",
			   "Dtstyle.TouchpadClickfinger", &typeRet, &val) && val.addr)
	    cf = ParseBoolValue(val.addr, cf);
	if (XrmGetResource(db, "dtstyle.touchpadDisableWhileTyping",
			   "Dtstyle.TouchpadDisableWhileTyping", &typeRet, &val) && val.addr)
	    dwt = ParseBoolValue(val.addr, dwt);
	if (XrmGetResource(db, "dtstyle.touchpadDisableWithExternalMouse",
			   "Dtstyle.TouchpadDisableWithExternalMouse", &typeRet, &val) && val.addr)
	    dext = ParseBoolValue(val.addr, dext);
	if (freeDb)
	    XrmDestroyDatabase(db);
    }

    if (tapOut)  *tapOut  = tap;
    if (natOut)  *natOut  = nat;
    if (cfOut)   *cfOut   = cf;
    if (dwtOut)  *dwtOut  = dwt;
    if (dextOut) *dextOut = dext;
}

static Boolean
DeviceHasPropertyAtom(Atom *props, int nprops, Atom target)
{
    int i;
    if (!props || target == None)
	return False;
    for (i = 0; i < nprops; ++i)
    {
	if (props[i] == target)
	    return True;
    }
    return False;
}

static void
ApplyTouchpadBackend(Boolean tap, Boolean nat, Boolean cf, Boolean dwt, Boolean dext)
{
    if (!style.display)
	return;

    if (DtstyleWaylandActive())
    {
	char cmd[128];
	snprintf(cmd, sizeof(cmd), "f.set_touchpad %d %d %d %d %d",
		 tap ? 1 : 0, nat ? 1 : 0, cf ? 1 : 0, dwt ? 1 : 0, dext ? 1 : 0);
	DtstyleSendWaylandRequest(cmd);
	return;
    }

    /* Classic X11 / XLibre backend via XInput2 libinput properties */
    {
	struct LocalXIDeviceInfo {
	    int deviceid;
	    char *name;
	    int use;
	    int attachment;
	    Bool enabled;
	    int num_classes;
	    void **classes;
	};
	typedef int (*pfn_XIQueryVersion)(Display *, int *, int *);
	typedef struct LocalXIDeviceInfo *(*pfn_XIQueryDevice)(Display *, int, int *);
	typedef void (*pfn_XIFreeDeviceInfo)(struct LocalXIDeviceInfo *);
	typedef Atom *(*pfn_XIListProperties)(Display *, int, int *);
	typedef void (*pfn_XIChangeProperty)(Display *, int, Atom, Atom, int, int,
					     unsigned char *, int);

	int evBase = 0, errBase = 0, xiOp = 0;
	int xiDevicesConfigured = 0;
	void *xiLib = nullptr;

	if (XQueryExtension(style.display, "XInputExtension", &xiOp, &evBase, &errBase))
	    xiLib = dlopen("libXi.so.6", RTLD_LAZY | RTLD_LOCAL);

	if (xiLib)
	{
	    pfn_XIQueryVersion fnVer = (pfn_XIQueryVersion)dlsym(xiLib, "XIQueryVersion");
	    pfn_XIQueryDevice fnQuery = (pfn_XIQueryDevice)dlsym(xiLib, "XIQueryDevice");
	    pfn_XIFreeDeviceInfo fnFree = (pfn_XIFreeDeviceInfo)dlsym(xiLib, "XIFreeDeviceInfo");
	    pfn_XIListProperties fnList = (pfn_XIListProperties)dlsym(xiLib, "XIListProperties");
	    pfn_XIChangeProperty fnChange = (pfn_XIChangeProperty)dlsym(xiLib, "XIChangeProperty");
	    int maj = 2, min = 0;

	    if (fnVer && fnQuery && fnFree && fnList && fnChange &&
		fnVer(style.display, &maj, &min) == Success && maj >= 2)
	    {
		Atom aTap = XInternAtom(style.display, "libinput Tapping Enabled", False);
		Atom aNat = XInternAtom(style.display, "libinput Natural Scrolling Enabled", False);
		Atom aCf  = XInternAtom(style.display, "libinput Click Method Enabled", False);
		Atom aDwt = XInternAtom(style.display, "libinput Disable While Typing Enabled", False);
		Atom aSend = XInternAtom(style.display, "libinput Send Events Mode Enabled", False);
		int ndevs = 0;
		int (*oldErr)(Display *, XErrorEvent *) = XSetErrorHandler(IgnoreXiError);
		struct LocalXIDeviceInfo *devs = fnQuery(style.display, 0 /* XIAllDevices */, &ndevs);

		if (devs)
		{
		    int di;
		    for (di = 0; di < ndevs; ++di)
		    {
			int nprops = 0;
			Atom *props;
			Boolean touched = False;
			/* XISlavePointer = 4, XIFloatingSlave = 5 */
			if (devs[di].use != 4 && devs[di].use != 5)
			    continue;
			props = fnList(style.display, devs[di].deviceid, &nprops);
			if (!props)
			    continue;

			if (DeviceHasPropertyAtom(props, nprops, aTap))
			{
			    unsigned char v = (unsigned char)(tap ? 1 : 0);
			    fnChange(style.display, devs[di].deviceid, aTap, XA_INTEGER,
				     8, PropModeReplace, &v, 1);
			    touched = True;
			}
			if (DeviceHasPropertyAtom(props, nprops, aNat))
			{
			    unsigned char v = (unsigned char)(nat ? 1 : 0);
			    fnChange(style.display, devs[di].deviceid, aNat, XA_INTEGER,
				     8, PropModeReplace, &v, 1);
			    touched = True;
			}
			if (DeviceHasPropertyAtom(props, nprops, aCf))
			{
			    unsigned char v[2];
			    v[0] = (unsigned char)(cf ? 0 : 1); /* button_areas */
			    v[1] = (unsigned char)(cf ? 1 : 0); /* clickfinger */
			    fnChange(style.display, devs[di].deviceid, aCf, XA_INTEGER,
				     8, PropModeReplace, v, 2);
			    touched = True;
			}
			if (DeviceHasPropertyAtom(props, nprops, aDwt))
			{
			    unsigned char v = (unsigned char)(dwt ? 1 : 0);
			    fnChange(style.display, devs[di].deviceid, aDwt, XA_INTEGER,
				     8, PropModeReplace, &v, 1);
			    touched = True;
			}
			if (DeviceHasPropertyAtom(props, nprops, aSend))
			{
			    unsigned char v[2];
			    v[0] = 0; /* disabled */
			    v[1] = (unsigned char)(dext ? 1 : 0); /* disabled_on_external_mouse */
			    fnChange(style.display, devs[di].deviceid, aSend, XA_INTEGER,
				     8, PropModeReplace, v, 2);
			    touched = True;
			}
			if (touched)
			    ++xiDevicesConfigured;
			XFree(props);
		    }
		    fnFree(devs);
		}
		XSync(style.display, False);
		XSetErrorHandler(oldErr);
	    }
	}

	if (style.root != None)
	{
	    Atom aTpState = XInternAtom(style.display, "_DT_X11_TOUCHPAD_STATE", False);
	    char summary[160];
	    snprintf(summary, sizeof(summary),
		     "tap=%d;nat=%d;cf=%d;dwt=%d;dext=%d;xi2_devices=%d",
		     tap ? 1 : 0, nat ? 1 : 0, cf ? 1 : 0,
		     dwt ? 1 : 0, dext ? 1 : 0, xiDevicesConfigured);
	    XChangeProperty(style.display, style.root, aTpState, XA_STRING, 8,
			    PropModeReplace, (const unsigned char *)summary,
			    (int)strlen(summary));
	}
    }
}

static void
PersistTouchpadResources(Boolean tap, Boolean nat, Boolean cf, Boolean dwt, Boolean dext)
{
    char res[512];
    int flags = style.xrdb.writeXrdbImmediate
	? (_DT_ATR_RESMGR | _DT_ATR_PREFS)
	: _DT_ATR_PREFS;

    snprintf(res, sizeof(res),
	     "*touchpadTapToClick: %s\n"
	     "*touchpadNaturalScroll: %s\n"
	     "*touchpadClickfinger: %s\n"
	     "*touchpadDisableWhileTyping: %s\n"
	     "*touchpadDisableWithExternalMouse: %s\n",
	     tap ? "True" : "False",
	     nat ? "True" : "False",
	     cf ? "True" : "False",
	     dwt ? "True" : "False",
	     dext ? "True" : "False");
    _DtAddResString(style.display, res, flags);
    XSync(style.display, False);
    RefreshXrmDatabaseFromRoot();
}

static void
SyncTouchpadTogglesFromState(void)
{
    if (mouse.tpTapToggle)
	XmToggleButtonGadgetSetState(mouse.tpTapToggle, mouse.tpTap, False);
    if (mouse.tpNaturalToggle)
	XmToggleButtonGadgetSetState(mouse.tpNaturalToggle, mouse.tpNatural, False);
    if (mouse.tpClickfingerToggle)
	XmToggleButtonGadgetSetState(mouse.tpClickfingerToggle, mouse.tpClickfinger, False);
    if (mouse.tpDwtToggle)
	XmToggleButtonGadgetSetState(mouse.tpDwtToggle, mouse.tpDwt, False);
    if (mouse.tpDextToggle)
	XmToggleButtonGadgetSetState(mouse.tpDextToggle, mouse.tpDext, False);
}

static void
touchpadToggleCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    if (mouse.tpTapToggle)
	mouse.tpTap = XmToggleButtonGadgetGetState(mouse.tpTapToggle);
    if (mouse.tpNaturalToggle)
	mouse.tpNatural = XmToggleButtonGadgetGetState(mouse.tpNaturalToggle);
    if (mouse.tpClickfingerToggle)
	mouse.tpClickfinger = XmToggleButtonGadgetGetState(mouse.tpClickfingerToggle);
    if (mouse.tpDwtToggle)
	mouse.tpDwt = XmToggleButtonGadgetGetState(mouse.tpDwtToggle);
    if (mouse.tpDextToggle)
	mouse.tpDext = XmToggleButtonGadgetGetState(mouse.tpDextToggle);
    mouse.tpChanged = True;
    mouse.systemDefaultFlag = False;
    ApplyTouchpadBackend(mouse.tpTap, mouse.tpNatural, mouse.tpClickfinger,
			 mouse.tpDwt, mouse.tpDext);
}

void
ApplyAndPersistTouchpadCLI(int tap, int nat, int cf, int dwt, int dext)
{
    Boolean bTap = tap ? True : False;
    Boolean bNat = nat ? True : False;
    Boolean bCf  = cf  ? True : False;
    Boolean bDwt = dwt ? True : False;
    Boolean bDext = dext ? True : False;
    style.xrdb.writeXrdbImmediate = True;
    ApplyTouchpadBackend(bTap, bNat, bCf, bDwt, bDext);
    PersistTouchpadResources(bTap, bNat, bCf, bDwt, bDext);
}

static void
LoadScrollWheelResources(Boolean *natOut, int *speedOut, Boolean *smoothOut)
{
    Boolean nat = False, smooth = True;
    int speed = 3;
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nitems = 0, bytesAfter = 0;
    unsigned char *prop = nullptr;
    XrmDatabase db = nullptr;
    Boolean freeDb = False;

    if (style.display && style.root != None &&
	XGetWindowProperty(style.display, style.root, XA_RESOURCE_MANAGER,
			   0, 65536, False, XA_STRING, &actualType,
			   &actualFormat, &nitems, &bytesAfter, &prop) == Success &&
	prop && actualFormat == 8)
    {
	db = XrmGetStringDatabase((const char *)prop);
	freeDb = (db != nullptr);
    }
    if (prop)
	XFree(prop);
    if (!db && style.display)
	db = XrmGetDatabase(style.display);

    if (db)
    {
	char *typeRet = nullptr;
	XrmValue val = { 0, nullptr };
	if (XrmGetResource(db, "dtstyle.mouseWheelNatural",
			   "Dtstyle.MouseWheelNatural", &typeRet, &val) && val.addr)
	    nat = ParseBoolValue(val.addr, nat);
	if (XrmGetResource(db, "dtstyle.mouseWheelSpeed",
			   "Dtstyle.MouseWheelSpeed", &typeRet, &val) && val.addr)
	{
	    int s = atoi(val.addr);
	    if (s >= 1 && s <= 10)
		speed = s;
	}
	if (XrmGetResource(db, "dtstyle.mouseWheelSmooth",
			   "Dtstyle.MouseWheelSmooth", &typeRet, &val) && val.addr)
	    smooth = ParseBoolValue(val.addr, smooth);
	if (freeDb)
	    XrmDestroyDatabase(db);
    }

    if (natOut)    *natOut    = nat;
    if (speedOut)  *speedOut  = speed;
    if (smoothOut) *smoothOut = smooth;
}

static void
ApplyScrollWheelBackend(Boolean nat, int speed, Boolean smooth)
{
    if (!style.display)
	return;
    if (speed < 1) speed = 1;
    if (speed > 10) speed = 10;

    if (DtstyleWaylandActive())
    {
	char cmd[128];
	snprintf(cmd, sizeof(cmd), "f.set_scroll_wheel %d %d %d",
		 nat ? 1 : 0, speed, smooth ? 1 : 0);
	DtstyleSendWaylandRequest(cmd);
	return;
    }

    /* Classic X11 / XLibre backend: configure non-touchpad XI2 pointer natural scroll */
    {
	struct LocalXIDeviceInfo {
	    int deviceid;
	    char *name;
	    int use;
	    int attachment;
	    Bool enabled;
	    int num_classes;
	    void **classes;
	};
	typedef int (*pfn_XIQueryVersion)(Display *, int *, int *);
	typedef struct LocalXIDeviceInfo *(*pfn_XIQueryDevice)(Display *, int, int *);
	typedef void (*pfn_XIFreeDeviceInfo)(struct LocalXIDeviceInfo *);
	typedef Atom *(*pfn_XIListProperties)(Display *, int, int *);
	typedef void (*pfn_XIChangeProperty)(Display *, int, Atom, Atom, int, int,
					     unsigned char *, int);

	int evBase = 0, errBase = 0, xiOp = 0;
	void *xiLib = nullptr;
	if (XQueryExtension(style.display, "XInputExtension", &xiOp, &evBase, &errBase))
	    xiLib = dlopen("libXi.so.6", RTLD_LAZY | RTLD_LOCAL);
	if (xiLib)
	{
	    pfn_XIQueryVersion fnVer = (pfn_XIQueryVersion)dlsym(xiLib, "XIQueryVersion");
	    pfn_XIQueryDevice fnQuery = (pfn_XIQueryDevice)dlsym(xiLib, "XIQueryDevice");
	    pfn_XIFreeDeviceInfo fnFree = (pfn_XIFreeDeviceInfo)dlsym(xiLib, "XIFreeDeviceInfo");
	    pfn_XIListProperties fnList = (pfn_XIListProperties)dlsym(xiLib, "XIListProperties");
	    pfn_XIChangeProperty fnChange = (pfn_XIChangeProperty)dlsym(xiLib, "XIChangeProperty");
	    int maj = 2, min = 0;
	    if (fnVer && fnQuery && fnFree && fnList && fnChange &&
		fnVer(style.display, &maj, &min) == Success)
	    {
		Atom aTap = XInternAtom(style.display, "libinput Tapping Enabled", False);
		Atom aNat = XInternAtom(style.display, "libinput Natural Scrolling Enabled", False);
		int (*oldHandler)(Display *, XErrorEvent *) = XSetErrorHandler(IgnoreXiError);
		int ndevs = 0;
		struct LocalXIDeviceInfo *devs = fnQuery(style.display, 0 /* XIAllDevices */, &ndevs);
		if (devs)
		{
		    int di;
		    for (di = 0; di < ndevs; ++di)
		    {
			int nprops = 0;
			Atom *props = fnList(style.display, devs[di].deviceid, &nprops);
			if (!props)
			    continue;
			if (!DeviceHasPropertyAtom(props, nprops, aTap) &&
			    DeviceHasPropertyAtom(props, nprops, aNat))
			{
			    unsigned char vNat = nat ? 1 : 0;
			    fnChange(style.display, devs[di].deviceid, aNat, XA_INTEGER,
				     8, PropModeReplace, &vNat, 1);
			}
			XFree(props);
		    }
		    fnFree(devs);
		}
		XSync(style.display, False);
		XSetErrorHandler(oldHandler);
	    }
	    dlclose(xiLib);
	}
    }
}

static void
PersistScrollWheelResources(Boolean nat, int speed, Boolean smooth)
{
    char res[256];
    int flags = style.xrdb.writeXrdbImmediate
	? (_DT_ATR_RESMGR | _DT_ATR_PREFS)
	: _DT_ATR_PREFS;
    if (speed < 1) speed = 1;
    if (speed > 10) speed = 10;

    snprintf(res, sizeof(res),
	     "*mouseWheelNatural: %s\n"
	     "*mouseWheelSpeed: %d\n"
	     "*mouseWheelSmooth: %s\n",
	     nat ? "True" : "False",
	     speed,
	     smooth ? "True" : "False");
    _DtAddResString(style.display, res, flags);
    XSync(style.display, False);
    RefreshXrmDatabaseFromRoot();
}

static void
SyncScrollWheelControlsFromState(void)
{
    if (mouse.wheelStandardToggle)
	XmToggleButtonGadgetSetState(mouse.wheelStandardToggle, !mouse.wheelNatural, False);
    if (mouse.wheelNaturalToggle)
	XmToggleButtonGadgetSetState(mouse.wheelNaturalToggle, mouse.wheelNatural, False);
    if (mouse.wheelSmoothToggle)
	XmToggleButtonGadgetSetState(mouse.wheelSmoothToggle, mouse.wheelSmooth, False);
    if (mouse.wheelSpeedScale)
    {
	int spd = mouse.wheelSpeed;
	if (spd < 1) spd = 1;
	if (spd > 10) spd = 10;
	XtVaSetValues(mouse.wheelSpeedScale, XmNvalue, spd, nullptr);
    }
}

static void
scrollWheelChangedCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)client_data;
    (void)call_data;
    if (w == mouse.wheelStandardToggle)
    {
	if (!XmToggleButtonGadgetGetState(mouse.wheelStandardToggle))
	    return;
	mouse.wheelNatural = False;
    }
    else if (w == mouse.wheelNaturalToggle)
    {
	if (!XmToggleButtonGadgetGetState(mouse.wheelNaturalToggle))
	    return;
	mouse.wheelNatural = True;
    }
    if (mouse.wheelSmoothToggle)
	mouse.wheelSmooth = XmToggleButtonGadgetGetState(mouse.wheelSmoothToggle);
    if (mouse.wheelSpeedScale)
	mouse.wheelSpeed = getValue(mouse.wheelSpeedScale);
    if (mouse.wheelSpeed < 1) mouse.wheelSpeed = 1;
    if (mouse.wheelSpeed > 10) mouse.wheelSpeed = 10;

    mouse.wheelChanged = True;
    mouse.systemDefaultFlag = False;
    ApplyScrollWheelBackend(mouse.wheelNatural, mouse.wheelSpeed, mouse.wheelSmooth);
}

static void
ApplyDialogBgFg(Widget reference, Widget target)
{
    Pixel bg;
    Arg args[1];

    if (!reference || !target)
	return;

    XtVaGetValues(reference, XmNbackground, &bg, nullptr);
    XtSetArg(args[0], XmNbackground, bg);
    XtSetValues(target, args, 1);
}

static void
ApplyOptionMenuBackground(Widget reference, Widget optionMenu)
{
    Pixel bg;
    Widget label;
    Widget button;

    if (!reference || !optionMenu)
	return;

    XtVaGetValues(reference, XmNbackground, &bg, nullptr);

    XtVaSetValues(optionMenu, XmNbackground, bg, nullptr);

    label = XtNameToWidget(optionMenu, "*OptionLabel");
    if (label)
	XtVaSetValues(label, XmNbackground, bg, nullptr);

    button = XtNameToWidget(optionMenu, "*OptionButton");
    if (button)
	XtVaSetValues(button, XmNbackground, bg, XmNarmColor, bg, nullptr);
}

static Pixel
GetCursorPreviewBgPixel(void)
{
    Pixel bg = style.secBgCol;

    if (mouse.cursorPreviewArea[0])
	XtVaGetValues(mouse.cursorPreviewArea[0], XmNbackground, &bg, nullptr);
    else if (mouse.cursorPreviewForm)
	XtVaGetValues(mouse.cursorPreviewForm, XmNbackground, &bg, nullptr);

    return bg;
}

static void
GetMaskInfo(unsigned long mask, int *shift, int *bits)
{
    int s = 0;
    int b = 0;

    if (!shift || !bits)
	return;

    if (mask == 0)
    {
	*shift = 0;
	*bits = 0;
	return;
    }

    while ((mask & 1UL) == 0)
    {
	mask >>= 1;
	s++;
    }

    while ((mask & 1UL) != 0)
    {
	mask >>= 1;
	b++;
    }

    *shift = s;
    *bits = b;
}

static unsigned long
PackComponent(unsigned char component, unsigned long mask, int shift, int bits)
{
    unsigned long max;
    unsigned long value;

    if (!mask || bits <= 0)
	return 0;

    if (bits >= (int)(8 * sizeof(unsigned long)))
	max = ~0UL;
    else
	max = (1UL << bits) - 1;

    value = (unsigned long)((component * max + 127) / 255);
    return (value << shift) & mask;
}

#define CURSOR_PREVIEW_SLOT_SIZE 72

static int
GetCursorPreviewSlotSize(Widget ref)
{
    double scale = 1.0;
    Widget w = ref ? ref : style.mouseDialog;
    if (w && XmGetWidgetScale(w, &scale) && scale > 0.25)
    {
	int sz = (int)lround((double)CURSOR_PREVIEW_SLOT_SIZE * scale);
	if (sz >= CURSOR_PREVIEW_SLOT_SIZE)
	    return sz;
    }
    return CURSOR_PREVIEW_SLOT_SIZE;
}

#ifdef HAVE_XCURSOR
static void
ComputeXcursorImageVisibleBounds(const XcursorImage *img,
                                 int *bxOut, int *byOut,
                                 int *bwOut, int *bhOut)
{
    int minX, minY, maxX = -1, maxY = -1;
    int x, y;

    if (!img || img->width == 0 || img->height == 0 || !img->pixels)
    {
	if (bxOut) *bxOut = 0;
	if (byOut) *byOut = 0;
	if (bwOut) *bwOut = 0;
	if (bhOut) *bhOut = 0;
	return;
    }

    minX = (int)img->width;
    minY = (int)img->height;
    for (y = 0; y < (int)img->height; y++)
    {
	const XcursorPixel *row = img->pixels + (size_t)y * img->width;
	for (x = 0; x < (int)img->width; x++)
	{
	    if ((row[x] >> 24) & 0xff)
	    {
		if (x < minX) minX = x;
		if (y < minY) minY = y;
		if (x > maxX) maxX = x;
		if (y > maxY) maxY = y;
	    }
	}
    }

    if (maxX < minX || maxY < minY)
    {
	minX = 0;
	minY = 0;
	maxX = (int)img->width - 1;
	maxY = (int)img->height - 1;
    }

    if (bxOut) *bxOut = minX;
    if (byOut) *byOut = minY;
    if (bwOut) *bwOut = maxX - minX + 1;
    if (bhOut) *bhOut = maxY - minY + 1;
}

static void
ComputeXcursorImagesVisibleBounds(const XcursorImages *imgs,
                                  int *bxOut, int *byOut,
                                  int *bwOut, int *bhOut)
{
    int uMinX = 0x7fffffff, uMinY = 0x7fffffff;
    int uMaxX = -1, uMaxY = -1;
    int fi;

    if (!imgs || imgs->nimage <= 0)
    {
	if (bxOut) *bxOut = 0;
	if (byOut) *byOut = 0;
	if (bwOut) *bwOut = 0;
	if (bhOut) *bhOut = 0;
	return;
    }

    for (fi = 0; fi < imgs->nimage; fi++)
    {
	const XcursorImage *im = imgs->images[fi];
	int bx = 0, by = 0, bw = 0, bh = 0;
	if (!im)
	    continue;
	ComputeXcursorImageVisibleBounds(im, &bx, &by, &bw, &bh);
	if (bw > 0 && bh > 0)
	{
	    if (bx < uMinX) uMinX = bx;
	    if (by < uMinY) uMinY = by;
	    if (bx + bw - 1 > uMaxX) uMaxX = bx + bw - 1;
	    if (by + bh - 1 > uMaxY) uMaxY = by + bh - 1;
	}
    }

    if (uMaxX < uMinX || uMaxY < uMinY)
    {
	ComputeXcursorImageVisibleBounds(imgs->images[0], bxOut, byOut, bwOut, bhOut);
	return;
    }

    if (bxOut) *bxOut = uMinX;
    if (byOut) *byOut = uMinY;
    if (bwOut) *bwOut = uMaxX - uMinX + 1;
    if (bhOut) *bhOut = uMaxY - uMinY + 1;
}

static Pixmap
CreateCursorPreviewPixmapFromImageCrop(const XcursorImage *image,
                                       int cropX, int cropY,
                                       int cropW, int cropH,
                                       int previewW, int previewH,
                                       int *width, int *height)
{
    XColor bgColor;
    unsigned char bgR = 0, bgG = 0, bgB = 0;
    Pixmap pixmap;
    Visual *visual;
    int depth;
    GC gc;
    XImage *ximage;
    unsigned char *data;
    int redShift = 0, greenShift = 0, blueShift = 0;
    int redBits = 0, greenBits = 0, blueBits = 0;
    int x, y;
    int drawW, drawH, maxFitW, maxFitH;
    int offsetX = 0, offsetY = 0;

    if (width) *width = 0;
    if (height) *height = 0;
    if (!image || previewW <= 0 || previewH <= 0)
	return None;

    if (cropW <= 0 || cropH <= 0)
	ComputeXcursorImageVisibleBounds(image, &cropX, &cropY, &cropW, &cropH);
    if (cropX < 0) cropX = 0;
    if (cropY < 0) cropY = 0;
    if (cropX + cropW > (int)image->width)
	cropW = (int)image->width - cropX;
    if (cropY + cropH > (int)image->height)
	cropH = (int)image->height - cropY;
    if (cropW <= 0 || cropH <= 0)
	return None;

    bgColor.pixel = GetCursorPreviewBgPixel();
    if (XQueryColor(style.display, style.colormap, &bgColor))
    {
	bgR = (unsigned char)(bgColor.red >> 8);
	bgG = (unsigned char)(bgColor.green >> 8);
	bgB = (unsigned char)(bgColor.blue >> 8);
    }

    visual = DefaultVisual(style.display, DefaultScreen(style.display));
    depth = DefaultDepth(style.display, DefaultScreen(style.display));
    pixmap = XCreatePixmap(style.display, style.root,
			   (unsigned int)previewW, (unsigned int)previewH, depth);
    if (pixmap == None)
	return None;

    gc = XCreateGC(style.display, pixmap, 0, nullptr);
    if (!gc)
    {
	XFreePixmap(style.display, pixmap);
	return None;
    }

    ximage = XCreateImage(style.display, visual, depth, ZPixmap, 0,
			  nullptr, previewW, previewH, 32, 0);
    if (!ximage)
    {
	XFreeGC(style.display, gc);
	XFreePixmap(style.display, pixmap);
	return None;
    }

    data = (unsigned char *)calloc(1, (size_t)ximage->bytes_per_line * previewH);
    if (!data)
    {
	XFreeGC(style.display, gc);
	XFreePixmap(style.display, pixmap);
	XDestroyImage(ximage);
	return None;
    }
    ximage->data = (char *)data;

    GetMaskInfo(visual->red_mask, &redShift, &redBits);
    GetMaskInfo(visual->green_mask, &greenShift, &greenBits);
    GetMaskInfo(visual->blue_mask, &blueShift, &blueBits);

    unsigned long bgPixel =
	PackComponent(bgR, visual->red_mask, redShift, redBits) |
	PackComponent(bgG, visual->green_mask, greenShift, greenBits) |
	PackComponent(bgB, visual->blue_mask, blueShift, blueBits);

    for (y = 0; y < previewH; y++)
	for (x = 0; x < previewW; x++)
	    XPutPixel(ximage, x, y, bgPixel);

    maxFitW = previewW > 4 ? (previewW - 4) : previewW;
    maxFitH = previewH > 4 ? (previewH - 4) : previewH;
    drawW = cropW;
    drawH = cropH;
    if (drawW > maxFitW || drawH > maxFitH)
    {
	if (drawW * maxFitH >= drawH * maxFitW)
	{
	    drawH = (drawH * maxFitW + drawW / 2) / drawW;
	    drawW = maxFitW;
	}
	else
	{
	    drawW = (drawW * maxFitH + drawH / 2) / drawH;
	    drawH = maxFitH;
	}
	if (drawW < 1) drawW = 1;
	if (drawH < 1) drawH = 1;
    }

    offsetX = (previewW - drawW) / 2;
    offsetY = (previewH - drawH) / 2;
    for (y = 0; y < drawH; y++)
    {
	int srcY = cropY + (drawH == cropH ? y : (y * cropH) / drawH);
	if (srcY < 0 || srcY >= (int)image->height)
	    continue;
	for (x = 0; x < drawW; x++)
	{
	    int srcX = cropX + (drawW == cropW ? x : (x * cropW) / drawW);
	    if (srcX < 0 || srcX >= (int)image->width)
		continue;
	    unsigned long pixel = image->pixels[(size_t)srcY * image->width + (size_t)srcX];
	    unsigned char a = (pixel >> 24) & 0xff;
	    unsigned char r = (pixel >> 16) & 0xff;
	    unsigned char g = (pixel >> 8) & 0xff;
	    unsigned char b = pixel & 0xff;
	    unsigned char outR, outG, outB;
	    unsigned long outPixel;

	    /* Xcursor pixels are already premultiplied ARGB: composite with
	     * Porter-Duff OVER (src + dst * (1 - alpha)) without re-multiplying
	     * src by alpha a second time. */
	    if (a == 255)
	    {
		outR = r;
		outG = g;
		outB = b;
	    }
	    else if (a == 0)
	    {
		outR = bgR;
		outG = bgG;
		outB = bgB;
	    }
	    else
	    {
		int br = (int)r + ((int)bgR * (255 - (int)a) + 127) / 255;
		int bg = (int)g + ((int)bgG * (255 - (int)a) + 127) / 255;
		int bb = (int)b + ((int)bgB * (255 - (int)a) + 127) / 255;
		outR = (unsigned char)(br > 255 ? 255 : br);
		outG = (unsigned char)(bg > 255 ? 255 : bg);
		outB = (unsigned char)(bb > 255 ? 255 : bb);
	    }

	    outPixel = PackComponent(outR, visual->red_mask, redShift, redBits) |
		       PackComponent(outG, visual->green_mask, greenShift, greenBits) |
		       PackComponent(outB, visual->blue_mask, blueShift, blueBits);

	    int dstX = offsetX + x;
	    int dstY = offsetY + y;
	    if (dstX < 0 || dstX >= previewW ||
		dstY < 0 || dstY >= previewH)
		continue;

	    XPutPixel(ximage, dstX, dstY, outPixel);
	}
    }

    XPutImage(style.display, pixmap, gc, ximage, 0, 0, 0, 0,
	      previewW, previewH);

    XFreeGC(style.display, gc);
    XDestroyImage(ximage);
    if (width) *width = previewW;
    if (height) *height = previewH;
    return pixmap;
}

static Pixmap
CreateCursorPreviewPixmapFromImage(const XcursorImage *image,
                                   int previewW, int previewH,
                                   int *width, int *height)
{
    return CreateCursorPreviewPixmapFromImageCrop(
	image, 0, 0, 0, 0, previewW, previewH, width, height);
}
#endif

static Pixmap
CreateCursorPreviewPixmap(int index, const char *theme,
                          int previewSize, int *width, int *height)
{
#ifdef HAVE_XCURSOR
    XcursorImage *image;
    Pixmap pixmap;
    const char *effectiveTheme =
	(theme && theme[0]) ? theme :
	(mouse.cursorTheme && mouse.cursorTheme[0]) ?
	mouse.cursorTheme : XcursorGetTheme(style.display);
    unsigned int effectiveSize = (previewSize > 0) ? (unsigned int)previewSize :
	(mouse.cursorSize > 0) ? (unsigned int)mouse.cursorSize :
	CURSOR_THEME_PREVIEW_ICON_SIZE;
    int previewW = (previewSize > 0) ? previewSize : (int)effectiveSize;
    int previewH = previewW;

    if (previewW <= 0)
	previewW = CURSOR_THEME_PREVIEW_ICON_SIZE;
    if (previewH <= 0)
	previewH = previewW;

    if (width) *width = 0;
    if (height) *height = 0;

    image = XcursorShapeLoadImage(cursorPreviewInfo[index].fontShape,
				  effectiveTheme,
				  effectiveSize);
    if (!image)
	return None;

    pixmap = CreateCursorPreviewPixmapFromImage(image, previewW, previewH, width, height);
    XcursorImageDestroy(image);
    return pixmap;
#else
    if (width) *width = 0;
    if (height) *height = 0;
    (void)index;
    (void)theme;
    (void)previewSize;
    return None;
#endif
}

static Pixmap
CreateCursorThemePreviewPixmap(const char *theme)
{
#ifdef HAVE_XCURSOR
    Visual *visual = DefaultVisual(style.display, DefaultScreen(style.display));
    int depth = DefaultDepth(style.display, DefaultScreen(style.display));
    const int iconSize = CURSOR_THEME_PREVIEW_ICON_SIZE;
    const int stripWidth = iconSize * CURSOR_PREVIEW_COUNT;
    const int stripHeight = iconSize;
    Pixmap strip = XCreatePixmap(style.display, style.root,
				 (unsigned int)stripWidth,
				 (unsigned int)stripHeight,
				 depth);
    if (strip == None)
	return None;

    GC gc = XCreateGC(style.display, strip, 0, nullptr);
    if (!gc)
    {
	XFreePixmap(style.display, strip);
	return None;
    }

    Pixel bgPixel = GetCursorPreviewBgPixel();
    XSetForeground(style.display, gc, bgPixel);
    XFillRectangle(style.display, strip, gc, 0, 0,
		   (unsigned int)stripWidth, (unsigned int)stripHeight);

    int i;
    for (i = 0; i < CURSOR_PREVIEW_COUNT; i++)
    {
	int iconW = 0;
	int iconH = 0;
	Pixmap icon = CreateCursorPreviewPixmap(i, theme, iconSize, &iconW, &iconH);
	if (icon == None)
	    continue;

	int yOffset = (stripHeight - iconH) / 2;
	if (yOffset < 0)
	    yOffset = 0;

	XCopyArea(style.display, icon, strip, gc,
		  0, 0, (unsigned int)iconW, (unsigned int)iconH,
		  i * iconSize, yOffset);
	XFreePixmap(style.display, icon);
    }

    XFreeGC(style.display, gc);
    return strip;
#else
    (void)theme;
    return None;
#endif
}

static char *
DupPrefResourceString(const char *resName, const char *resClass)
{
    char *type;
    char *result = nullptr;
    XrmDatabase db;
    XrmValue value;
    char *prefString;

    prefString = _DtGetResString(style.display, _DT_ATR_PREFS);
    if (!prefString)
	return nullptr;
    if (!prefString[0])
    {
	XtFree(prefString);
	return nullptr;
    }

    db = XrmGetStringDatabase(prefString);
    XtFree(prefString);
    if (!db)
	return nullptr;

    if (XrmGetResource(db, resName, resClass, &type, &value))
    {
	if (value.addr && value.size > 0)
	    result = XtNewString((const char *)value.addr);
    }

    XrmDestroyDatabase(db);
    return result;
}

static char *
DupXrmResourceString(const char *resName, const char *resClass)
{
    XrmDatabase db;
    XrmValue value;
    char *type;

    RefreshXrmDatabaseFromRoot();

    db = XrmGetDatabase(style.display);
    if (db && XrmGetResource(db, resName, resClass, &type, &value))
    {
	if (value.addr && value.size > 0)
	    return XtNewString((const char *)value.addr);
    }

    return DupPrefResourceString(resName, resClass);
}

static int
SnapCursorSize(int requestedSize, int maxSize)
{
    static const int sizes[] = {16, 24, 32, 48, 64};
    int best = 16;
    int bestDist = 0x7fffffff;
    int i;

    if (maxSize < 16)
	maxSize = 16;

    if (requestedSize < 1)
	requestedSize = 24;

    for (i = 0; i < (int)(sizeof(sizes) / sizeof(sizes[0])); i++)
    {
	int s = sizes[i];
	int d;

	if (s > maxSize)
	    continue;

	d = requestedSize - s;
	if (d < 0)
	    d = -d;

	if (d < bestDist)
	{
	    best = s;
	    bestDist = d;
	}
    }

    if (best > maxSize)
	best = maxSize;

    return best;
}

static int
GetEffectiveCursorSize(void)
{
    const char *env = getenv("XCURSOR_SIZE");
    int size = 0;
    char *res;

    RefreshXrmDatabaseFromRoot();

    if (env && *env)
    {
	size = atoi(env);
	if (size > 0)
	    return size;
    }

    res = DupXrmResourceString("Xcursor.size", "Xcursor.Size");
    if (res)
    {
	size = atoi(res);
	XtFree(res);
	if (size > 0)
	    return size;
    }

    return 24;
}

static char *
GetEffectiveCursorTheme(void)
{
    const char *env = getenv("XCURSOR_THEME");
    char *res;

    RefreshXrmDatabaseFromRoot();

    if (env && *env)
	return XtNewString(env);

    res = DupXrmResourceString("Xcursor.theme", "Xcursor.Theme");
    if (res && *res)
	return res;

    if (res)
	XtFree(res);

    return XtNewString("default");
}

static int
QueryMaxCursorSize(void)
{
    unsigned int bestW = 0, bestH = 0;

    if (XQueryBestCursor(style.display, style.root, 128, 128, &bestW, &bestH))
    {
	if (bestW > 0)
	    return (int)bestW;
    }

    return 64;
}

static Cursor
LoadXcursorOrFontCursor(const char *xcursorName, unsigned int fontShape)
{
#ifdef HAVE_XCURSOR
    if (xcursorName && xcursorName[0])
    {
	Cursor cursor = XcursorLibraryLoadCursor(style.display, xcursorName);
	if (cursor != None)
	{
	    return cursor;
	}
    }
#endif
    return XCreateFontCursor(style.display, fontShape);
}

static void
SetRootCursorNow(void)
{
    Cursor cursor = LoadXcursorOrFontCursor("X_cursor", XC_X_cursor);
    if (cursor != None)
    {
	XDefineCursor(style.display, style.root, cursor);
	XFlush(style.display);
	XFreeCursor(style.display, cursor);
    }
}

#ifdef HAVE_XCURSOR
static void
StopCursorPreviewAnimTimer(void)
{
    if (mouse.cursorPreviewAnimTimer != 0)
    {
	XtRemoveTimeOut(mouse.cursorPreviewAnimTimer);
	mouse.cursorPreviewAnimTimer = 0;
    }
}

static void
CursorPreviewAnimTimerCB(XtPointer client_data, XtIntervalId *id)
{
    (void)client_data;
    (void)id;
    mouse.cursorPreviewAnimTimer = 0;

    if (!style.mouseDialog || !XtIsManaged(style.mouseDialog))
	return;

    Boolean anyAnimated = False;
    int previewSz = GetCursorPreviewSlotSize(style.mouseDialog);
    for (int i = 0; i < CURSOR_PREVIEW_COUNT; i++)
    {
	XcursorImages *imgs = mouse.cursorPreviewAnim[i];
	if (!imgs || imgs->nimage <= 1 || !mouse.cursorPreviewArea[i] ||
	    !XtIsRealized(mouse.cursorPreviewArea[i]))
	    continue;

	anyAnimated = True;
	int step = (imgs->images[0] && imgs->images[0]->delay > 0 && imgs->images[0]->delay < 25) ? 2 : 1;
	mouse.cursorPreviewAnimFrame[i] = (mouse.cursorPreviewAnimFrame[i] + step) % imgs->nimage;
	const XcursorImage *frameImg = imgs->images[mouse.cursorPreviewAnimFrame[i]];
	int nw = 0, nh = 0;
	Pixmap npix = CreateCursorPreviewPixmapFromImageCrop(
	    frameImg,
	    mouse.cursorPreviewCropX[i], mouse.cursorPreviewCropY[i],
	    mouse.cursorPreviewCropW[i], mouse.cursorPreviewCropH[i],
	    previewSz, previewSz, &nw, &nh);
	if (npix != None)
	{
	    if (mouse.cursorPreviewPixmaps[i] != None)
		XFreePixmap(style.display, mouse.cursorPreviewPixmaps[i]);
	    mouse.cursorPreviewPixmaps[i] = npix;
	    mouse.cursorPreviewPixmapW[i] = nw;
	    mouse.cursorPreviewPixmapH[i] = nh;

	    Window win = XtWindow(mouse.cursorPreviewArea[i]);
	    int wWidth = XtWidth(mouse.cursorPreviewArea[i]);
	    int wHeight = XtHeight(mouse.cursorPreviewArea[i]);
	    int dstX = (wWidth - nw) / 2;
	    int dstY = (wHeight - nh) / 2;
	    if (dstX < 0) dstX = 0;
	    if (dstY < 0) dstY = 0;
	    GC gc = XCreateGC(style.display, win, 0, nullptr);
	    if (gc)
	    {
		XCopyArea(style.display, npix, win, gc, 0, 0, (unsigned)nw, (unsigned)nh, dstX, dstY);
		XFreeGC(style.display, gc);
	    }
	}
    }
    XFlush(style.display);
    if (anyAnimated)
    {
	mouse.cursorPreviewAnimTimer = XtAppAddTimeOut(
	    XtWidgetToApplicationContext(style.mouseDialog),
	    40, CursorPreviewAnimTimerCB, nullptr);
    }
}
#endif

static void
UpdateCursorPreview(void)
{
    int i;
#ifdef HAVE_XCURSOR
    Boolean anyAnimated = False;
    const char *effectiveTheme =
	(mouse.cursorTheme && mouse.cursorTheme[0]) ?
	mouse.cursorTheme : XcursorGetTheme(style.display);
    unsigned int effectiveSize = (mouse.cursorSize > 0) ? (unsigned int)mouse.cursorSize : 24;

    StopCursorPreviewAnimTimer();
    if (mouse.cursorTheme && mouse.cursorTheme[0])
	XcursorSetTheme(style.display, mouse.cursorTheme);
    XcursorSetDefaultSize(style.display, effectiveSize);
#endif

    for (i = 0; i < CURSOR_PREVIEW_COUNT; i++)
    {
	if (mouse.cursorPreviewCursors[i] != None)
	{
	    XFreeCursor(style.display, mouse.cursorPreviewCursors[i]);
	    mouse.cursorPreviewCursors[i] = None;
	}
	if (mouse.cursorPreviewPixmaps[i] != None)
	{
	    XFreePixmap(style.display, mouse.cursorPreviewPixmaps[i]);
	    mouse.cursorPreviewPixmaps[i] = None;
	}
#ifdef HAVE_XCURSOR
	if (mouse.cursorPreviewAnim[i] != nullptr)
	{
	    XcursorImagesDestroy(mouse.cursorPreviewAnim[i]);
	    mouse.cursorPreviewAnim[i] = nullptr;
	}
	mouse.cursorPreviewAnimFrame[i] = 0;
#endif
	mouse.cursorPreviewPixmapW[i] = 0;
	mouse.cursorPreviewPixmapH[i] = 0;
    }

    for (i = 0; i < CURSOR_PREVIEW_COUNT; i++)
    {
	if (!mouse.cursorPreviewArea[i])
	    continue;

	if (!XtIsRealized(mouse.cursorPreviewArea[i]))
	    continue;

	mouse.cursorPreviewCursors[i] =
	    LoadXcursorOrFontCursor(cursorPreviewInfo[i].name,
				    cursorPreviewInfo[i].fontShape);

	if (mouse.cursorPreviewCursors[i] != None)
	{
	    XDefineCursor(style.display,
			  XtWindow(mouse.cursorPreviewArea[i]),
			  mouse.cursorPreviewCursors[i]);
	}

#ifdef HAVE_XCURSOR
	mouse.cursorPreviewAnim[i] = XcursorLibraryLoadImages(
	    cursorPreviewInfo[i].name, effectiveTheme, (int)effectiveSize);
	if (mouse.cursorPreviewAnim[i] && mouse.cursorPreviewAnim[i]->nimage > 0)
	{
	    ComputeXcursorImagesVisibleBounds(
		mouse.cursorPreviewAnim[i],
		&mouse.cursorPreviewCropX[i], &mouse.cursorPreviewCropY[i],
		&mouse.cursorPreviewCropW[i], &mouse.cursorPreviewCropH[i]);
	    int previewSz = GetCursorPreviewSlotSize(mouse.cursorPreviewArea[i]);
	    mouse.cursorPreviewPixmaps[i] = CreateCursorPreviewPixmapFromImageCrop(
		mouse.cursorPreviewAnim[i]->images[0],
		mouse.cursorPreviewCropX[i], mouse.cursorPreviewCropY[i],
		mouse.cursorPreviewCropW[i], mouse.cursorPreviewCropH[i],
		previewSz, previewSz,
		&mouse.cursorPreviewPixmapW[i],
		&mouse.cursorPreviewPixmapH[i]);
	    if (mouse.cursorPreviewAnim[i]->nimage > 1)
		anyAnimated = True;
	}
	else
#endif
	{
	    mouse.cursorPreviewPixmaps[i] =
		CreateCursorPreviewPixmap(i,
					  mouse.cursorTheme,
					  mouse.cursorSize,
					  &mouse.cursorPreviewPixmapW[i],
					  &mouse.cursorPreviewPixmapH[i]);
	}
	XClearArea(style.display, XtWindow(mouse.cursorPreviewArea[i]),
		   0, 0, 0, 0, True);
    }

#ifdef HAVE_XCURSOR
    if (anyAnimated && style.mouseDialog && XtIsManaged(style.mouseDialog))
    {
	mouse.cursorPreviewAnimTimer = XtAppAddTimeOut(
	    XtWidgetToApplicationContext(style.mouseDialog),
	    40, CursorPreviewAnimTimerCB, nullptr);
    }
#endif

    XFlush(style.display);
}


/*+++++++++++++++++++++++++++++++++++++++*/
/* popup_mouseBB                         */
/*+++++++++++++++++++++++++++++++++++++++*/

void 
popup_mouseBB( Widget shell )
{
    if (style.mouseDialog == nullptr) {
        _DtTurnOnHourGlass(shell);  
        build_mouseDialog(shell);
        mouse.systemDefaultFlag = True;
	mouse.dclickChanged = False;
	mouse.btn2Changed = False;
        XtManageChild(style.mouseDialog);
        _DtTurnOffHourGlass(shell);  
    }
    else 
    {
        XtManageChild(style.mouseDialog);
        raiseWindow(XtWindow(XtParent(style.mouseDialog)));
    }
}

/*+++++++++++++++++++++++++++++++++++++++*/
/* build__mouseDialog                    */
/*+++++++++++++++++++++++++++++++++++++++*/
static Widget 
build_mouseDialog( Widget shell )
{
    int     i, n;
    Arg              args[MAX_ARGS];
    int              nmap;
    XmString         button_string[NUM_LABELS]; 
    Widget           form;
    int              count = 0;
    Widget           widget_list[48]; 
    XmString         string;
    
    Dimension        MaxLabelWidth1;
    Dimension        HandLabelWidth;
    Dimension        MiddleLabelWidth;
    Dimension        MaxToggleWidth;
    Dimension        RightToggleWidth;
    Dimension        TransferToggleWidth;

    XrmDatabase database;
    char *str_name;
    char *str_class;
    char **str_type_return;
    XrmValue value_return;
    
    /* check hil device# for 2 or 3 button mouse      */
    /* IMPORTANT NOTE:                                */
    /* X servers will return different values for     */
    /* mouse.numButtons in the call below.            */ 
    /* The HP X server will return 5 if a quarted     */ 
    /* threebutton mouse is present and a 3 if a      */
    /* quarted two-button mouse is present. The IBM   */
    /* does not support a two-button mouse.           */
    /* The cod ebelow always loads a three-button     */
    /* for IBM/Sun/HP unless a non-quarted two-button */
    /* mouse is present on the HP server.             */
    /* When porting this code to a non IBM/Sun/HP     */
    /* platform that has or supports a two-button     */
    /* mouse the code below may have to be changed to */
    /* take into account the behavior of the new      */
    /* server.                                        */ 
    
    EnsureXcursorPath();
    nmap = 256;
    mouse.numButtons = XGetPointerMapping(style.display, mouse.map_return, 
					   nmap);
    if ( mouse.numButtons == 2 )
          mouse.type = POINTER2B;
    else
        mouse.type = POINTER3B;
    /* choose a bitmap based on button mapping   *
     * and number of buttons                     */

    if(mouse.type == POINTER3B) 
    {
        mouse.b3     = LoadMouseBitmap(B3, style.secBSCol, style.secTSCol);
        mouse.b3_rev = LoadMouseBitmap(B3_REV, style.secBSCol, style.secTSCol);
        mouse.b3Sel  = LoadMouseBitmap(B3, style.secTSCol, style.secBSCol);
        mouse.b3_revSel = LoadMouseBitmap(B3_REV, style.secTSCol, style.secBSCol);

        if(mouse.map_return[0] == 1) 
	{
            mouse.pixmap = mouse.b3;
            mouse.pixmapSel = mouse.b3Sel;
        }
        else 
	{
            mouse.pixmap = mouse.b3_rev;
            mouse.pixmapSel = mouse.b3_revSel;
        }
    }
    else /* 2 button mouse */ 
    {
	mouse.b2        = LoadMouseBitmap(B2, style.secBSCol, style.secTSCol);
	mouse.b2_rev    = LoadMouseBitmap(B2_REV, style.secBSCol, style.secTSCol);
        mouse.b2Sel     = LoadMouseBitmap(B2, style.secTSCol, style.secBSCol);
        mouse.b2_revSel = LoadMouseBitmap(B2_REV, style.secTSCol, style.secBSCol);

        if(mouse.map_return[0] == 1)
        {
            mouse.pixmap = mouse.b2;
            mouse.pixmapSel = mouse.b2Sel;
        }
        else
        {
            mouse.pixmap = mouse.b2_rev;
            mouse.pixmapSel = mouse.b2_revSel;
        }
    }

    /*Get the value of doubleclick*/
    {
        char *mctRes = DupXrmResourceString("dtstyle.multiClickTime", "Dtstyle.MultiClickTime");
        if (!mctRes)
            mctRes = DupXrmResourceString("multiClickTime", "MultiClickTime");
        if (mctRes)
        {
            int parsed = atoi(mctRes);
            XtFree(mctRes);
            if (parsed >= 100 && parsed <= 2000)
                XtSetMultiClickTime(style.display, parsed);
        }
        else if (XtGetMultiClickTime(style.display) <= 200)
        {
            XtSetMultiClickTime(style.display, DCLICKDEFAULT);
        }
    }
    mouse.mydclick = XtGetMultiClickTime(style.display);
    if (mouse.mydclick > 0)
      mouse.mydclick = mouse.mydclick/100;
    if (mouse.mydclick < 1)
      mouse.mydclick = 1;

    /*Get the value of the enableBtn1Transfer resource */
    /*from the data base. */

    str_name = "*enableBtn1Transfer";
    str_class = "*EnableBtn1Transfer";
    
    database = XrmGetDatabase(style.display); 
    if (!XrmGetResource(database, str_name, str_class, (char **)&str_type_return, &value_return))
      {	
	mouse.function = TRANSFER;
	mouse.origFunction = TRANSFER;
      }
    else 
      if (strcmp(value_return.addr, "True") == 0) 
	{
	  mouse.function = ADJUST;
	  mouse.origFunction = ADJUST;
	}
      else
	{
	  mouse.function = TRANSFER;
	  mouse.origFunction = TRANSFER;
	}
  
    /* Set up DialogBoxDialog button labels */
    button_string[0] = CMPSTR((String) _DtOkString);
    button_string[1] = CMPSTR((String) _DtCancelString);
    button_string[2] = CMPSTR((String) _DtHelpString);

    /* Create toplevel DialogBox */
    /* saveRestore
     * Note that save.poscnt has been initialized elsewhere.  
     * save.posArgs may contain information from restoreMouse().*/

    XtSetArg(save.posArgs[save.poscnt], XmNbuttonCount, NUM_LABELS);  
    save.poscnt++;
    XtSetArg(save.posArgs[save.poscnt], XmNbuttonLabelStrings, button_string);
    save.poscnt++;
    XtSetArg (save.posArgs[save.poscnt], XmNdefaultPosition, False); 
    save.poscnt++;
    style.mouseDialog = __DtCreateDialogBoxDialog(shell, mouseString, 
                   save.posArgs, save.poscnt);
    XtAddCallback(style.mouseDialog, XmNcallback, ButtonCB, nullptr);
    XtAddCallback(style.mouseDialog, XmNhelpCallback,
            HelpRequestCB, (XtPointer)HELP_MOUSE_DIALOG);


    XmStringFree(button_string[0]);
    XmStringFree(button_string[1]);
    XmStringFree(button_string[2]);

    widget_list[0] = _DtDialogBoxGetButton(style.mouseDialog,2);
    n=0;
    XtSetArg(args[n], XmNautoUnmanage, False); n++;
    XtSetArg(args[n], XmNcancelButton, widget_list[0]); n++;
    XtSetValues (style.mouseDialog, args, n);

    n=0;
    XtSetArg(args[n], XmNtitle, ((char *)GETMESSAGE(9, 18, "Style Manager - Mouse"))); n++;
    XtSetArg (args[n], XmNuseAsyncGeometry, True); n++;
    XtSetArg(args[n], XmNmwmFunctions, DIALOG_MWM_FUNC); n++;  
    XtSetValues (XtParent(style.mouseDialog), args, n);

    n = 0;
    XtSetArg(args[n], XmNhorizontalSpacing, style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNverticalSpacing, style.verticalSpacing); n++;
    XtSetArg(args[n], XmNallowOverlap, False); n++;
    XtSetArg(args[n], XmNchildType, XmWORK_AREA);  n++;
    form = XmCreateForm(style.mouseDialog, "mouseForm", args, n);

    n = 0;
    XtSetArg(args[n], XmNmarginHeight, LB_MARGIN_HEIGHT);  n++;
    XtSetArg(args[n], XmNmarginWidth, LB_MARGIN_WIDTH);  n++;
    string = CMPSTR(((char *)GETMESSAGE(9, 5, "Default")));
    XtSetArg(args[n], XmNlabelString, string); n++;
    widget_list[count++] = 
    mouse.systemDefault= XmCreatePushButtonGadget(form, "systemDefault", args, n);
    XmStringFree(string);

    n = 0;
    XtSetArg(args[n], XmNorientation, XmVERTICAL); n++; 
    widget_list[count++] = 
    mouse.pictFrame= XmCreateFrame(form, "pictFrame", args, n);

    n = 0;
    XtSetArg(args[n], XmNorientation, XmVERTICAL); n++; 
    XtSetArg(args[n], XmNadjustMargin, False); n++; 
    XtSetArg(args[n], XmNentryAlignment, XmALIGNMENT_BEGINNING); n++; 
    mouse.pictRC = XmCreateRowColumn(mouse.pictFrame, "pictRC", args, n);

    n = 0;
    XtSetArg(args[n], XmNlabelType, XmPIXMAP); n++; 
    XtSetArg(args[n], XmNlabelPixmap, mouse.pixmap); n++; 
    XtSetArg(args[n], XmNrecomputeSize, False); n++; 
    XtSetArg(args[n], XmNmarginWidth, 0); n++; 
    XtSetArg(args[n], XmNmarginHeight, 0); n++; 
    XtSetArg(args[n], XmNshadowThickness, 0); n++; 
    XtSetArg(args[n], XmNmultiClick, XmMULTICLICK_KEEP); n++;
    mouse.pictButton= XmCreatePushButton(mouse.pictRC, "mousePict", args, n);

    n = 0;
    string = CMPSTR ((char *)GETMESSAGE(9, 17, "Test\nDouble-Click"));
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNshadowThickness, 0); n++;
    XtSetArg(args[n], XmNhighlightThickness, 0); n++;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING); n++;
    mouse.pictLabel= XmCreatePushButton(mouse.pictRC, "mouseLabel", args, n);
    XmStringFree(string);

    n = 0;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING); n++;
    XtSetArg(args[n], XmNmarginHeight, 0); n++;
    string = CMPSTR(HANDMSG);
    XtSetArg(args[n], XmNlabelString, string); n++;
    widget_list[count++] = 
    mouse.handLabGad= XmCreateLabelGadget(form,"handLabGad", args, n);
    XmStringFree(string);

    n=0;
    string = CMPSTR(RIGHTMSG); 
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNnavigationType, XmTAB_GROUP); n++;  
    XtSetArg(args[n], XmNindicatorType, XmONE_OF_MANY); n++;  
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING); n++;
    mouse.rightToggle= XmCreateToggleButtonGadget(form, 
                   "rightToggle", args, n);
    XmStringFree(string);

    n=0;
    string = CMPSTR(LEFTMSG); 
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNnavigationType, XmTAB_GROUP); n++;  
    XtSetArg(args[n], XmNindicatorType, XmONE_OF_MANY); n++;  
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING); n++;
    mouse.leftToggle= XmCreateToggleButtonGadget(form, 
                  "leftToggle", args, n);
    XmStringFree(string);


    n = 0;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING); n++;
    XtSetArg(args[n], XmNmarginHeight, 0); n++;
    string = CMPSTR(MIDDLEMSG);
    XtSetArg(args[n], XmNlabelString, string); n++;
    widget_list[count++] =
    mouse.middleLabGad= XmCreateLabelGadget(form,"middleLabGad", args, n);
    XmStringFree(string);

    n=0;
    string = CMPSTR(TRANSMSG); 
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNindicatorType, XmONE_OF_MANY); n++;  
    XtSetArg(args[n], XmNnavigationType, XmTAB_GROUP); n++;  
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING); n++;
    mouse.transferToggle= XmCreateToggleButtonGadget(form, 
                   "transToggle", args, n);
    XmStringFree(string);

    n=0;
    string = CMPSTR(ADJMSG); 
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNnavigationType, XmTAB_GROUP); n++;  
    XtSetArg(args[n], XmNindicatorType, XmONE_OF_MANY); n++;  
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING); n++;
    mouse.adjustToggle= XmCreateToggleButtonGadget(form, 
                  "adjToggle", args, n);
    XmStringFree(string);

    n = 0;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_END); n++;
    XtSetArg(args[n], XmNmarginHeight, 0); n++;
    string = CMPSTR(((char *)GETMESSAGE(9, 6, "Double-Click")));
    XtSetArg(args[n], XmNlabelString, string); n++;
    widget_list[count++] = 
    mouse.dclickLabGad= XmCreateLabelGadget(form,"dclickLabGad", args, n);
    XmStringFree(string);

    n = 0;
    XtSetArg(args[n], XmNmaximum, DCLICK_MAX); n++; 
    XtSetArg(args[n], XmNminimum, DCLICK_MIN); n++; 
    XtSetArg(args[n], XmNorientation, XmHORIZONTAL); n++;
    XtSetArg(args[n], XmNprocessingDirection, XmMAX_ON_RIGHT); n++;
    XtSetArg(args[n], XmNhighlightThickness, SCALE_HIGHLIGHT_THICKNESS);  n++; 
    XtSetArg(args[n], XmNshowValue, True); n++;
    XtSetArg(args[n], XmNvalue, mouse.mydclick); n++;
    XtSetArg(args[n], XmNdecimalPoints, (short)1); n++;
    widget_list[count++] = 
    mouse.dclickScale= XmCreateScale(form,"dclickScale", args, n);

    n = 0;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_END); n++;
    XtSetArg(args[n], XmNmarginHeight, 0); n++;
    string = CMPSTR(((char *)GETMESSAGE(9, 8, "Acceleration")));
    XtSetArg(args[n], XmNlabelString, string); n++;
    widget_list[count++] = 
    mouse.accelLabGad= XmCreateLabelGadget(form,"accelLabGad", args, n);
    XmStringFree(string);

    n = 0;
    XtSetArg(args[n], XmNminimum, ACCEL_MIN); n++; 
    XtSetArg(args[n], XmNmaximum, ACCEL_MAX); n++; 
    XtSetArg(args[n], XmNorientation, XmHORIZONTAL); n++;
    XtSetArg(args[n], XmNprocessingDirection, XmMAX_ON_RIGHT); n++;
    XtSetArg(args[n], XmNhighlightThickness, SCALE_HIGHLIGHT_THICKNESS);  n++; 
    XtSetArg(args[n], XmNshowValue, True); n++;
    widget_list[count++] = 
    mouse.accelScale= XmCreateScale(form,"accelScale", args, n);

    n = 0;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_END); n++;
    XtSetArg(args[n], XmNmarginHeight, 0); n++;
    string = CMPSTR(((char *)GETMESSAGE(9, 9, "Threshold")));
    XtSetArg(args[n], XmNlabelString, string); n++;
    widget_list[count++] = 
    mouse.threshLabGad= XmCreateLabelGadget(form,"threshLabGad", args, n);
    XmStringFree(string);

    n = 0;
    XtSetArg(args[n], XmNminimum, THRESH_MIN); n++; 
    XtSetArg(args[n], XmNmaximum, THRESH_MAX); n++; 
    XtSetArg(args[n], XmNorientation, XmHORIZONTAL); n++;
    XtSetArg(args[n], XmNprocessingDirection, XmMAX_ON_RIGHT); n++;
    XtSetArg(args[n], XmNhighlightThickness, SCALE_HIGHLIGHT_THICKNESS);  n++; 
    XtSetArg(args[n], XmNshowValue, True); n++;
    widget_list[count++] = 
    mouse.threshScale= XmCreateScale(form,"threshScale", args, n);

    /* Xcursor theme/size controls */
    int oldCursorThemeCount = mouse.cursorThemeCount;
    Pixmap *oldCursorThemePreviewPixmaps = mouse.cursorThemePreviewPixmaps;
    mouse.cursorThemes = nullptr;
    mouse.cursorThemeItems = nullptr;
    mouse.cursorThemeCount = 0;
    mouse.cursorChanged = False;

    mouse.maxCursorSize = QueryMaxCursorSize();
    mouse.cursorSize = GetEffectiveCursorSize();
    mouse.cursorSize = SnapCursorSize(mouse.cursorSize, mouse.maxCursorSize);
    mouse.origCursorSize = mouse.cursorSize;

    mouse.cursorTheme = GetEffectiveCursorTheme();
    mouse.origCursorTheme = XtNewString(mouse.cursorTheme);

    n = 0;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_END); n++;
    XtSetArg(args[n], XmNmarginHeight, 0); n++;
    string = CMPSTR(CURSOR_SIZE_MSG);
    XtSetArg(args[n], XmNlabelString, string); n++;
    widget_list[count++] =
    mouse.cursorSizeLabGad = XmCreateLabelGadget(form, "cursorSizeLabGad", args, n);
    XmStringFree(string);

    n = 0;
    XtSetArg(args[n], XmNorientation, XmHORIZONTAL); n++;
    XtSetArg(args[n], XmNpacking, XmPACK_TIGHT); n++;
    XtSetArg(args[n], XmNspacing, style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNradioBehavior, True); n++;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING); n++;
    widget_list[count++] =
    mouse.cursorSizeRC = XmCreateRowColumn(form, "cursorSizeRC", args, n);

    mouse.cursorSizeChoiceCount = 0;
    for (i = 0; i < CURSOR_SIZE_CHOICE_COUNT; i++)
    {
	XmString sizeLabel;
	char sizeBuf[16];
	int sizeValue = cursorSizeChoices[i];

	if (sizeValue > mouse.maxCursorSize)
	    continue;

	snprintf(sizeBuf, sizeof(sizeBuf), "%d", sizeValue);
	sizeLabel = XmStringCreateLocalized(sizeBuf);

	n = 0;
	XtSetArg(args[n], XmNlabelString, sizeLabel); n++;
	XtSetArg(args[n], XmNindicatorType, XmONE_OF_MANY); n++;
	XtSetArg(args[n], XmNset, sizeValue == mouse.cursorSize); n++;
	mouse.cursorSizeToggles[mouse.cursorSizeChoiceCount] =
	    XmCreateToggleButtonGadget(mouse.cursorSizeRC, "cursorSizeToggle", args, n);
	mouse.cursorSizeChoiceValues[mouse.cursorSizeChoiceCount] = sizeValue;
	XmStringFree(sizeLabel);
	XtAddCallback(mouse.cursorSizeToggles[mouse.cursorSizeChoiceCount],
		      XmNvalueChangedCallback,
		      cursorSizeChangedCB, (XtPointer)(intptr_t)sizeValue);
	XtManageChild(mouse.cursorSizeToggles[mouse.cursorSizeChoiceCount]);
	mouse.cursorSizeChoiceCount++;
    }

    n = 0;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_END); n++;
    XtSetArg(args[n], XmNmarginHeight, 0); n++;
    string = CMPSTR(CURSOR_THEME_MSG);
    XtSetArg(args[n], XmNlabelString, string); n++;
    widget_list[count++] =
    mouse.cursorThemeLabGad = XmCreateLabelGadget(form, "cursorThemeLabGad", args, n);
    XmStringFree(string);

    n = 0;
    mouse.cursorThemeMenu = XmCreatePulldownMenu(form, "cursorThemeMenu", args, n);

    {
	int cap = 0;
	char iconsDir[1024];
	const char *home = getenv("HOME");
	int i;
	Widget history = (Widget)nullptr;

	AddTheme(&mouse.cursorThemes, &mouse.cursorThemeItems,
		 &mouse.cursorThemeCount, &cap, "default");

	if (home && *home)
	{
	    snprintf(iconsDir, sizeof(iconsDir), "%s/.icons", home);
	    ScanThemeDir(iconsDir, &mouse.cursorThemes, &mouse.cursorThemeItems,
			 &mouse.cursorThemeCount, &cap);
	    snprintf(iconsDir, sizeof(iconsDir), "%s/.local/share/icons", home);
	    ScanThemeDir(iconsDir, &mouse.cursorThemes, &mouse.cursorThemeItems,
			 &mouse.cursorThemeCount, &cap);
	}
	ScanThemeDir("/usr/share/icons", &mouse.cursorThemes, &mouse.cursorThemeItems,
		     &mouse.cursorThemeCount, &cap);

	if (oldCursorThemePreviewPixmaps)
	{
	    for (i = 0; i < oldCursorThemeCount; i++)
	    {
		if (oldCursorThemePreviewPixmaps[i] != None)
		    XFreePixmap(style.display,
				oldCursorThemePreviewPixmaps[i]);
	    }
	    XtFree((char *)oldCursorThemePreviewPixmaps);
	}
	if (mouse.cursorThemeCount > 0)
	    mouse.cursorThemePreviewPixmaps =
		(Pixmap *)XtCalloc((Cardinal)mouse.cursorThemeCount,
				  (Cardinal)sizeof(Pixmap));
	else
	    mouse.cursorThemePreviewPixmaps = nullptr;

	for (i = 0; i < mouse.cursorThemeCount; i++)
	{
	    Pixmap preview = CreateCursorThemePreviewPixmap(mouse.cursorThemes[i]);
	    XmString t = XmStringCreateLocalized(mouse.cursorThemes[i]);
	    n = 0;
	    XtSetArg(args[n], XmNlabelString, t); n++;
	    if (preview != None)
	    {
		XtSetArg(args[n], XmNlabelPixmap, preview); n++;
		mouse.cursorThemePreviewPixmaps[i] = preview;
	    }
	    mouse.cursorThemeItems[i] =
		XmCreatePushButtonGadget(mouse.cursorThemeMenu, "cursorThemeItem", args, n);
	    XmStringFree(t);
	    XtAddCallback(mouse.cursorThemeItems[i], XmNactivateCallback,
			  cursorThemeSelectCB, (XtPointer)mouse.cursorThemes[i]);
	    XtManageChild(mouse.cursorThemeItems[i]);

	    if (!history && mouse.cursorTheme &&
		strcmp(mouse.cursorThemes[i], mouse.cursorTheme) == 0)
	    {
		history = mouse.cursorThemeItems[i];
	    }
	}

	if (!history && mouse.cursorThemeCount > 0)
	    history = mouse.cursorThemeItems[0];

	n = 0;
	XtSetArg(args[n], XmNsubMenuId, mouse.cursorThemeMenu); n++;
	if (history)
	    XtSetArg(args[n], XmNmenuHistory, history), n++;
	widget_list[count++] =
	mouse.cursorThemeOM = XmCreateOptionMenu(form, "cursorThemeOM", args, n);
    }

    n = 0;
    string = CMPSTR(CURSOR_PREVIEW_MSG);
    XtSetArg(args[n], XmNlabelString, string); n++;
    widget_list[count++] =
    mouse.cursorPreviewFrame = XmCreateFrame(form, "cursorPreviewFrame", args, n);
    XmStringFree(string);

    n = 0;
    mouse.cursorPreviewForm = XmCreateForm(mouse.cursorPreviewFrame, "cursorPreviewForm", args, n);
    XtManageChild(mouse.cursorPreviewForm);

    n = 0;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
    string = CMPSTR(CURSOR_PREVIEW_HINT_MSG);
    XtSetArg(args[n], XmNlabelString, string); n++;
    mouse.cursorPreviewHint = XmCreateLabelGadget(mouse.cursorPreviewForm, "cursorPreviewHint", args, n);
    XmStringFree(string);
    XtManageChild(mouse.cursorPreviewHint);

    n = 0;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNtopWidget, mouse.cursorPreviewHint); n++;
    XtSetArg(args[n], XmNtopOffset, 4); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNbottomOffset, style.verticalSpacing); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftOffset, style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightOffset, style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNfractionBase, CURSOR_PREVIEW_COUNT); n++;
    mouse.cursorPreviewGrid = XmCreateForm(mouse.cursorPreviewForm, "cursorPreviewGrid", args, n);
    XtManageChild(mouse.cursorPreviewGrid);

    {
	int previewSize = GetCursorPreviewSlotSize(mouse.cursorPreviewGrid);

	for (i = 0; i < CURSOR_PREVIEW_COUNT; i++)
	{
	    Widget cell;
	    Widget previewLabel;
	    XmString label;

	    n = 0;
	    XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
	    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_FORM); n++;
	    XtSetArg(args[n], XmNleftAttachment, XmATTACH_POSITION); n++;
	    XtSetArg(args[n], XmNleftPosition, i); n++;
	    XtSetArg(args[n], XmNrightAttachment, XmATTACH_POSITION); n++;
	    XtSetArg(args[n], XmNrightPosition, i + 1); n++;
	    cell = XmCreateForm(mouse.cursorPreviewGrid, "cursorPreviewCell", args, n);
	    mouse.cursorPreviewCell[i] = cell;
	    XtManageChild(cell);

	    n = 0;
	    label = CMPSTR((char *)GETMESSAGE(9,
			 cursorPreviewInfo[i].labelMsg,
			 cursorPreviewInfo[i].labelDefault));
	    XtSetArg(args[n], XmNlabelString, label); n++;
	    XtSetArg(args[n], XmNalignment, XmALIGNMENT_CENTER); n++;
	    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_FORM); n++;
	    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
	    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
	    previewLabel = XmCreateLabelGadget(cell, "cursorPreviewLabel", args, n);
	    mouse.cursorPreviewLabel[i] = previewLabel;
	    XmStringFree(label);
	    XtManageChild(previewLabel);

	    n = 0;
	    XtSetArg(args[n], XmNwidth, previewSize); n++;
	    XtSetArg(args[n], XmNheight, previewSize); n++;
	    XtSetArg(args[n], XmNmarginWidth, 0); n++;
	    XtSetArg(args[n], XmNmarginHeight, 0); n++;
	    XtSetArg(args[n], XmNshadowThickness, 0); n++;
	    XtSetArg(args[n], XmNborderWidth, 0); n++;
	    XtSetArg(args[n], XmNhighlightThickness, 0); n++;
	    XtSetArg(args[n], XmNresizePolicy, XmRESIZE_NONE); n++;
	    XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
	    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_WIDGET); n++;
	    XtSetArg(args[n], XmNbottomWidget, previewLabel); n++;
	    XtSetArg(args[n], XmNbottomOffset, 2); n++;
	    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
	    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
	    mouse.cursorPreviewArea[i] = XmCreateDrawingArea(cell, "cursorPreviewArea", args, n);
	    XtManageChild(mouse.cursorPreviewArea[i]);
	    XtAddCallback(mouse.cursorPreviewArea[i], XmNexposeCallback,
			  cursorPreviewExposeCB, (XtPointer)(intptr_t)i);
	}
    }

    /* Scroll Wheel configuration frame (Direction, Smooth Scrolling, Speed) */
    LoadScrollWheelResources(&mouse.wheelNatural, &mouse.wheelSpeed, &mouse.wheelSmooth);
    mouse.origWheelNatural = mouse.wheelNatural;
    mouse.origWheelSpeed = mouse.wheelSpeed;
    mouse.origWheelSmooth = mouse.wheelSmooth;
    mouse.wheelChanged = False;

    n = 0;
    XtSetArg(args[n], XmNshadowType, XmSHADOW_ETCHED_IN); n++;
    XtSetArg(args[n], XmNmarginWidth, style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNmarginHeight, style.verticalSpacing); n++;
    widget_list[count++] =
    mouse.scrollWheelFrame = XmCreateFrame(form, "scrollWheelFrame", args, n);

    n = 0;
    string = CMPSTR(WHEEL_FRAME_MSG);
    XtSetArg(args[n], XmNchildType, XmFRAME_TITLE_CHILD); n++;
    XtSetArg(args[n], XmNchildHorizontalAlignment, XmALIGNMENT_BEGINNING); n++;
    XtSetArg(args[n], XmNlabelString, string); n++;
    mouse.scrollWheelFrameTitle =
	XmCreateLabelGadget(mouse.scrollWheelFrame, "scrollWheelFrameTitle", args, n);
    XmStringFree(string);
    XtManageChild(mouse.scrollWheelFrameTitle);

    n = 0;
    XtSetArg(args[n], XmNfractionBase, 100); n++;
    XtSetArg(args[n], XmNhorizontalSpacing, style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNverticalSpacing, 2); n++;
    mouse.scrollWheelForm =
	XmCreateForm(mouse.scrollWheelFrame, "scrollWheelForm", args, n);
    XtManageChild(mouse.scrollWheelForm);

    n = 0;
    string = CMPSTR(WHEEL_DIR_MSG);
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    mouse.wheelDirLabel =
	XmCreateLabelGadget(mouse.scrollWheelForm, "wheelDirLabel", args, n);
    XmStringFree(string);
    XtManageChild(mouse.wheelDirLabel);

    n = 0;
    XtSetArg(args[n], XmNorientation, XmHORIZONTAL); n++;
    XtSetArg(args[n], XmNpacking, XmPACK_TIGHT); n++;
    XtSetArg(args[n], XmNmarginHeight, 0); n++;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNleftWidget, mouse.wheelDirLabel); n++;
    XtSetArg(args[n], XmNleftOffset, style.horizontalSpacing); n++;
    mouse.wheelDirRC =
	XmCreateRadioBox(mouse.scrollWheelForm, "wheelDirRC", args, n);
    XtManageChild(mouse.wheelDirRC);

    XtVaSetValues(mouse.wheelDirLabel,
		  XmNtopAttachment, XmATTACH_OPPOSITE_WIDGET,
		  XmNtopWidget, mouse.wheelDirRC,
		  XmNbottomAttachment, XmATTACH_OPPOSITE_WIDGET,
		  XmNbottomWidget, mouse.wheelDirRC,
		  nullptr);

    n = 0;
    string = CMPSTR(WHEEL_STD_MSG);
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNindicatorType, XmONE_OF_MANY); n++;
    mouse.wheelStandardToggle =
	XmCreateToggleButtonGadget(mouse.wheelDirRC, "wheelStandardToggle", args, n);
    XmStringFree(string);
    XtManageChild(mouse.wheelStandardToggle);
    XtAddCallback(mouse.wheelStandardToggle, XmNvalueChangedCallback, scrollWheelChangedCB, nullptr);

    n = 0;
    string = CMPSTR(WHEEL_NAT_MSG);
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNindicatorType, XmONE_OF_MANY); n++;
    mouse.wheelNaturalToggle =
	XmCreateToggleButtonGadget(mouse.wheelDirRC, "wheelNaturalToggle", args, n);
    XmStringFree(string);
    XtManageChild(mouse.wheelNaturalToggle);
    XtAddCallback(mouse.wheelNaturalToggle, XmNvalueChangedCallback, scrollWheelChangedCB, nullptr);

    n = 0;
    string = CMPSTR(WHEEL_SMOOTH_MSG);
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING); n++;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_OPPOSITE_WIDGET); n++;
    XtSetArg(args[n], XmNtopWidget, mouse.wheelDirRC); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_OPPOSITE_WIDGET); n++;
    XtSetArg(args[n], XmNbottomWidget, mouse.wheelDirRC); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNleftWidget, mouse.wheelDirRC); n++;
    XtSetArg(args[n], XmNleftOffset, style.horizontalSpacing * 2); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
    mouse.wheelSmoothToggle =
	XmCreateToggleButtonGadget(mouse.scrollWheelForm, "wheelSmoothToggle", args, n);
    XmStringFree(string);
    XtManageChild(mouse.wheelSmoothToggle);
    XtAddCallback(mouse.wheelSmoothToggle, XmNvalueChangedCallback, scrollWheelChangedCB, nullptr);

    n = 0;
    string = CMPSTR(WHEEL_SPEED_MSG);
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNbottomOffset, 2); n++;
    mouse.wheelSpeedLabel =
	XmCreateLabelGadget(mouse.scrollWheelForm, "wheelSpeedLabel", args, n);
    XmStringFree(string);
    XtManageChild(mouse.wheelSpeedLabel);

    n = 0;
    XtSetArg(args[n], XmNorientation, XmHORIZONTAL); n++;
    XtSetArg(args[n], XmNminimum, 1); n++;
    XtSetArg(args[n], XmNmaximum, 10); n++;
    XtSetArg(args[n], XmNvalue, mouse.wheelSpeed); n++;
    XtSetArg(args[n], XmNshowValue, True); n++;
    XtSetArg(args[n], XmNscaleMultiple, 1); n++;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNtopWidget, mouse.wheelDirRC); n++;
    XtSetArg(args[n], XmNtopOffset, 2); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNleftWidget, mouse.wheelSpeedLabel); n++;
    XtSetArg(args[n], XmNleftOffset, style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_FORM); n++;
    mouse.wheelSpeedScale =
	XmCreateScale(mouse.scrollWheelForm, "wheelSpeedScale", args, n);
    XtManageChild(mouse.wheelSpeedScale);
    XtAddCallback(mouse.wheelSpeedScale, XmNvalueChangedCallback, scrollWheelChangedCB, nullptr);
    XtAddCallback(mouse.wheelSpeedScale, XmNdragCallback, scrollWheelChangedCB, nullptr);

    SyncScrollWheelControlsFromState();

    /* Touchpad configuration frame (libinput on Wayland, XInput2 on X11) */
    LoadTouchpadResources(&mouse.tpTap, &mouse.tpNatural, &mouse.tpClickfinger,
			  &mouse.tpDwt, &mouse.tpDext);
    mouse.origTpTap = mouse.tpTap;
    mouse.origTpNatural = mouse.tpNatural;
    mouse.origTpClickfinger = mouse.tpClickfinger;
    mouse.origTpDwt = mouse.tpDwt;
    mouse.origTpDext = mouse.tpDext;
    mouse.tpChanged = False;

    n = 0;
    XtSetArg(args[n], XmNshadowType, XmSHADOW_ETCHED_IN); n++;
    XtSetArg(args[n], XmNmarginWidth, style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNmarginHeight, style.verticalSpacing); n++;
    widget_list[count++] =
    mouse.touchpadFrame = XmCreateFrame(form, "touchpadFrame", args, n);

    n = 0;
    string = CMPSTR(TOUCHPAD_FRAME_MSG);
    XtSetArg(args[n], XmNchildType, XmFRAME_TITLE_CHILD); n++;
    XtSetArg(args[n], XmNchildHorizontalAlignment, XmALIGNMENT_BEGINNING); n++;
    XtSetArg(args[n], XmNlabelString, string); n++;
    mouse.touchpadFrameTitle =
	XmCreateLabelGadget(mouse.touchpadFrame, "touchpadFrameTitle", args, n);
    XmStringFree(string);
    XtManageChild(mouse.touchpadFrameTitle);

    n = 0;
    XtSetArg(args[n], XmNfractionBase, 100); n++;
    XtSetArg(args[n], XmNhorizontalSpacing, style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNverticalSpacing, 2); n++;
    mouse.touchpadForm =
	XmCreateForm(mouse.touchpadFrame, "touchpadForm", args, n);
    XtManageChild(mouse.touchpadForm);

    n = 0;
    string = CMPSTR(TP_TAP_MSG);
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING); n++;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_POSITION); n++;
    XtSetArg(args[n], XmNrightPosition, 49); n++;
    mouse.tpTapToggle =
	XmCreateToggleButtonGadget(mouse.touchpadForm, "tpTapToggle", args, n);
    XmStringFree(string);
    XtManageChild(mouse.tpTapToggle);
    XtAddCallback(mouse.tpTapToggle, XmNvalueChangedCallback, touchpadToggleCB, nullptr);

    n = 0;
    string = CMPSTR(TP_NATURAL_MSG);
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING); n++;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_POSITION); n++;
    XtSetArg(args[n], XmNleftPosition, 50); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
    mouse.tpNaturalToggle =
	XmCreateToggleButtonGadget(mouse.touchpadForm, "tpNaturalToggle", args, n);
    XmStringFree(string);
    XtManageChild(mouse.tpNaturalToggle);
    XtAddCallback(mouse.tpNaturalToggle, XmNvalueChangedCallback, touchpadToggleCB, nullptr);

    n = 0;
    string = CMPSTR(TP_CLICKFINGER_MSG);
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING); n++;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNtopWidget, mouse.tpTapToggle); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_POSITION); n++;
    XtSetArg(args[n], XmNrightPosition, 49); n++;
    mouse.tpClickfingerToggle =
	XmCreateToggleButtonGadget(mouse.touchpadForm, "tpClickfingerToggle", args, n);
    XmStringFree(string);
    XtManageChild(mouse.tpClickfingerToggle);
    XtAddCallback(mouse.tpClickfingerToggle, XmNvalueChangedCallback, touchpadToggleCB, nullptr);

    n = 0;
    string = CMPSTR(TP_DWT_MSG);
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING); n++;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNtopWidget, mouse.tpNaturalToggle); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_POSITION); n++;
    XtSetArg(args[n], XmNleftPosition, 50); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
    mouse.tpDwtToggle =
	XmCreateToggleButtonGadget(mouse.touchpadForm, "tpDwtToggle", args, n);
    XmStringFree(string);
    XtManageChild(mouse.tpDwtToggle);
    XtAddCallback(mouse.tpDwtToggle, XmNvalueChangedCallback, touchpadToggleCB, nullptr);

    n = 0;
    string = CMPSTR(TP_DEXT_MSG);
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING); n++;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNtopWidget, mouse.tpClickfingerToggle); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
    mouse.tpDextToggle =
	XmCreateToggleButtonGadget(mouse.touchpadForm, "tpDextToggle", args, n);
    XmStringFree(string);
    XtManageChild(mouse.tpDextToggle);
    XtAddCallback(mouse.tpDextToggle, XmNvalueChangedCallback, touchpadToggleCB, nullptr);

    SyncTouchpadTogglesFromState();

    n = 0;
    string = CMPSTR(RESTART_WM_MSG);
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING); n++;
    mouse.dtwmRestartTG = XmCreateToggleButtonGadget(form, "dtwmRestartTG", args, n);
    if (!DtstyleWaylandActive())
        widget_list[count++] = mouse.dtwmRestartTG;
    XmStringFree(string);
    XmToggleButtonGadgetSetState(mouse.dtwmRestartTG, !DtstyleWaylandActive(), False);

    {
	Widget colorRef = mouse.threshScale ? mouse.threshScale : form;

	ApplyDialogBgFg(colorRef, mouse.cursorSizeLabGad);
	ApplyDialogBgFg(colorRef, mouse.cursorThemeLabGad);
	ApplyDialogBgFg(colorRef, mouse.cursorSizeRC);
	for (i = 0; i < mouse.cursorSizeChoiceCount; i++)
	    ApplyDialogBgFg(colorRef, mouse.cursorSizeToggles[i]);
	ApplyOptionMenuBackground(colorRef, mouse.cursorThemeOM);
	ApplyDialogBgFg(colorRef, mouse.cursorThemeMenu);
	ApplyDialogBgFg(colorRef, mouse.cursorPreviewFrame);
	ApplyDialogBgFg(colorRef, mouse.cursorPreviewForm);
	ApplyDialogBgFg(colorRef, mouse.cursorPreviewGrid);
	ApplyDialogBgFg(colorRef, mouse.cursorPreviewHint);
	ApplyDialogBgFg(colorRef, mouse.scrollWheelFrame);
	ApplyDialogBgFg(colorRef, mouse.scrollWheelFrameTitle);
	ApplyDialogBgFg(colorRef, mouse.scrollWheelForm);
	ApplyDialogBgFg(colorRef, mouse.wheelDirLabel);
	ApplyDialogBgFg(colorRef, mouse.wheelDirRC);
	ApplyDialogBgFg(colorRef, mouse.wheelStandardToggle);
	ApplyDialogBgFg(colorRef, mouse.wheelNaturalToggle);
	ApplyDialogBgFg(colorRef, mouse.wheelSmoothToggle);
	ApplyDialogBgFg(colorRef, mouse.wheelSpeedLabel);
	ApplyDialogBgFg(colorRef, mouse.wheelSpeedScale);
	ApplyDialogBgFg(colorRef, mouse.touchpadFrame);
	ApplyDialogBgFg(colorRef, mouse.touchpadFrameTitle);
	ApplyDialogBgFg(colorRef, mouse.touchpadForm);
	ApplyDialogBgFg(colorRef, mouse.tpTapToggle);
	ApplyDialogBgFg(colorRef, mouse.tpNaturalToggle);
	ApplyDialogBgFg(colorRef, mouse.tpClickfingerToggle);
	ApplyDialogBgFg(colorRef, mouse.tpDwtToggle);
	ApplyDialogBgFg(colorRef, mouse.tpDextToggle);
	ApplyDialogBgFg(colorRef, mouse.dtwmRestartTG);
	for (i = 0; i < mouse.cursorThemeCount; i++)
	    ApplyDialogBgFg(colorRef, mouse.cursorThemeItems[i]);
	for (i = 0; i < CURSOR_PREVIEW_COUNT; i++)
	{
	    ApplyDialogBgFg(colorRef, mouse.cursorPreviewCell[i]);
	    ApplyDialogBgFg(colorRef, mouse.cursorPreviewLabel[i]);
	    ApplyDialogBgFg(colorRef, mouse.cursorPreviewArea[i]);
	}
    }

    XtAddCallback(style.mouseDialog, XmNmapCallback, _DtmapCB_mouseDialog, shell);
    XtAddCallback(mouse.systemDefault, XmNactivateCallback, 
                  systemDefaultCB, nullptr);
    
    /* calculate the max width for toggles for the overall alignment */
    /* Set labels to max label width */ 

    MaxLabelWidth1 = HandLabelWidth = XtWidth(mouse.handLabGad);
    MiddleLabelWidth = XtWidth(mouse.middleLabGad);
    if (MiddleLabelWidth > MaxLabelWidth1) 
    {
	MaxLabelWidth1 = MiddleLabelWidth;
    }
   
    MaxToggleWidth = RightToggleWidth = XtWidth(mouse.rightToggle);
    TransferToggleWidth = XtWidth(mouse.transferToggle);
    if (TransferToggleWidth > MaxToggleWidth) 
    {
	MaxToggleWidth = TransferToggleWidth;
    }
    

    if (mouse.function == TRANSFER)
      {
	XmToggleButtonGadgetSetState(mouse.transferToggle, True, True);
	XmToggleButtonGadgetSetState(mouse.adjustToggle, False, False);
      }
    else
      {
	XmToggleButtonGadgetSetState(mouse.adjustToggle, True, True);
	XmToggleButtonGadgetSetState(mouse.transferToggle, False, False);
      }

    XtVaSetValues(mouse.handLabGad, XmNwidth, MaxLabelWidth1, nullptr);
    XtVaSetValues(mouse.middleLabGad, XmNwidth, MaxLabelWidth1, nullptr);

    XtVaSetValues(mouse.rightToggle, XmNwidth, MaxToggleWidth, nullptr);
    XtVaSetValues(mouse.transferToggle, XmNwidth, MaxToggleWidth, nullptr);

    XtAddCallback(mouse.rightToggle, XmNvalueChangedCallback, 
                  warnToggleCB, (XtPointer)RIGHT_HANDED);

    XtAddCallback(mouse.leftToggle, XmNvalueChangedCallback, 
                  warnToggleCB, (XtPointer)LEFT_HANDED);

    XtAddCallback(mouse.transferToggle, XmNvalueChangedCallback, 
                  midwarnToggleCB, (XtPointer)TRANSFER);

    XtAddCallback(mouse.adjustToggle, XmNvalueChangedCallback, 
                  midwarnToggleCB, (XtPointer)ADJUST);

    XtAddCallback(mouse.accelScale, XmNvalueChangedCallback, 
                  valueChangedCB, nullptr);

    XtAddCallback(mouse.threshScale, XmNvalueChangedCallback, 
                  valueChangedCB, nullptr);
    XtAddCallback(mouse.dclickScale, XmNvalueChangedCallback, 
                  dclickVCCB, (caddr_t)mouse.pictButton);
    XtAddCallback(mouse.cursorPreviewForm, XmNmapCallback,
                  cursorPreviewMapCB, nullptr);
    XtAddEventHandler(mouse.pictButton, ButtonPressMask, False,
                      pictBoxButtonPressEH, nullptr);
    XtAddEventHandler(mouse.pictLabel, ButtonPressMask, False,
                      pictBoxButtonPressEH, nullptr);
    XtAddEventHandler(mouse.pictRC, ButtonPressMask, False,
                      pictBoxButtonPressEH, nullptr);

    XtManageChild(mouse.pictButton);
    XtManageChild(mouse.pictLabel);
    XtManageChild(mouse.pictRC);
    XtManageChild(mouse.rightToggle);
    XtManageChild(mouse.leftToggle);

    /*
     * If there are only two mouse buttons then switch the
     * label for the transferToggle to "Menu" and mark it
     * insensitive but checked.
     * Otherwise manage both the transfer and adjust toggles.
     */
    if (mouse.numButtons == 2) {
     	n=0;
     	string = CMPSTR(MENUMSG); 
     	XtSetArg(args[n], XmNlabelString, string); n++;
	XtSetValues(mouse.transferToggle, args, n);
    	XmStringFree(string);
	XmToggleButtonGadgetSetState(mouse.transferToggle, True, False);
	XtManageChild(mouse.transferToggle);
    } else {
	XtManageChild(mouse.transferToggle);
	XtManageChild(mouse.adjustToggle);
    }

    XtManageChildren(widget_list,count);
    formLayoutCB(style.mouseDialog, nullptr, nullptr);
    XtManageChild(form);

    return(style.mouseDialog);
}


/*+++++++++++++++++++++++++++++++++++++++*/
/* formLayoutCB                          */
/*+++++++++++++++++++++++++++++++++++++++*/
static void 
formLayoutCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
    int              n;
    Arg              args[MAX_ARGS];
    Dimension        MaxLabelWidth;
    Dimension        AccelLabelWidth;
    Dimension        ThreshLabelWidth;
    Dimension        DclickLabelWidth;
    Dimension        CursorSizeLabelWidth;
    Dimension        CursorThemeLabelWidth;
    Dimension        ScaleHeight;
    Dimension        LabelHeight;
    int              TopOffset;
    int              LeftOffset;

    /* Do the scale and label attachments */

    ScaleHeight = XtHeight(mouse.accelScale);

    MaxLabelWidth = AccelLabelWidth = XtWidth(mouse.accelLabGad);
    LabelHeight = XtHeight(mouse.accelLabGad);

    ThreshLabelWidth = XtWidth(mouse.threshLabGad);
    if (ThreshLabelWidth > MaxLabelWidth) 
    {
	MaxLabelWidth = ThreshLabelWidth;
    }

    DclickLabelWidth = XtWidth(mouse.dclickLabGad);
    if (DclickLabelWidth > MaxLabelWidth) 
    {
	MaxLabelWidth = DclickLabelWidth;
    }

    CursorSizeLabelWidth = XtWidth(mouse.cursorSizeLabGad);
    if (CursorSizeLabelWidth > MaxLabelWidth)
    {
	MaxLabelWidth = CursorSizeLabelWidth;
    }

    CursorThemeLabelWidth = XtWidth(mouse.cursorThemeLabGad);
    if (CursorThemeLabelWidth > MaxLabelWidth)
    {
	MaxLabelWidth = CursorThemeLabelWidth;
    }
   
    /* Mouse Picture PushButton */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,    XmATTACH_FORM);      n++;
    XtSetArg(args[n], XmNtopOffset,        style.verticalSpacing);   n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_NONE);      n++;
    XtSetArg(args[n], XmNleftAttachment,   XmATTACH_FORM);      n++;
    XtSetArg(args[n], XmNleftOffset,       style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNrightAttachment,  XmATTACH_NONE);      n++;
    XtSetValues (mouse.pictFrame, args, n);

    
    /* System Default Button */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,    XmATTACH_FORM);      n++;
    XtSetArg(args[n], XmNtopOffset,        style.verticalSpacing);   n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_NONE);      n++;
    XtSetArg(args[n], XmNleftAttachment,   XmATTACH_NONE);      n++;
    XtSetArg(args[n], XmNrightAttachment,  XmATTACH_FORM);      n++;
    XtSetArg(args[n], XmNrightOffset,      style.horizontalSpacing); n++;
    XtSetValues (mouse.systemDefault, args, n);
 
 
    /*  Handedness Label */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,    XmATTACH_WIDGET);    n++;
    XtSetArg(args[n], XmNtopWidget,        mouse.systemDefault); n++;
    XtSetArg(args[n], XmNtopOffset,        2*style.verticalSpacing);   n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_NONE);      n++;
    XtSetArg(args[n], XmNleftAttachment,   XmATTACH_WIDGET);    n++;
    XtSetArg(args[n], XmNleftWidget,       mouse.pictFrame);    n++;
    XtSetArg(args[n], XmNleftOffset,       2*style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNrightAttachment,  XmATTACH_NONE);    n++;
    XtSetValues (mouse.handLabGad, args, n);

    /* right toggle */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,    XmATTACH_OPPOSITE_WIDGET);    n++;
    XtSetArg(args[n], XmNtopWidget,        mouse.handLabGad); n++;
    XtSetArg(args[n], XmNtopOffset,        -5);   n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_NONE);      n++;
    XtSetArg(args[n], XmNleftAttachment,   XmATTACH_WIDGET);    n++;
    XtSetArg(args[n], XmNleftWidget,       mouse.handLabGad);    n++;
    XtSetArg(args[n], XmNleftOffset,       style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNrightAttachment,  XmATTACH_NONE);    n++;
    XtSetValues (mouse.rightToggle, args, n);
    
    /* left toggle */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,    XmATTACH_OPPOSITE_WIDGET);    n++;
    XtSetArg(args[n], XmNtopWidget,        mouse.handLabGad); n++;
    XtSetArg(args[n], XmNtopOffset,        -5);   n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_NONE);      n++;
    XtSetArg(args[n], XmNleftAttachment,   XmATTACH_WIDGET);    n++;
    XtSetArg(args[n], XmNleftWidget,       mouse.rightToggle);    n++;
    XtSetArg(args[n], XmNleftOffset,       style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNrightAttachment,  XmATTACH_FORM);    n++;
    XtSetValues (mouse.leftToggle, args, n);


    /*  Middle button Label */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,    XmATTACH_WIDGET);    n++;
    XtSetArg(args[n], XmNtopWidget,        mouse.handLabGad); n++;
    XtSetArg(args[n], XmNtopOffset,        2*style.verticalSpacing);   n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_NONE);      n++;
    XtSetArg(args[n], XmNleftAttachment,   XmATTACH_WIDGET);    n++;
    XtSetArg(args[n], XmNleftWidget,       mouse.pictFrame);    n++;
    XtSetArg(args[n], XmNleftOffset,       2*style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNrightAttachment,  XmATTACH_NONE);    n++;
    XtSetValues (mouse.middleLabGad, args, n);


    /* transfer toggle */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,    XmATTACH_OPPOSITE_WIDGET);    n++;
    XtSetArg(args[n], XmNtopWidget,        mouse.middleLabGad); n++;
    XtSetArg(args[n], XmNtopOffset,        -5);   n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_NONE);      n++;
    XtSetArg(args[n], XmNleftAttachment,   XmATTACH_OPPOSITE_WIDGET);    n++;
    XtSetArg(args[n], XmNleftWidget,       mouse.rightToggle);    n++;
    XtSetArg(args[n], XmNleftOffset,       0); n++;
    XtSetArg(args[n], XmNrightAttachment,  XmATTACH_NONE);    n++;
    XtSetValues (mouse.transferToggle, args, n);
    
    /* adjust toggle */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,    XmATTACH_OPPOSITE_WIDGET);    n++;
    XtSetArg(args[n], XmNtopWidget,        mouse.middleLabGad); n++;
    XtSetArg(args[n], XmNtopOffset,        -5);   n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_NONE);      n++;
    XtSetArg(args[n], XmNleftAttachment,   XmATTACH_WIDGET);    n++;
    XtSetArg(args[n], XmNleftWidget,       mouse.transferToggle);    n++;
    XtSetArg(args[n], XmNleftOffset,       style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNrightAttachment,  XmATTACH_FORM);    n++;
    XtSetValues (mouse.adjustToggle, args, n);



    /* Chain the controls top-to-bottom via XmATTACH_WIDGET so XmForm uses each
     * scale/row/menu widget's true preferred height (including showValue text)
     * before realization, and align each left-hand label to its control! */
    {
        XtWidgetGeometry pg = {0}, sg = {0}, hg = {0}, tg = {0};
        XtQueryGeometry(mouse.pictFrame, nullptr, &pg);
        XtQueryGeometry(mouse.systemDefault, nullptr, &sg);
        XtQueryGeometry(mouse.handLabGad, nullptr, &hg);
        XtQueryGeometry(mouse.transferToggle, nullptr, &tg);
        int pictBottom = style.verticalSpacing + (int)(pg.height ? pg.height : XtHeight(mouse.pictFrame));
        int rightBottom = style.verticalSpacing + (int)(sg.height ? sg.height : XtHeight(mouse.systemDefault)) +
                          2 * style.verticalSpacing + (int)(hg.height ? hg.height : XtHeight(mouse.handLabGad)) +
                          2 * style.verticalSpacing + (int)(tg.height ? tg.height : XtHeight(mouse.transferToggle)) +
                          style.verticalSpacing;
        Widget topAnchor = (pictBottom >= rightBottom) ? mouse.pictFrame : mouse.transferToggle;

        /* DoubleClick Scale */
        n=0;
        XtSetArg(args[n], XmNtopAttachment,    XmATTACH_WIDGET);    n++;
        XtSetArg(args[n], XmNtopWidget,        topAnchor);          n++;
        XtSetArg(args[n], XmNtopOffset,        style.verticalSpacing); n++;
        XtSetArg(args[n], XmNbottomAttachment, XmATTACH_NONE);      n++;
        XtSetArg(args[n], XmNleftAttachment,   XmATTACH_WIDGET);    n++;
        XtSetArg(args[n], XmNleftWidget,       mouse.dclickLabGad); n++;
        XtSetArg(args[n], XmNleftOffset,       2*style.horizontalSpacing); n++;
        XtSetArg(args[n], XmNrightAttachment,  XmATTACH_FORM);      n++;
        XtSetValues (mouse.dclickScale, args, n);
    }

    /* DoubleClick Label */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,    XmATTACH_NONE);          n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_OPPOSITE_WIDGET); n++;
    XtSetArg(args[n], XmNbottomWidget,     mouse.dclickScale);      n++;
    XtSetArg(args[n], XmNbottomOffset,     0);                      n++;
    XtSetArg(args[n], XmNleftAttachment,   XmATTACH_FORM);          n++;
    LeftOffset =  MaxLabelWidth + style.horizontalSpacing - DclickLabelWidth;
    XtSetArg(args[n], XmNleftOffset,       LeftOffset);             n++;
    XtSetArg(args[n], XmNrightAttachment,  XmATTACH_NONE);          n++;
    XtSetValues (mouse.dclickLabGad, args, n);

    /* Acceleration Scale */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,    XmATTACH_WIDGET);        n++;
    XtSetArg(args[n], XmNtopWidget,        mouse.dclickScale);      n++;
    XtSetArg(args[n], XmNtopOffset,        style.verticalSpacing);  n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_NONE);          n++;
    XtSetArg(args[n], XmNleftAttachment,   XmATTACH_WIDGET);        n++;
    XtSetArg(args[n], XmNleftWidget,       mouse.accelLabGad);      n++;
    XtSetArg(args[n], XmNleftOffset,       2*style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNrightAttachment,  XmATTACH_FORM);          n++;
    XtSetValues (mouse.accelScale, args, n);

    /* Acceleration Label */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,    XmATTACH_NONE);          n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_OPPOSITE_WIDGET); n++;
    XtSetArg(args[n], XmNbottomWidget,     mouse.accelScale);       n++;
    XtSetArg(args[n], XmNbottomOffset,     0);                      n++;
    XtSetArg(args[n], XmNleftAttachment,   XmATTACH_FORM);          n++;
    LeftOffset =  MaxLabelWidth + style.horizontalSpacing - AccelLabelWidth;
    XtSetArg(args[n], XmNleftOffset,       LeftOffset);             n++;
    XtSetArg(args[n], XmNrightAttachment,  XmATTACH_NONE);          n++;
    XtSetValues (mouse.accelLabGad, args, n);

    /* Threshold Scale */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,    XmATTACH_WIDGET);        n++;
    XtSetArg(args[n], XmNtopWidget,        mouse.accelScale);       n++;
    XtSetArg(args[n], XmNtopOffset,        style.verticalSpacing);  n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_NONE);          n++;
    XtSetArg(args[n], XmNleftAttachment,   XmATTACH_WIDGET);        n++;
    XtSetArg(args[n], XmNleftWidget,       mouse.threshLabGad);     n++;
    XtSetArg(args[n], XmNleftOffset,       2*style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNrightAttachment,  XmATTACH_FORM);          n++;
    XtSetValues (mouse.threshScale, args, n);

    /* Threshold Label */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,    XmATTACH_NONE);          n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_OPPOSITE_WIDGET); n++;
    XtSetArg(args[n], XmNbottomWidget,     mouse.threshScale);      n++;
    XtSetArg(args[n], XmNbottomOffset,     0);                      n++;
    XtSetArg(args[n], XmNleftAttachment,   XmATTACH_FORM);          n++;
    LeftOffset =  MaxLabelWidth + style.horizontalSpacing - ThreshLabelWidth;
    XtSetArg(args[n], XmNleftOffset,       LeftOffset);             n++;
    XtSetArg(args[n], XmNrightAttachment,  XmATTACH_NONE);          n++;
    XtSetValues (mouse.threshLabGad, args, n);

    /* Cursor Size RowColumn */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,    XmATTACH_WIDGET);        n++;
    XtSetArg(args[n], XmNtopWidget,        mouse.threshScale);      n++;
    XtSetArg(args[n], XmNtopOffset,        style.verticalSpacing);  n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_NONE);          n++;
    XtSetArg(args[n], XmNleftAttachment,   XmATTACH_WIDGET);        n++;
    XtSetArg(args[n], XmNleftWidget,       mouse.cursorSizeLabGad); n++;
    XtSetArg(args[n], XmNleftOffset,       2*style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNrightAttachment,  XmATTACH_FORM);          n++;
    XtSetValues (mouse.cursorSizeRC, args, n);

    /* Cursor Size Label */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,    XmATTACH_OPPOSITE_WIDGET); n++;
    XtSetArg(args[n], XmNtopWidget,        mouse.cursorSizeRC);     n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_OPPOSITE_WIDGET); n++;
    XtSetArg(args[n], XmNbottomWidget,     mouse.cursorSizeRC);     n++;
    XtSetArg(args[n], XmNleftAttachment,   XmATTACH_FORM);          n++;
    LeftOffset =  MaxLabelWidth + style.horizontalSpacing - CursorSizeLabelWidth;
    XtSetArg(args[n], XmNleftOffset,       LeftOffset);             n++;
    XtSetArg(args[n], XmNrightAttachment,  XmATTACH_NONE);          n++;
    XtSetValues (mouse.cursorSizeLabGad, args, n);

    /* Cursor Theme Option Menu */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,    XmATTACH_WIDGET);        n++;
    XtSetArg(args[n], XmNtopWidget,        mouse.cursorSizeRC);     n++;
    XtSetArg(args[n], XmNtopOffset,        style.verticalSpacing);  n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_NONE);          n++;
    XtSetArg(args[n], XmNleftAttachment,   XmATTACH_WIDGET);        n++;
    XtSetArg(args[n], XmNleftWidget,       mouse.cursorThemeLabGad); n++;
    XtSetArg(args[n], XmNleftOffset,       2*style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNrightAttachment,  XmATTACH_FORM);          n++;
    XtSetValues (mouse.cursorThemeOM, args, n);

    /* Cursor Theme Label */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,    XmATTACH_OPPOSITE_WIDGET); n++;
    XtSetArg(args[n], XmNtopWidget,        mouse.cursorThemeOM);    n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_OPPOSITE_WIDGET); n++;
    XtSetArg(args[n], XmNbottomWidget,     mouse.cursorThemeOM);    n++;
    XtSetArg(args[n], XmNleftAttachment,   XmATTACH_FORM);          n++;
    LeftOffset =  MaxLabelWidth + style.horizontalSpacing - CursorThemeLabelWidth;
    XtSetArg(args[n], XmNleftOffset,       LeftOffset);             n++;
    XtSetArg(args[n], XmNrightAttachment,  XmATTACH_NONE);          n++;
    XtSetValues (mouse.cursorThemeLabGad, args, n);

    /* Cursor Preview Frame */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,    XmATTACH_WIDGET);    n++;
    XtSetArg(args[n], XmNtopWidget,        mouse.cursorThemeOM); n++;
    XtSetArg(args[n], XmNtopOffset,        style.verticalSpacing); n++;
    XtSetArg(args[n], XmNleftAttachment,   XmATTACH_FORM);      n++;
    XtSetArg(args[n], XmNleftOffset,       style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNrightAttachment,  XmATTACH_FORM);      n++;
    XtSetArg(args[n], XmNrightOffset,      style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_NONE);      n++;
    XtSetValues (mouse.cursorPreviewFrame, args, n);

    /* Cursor Preview Form (inside frame) */
    n=0;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
    XtSetValues (mouse.cursorPreviewForm, args, n);

    /* Scroll Wheel Frame */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,    XmATTACH_WIDGET);    n++;
    XtSetArg(args[n], XmNtopWidget,        mouse.cursorPreviewFrame); n++;
    XtSetArg(args[n], XmNtopOffset,        style.verticalSpacing); n++;
    XtSetArg(args[n], XmNleftAttachment,   XmATTACH_FORM);      n++;
    XtSetArg(args[n], XmNleftOffset,       style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNrightAttachment,  XmATTACH_FORM);      n++;
    XtSetArg(args[n], XmNrightOffset,      style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_NONE);      n++;
    XtSetValues (mouse.scrollWheelFrame, args, n);

    /* Touchpad Frame */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,    XmATTACH_WIDGET);    n++;
    XtSetArg(args[n], XmNtopWidget,        mouse.scrollWheelFrame); n++;
    XtSetArg(args[n], XmNtopOffset,        style.verticalSpacing); n++;
    XtSetArg(args[n], XmNleftAttachment,   XmATTACH_FORM);      n++;
    XtSetArg(args[n], XmNleftOffset,       style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNrightAttachment,  XmATTACH_FORM);      n++;
    XtSetArg(args[n], XmNrightOffset,      style.horizontalSpacing); n++;
    if (DtstyleWaylandActive())
    {
        XtSetArg(args[n], XmNbottomAttachment, XmATTACH_FORM);  n++;
        XtSetArg(args[n], XmNbottomOffset,     style.verticalSpacing); n++;
    }
    else
    {
        XtSetArg(args[n], XmNbottomAttachment, XmATTACH_NONE);  n++;
    }
    XtSetValues (mouse.touchpadFrame, args, n);

    /* Restart toggle */
    if (!DtstyleWaylandActive())
    {
        n=0;
        XtSetArg(args[n], XmNtopAttachment,    XmATTACH_WIDGET);    n++;
        XtSetArg(args[n], XmNtopWidget,        mouse.touchpadFrame); n++;
        XtSetArg(args[n], XmNtopOffset,        style.verticalSpacing); n++;
        XtSetArg(args[n], XmNleftAttachment,   XmATTACH_FORM);      n++;
        XtSetArg(args[n], XmNleftOffset,       style.horizontalSpacing); n++;
        XtSetArg(args[n], XmNrightAttachment,  XmATTACH_FORM);      n++;
        XtSetArg(args[n], XmNbottomAttachment, XmATTACH_FORM);      n++;
        XtSetArg(args[n], XmNbottomOffset,     style.verticalSpacing); n++;
        XtSetValues (mouse.dtwmRestartTG, args, n);
    }

}

/*+++++++++++++++++++++++++++++++++++++++*/
/* _DtmapCB_mouseDialog                     */
/*+++++++++++++++++++++++++++++++++++++++*/
static void 
_DtmapCB_mouseDialog(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
    int         n;
    int         i;
    int         nmap;
    Arg         args[MAX_ARGS];
    static int  first_time = 1;

    if (first_time)
    {
        DtWsmRemoveWorkspaceFunctions(style.display, XtWindow(XtParent(w)));

        if (!save.restoreFlag)
	    putDialog((Widget)client_data, w);

        first_time = 0;
    }
    else
    {
        /* Don't need to do XGetPointerMapping first time dialog 
         * is mapped because it is done when dialog is created 
         * The type of device will not change between remapping 
         */
        nmap = 256;
        mouse.numButtons = XGetPointerMapping(style.display, 
                                mouse.map_return, nmap);
    }

    /* save mouse mapping values */
    for (i=0; i<mouse.numButtons; i++)
        mouse.new_map_return[i] = mouse.map_return[i];

    /* change Pointer Mapping */
    /* the ReverseToggleCB will set the correct Pixmap */
    if (mouse.map_return[0] == 1)  /* right-handed */
    {
        mouse.handed = RIGHT_HANDED;
        mouse.origHanded = RIGHT_HANDED;
	XmToggleButtonGadgetSetState(mouse.rightToggle, True, True);
	XmToggleButtonGadgetSetState(mouse.leftToggle, False, False);
    }
    else  /* left-handed */
    {
        mouse.handed = LEFT_HANDED;
        mouse.origHanded = LEFT_HANDED;
	XmToggleButtonGadgetSetState(mouse.leftToggle, True, True);
	XmToggleButtonGadgetSetState(mouse.rightToggle, False, False);
    }

    /* MultiClickTime will not change between remapping */

    XGetPointerControl(style.display, &mouse.accel_numerator, 
		&mouse.accel_denominator, &mouse.threshold);
    n=0;
    XtSetArg(args[n], XmNvalue,mouse.accel_numerator); n++;
    XtSetValues(mouse.accelScale, args, n);

    n=0;
    XtSetArg(args[n], XmNvalue,mouse.threshold); n++;
    XtSetValues(mouse.threshScale, args, n);

    /* Xcursor controls */
    {
	int i;
	Widget history = (Widget)nullptr;
	int effectiveSize = GetEffectiveCursorSize();
	char *effectiveTheme = GetEffectiveCursorTheme();

	effectiveSize = SnapCursorSize(effectiveSize, mouse.maxCursorSize);

	mouse.cursorSize = effectiveSize;
	for (i = 0; i < mouse.cursorSizeChoiceCount; i++)
	{
	    Boolean set = (mouse.cursorSizeChoiceValues[i] == mouse.cursorSize);
	    XmToggleButtonGadgetSetState(mouse.cursorSizeToggles[i], set, False);
	}

	for (i = 0; i < mouse.cursorThemeCount; i++)
	{
	    if (mouse.cursorThemes[i] &&
		strcmp(mouse.cursorThemes[i], effectiveTheme) == 0)
	    {
		history = mouse.cursorThemeItems[i];
		break;
	    }
	}
	if (!history && mouse.cursorThemeCount > 0)
	    history = mouse.cursorThemeItems[0];

	if (history)
	    XtVaSetValues(mouse.cursorThemeOM, XmNmenuHistory, history, nullptr);

	if (mouse.cursorTheme)
	    XtFree(mouse.cursorTheme);
	mouse.cursorTheme = effectiveTheme;

	mouse.origCursorSize = mouse.cursorSize;
	if (mouse.origCursorTheme)
	    XtFree(mouse.origCursorTheme);
	mouse.origCursorTheme = XtNewString(mouse.cursorTheme);
	mouse.cursorChanged = False;

	{
	    Boolean hasXcursor =
#ifdef HAVE_XCURSOR
		True
#else
		False
#endif
		;
	    Boolean canRestart = hasXcursor && style.xrdb.writeXrdbImmediate;
	    int previewIndex;

	    if (mouse.cursorSizeRC)
		XtSetSensitive(mouse.cursorSizeRC, hasXcursor);
	    for (previewIndex = 0; previewIndex < mouse.cursorSizeChoiceCount; previewIndex++)
		if (mouse.cursorSizeToggles[previewIndex])
		    XtSetSensitive(mouse.cursorSizeToggles[previewIndex], hasXcursor);
	    XtSetSensitive(mouse.cursorThemeOM, hasXcursor);
	    XtSetSensitive(mouse.cursorPreviewFrame, hasXcursor);
	    if (mouse.cursorPreviewGrid)
		XtSetSensitive(mouse.cursorPreviewGrid, hasXcursor);
	    for (previewIndex = 0; previewIndex < CURSOR_PREVIEW_COUNT; previewIndex++)
		if (mouse.cursorPreviewArea[previewIndex])
		    XtSetSensitive(mouse.cursorPreviewArea[previewIndex], hasXcursor);

	    XtSetSensitive(mouse.dtwmRestartTG, canRestart);
	    if (!canRestart)
		XmToggleButtonGadgetSetState(mouse.dtwmRestartTG, False, False);
	}

	UpdateCursorPreview();
    }

    /* Scroll Wheel controls */
    LoadScrollWheelResources(&mouse.wheelNatural, &mouse.wheelSpeed, &mouse.wheelSmooth);
    mouse.origWheelNatural = mouse.wheelNatural;
    mouse.origWheelSpeed = mouse.wheelSpeed;
    mouse.origWheelSmooth = mouse.wheelSmooth;
    mouse.wheelChanged = False;
    SyncScrollWheelControlsFromState();

    /* Touchpad controls */
    LoadTouchpadResources(&mouse.tpTap, &mouse.tpNatural, &mouse.tpClickfinger,
			  &mouse.tpDwt, &mouse.tpDext);
    mouse.origTpTap = mouse.tpTap;
    mouse.origTpNatural = mouse.tpNatural;
    mouse.origTpClickfinger = mouse.tpClickfinger;
    mouse.origTpDwt = mouse.tpDwt;
    mouse.origTpDext = mouse.tpDext;
    mouse.tpChanged = False;
    SyncTouchpadTogglesFromState();
}

static int 
getValue(
        Widget w )
{
    int n, value;
    Arg args[1];

    n=0;
    XtSetArg(args[n], XmNvalue, &value); n++;
    XtGetValues(w, args, n);
    return(value);
}


/*+++++++++++++++++++++++++++++++++++++++*/
/* valueChangedCB                        */
/*+++++++++++++++++++++++++++++++++++++++*/
static void 
valueChangedCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{

    int     new_acceln, new_acceld, new_thresh;

     /* set mouse parameters to new scale values */
     new_acceld = 1;  
     new_acceln = getValue(mouse.accelScale);
     new_thresh = getValue(mouse.threshScale);

     XChangePointerControl(style.display, True, True, new_acceln,
                         new_acceld, new_thresh);
     if (DtstyleWaylandActive())
     {
         char cmd[128];
         snprintf(cmd, sizeof(cmd), "f.set_pointer_accel %d %d %d",
                  new_acceln, new_acceld, new_thresh);
         DtstyleSendWaylandRequest(cmd);
     }

     mouse.systemDefaultFlag = False;

}

static void
cursorPreviewMapCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
    (void)w;
    (void)client_data;
    (void)call_data;

    UpdateCursorPreview();
}

static void
cursorPreviewExposeCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
    XmDrawingAreaCallbackStruct *cb = (XmDrawingAreaCallbackStruct *)call_data;
    int index;

    if (!cb || cb->reason != XmCR_EXPOSE)
        return;

    index = (int)(intptr_t)client_data;
    if (index < 0 || index >= CURSOR_PREVIEW_COUNT)
        return;

    if (mouse.cursorPreviewPixmaps[index] == None)
        return;

    Display *dpy = style.display;
    Window win = XtWindow(w);
    GC gc = XCreateGC(dpy, win, 0, nullptr);
    int width = mouse.cursorPreviewPixmapW[index];
    int height = mouse.cursorPreviewPixmapH[index];
    int wWidth = XtWidth(w);
    int wHeight = XtHeight(w);
    int dstX = (wWidth - width) / 2;
    int dstY = (wHeight - height) / 2;

    if (width <= 0 || height <= 0)
        goto done;

    if (dstX < 0)
        dstX = 0;
    if (dstY < 0)
        dstY = 0;

    XCopyArea(dpy, mouse.cursorPreviewPixmaps[index], win, gc,
              0, 0, width, height, dstX, dstY);

done:
    XFreeGC(dpy, gc);
}

static void
cursorSizeChangedCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
    XmToggleButtonCallbackStruct *cb = (XmToggleButtonCallbackStruct *)call_data;
    int sizeValue = (int)(intptr_t)client_data;

    if (!cb || !cb->set)
	return;

    mouse.cursorSize = sizeValue;
    mouse.cursorChanged = True;
    UpdateCursorPreview();
}

static void
cursorThemeSelectCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
    (void)call_data;

    if (mouse.cursorTheme)
    {
	XtFree(mouse.cursorTheme);
	mouse.cursorTheme = nullptr;
    }

    if (client_data)
	mouse.cursorTheme = XtNewString((const char *)client_data);
    else
	mouse.cursorTheme = XtNewString("default");

    mouse.cursorChanged = True;
    if (mouse.cursorThemeOM && w)
	XtVaSetValues(mouse.cursorThemeOM, XmNmenuHistory, w, nullptr);
    UpdateCursorPreview();
}

/*+++++++++++++++++++++++++++++++++++++++*/
/* dclickVCCB                            */
/*+++++++++++++++++++++++++++++++++++++++*/
static void 
dclickVCCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
  XmScaleCallbackStruct *cb = (XmScaleCallbackStruct *) call_data;

  mouse.dclickChanged = True;
  XtSetMultiClickTime (style.display, (cb->value)*100);
}

/*+++++++++++++++++++++++++++++++++++++++*/
/* dclickTestCB                          */
/*+++++++++++++++++++++++++++++++++++++++*/
static void 
dclickTestCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
    XmPushButtonCallbackStruct *cb = (XmPushButtonCallbackStruct *)call_data;
    (void)w;
    (void)client_data;

    if (!cb || !cb->event)
        return;

    if (cb->event->xbutton.button == Button1 &&
        cb->click_count >= 2 && (cb->click_count % 2) == 0)
    {
        toggleDoubleClickTestVisual();
    }
}





/*+++++++++++++++++++++++++++++++++++++++**/
/* warnToggleCB - put up a warning dialog */
/* about changing the handedness.         */
/*+++++++++++++++++++++++++++++++++++++++**/

static void 
warnToggleCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
  
  int     n;
  Arg              args[10];
  XmString         string;
  Boolean          set;


  if (w == mouse.leftToggle && mouse.handed == LEFT_HANDED) 
    {
      XmToggleButtonGadgetSetState(mouse.leftToggle, True, False);
      return;
    }
  if (w == mouse.rightToggle && mouse.handed == RIGHT_HANDED) 
    {
      XmToggleButtonGadgetSetState(mouse.rightToggle, True, False);
      return;
    }
  if (w == mouse.rightToggle)
    {
      XmToggleButtonGadgetSetState(mouse.leftToggle, False, False);
    }
  else
    {
      XmToggleButtonGadgetSetState(mouse.rightToggle, False, False);
    }
  
  n = 0;
  XtSetArg(args[n], XmNokLabelString, CMPSTR((String) _DtOkString)); n++;
  XtSetArg(args[n], XmNcancelLabelString, CMPSTR((String) _DtCancelString)); n++;
  string = CMPSTR(WARN_MSG);
  XtSetArg(args[n], XmNmessageString, string); n++;
  XtSetArg(args[n], XmNmwmFunctions, DIALOG_MWM_FUNC ); n++;
  XtSetArg(args[n], XmNborderWidth, 3); n++;
  XtSetArg(args[n], XmNdialogStyle, XmDIALOG_SYSTEM_MODAL); n++;
  XtSetArg(args[n], XmNdialogTitle, CMPSTR((char *)GETMESSAGE(2,2, "Notice"))); n++;
  warnDialog = XmCreateInformationDialog(style.shell, "WarnDialog", args, n);
  XmStringFree(string);
  XtUnmanageChild (XmMessageBoxGetChild(warnDialog, XmDIALOG_HELP_BUTTON)); 
  XtAddCallback(warnDialog, XmNokCallback, reverseToggleCB, client_data);
  XtAddCallback(warnDialog, XmNcancelCallback, cancelWarnCB, client_data); 
  XtManageChild(warnDialog);       
} 


/*++++++++++++++++++++++++++++++++++++++++++**/
/* midwarnToggleCB - put up a warning dialog */
/* about changing the function of the middle */
/* button.                                   */
/*++++++++++++++++++++++++++++++++++++++++++**/

static void 
midwarnToggleCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
  
  int     n;
  Arg              args[10];
  XmString         warnstring;
  Boolean          set;
  
  if (w == mouse.adjustToggle && mouse.function == ADJUST)
    {
      XmToggleButtonGadgetSetState(mouse.adjustToggle, True, False);
      return;
    }
  if (w == mouse.transferToggle && mouse.function == TRANSFER)
    {
      XmToggleButtonGadgetSetState(mouse.transferToggle, True, False);
      return;
    }

  n = 0;
  XtSetArg(args[n], XmNokLabelString, CMPSTR((String) _DtOkString)); n++;
  XtSetArg(args[n], XmNcancelLabelString, CMPSTR((String) _DtCancelString)); n++;
  XtSetArg(args[n], XmNborderWidth, 3); n++;
  XtSetArg(args[n], XmNmwmFunctions, DIALOG_MWM_FUNC ); n++;
  XtSetArg(args[n], XmNdialogStyle, XmDIALOG_FULL_APPLICATION_MODAL); n++; 
  XtSetArg(args[n], XmNdialogTitle, CMPSTR((char *)GETMESSAGE(2,2, "Notice"))); n++;
  midwarnDialog = XmCreateInformationDialog(style.shell, "MidWarnDialog", args, n);
  
  XtUnmanageChild (XmMessageBoxGetChild(midwarnDialog, XmDIALOG_HELP_BUTTON)); 
  
  
  if ((intptr_t)client_data == TRANSFER)
    {
      warnstring = CMPSTR(MID_WARN_MSG_T);
      XtVaSetValues(midwarnDialog, XmNmessageString, warnstring, nullptr); n++; 
      
      XmToggleButtonGadgetSetState(mouse.adjustToggle, False, False);
    }
  else
    {
      warnstring = CMPSTR(MID_WARN_MSG_A);
      XtVaSetValues(midwarnDialog, XmNmessageString, warnstring, nullptr); n++; 
      
      XmToggleButtonGadgetSetState(mouse.transferToggle, False, False);
    }
  XmStringFree(warnstring);

  
  XtAddCallback(midwarnDialog, XmNokCallback, midreverseToggleCB, 
		client_data); 
  XtAddCallback(midwarnDialog, XmNcancelCallback, midcancelWarnCB, 
		client_data); 
  XtManageChild(midwarnDialog);       
}

/*+++++++++++++++++++++++++++++++++++++++*/
/* cancelWarnCB - callback for the      */
/* cancel button of the warnDialog      */
/*+++++++++++++++++++++++++++++++++++++++*/

static void 
cancelWarnCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{

  int     n;
  Arg              args[10];
  XmString         string;
  
  if (mouse.handed == RIGHT_HANDED)
    {
      XmToggleButtonGadgetSetState(mouse.rightToggle, True, False);
      XmToggleButtonGadgetSetState(mouse.leftToggle, False, False);
    }
  else
    {
      XmToggleButtonGadgetSetState(mouse.leftToggle, True, False);
      XmToggleButtonGadgetSetState(mouse.rightToggle, False, False);
    }
  XtDestroyWidget(warnDialog);
}

/*+++++++++++++++++++++++++++++++++++++++*/
/* midcancelWarnCB - callback for the    */
/* cancel button of the midwarnDialog    */
/*+++++++++++++++++++++++++++++++++++++++*/

static void 
midcancelWarnCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{

  int     n;
  Arg              args[10];
  XmString         string;
  
  if (mouse.function == TRANSFER)
    {
      XmToggleButtonGadgetSetState(mouse.transferToggle, True, True);
      XmToggleButtonGadgetSetState(mouse.adjustToggle, False, False);
    }
  else
    {
      XmToggleButtonGadgetSetState(mouse.adjustToggle, True, True);
      XmToggleButtonGadgetSetState(mouse.transferToggle, False, False);
    }
  XtDestroyWidget(midwarnDialog);
}

/*+++++++++++++++++++++++++++++++++++++++*/
/* reverseToggleCB                       */
/*+++++++++++++++++++++++++++++++++++++++*/

static void 
reverseToggleCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
  int     n;
  int              i, j;
  Arg              args[MAX_ARGS];
  Boolean          set;
  Boolean          toggle;
  
  toggle = (Boolean) (intptr_t) client_data;
  
  if(mouse.type == POINTER3B)
    {
	if (toggle == LEFT_HANDED)
	  {
	    if (mouse.handed == LEFT_HANDED)
	      return;
	    mouse.handed = LEFT_HANDED;
	    mouse.pixmap = mouse.b3_rev;
	    mouse.pixmapSel = mouse.b3_revSel;
	    for(i = 0, j = 3; i < 3; i++, j--)
	      mouse.new_map_return[i] = j;
	  }
	else
	  {
	    if (mouse.handed == RIGHT_HANDED)
	      return;
	    mouse.handed = RIGHT_HANDED;
	    mouse.pixmap = mouse.b3;
	    mouse.pixmapSel = mouse.b3Sel;
	    for(i = 0, j = 1; i < 3; i++, j++)
	      mouse.new_map_return[i] = j;
	  }
      }
  else     /* 2B mouse */
    {
           if (toggle == LEFT_HANDED)
	     {
               if (mouse.handed == LEFT_HANDED)
		 return;
               mouse.handed = LEFT_HANDED;
               mouse.pixmap = mouse.b2_rev;
               mouse.pixmapSel = mouse.b2_revSel;
               for(i = 0, j = 2; i < 2; i++, j--)
		 mouse.new_map_return[i] = j;
	     }
           else
           {
	     if (mouse.handed == RIGHT_HANDED)
	       return;
	     mouse.handed = RIGHT_HANDED;
	     mouse.pixmap = mouse.b2;
               mouse.pixmapSel = mouse.b2Sel;
               for(i = 0, j = 1; i < 2; i++, j++)
		 mouse.new_map_return[i] = j;
           }
	 }
  
  n=0;
  XtSetArg(args[n], XmNlabelPixmap, mouse.pixmap); n++; 
        XtSetValues(mouse.pictButton, args, n);
  
  /* change Pointer Mapping */
  if (DtstyleWaylandActive())
  {
    DtstyleSendWaylandRequest(mouse.handed == LEFT_HANDED ? "f.set_left_handed 1" : "f.set_left_handed 0");
  }
  else if (XSetPointerMapping(style.display, mouse.new_map_return, mouse.numButtons) 
                   != MappingSuccess)
    _DtSimpleError (progName, DtWarning, nullptr, MSG3, nullptr);
  
  mouse.systemDefaultFlag = False;
  XtDestroyWidget(warnDialog);
    
}


/*+++++++++++++++++++++++++++++++++++++++*/
/* midreverseToggleCB                    */
/*+++++++++++++++++++++++++++++++++++++++*/

static void 
midreverseToggleCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
    int     n;
    int              i, j;
    Arg              args[MAX_ARGS];
    Boolean          set;
    Boolean          toggle;

     toggle = (Boolean) (intptr_t) client_data;

    if (toggle == ADJUST)
      {
	if (mouse.function == ADJUST)
	  return;
	mouse.function = ADJUST;
      }
    else
      {
	if (mouse.function == TRANSFER)
	  return;
	mouse.function = TRANSFER;
      }
    mouse.btn2Changed = True;
    mouse.systemDefaultFlag = False;
    
    XtDestroyWidget(midwarnDialog);
}


/*+++++++++++++++++++++++++++++++++++++++*/
/* systemDefaultCB                       */
/*+++++++++++++++++++++++++++++++++++++++*/
static void 
systemDefaultCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
  int           n, i, j;
  Arg           args[MAX_ARGS];
  Bool          do_accel, do_thresh;
  int           new_acceln, new_acceld, new_thresh;
  char          dclickRes[40];
  
  /* set default mouse values */
  new_acceld = -1;  
  new_acceln = -1;
  new_thresh = THRESHDEFAULT;
  do_accel = 1;
  do_thresh = 1;
  XChangePointerControl(style.display, do_accel, do_thresh, new_acceln,
			new_acceld, new_thresh);
  
  /*get default mouse values for sliders*/
  XGetPointerControl(style.display, &new_acceln, &new_acceld, &new_thresh);
  
  n=0;
  XtSetArg(args[n], XmNvalue,new_acceln); n++;
  XtSetValues(mouse.accelScale, args, n);
  
  n=0;
  XtSetArg(args[n], XmNvalue,new_thresh); n++;
  XtSetValues(mouse.threshScale, args, n);
  
  n=0;
  XtSetArg(args[n], XmNvalue,DCLICKDEFAULT/100); n++;
  XtSetValues(mouse.dclickScale, args, n);
  
  sprintf(dclickRes, multiClickTimeString, DCLICKDEFAULT);
  _DtAddToResource(style.display,dclickRes);
  
  /* change mouse mapping to default */
  if(mouse.type == POINTER3B)
    {
      mouse.pixmap = mouse.b3;
      mouse.pixmapSel = mouse.b3Sel;
      for(i = 0, j = 1; i < 3; i++, j++)
	mouse.new_map_return[i] = j;
      mouse.new_map_return[3] = 4;
      mouse.new_map_return[4] = 5;
    }
  else
    {
      mouse.pixmap = mouse.b2;
      mouse.pixmapSel = mouse.b2Sel;
      for(i = 0, j = 1; i < 2; i++, j++)
	mouse.new_map_return[i] = j;
    }
  
  if (mouse.handed != RIGHT_HANDED)
    {
      /* change Pointer Mapping */
      if(XSetPointerMapping(style.display, mouse.new_map_return,
			    mouse.numButtons) != MappingSuccess)
	_DtSimpleError (progName, DtWarning, nullptr, MSG3, nullptr);
      
      n=0;
      XtSetArg(args[n], XmNlabelPixmap, mouse.pixmap); n++;
      XtSetValues(mouse.pictButton, args, n);
      
      
      mouse.handed = RIGHT_HANDED;
      XmToggleButtonGadgetSetState(mouse.rightToggle, True, True);
      XmToggleButtonGadgetSetState(mouse.leftToggle, False, False);
      
    }
  
  if (mouse.function != TRANSFER)
    {
      mouse.function = TRANSFER;
      mouse.btn2Changed = True;
      XmToggleButtonGadgetSetState(mouse.transferToggle, True, True);
      XmToggleButtonGadgetSetState(mouse.adjustToggle, False, False);
    }

  /* reset cursor theme/size to defaults */
  {
      int i;
      Widget history = (Widget)nullptr;
      char cursorRes[256];

      mouse.cursorSize = 24;
      if (mouse.cursorSize > mouse.maxCursorSize)
	  mouse.cursorSize = mouse.maxCursorSize;

      for (i = 0; i < mouse.cursorSizeChoiceCount; i++)
      {
	  Boolean set = (mouse.cursorSizeChoiceValues[i] == mouse.cursorSize);
	  XmToggleButtonGadgetSetState(mouse.cursorSizeToggles[i], set, False);
      }

      if (mouse.cursorTheme)
	  XtFree(mouse.cursorTheme);
      mouse.cursorTheme = XtNewString("default");

      for (i = 0; i < mouse.cursorThemeCount; i++)
      {
	  if (mouse.cursorThemes[i] &&
	      strcmp(mouse.cursorThemes[i], mouse.cursorTheme) == 0)
	  {
	      history = mouse.cursorThemeItems[i];
	      break;
	  }
      }
      if (!history && mouse.cursorThemeCount > 0)
	  history = mouse.cursorThemeItems[0];
      if (history)
	  XtVaSetValues(mouse.cursorThemeOM, XmNmenuHistory, history, nullptr);

      snprintf(cursorRes, sizeof(cursorRes),
	       "Xcursor.theme: %s\nXcursor.size: %d\n",
	       mouse.cursorTheme, mouse.cursorSize);
      if (style.xrdb.writeXrdbImmediate)
      {
	  _DtAddResString(style.display, cursorRes, _DT_ATR_RESMGR|_DT_ATR_PREFS);
	  XSync(style.display, False);
	  RefreshXrmDatabaseFromRoot();
      }
      else
      {
	  _DtAddResString(style.display, cursorRes, _DT_ATR_PREFS);
	  XSync(style.display, False);
	  RefreshXrmDatabaseFromRoot();
      }

      mouse.cursorChanged = True;
      UpdateCursorPreview();
  }

  /* reset scroll wheel settings to defaults */
  mouse.wheelNatural = False;
  mouse.wheelSpeed = 3;
  mouse.wheelSmooth = True;
  mouse.wheelChanged = True;
  SyncScrollWheelControlsFromState();
  ApplyScrollWheelBackend(mouse.wheelNatural, mouse.wheelSpeed, mouse.wheelSmooth);
  PersistScrollWheelResources(mouse.wheelNatural, mouse.wheelSpeed, mouse.wheelSmooth);

  /* reset touchpad settings to defaults */
  mouse.tpTap = True;
  mouse.tpNatural = False;
  mouse.tpClickfinger = True;
  mouse.tpDwt = True;
  mouse.tpDext = False;
  mouse.tpChanged = True;
  SyncTouchpadTogglesFromState();
  ApplyTouchpadBackend(mouse.tpTap, mouse.tpNatural, mouse.tpClickfinger,
		       mouse.tpDwt, mouse.tpDext);
  PersistTouchpadResources(mouse.tpTap, mouse.tpNatural, mouse.tpClickfinger,
			   mouse.tpDwt, mouse.tpDext);
  
  mouse.systemDefaultFlag = True;
}


/*+++++++++++++++++++++++++++++++++++++++*/
/* ButtonCB                              */
/* callback for PushButtons in DialogBox */
/*+++++++++++++++++++++++++++++++++++++++*/
static void 
ButtonCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
    int           n, i, j;
    Arg           args[MAX_ARGS];
    DtDialogBoxCallbackStruct *cb = (DtDialogBoxCallbackStruct *) call_data;
    Bool          do_accel, do_thresh;
    Bool          set;
    char          message1[6], message2[50], message3[6], message4[6];
    char          pointerStr[128];
    static char   dclickRes[40];
    static char   enableBtn1Res[40];
    int           dclick;
    int     new_acceln, new_acceld, new_thresh;

    switch (cb->button_position)
    {
      case OK_BUTTON:

#ifdef HAVE_XCURSOR
         StopCursorPreviewAnimTimer();
#endif
         XtUnmanageChild(w);

         /* if sendSettings is true send data to Session Manager */

         if(style.smState.smSendSettings)
         {   
             if (mouse.systemDefaultFlag && !mouse.dclickChanged)
             {
                SmDefaultPointerSettings();                       
             }
             else /* dclick or one of the sliders changed */
             {
                sprintf(message1,"%d", mouse.numButtons );
                if(mouse.numButtons == 3 || mouse.numButtons == 5)
                   sprintf(message2,"%d_%d_%d", mouse.new_map_return[0],
                                                mouse.new_map_return[1], 
						mouse.new_map_return[2]);
                else
                   sprintf(message2,"%d_%d_%d_%d_%d", mouse.new_map_return[0],
                                                      mouse.new_map_return[1], 
						      mouse.new_map_return[2],
                                                      mouse.new_map_return[3], 
						      mouse.new_map_return[4]);
                sprintf(message3,"%d", getValue(mouse.accelScale));
                sprintf(message4,"%d", getValue(mouse.threshScale));

                sprintf(dclickRes, multiClickTimeString,
                                   getValue(mouse.dclickScale)*100);
		
                if (mouse.dclickChanged)
                {
		  if (style.xrdb.writeXrdbImmediate) {
		    InfoDialog(IMMEDIATE_MSG, style.shell, False);
		   _DtAddToResource(style.display,dclickRes);
	          }
		  else
		    InfoDialog(LATER_MSG, style.shell, False);
		}
		
                sprintf (pointerStr,"%s %s %s %s %s",
			 message1, message2, message3, message4, 
			 dclickRes );
		
                SmNewPointerSettings (pointerStr);
		
	      }
	   }
	 else 
	   {  /* sendSettings is False. If double click has changed update Xrdb */
	     
             if (mouse.dclickChanged) 
	       {
		 InfoDialog(IMMEDIATE_MSG, style.shell, False);
		 sprintf(dclickRes, multiClickTimeString,
			 getValue(mouse.dclickScale)*100);
		 _DtAddToResource(style.display,dclickRes);
	       }
	   }
	 mouse.dclickChanged = False;

	 if (mouse.btn2Changed)
	   {
	     if(mouse.function == TRANSFER)
	       sprintf(enableBtn1Res, enableBtn1TransferString, "button2_transfer");
	     else
	       sprintf(enableBtn1Res, enableBtn1TransferString, "True");
	     _DtAddToResource(style.display,enableBtn1Res);
	   }

	 /* Cursor theme/size */
	 {
	     Boolean hasXcursor =
#ifdef HAVE_XCURSOR
		 True
#else
		 False
#endif
		 ;
	     if (hasXcursor)
	     {
			 int newSize = mouse.cursorSize;
			 mouse.cursorSize = SnapCursorSize(newSize, mouse.maxCursorSize);

		 if (mouse.cursorTheme == nullptr)
		     mouse.cursorTheme = XtNewString("default");

		 {
		     Boolean themeChanged;
		     themeChanged = (mouse.cursorTheme == nullptr) ?
			 (mouse.origCursorTheme != nullptr) :
			 (mouse.origCursorTheme == nullptr ||
			  strcmp(mouse.cursorTheme, mouse.origCursorTheme) != 0);

		     if ((mouse.cursorSize != mouse.origCursorSize) || themeChanged)
		 {
		     char cursorRes[256];

		     snprintf(cursorRes, sizeof(cursorRes),
			      "Xcursor.theme: %s\nXcursor.size: %d\n",
			      mouse.cursorTheme, mouse.cursorSize);

		     if (style.xrdb.writeXrdbImmediate)
		     {
			 if (!DtstyleWaylandActive())
			     InfoDialog(IMMEDIATE_MSG, style.shell, False);
			 _DtAddResString(style.display, cursorRes,
					 _DT_ATR_RESMGR|_DT_ATR_PREFS);
			 XSync(style.display, False);
			 RefreshXrmDatabaseFromRoot();
			 SetRootCursorNow();

			 if (!DtstyleWaylandActive() &&
			     mouse.dtwmRestartTG &&
			     XmToggleButtonGadgetGetState(mouse.dtwmRestartTG))
			 {
			     _DtWmRestartNoConfirm(style.display, style.root);
			 }
		     }
		     else
		     {
			 InfoDialog(LATER_MSG, style.shell, False);
			 _DtAddResString(style.display, cursorRes, _DT_ATR_PREFS);
			 XSync(style.display, False);
			 RefreshXrmDatabaseFromRoot();
		     }

		     mouse.origCursorSize = mouse.cursorSize;
		     if (mouse.origCursorTheme)
			 XtFree(mouse.origCursorTheme);
		     mouse.origCursorTheme = XtNewString(mouse.cursorTheme);
		     mouse.cursorChanged = False;
		 }
		 }
	     }
	 }

	 /* Scroll Wheel settings */
	 if (mouse.wheelChanged ||
	     mouse.wheelNatural != mouse.origWheelNatural ||
	     mouse.wheelSpeed != mouse.origWheelSpeed ||
	     mouse.wheelSmooth != mouse.origWheelSmooth)
	 {
	     ApplyScrollWheelBackend(mouse.wheelNatural, mouse.wheelSpeed, mouse.wheelSmooth);
	     PersistScrollWheelResources(mouse.wheelNatural, mouse.wheelSpeed, mouse.wheelSmooth);
	     mouse.origWheelNatural = mouse.wheelNatural;
	     mouse.origWheelSpeed = mouse.wheelSpeed;
	     mouse.origWheelSmooth = mouse.wheelSmooth;
	     mouse.wheelChanged = False;
	 }

	 /* Touchpad settings */
	 if (mouse.tpChanged ||
	     mouse.tpTap != mouse.origTpTap ||
	     mouse.tpNatural != mouse.origTpNatural ||
	     mouse.tpClickfinger != mouse.origTpClickfinger ||
	     mouse.tpDwt != mouse.origTpDwt ||
	     mouse.tpDext != mouse.origTpDext)
	 {
	     ApplyTouchpadBackend(mouse.tpTap, mouse.tpNatural, mouse.tpClickfinger,
				  mouse.tpDwt, mouse.tpDext);
	     PersistTouchpadResources(mouse.tpTap, mouse.tpNatural, mouse.tpClickfinger,
				      mouse.tpDwt, mouse.tpDext);
	     mouse.origTpTap = mouse.tpTap;
	     mouse.origTpNatural = mouse.tpNatural;
	     mouse.origTpClickfinger = mouse.tpClickfinger;
	     mouse.origTpDwt = mouse.tpDwt;
	     mouse.origTpDext = mouse.tpDext;
	     mouse.tpChanged = False;
	 }

	 break;
	 
       case CANCEL_BUTTON:
	 
#ifdef HAVE_XCURSOR
	 StopCursorPreviewAnimTimer();
#endif
	 XtUnmanageChild(w);
	 
	 /* check if values and changePonterControl only if needed */
	 
          new_acceln = getValue(mouse.accelScale);
          if (new_acceln != mouse.accel_numerator)
          {
              /* reset to incoming mouse values */ 
              n=0;
              XtSetArg(args[n], XmNvalue,mouse.accel_numerator); n++;
              XtSetValues(mouse.accelScale, args, n);
          }

          new_thresh = getValue(mouse.threshScale);
          if (new_thresh != mouse.threshold)
          {
              /* reset to incoming mouse values */ 
    	      n=0;
              XtSetArg(args[n], XmNvalue,mouse.threshold); n++;
	      XtSetValues(mouse.threshScale, args, n);
          }

	  n=0;
	  XtSetArg(args[n], XmNvalue,mouse.mydclick); n++;
	  XtSetValues(mouse.dclickScale, args, n);
	  mouse.dclickChanged = False;
          XtSetMultiClickTime(style.display, mouse.mydclick*100);

          if ((new_thresh != mouse.threshold) ||
              (new_acceln != mouse.accel_numerator))
          {
              do_accel = mouse.accel_numerator ? 1:0;
              do_thresh = mouse.threshold ? 1:0;
              XChangePointerControl(style.display, do_accel, do_thresh, 
                   mouse.accel_numerator, mouse.accel_denominator, 
                   mouse.threshold);
          }

	 
	 if (mouse.handed != mouse.origHanded)
	   {
	     if (XSetPointerMapping(style.display, mouse.map_return, 
				    mouse.numButtons) != MappingSuccess)
	       _DtSimpleError (progName, DtWarning, nullptr, MSG3, nullptr);
	     
	     /* load the correct mouse pixmaps */
	     
	     if(mouse.type == POINTER3B)
	       {
		 if (mouse.origHanded == LEFT_HANDED)
		   {
		     mouse.pixmap = mouse.b3_rev;
		     mouse.pixmapSel = mouse.b3_revSel;
		   }
		 else
		   {
		     if (mouse.origHanded == RIGHT_HANDED)
		       {
			 mouse.pixmap = mouse.b3;
			 mouse.pixmapSel = mouse.b3Sel;
		       }
		   }
	       }
	     else     /* 2B mouse */
	       {
		 if (mouse.origHanded == LEFT_HANDED)
		   {
		     mouse.pixmap = mouse.b2_rev;
		     mouse.pixmapSel = mouse.b2_revSel;
		   }
		 else
		   {
		     if (mouse.origHanded == RIGHT_HANDED)
		       {
			 mouse.pixmap = mouse.b2;
			 mouse.pixmapSel = mouse.b2Sel;
		       }
		   }
	       }
	     
	     n=0;
	     XtSetArg(args[n], XmNlabelPixmap, mouse.pixmap); n++; 
	     XtSetValues(mouse.pictButton, args, n);
	   }
	 
	 if (mouse.function != mouse.origFunction)
	   {
	     mouse.function = mouse.origFunction;
	     if (mouse.origFunction == TRANSFER)
	       {
		 XmToggleButtonGadgetSetState(mouse.transferToggle, True, True);
		 XmToggleButtonGadgetSetState(mouse.adjustToggle, False, False);
	       }
	     else
	       {
		 XmToggleButtonGadgetSetState(mouse.transferToggle, False, False);
		 XmToggleButtonGadgetSetState(mouse.adjustToggle, True, True);
	       }
	   }

	 if (mouse.wheelChanged ||
	     mouse.wheelNatural != mouse.origWheelNatural ||
	     mouse.wheelSpeed != mouse.origWheelSpeed ||
	     mouse.wheelSmooth != mouse.origWheelSmooth)
	   {
	     mouse.wheelNatural = mouse.origWheelNatural;
	     mouse.wheelSpeed = mouse.origWheelSpeed;
	     mouse.wheelSmooth = mouse.origWheelSmooth;
	     mouse.wheelChanged = False;
	     SyncScrollWheelControlsFromState();
	     ApplyScrollWheelBackend(mouse.wheelNatural, mouse.wheelSpeed, mouse.wheelSmooth);
	   }

	 if (mouse.tpChanged ||
	     mouse.tpTap != mouse.origTpTap ||
	     mouse.tpNatural != mouse.origTpNatural ||
	     mouse.tpClickfinger != mouse.origTpClickfinger ||
	     mouse.tpDwt != mouse.origTpDwt ||
	     mouse.tpDext != mouse.origTpDext)
	   {
	     mouse.tpTap = mouse.origTpTap;
	     mouse.tpNatural = mouse.origTpNatural;
	     mouse.tpClickfinger = mouse.origTpClickfinger;
	     mouse.tpDwt = mouse.origTpDwt;
	     mouse.tpDext = mouse.origTpDext;
	     mouse.tpChanged = False;
	     SyncTouchpadTogglesFromState();
	     ApplyTouchpadBackend(mouse.tpTap, mouse.tpNatural, mouse.tpClickfinger,
				  mouse.tpDwt, mouse.tpDext);
	   }
	 break;
	 
       case HELP_BUTTON:
	 XtCallCallbacks(style.mouseDialog, XmNhelpCallback, (XtPointer)nullptr);
	 break;
	 
       default:
	 break;
       }
  }


/************************************************************************
 * restoreMouse()
 *
 * restore any state information saved with saveMouse.
 * This is called from restoreSession with the application
 * shell and the special xrm database retrieved for restore.
************************************************************************/
void 
restoreMouse(
        Widget shell,
        XrmDatabase db )
{
    XrmName xrm_name[5];
    XrmRepresentation rep_type;
    XrmValue value;

    xrm_name [0] = XrmStringToQuark ("Mouse");
    xrm_name [2] = 0;

    /* get x position */
    xrm_name [1] = XrmStringToQuark ("x");
    if (XrmQGetResource (db, xrm_name, xrm_name, &rep_type, &value)){
        XtSetArg (save.posArgs[save.poscnt], XmNx, atoi((char *)value.addr)); save.poscnt++;
        save.restoreFlag = True;
    }

    /* get y position */
    xrm_name [1] = XrmStringToQuark ("y");
    if (XrmQGetResource (db, xrm_name, xrm_name, &rep_type, &value)){
      XtSetArg (save.posArgs[save.poscnt], XmNy, atoi((char *)value.addr)); save.poscnt++;
    }

    xrm_name [1] = XrmStringToQuark ("ismapped");
    if (XrmQGetResource (db, xrm_name, xrm_name, &rep_type, &value) &&
        value.addr != nullptr &&
        strcmp(value.addr, "True") == 0) 
      popup_mouseBB(shell);
}

/************************************************************************
 * saveMouse()
 *
 * This routine will write out to the passed file descriptor any state
 * information this dialog needs.  It is called from saveSessionCB with the
 * file already opened.
 * All information is saved in xrm format.  There is no restriction
 * on what can be saved.  It doesn't have to be defined or be part of any
 * widget or Xt definition.  Just name and save it here and recover it in
 * restoreBackdrop.  The suggested minimum is whether you are mapped, and your
 * location.
 ************************************************************************/
void 
saveMouse(
        int fd )
{
    Position x,y;
    char bufr[1024];     /* size=[1024], make bigger if needed */
    XmVendorShellExtObject  vendorExt;
    XmWidgetExtData         extData;

    if (style.mouseDialog != nullptr)
    {
        if (XtIsManaged(style.mouseDialog))
            sprintf(bufr, "*Mouse.ismapped: True\n");
        else
            sprintf(bufr, "*Mouse.ismapped: False\n");

        WRITE_STR2FD(fd, bufr);

	/* Get and write out the geometry info for our Window */
	x = XtX(XtParent(style.mouseDialog));
	y = XtY(XtParent(style.mouseDialog));

        /* Modify x & y to take into account window mgr frames
         * This is pretty bogus, but I don't know a better way to do it.
         */
        extData = _XmGetWidgetExtData(style.shell, XmSHELL_EXTENSION);
        vendorExt = (XmVendorShellExtObject)extData->widget;
        x -= vendorExt->vendor.xOffset;
        y -= vendorExt->vendor.yOffset;

	snprintf(bufr, sizeof(bufr), "*Mouse.x: %d\n", x);
        WRITE_STR2FD(fd, bufr);
	snprintf(bufr, sizeof(bufr), "*Mouse.y: %d\n", y);
        WRITE_STR2FD(fd, bufr);
    }
}
