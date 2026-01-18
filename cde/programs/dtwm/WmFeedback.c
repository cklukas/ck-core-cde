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
/* 
 * (c) Copyright 1989, 1990, 1991, 1992, 1993 OPEN SOFTWARE FOUNDATION, INC. 
 * ALL RIGHTS RESERVED 
*/ 
/* 
 * Motif Release 1.2.3
*/
/*
 * (c) Copyright 1987, 1988, 1989, 1990 HEWLETT-PACKARD COMPANY */

/*
 * Included Files:
 */
#include "WmGlobal.h"
#include "WmResNames.h"

#include "WmError.h"
#include <Xm/Xm.h>
#include <X11/Shell.h>
#include <X11/cursorfont.h>
#include <X11/Xutil.h>
#include <Xm/Label.h>
#include <Xm/DialogS.h>
#include <Xm/BulletinB.h>
#include <Xm/MessageB.h>

#include <Dt/HourGlass.h>
#include <math.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>

#define MOVE_OUTLINE_WIDTH	2
#define FEEDBACK_BEVEL		2

#define DEFAULT_POSITION_STRING	"(0000x0000)"

#define  CB_HIGHLIGHT_THICKNESS  3

#define FB_SNAP_CELL_MIN	12
#define FB_SNAP_CELL_PAD	3
#define FB_SNAP_CELL_GAP	4
#define FB_SNAP_TEXT_GAP	6
#define FB_SNAP_ROW_GAP		6
#define FB_SNAP_BASE_AREA	2400
#define FB_SNAP_SCALE		2

#define TASK_SWITCH_MARGIN	12
#define TASK_SWITCH_GAP		10
#define TASK_SWITCH_TEXT_GAP	6
#define TASK_SWITCH_CELL_PAD	6
#define TASK_SWITCH_TITLE_PAD	6
#define TASK_SWITCH_MAX_COLS	6
#define TASK_SWITCH_TIMEOUT_MS	250
#define TASK_SWITCH_MIN_WIDTH	320
#define TASK_SWITCH_MIN_HEIGHT	180
#define TASK_SWITCH_LOG_ENV	"DTWM_TASK_SWITCHER_LOG"

/*
 * include extern functions
 */
#include "WmFeedback.h"
#include "WmFunction.h"
#include "WmGraphics.h"
#include "WmPanelP.h"
#include "WmManage.h"
#include "WmColormap.h"
#include "WmWrkspace.h"
#include "WmIDecor.h"
#include "stdio.h"


/*
 * Global Variables:
 */
static Cursor  waitCursor = (Cursor)0L;
static XtIntervalId taskSwitchTimer = (XtIntervalId)0;
static GC taskSwitchActiveBorderGC = (GC)0L;
static GC taskSwitchInactiveBorderGC = (GC)0L;
static GC taskSwitchActiveFillGC = (GC)0L;
static GC taskSwitchInactiveFillGC = (GC)0L;
static GC taskSwitchBackgroundGC = (GC)0L;
static GC taskSwitchPinnedTitleFillGC = (GC)0L;
static GC taskSwitchPinnedTitleTextGC = (GC)0L;
static int taskSwitchGCscreen = -1;
static Boolean taskSwitchDragging = False;
static int taskSwitchDragStartX = 0;
static int taskSwitchDragStartY = 0;
static int taskSwitchDragWinX = 0;
static int taskSwitchDragWinY = 0;
static WmScreenData *taskSwitchDragSD = NULL;
static Cursor taskSwitchCursorNormal = (Cursor)0L;
static Cursor taskSwitchCursorHand = (Cursor)0L;
static Cursor taskSwitchCursorDrag = (Cursor)0L;
static Cursor taskSwitchCursorCurrent = (Cursor)0L;
static Window taskSwitchCursorWindow = (Window)0L;
static Boolean taskSwitchPinnedAlt = False;

static void
TaskSwitcherEnsureCursors (void)
{
    if (!taskSwitchCursorNormal)
    {
        taskSwitchCursorNormal = wmGD.workspaceCursor ?
            wmGD.workspaceCursor : XCreateFontCursor (DISPLAY, XC_left_ptr);
    }
    if (!taskSwitchCursorHand)
    {
        taskSwitchCursorHand = XCreateFontCursor (DISPLAY, XC_hand2);
        if (!taskSwitchCursorHand)
            taskSwitchCursorHand = taskSwitchCursorNormal;
    }
    if (!taskSwitchCursorDrag)
    {
        taskSwitchCursorDrag = wmGD.configCursor ?
            wmGD.configCursor : XCreateFontCursor (DISPLAY, XC_fleur);
        if (!taskSwitchCursorDrag)
            taskSwitchCursorDrag = taskSwitchCursorNormal;
    }
}

static void
TaskSwitcherSetCursor (WmScreenData *pSD, Cursor cursor)
{
    if (!pSD || !pSD->taskSwitchWin)
        return;
    if (taskSwitchCursorWindow == pSD->taskSwitchWin &&
        taskSwitchCursorCurrent == cursor)
    {
        return;
    }
    XDefineCursor (DISPLAY, pSD->taskSwitchWin, cursor);
    taskSwitchCursorWindow = pSD->taskSwitchWin;
    taskSwitchCursorCurrent = cursor;
}

Boolean
TaskSwitcherPointerInWindow (WmScreenData *pSD)
{
    Window rootRet, childRet;
    int rootX, rootY;
    int winX, winY;
    unsigned int mask = 0;

    if (!pSD || !pSD->taskSwitchWin)
        return False;

    if (!XQueryPointer (DISPLAY, pSD->rootWindow, &rootRet, &childRet,
                        &rootX, &rootY, &winX, &winY, &mask))
    {
        return False;
    }

    if (childRet == pSD->taskSwitchWin)
        return True;

    if (winX >= 0 && winY >= 0 &&
        winX < (int)pSD->taskSwitchWidth &&
        winY < (int)pSD->taskSwitchHeight)
    {
        return True;
    }

    return False;
}

void
TaskSwitcherActivateSelection (WmScreenData *pSD, ClientData *pCD, Time time)
{
    XEvent ev;

    if (!pSD)
        return;
    if (!pCD &&
        pSD->taskSwitchIndex >= 0 &&
        pSD->taskSwitchIndex < pSD->taskSwitchCount)
    {
        pCD = pSD->taskSwitchList[pSD->taskSwitchIndex];
    }
    if (!pCD)
        return;

    memset(&ev, 0, sizeof(ev));
    ev.xbutton.time = time;
    F_Normalize_And_Raise (NULL, pCD, &ev);
}

void
TaskSwitcherSetPinnedAlt (Boolean active)
{
    taskSwitchPinnedAlt = active;
}

Boolean
TaskSwitcherGetPinnedAlt (void)
{
    return taskSwitchPinnedAlt;
}

void
TaskSwitcherLog (const char *fmt, ...)
{
    if (!fmt)
        return;

    const char *logPath = getenv(TASK_SWITCH_LOG_ENV);
    if (!logPath || logPath[0] == '\0')
        logPath = "/home/klukas/git/cde/cde/log.txt";

    FILE *fp = fopen(logPath, "a");
    if (!fp)
    {
        logPath = "/tmp/dtwm-task-switcher.log";
        fp = fopen(logPath, "a");
        if (!fp)
        {
            va_list ap;
            va_start(ap, fmt);
            fprintf(stderr, "dtwm task switcher log failed: ");
            vfprintf(stderr, fmt, ap);
            fprintf(stderr, "\n");
            va_end(ap);
            return;
        }
    }

    static Boolean wroteHeader = False;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char timestr[32];
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", &tm);

    fprintf(fp, "%s ", timestr);

    if (!wroteHeader)
    {
        wroteHeader = True;
        char exePath[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
        if (len > 0)
        {
            exePath[len] = '\0';
            fprintf(fp, "pid=%ld exe=%s ", (long)getpid(), exePath);
        }
        else
        {
            fprintf(fp, "pid=%ld exe=(unknown) ", (long)getpid());
        }
    }

    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);

    fputc('\n', fp);
    fclose(fp);
}

static void
EnsureTaskSwitchBorderGCs (WmScreenData *pSD)
{
    XGCValues values;

    if (!pSD)
        return;

    if (taskSwitchGCscreen == pSD->screen &&
        taskSwitchActiveBorderGC && taskSwitchInactiveBorderGC &&
        taskSwitchBackgroundGC && taskSwitchPinnedTitleFillGC &&
        taskSwitchPinnedTitleTextGC)
    {
        return;
    }

    if (taskSwitchActiveBorderGC)
    {
        XFreeGC (DISPLAY, taskSwitchActiveBorderGC);
        taskSwitchActiveBorderGC = (GC)0L;
    }
    if (taskSwitchInactiveBorderGC)
    {
        XFreeGC (DISPLAY, taskSwitchInactiveBorderGC);
        taskSwitchInactiveBorderGC = (GC)0L;
    }
    if (taskSwitchActiveFillGC)
    {
        XFreeGC (DISPLAY, taskSwitchActiveFillGC);
        taskSwitchActiveFillGC = (GC)0L;
    }
    if (taskSwitchInactiveFillGC)
    {
        XFreeGC (DISPLAY, taskSwitchInactiveFillGC);
        taskSwitchInactiveFillGC = (GC)0L;
    }
    if (taskSwitchBackgroundGC)
    {
        XFreeGC (DISPLAY, taskSwitchBackgroundGC);
        taskSwitchBackgroundGC = (GC)0L;
    }
    if (taskSwitchPinnedTitleFillGC)
    {
        XFreeGC (DISPLAY, taskSwitchPinnedTitleFillGC);
        taskSwitchPinnedTitleFillGC = (GC)0L;
    }
    if (taskSwitchPinnedTitleTextGC)
    {
        XFreeGC (DISPLAY, taskSwitchPinnedTitleTextGC);
        taskSwitchPinnedTitleTextGC = (GC)0L;
    }

    values.foreground = pSD->feedbackAppearance.activeForeground;
    values.background = pSD->feedbackAppearance.background;
    taskSwitchActiveBorderGC = XCreateGC (DISPLAY, pSD->rootWindow,
                                          GCForeground | GCBackground, &values);

    values.foreground = pSD->feedbackAppearance.foreground;
    values.background = pSD->feedbackAppearance.background;
    taskSwitchInactiveBorderGC = XCreateGC (DISPLAY, pSD->rootWindow,
                                            GCForeground | GCBackground, &values);

    values.foreground = pSD->iconAppearance.activeBackground;
    values.background = pSD->iconAppearance.background;
    taskSwitchActiveFillGC = XCreateGC (DISPLAY, pSD->rootWindow,
                                        GCForeground | GCBackground, &values);

    values.foreground = pSD->iconAppearance.background;
    values.background = pSD->iconAppearance.background;
    taskSwitchInactiveFillGC = XCreateGC (DISPLAY, pSD->rootWindow,
                                          GCForeground | GCBackground, &values);

    values.foreground = pSD->iconAppearance.background;
    values.background = pSD->iconAppearance.background;
    taskSwitchBackgroundGC = XCreateGC (DISPLAY, pSD->rootWindow,
                                        GCForeground | GCBackground, &values);

    values.foreground = pSD->clientTitleAppearance.activeBackground ?
        pSD->clientTitleAppearance.activeBackground :
        pSD->iconAppearance.background;
    values.background = values.foreground;
    taskSwitchPinnedTitleFillGC = XCreateGC (DISPLAY, pSD->rootWindow,
                                             GCForeground | GCBackground, &values);

    values.foreground = pSD->clientTitleAppearance.activeForeground ?
        pSD->clientTitleAppearance.activeForeground :
        pSD->iconAppearance.foreground;
    values.background = pSD->clientTitleAppearance.activeBackground ?
        pSD->clientTitleAppearance.activeBackground :
        pSD->iconAppearance.background;
    taskSwitchPinnedTitleTextGC = XCreateGC (DISPLAY, pSD->rootWindow,
                                             GCForeground | GCBackground, &values);

    taskSwitchGCscreen = pSD->screen;
}

static void
FillRoundedRect (Drawable target, GC gc, int x, int y, int w, int h, int radius)
{
    int r = radius;
    if (w <= 0 || h <= 0)
        return;
    if (r < 0)
        r = 0;
    if (r * 2 > w)
        r = w / 2;
    if (r * 2 > h)
        r = h / 2;

    if (r == 0)
    {
        XFillRectangle (DISPLAY, target, gc, x, y, (unsigned int)w, (unsigned int)h);
        return;
    }

    XFillRectangle (DISPLAY, target, gc, x + r, y, (unsigned int)(w - 2 * r), (unsigned int)h);
    XFillRectangle (DISPLAY, target, gc, x, y + r, (unsigned int)r, (unsigned int)(h - 2 * r));
    XFillRectangle (DISPLAY, target, gc, x + w - r, y + r, (unsigned int)r, (unsigned int)(h - 2 * r));

    XFillArc (DISPLAY, target, gc, x, y, (unsigned int)(2 * r), (unsigned int)(2 * r), 90 * 64, 90 * 64);
    XFillArc (DISPLAY, target, gc, x + w - 2 * r, y, (unsigned int)(2 * r), (unsigned int)(2 * r), 0 * 64, 90 * 64);
    XFillArc (DISPLAY, target, gc, x, y + h - 2 * r, (unsigned int)(2 * r), (unsigned int)(2 * r), 180 * 64, 90 * 64);
    XFillArc (DISPLAY, target, gc, x + w - 2 * r, y + h - 2 * r,
              (unsigned int)(2 * r), (unsigned int)(2 * r), 270 * 64, 90 * 64);
}

/* see WmGlobal.h for index defines: */

#ifndef NO_MESSAGE_CATALOG
static char *confirm_mesg[4] = {"Switch to Default Behavior?",
				"Switch to Custom Behavior?",
                                "Restart Mwm?",
                                "QUIT Mwm?"};


void
initMesg(void)
{

    char * tmpString;

    /*
     * catgets returns a pointer to an area that is over written
     * on each call to catgets.  
     */

    tmpString = ((char *)GETMESSAGE(22, 12, "Switch to Default Behavior?"));
    if ((confirm_mesg[0] =
         (char *)XtMalloc ((unsigned int) (strlen(tmpString) + 1))) == NULL)
    {
        Warning (((char *)GETMESSAGE(22, 2, "Insufficient memory for local message string")));
	confirm_mesg[0] = "Switch to Default Behavior?";
    }
    else
    {
	strcpy(confirm_mesg[0], tmpString);
    }

    tmpString = ((char *)GETMESSAGE(22, 13, "Switch to Custom Behavior?"));
    if ((confirm_mesg[1] =
         (char *)XtMalloc ((unsigned int) (strlen(tmpString) + 1))) == NULL)
    {
        Warning (((char *)GETMESSAGE(22, 2, "Insufficient memory for local message string")));
	confirm_mesg[1] = "Switch to Custom Behavior?";
    }
    else
    {
	strcpy(confirm_mesg[1], tmpString);
    }

    if (MwmBehavior)
    {
	tmpString = ((char *)GETMESSAGE(22, 3, "Restart Mwm?"));
    }
    else
    {
	tmpString = ((char *)GETMESSAGE(22, 10, "Restart Workspace Manager?"));
    }
    if ((confirm_mesg[2] =
         (char *)XtMalloc ((unsigned int) (strlen(tmpString) + 1))) == NULL)
    {
        Warning (((char *)GETMESSAGE(22, 5, "Insufficient memory for local message string")));
	if (MwmBehavior)
	{
	    confirm_mesg[2] = "Restart Mwm?";
	}
	else
	{
	    confirm_mesg[2] = "Restart Workspace Manager?";
	}
    }
    else
    {
	strcpy(confirm_mesg[2], tmpString);
    }



    if (MwmBehavior)
    {
	tmpString = ((char *)GETMESSAGE(22, 6, "QUIT Mwm?"));
    }
    else
    {
	if (wmGD.dtLite)
	{
	    tmpString = ((char *)GETMESSAGE(22, 9, "Log out?"));
	}
	else
	{
	    tmpString = ((char *)GETMESSAGE(22, 11, "QUIT Workspace Manager?"));
	}
    }
    
    if ((confirm_mesg[3] =
         (char *)XtMalloc ((unsigned int) (strlen(tmpString) + 1))) == NULL)
    {
        Warning (((char *)GETMESSAGE(22, 8, "Insufficient memory for local message string")));
	if (MwmBehavior)
	{
	    confirm_mesg[3] = "QUIT Mwm?";
	}
	else
	if (wmGD.dtLite)
	{
	    confirm_mesg[3] = "Log out?";
	}
	else
	{
	    confirm_mesg[3] = "QUIT Workspace Manager?";
	}
    }
    else
    {
	strcpy(confirm_mesg[3], tmpString);
    }


}
#else
static char *confirm_mesg[4] = {"Toggle to Default Behavior?",
				"Toggle to Custom Behavior?",
                                "Restart Mwm?",
                                "QUIT Mwm?"};

#endif
static char *confirm_widget[4] = {"confirmDefaultBehavior",
				  "confirmCustomBehavior",
				  "confirmRestart",
				  "confirmQuit"};


typedef void (*ConfirmFunc)(Boolean);
static ConfirmFunc confirm_func[4] = {Do_Set_Behavior,
				      Do_Set_Behavior,
				      Do_Restart,
				      Do_Quit_Mwm};

static Boolean
FeedbackSnapEnabled (WmScreenData *pSD)
{
    return (pSD->fbSnapEnabled && (pSD->fbStyle & FB_POSITION) &&
	    !(pSD->fbStyle & FB_SIZE));
}

static void
FeedbackSnapLayout (WmScreenData *pSD, int *cellW, int *cellH,
		    int *row1Y, int *row2Y)
{
    int screenW = DisplayWidth (DISPLAY, pSD->screen);
    int screenH = DisplayHeight (DISPLAY, pSD->screen);
    double ratio = 1.0;
    double baseArea = FB_SNAP_BASE_AREA;
    double baseW;
    double baseH;
    int w;
    int h;
    int fontH = pSD->feedbackAppearance.font->ascent +
		pSD->feedbackAppearance.font->descent;

    if (screenH > 0)
    {
	ratio = (double)screenW / (double)screenH;
    }

    baseW = sqrt (baseArea * ratio);
    baseH = sqrt (baseArea / ratio);

    w = (int)(baseW + 0.5);
    h = (int)(baseH + 0.5);

    w *= FB_SNAP_SCALE;
    h *= FB_SNAP_SCALE;

    if (w < FB_SNAP_CELL_MIN)
    {
	w = FB_SNAP_CELL_MIN;
    }
    if (h < FB_SNAP_CELL_MIN)
    {
	h = FB_SNAP_CELL_MIN;
    }

    *cellW = w;
    *cellH = h;

    *row1Y = FEEDBACK_BEVEL + fontH + FB_SNAP_TEXT_GAP;
    *row2Y = *row1Y + *cellH + FB_SNAP_ROW_GAP;
}

static int
TaskSwitchBodyHeight (WmScreenData *pSD)
{
    if (!pSD || !pSD->feedbackAppearance.font)
        return 0;
    return TEXT_HEIGHT (pSD->feedbackAppearance.font) + (2 * TASK_SWITCH_TITLE_PAD);
}

static XmString
TaskSwitchEllipsize (XmFontList fontList, XmString src, int maxWidth, Boolean *needsFree)
{
    char *text = NULL;
    int len;
    int lo;
    int hi;
    int best = -1;
    XmString candidate;
    XmString result = src;

    if (needsFree)
        *needsFree = False;

    if (!src || !fontList || maxWidth <= 0)
        return src;

    if (XmStringWidth (fontList, src) <= (Dimension)maxWidth)
        return src;

    if (!XmStringGetLtoR (src, XmFONTLIST_DEFAULT_TAG, &text) || !text)
        return src;

    len = (int)strlen(text);
    lo = 0;
    hi = len;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        char *tmp;
        size_t tmpLen = (size_t)mid + 3;
        tmp = (char *)XtMalloc (tmpLen + 1);
        if (!tmp)
            break;
        memcpy(tmp, text, (size_t)mid);
        memcpy(tmp + mid, "...", 3);
        tmp[tmpLen] = '\0';
        candidate = XmStringCreateLocalized (tmp);
        XtFree (tmp);
        if (candidate && XmStringWidth (fontList, candidate) <= (Dimension)maxWidth)
        {
            best = mid;
            XmStringFree (candidate);
            lo = mid + 1;
        }
        else
        {
            if (candidate)
                XmStringFree (candidate);
            hi = mid - 1;
        }
    }

    if (best >= 0)
    {
        char *tmp;
        size_t tmpLen = (size_t)best + 3;
        tmp = (char *)XtMalloc (tmpLen + 1);
        if (tmp)
        {
            memcpy(tmp, text, (size_t)best);
            memcpy(tmp + best, "...", 3);
            tmp[tmpLen] = '\0';
            result = XmStringCreateLocalized (tmp);
            XtFree (tmp);
            if (result && needsFree)
                *needsFree = True;
        }
    }

    XtFree (text);
    return result;
}

static void
DrawTaskSwitchIconPixmapScaled (Drawable target, WmScreenData *pSD, ClientData *pCD,
                                int frameX, int frameY, int frameW, int frameH,
                                int labelH)
{
    Pixmap image;
    Window root;
    int src_x, src_y;
    unsigned int src_w, src_h;
    unsigned int border;
    unsigned int depth;
    unsigned int draw_w;
    unsigned int draw_h;
    int iconAreaH;
    int dest_x;
    int dest_y;

    if (!pSD || !pCD)
        return;

    image = pCD->iconPixmap;
    if (pCD->iconWindow)
        image = DEFAULT_PIXMAP(pCD);
    if (!image)
        return;

    if (!XGetGeometry (DISPLAY, image, &root, &src_x, &src_y,
                       &src_w, &src_h, &border, &depth))
    {
        return;
    }

    iconAreaH = frameH - labelH;
    if (iconAreaH <= 0 || frameW <= 0)
        return;

    draw_w = (src_w < (unsigned int)frameW) ? src_w : (unsigned int)frameW;
    draw_h = (src_h < (unsigned int)iconAreaH) ? src_h : (unsigned int)iconAreaH;
    if (draw_w == 0 || draw_h == 0)
        return;

    dest_x = frameX + (frameW - (int)draw_w) / 2;
    dest_y = frameY + (iconAreaH - (int)draw_h) / 2;
    if (dest_x < frameX)
        dest_x = frameX;
    if (dest_y < frameY)
        dest_y = frameY;

    if (pCD->iconMask)
    {
        XSetClipMask (DISPLAY, ICON_APPEARANCE(pCD).inactiveGC, pCD->iconMask);
        XSetClipOrigin (DISPLAY, ICON_APPEARANCE(pCD).inactiveGC, dest_x, dest_y);
    }

    if (depth == 1)
    {
        XCopyPlane (DISPLAY, image, target,
                    ICON_APPEARANCE(pCD).inactiveGC,
                    0, 0, draw_w, draw_h, dest_x, dest_y, 1L);
    }
    else
    {
        XCopyArea (DISPLAY, image, target,
                   ICON_APPEARANCE(pCD).inactiveGC,
                   0, 0, draw_w, draw_h, dest_x, dest_y);
    }

    if (pCD->iconMask)
    {
        XSetClipMask (DISPLAY, ICON_APPEARANCE(pCD).inactiveGC, None);
        XSetClipOrigin (DISPLAY, ICON_APPEARANCE(pCD).inactiveGC, 0, 0);
    }
}

static void
DrawTaskSwitchIconPixmap (WmScreenData *pSD, ClientData *pCD, int frameX, int frameY)
{
    Pixmap image;
    Window root;
    int src_x, src_y;
    int dest_x, dest_y;
    unsigned int src_w, src_h;
    unsigned int border;
    unsigned int depth;
    unsigned int draw_w;
    unsigned int draw_h;
    unsigned int want_w;
    unsigned int want_h;

    if (!pSD || !pCD)
        return;

    image = pCD->iconPixmap;
    if (pCD->iconWindow)
        image = DEFAULT_PIXMAP(pCD);
    if (!image)
        return;

    if (!XGetGeometry (DISPLAY, image, &root, &src_x, &src_y,
                       &src_w, &src_h, &border, &depth))
    {
        return;
    }

    dest_x = frameX + ICON_INNER_X_OFFSET + ICON_INTERNAL_SHADOW_WIDTH;
    dest_y = frameY + ICON_INNER_Y_OFFSET + ICON_INTERNAL_SHADOW_WIDTH;

    want_w = pSD->iconImageMaximum.width + (2 * ICON_INTERNAL_SHADOW_WIDTH);
    want_h = pSD->iconImageMaximum.height + (2 * ICON_INTERNAL_SHADOW_WIDTH);
    if (want_w == 0 || want_h == 0)
    {
        want_w = src_w;
        want_h = src_h;
    }

    draw_w = (src_w < want_w) ? src_w : want_w;
    draw_h = (src_h < want_h) ? src_h : want_h;
    if (draw_w == 0 || draw_h == 0)
        return;

    if (depth == 1)
    {
        XCopyPlane (DISPLAY, image, pSD->taskSwitchWin,
                    ICON_APPEARANCE(pCD).inactiveGC,
                    0, 0, draw_w, draw_h, dest_x, dest_y, 1L);
    }
    else
    {
        XCopyArea (DISPLAY, image, pSD->taskSwitchWin,
                   ICON_APPEARANCE(pCD).inactiveGC,
                   0, 0, draw_w, draw_h, dest_x, dest_y);
    }
}

static void
DrawTaskSwitchIconFromFrame (WmScreenData *pSD, ClientData *pCD,
                             int frameX, int frameY, int frameW, int frameH)
{
    Window iconWin;
    Window root;
    int src_x, src_y;
    unsigned int src_w, src_h;
    unsigned int border;
    unsigned int depth;
    unsigned int draw_w;
    unsigned int draw_h;

    if (!pSD || !pCD)
        return;

    iconWin = ICON_FRAME_WIN(pCD);
    if (!iconWin)
    {
        DrawTaskSwitchIconPixmap (pSD, pCD, frameX, frameY);
        return;
    }

    IconExposureProc (pCD, False);

    if (!XGetGeometry (DISPLAY, iconWin, &root, &src_x, &src_y,
                       &src_w, &src_h, &border, &depth))
    {
        DrawTaskSwitchIconPixmap (pSD, pCD, frameX, frameY);
        return;
    }

    draw_w = (src_w < (unsigned int)frameW) ? src_w : (unsigned int)frameW;
    draw_h = (src_h < (unsigned int)frameH) ? src_h : (unsigned int)frameH;
    if (draw_w == 0 || draw_h == 0)
        return;

    XCopyArea (DISPLAY, iconWin, pSD->taskSwitchWin,
               ICON_APPEARANCE(pCD).inactiveGC,
               0, 0, draw_w, draw_h, frameX, frameY);
}

static void
FeedbackGetUsableArea (WmScreenData *pSD, Boolean fullScreen,
		       int *pX, int *pY, unsigned int *pWidth,
		       unsigned int *pHeight)
{
    int screenW = DisplayWidth (DISPLAY, pSD->screen);
    int screenH = DisplayHeight (DISPLAY, pSD->screen);
    int left = 0;
    int right = 0;
    int top = 0;
    int bottom = 0;

    if (!fullScreen)
    {
	int iconMargin = (pSD->iconPlacementMargin >= 0) ?
	    pSD->iconPlacementMargin : MINIMUM_ICON_SPACING;
	unsigned int iconW = pSD->iconWidth;
	unsigned int iconH = pSD->iconHeight;
	int iconReserveW = (int)iconW + 2 * iconMargin;
	int iconReserveH = (int)iconH + 2 * iconMargin;
	Boolean hasLeft = (pSD->iconPlacement &
			   (ICON_PLACE_LEFT_PRIMARY | ICON_PLACE_LEFT_SECONDARY)) != 0;
	Boolean hasRight = (pSD->iconPlacement &
			    (ICON_PLACE_RIGHT_PRIMARY | ICON_PLACE_RIGHT_SECONDARY)) != 0;
	Boolean hasTop = (pSD->iconPlacement &
			  (ICON_PLACE_TOP_PRIMARY | ICON_PLACE_TOP_SECONDARY)) != 0;
	Boolean hasBottom = (pSD->iconPlacement &
			     (ICON_PLACE_BOTTOM_PRIMARY | ICON_PLACE_BOTTOM_SECONDARY)) != 0;

	if (!pSD->useIconBox &&
	    pSD->pActiveWS && pSD->pActiveWS->ppClients)
	{
	    int minX = screenW;
	    int minY = screenH;
	    int maxX = 0;
	    int maxY = 0;
	    Boolean found = False;
	    unsigned int i;

	    for (i = 0; i < pSD->pActiveWS->numClients; i++)
	    {
		ClientData *pcd = pSD->pActiveWS->ppClients[i];
		int x, y;
		int w, h;

		if (!pcd || !ICON_FRAME_WIN(pcd))
		{
		    continue;
		}
		if (pcd->clientState != MINIMIZED_STATE &&
		    !(pcd->pSD->showOpenWindowIcons))
		{
		    continue;
		}

		x = ICON_X(pcd);
		y = ICON_Y(pcd);
		w = (int)ICON_WIDTH(pcd);
		h = (int)ICON_HEIGHT(pcd);

		if (x < minX) minX = x;
		if (y < minY) minY = y;
		if ((x + w) > maxX) maxX = x + w;
		if ((y + h) > maxY) maxY = y + h;
		found = True;
	    }

	    if (found)
	    {
		if (hasLeft)
		{
		    int reserve = maxX + iconMargin;
		    if (left < reserve)
		    {
			left = reserve;
		    }
		}
		if (hasRight)
		{
		    int reserve = (screenW - minX) + iconMargin;
		    if (right < reserve)
		    {
			right = reserve;
		    }
		}
		if (!hasLeft && !hasRight)
		{
		    if (hasTop)
		    {
			int reserve = maxY + iconMargin;
			if (top < reserve)
			{
			    top = reserve;
			}
		    }
		    if (hasBottom)
		    {
			int reserve = (screenH - minY) + iconMargin;
			if (bottom < reserve)
			{
			    bottom = reserve;
			}
		    }
		}
	    }
	}

	if (hasLeft || hasRight)
	{
	    if (hasLeft)
	    {
		if (left < iconReserveW)
		{
		    left = iconReserveW;
		}
	    }
	    if (hasRight)
	    {
		if (right < iconReserveW)
		{
		    right = iconReserveW;
		}
	    }
	}
	else
	{
	    if (hasTop)
	    {
		if (top < iconReserveH)
		{
		    top = iconReserveH;
		}
	    }
	    if (hasBottom)
	    {
		if (bottom < iconReserveH)
		{
		    bottom = iconReserveH;
		}
	    }
	}

	if (pSD->launcherList)
	{
	    LauncherWindow *entry;
	    for (entry = pSD->launcherList; entry; entry = entry->next)
	    {
		ClientData *pCD = entry->pCD;
		int px, py, pw, ph;
		Boolean horizontal;

		if (!pCD)
		{
		    if (entry->win != None &&
			!XFindContext (DISPLAY, entry->win,
				       wmGD.windowContextType,
				       (caddr_t *)&pCD))
		    {
			entry->pCD = pCD;
		    }
		}

		if (!pCD)
		{
		    continue;
		}

		if (pCD->clientState == MINIMIZED_STATE)
		{
		    continue;
		}

		px = pCD->clientX;
		py = pCD->clientY;
		pw = pCD->clientWidth;
		ph = pCD->clientHeight;

		horizontal = (pw >= ph);
		if (horizontal)
		{
		    if (py < (screenH / 2))
		    {
			int reserve = py + ph + iconMargin;
			if (top < reserve) top = reserve;
		    }
		    else
		    {
			int reserve = screenH - py + iconMargin;
			if (bottom < reserve) bottom = reserve;
		    }
		}
		else
		{
		    if (px < (screenW / 2))
		    {
			int reserve = px + pw + iconMargin;
			if (left < reserve) left = reserve;
		    }
		    else
		    {
			int reserve = screenW - px + iconMargin;
			if (right < reserve) right = reserve;
		    }
		}
	    }
	}
    }

    *pX = left;
    *pY = top;
    *pWidth = (unsigned int)(screenW - left - right);
    *pHeight = (unsigned int)(screenH - top - bottom);

    if ((int)*pWidth < 1)
    {
	*pWidth = 1;
    }
    if ((int)*pHeight < 1)
    {
	*pHeight = 1;
    }
}

static int
FeedbackGetSnapZone (WmScreenData *pSD, int rootX, int rootY)
{
    int cellW, cellH, row1Y, row2Y, rowX;
    int localX, localY;
    int i;

    if (!FeedbackSnapEnabled(pSD) || !pSD->feedbackWin)
    {
	return FB_SNAP_NONE;
    }

    if (rootX < pSD->fbWinX || rootY < pSD->fbWinY ||
	rootX >= (pSD->fbWinX + (int)pSD->fbWinWidth) ||
	rootY >= (pSD->fbWinY + (int)pSD->fbWinHeight))
    {
	return FB_SNAP_NONE;
    }

    localX = rootX - pSD->fbWinX;
    localY = rootY - pSD->fbWinY;

    FeedbackSnapLayout (pSD, &cellW, &cellH, &row1Y, &row2Y);
    rowX = FEEDBACK_BEVEL +
	(((int)pSD->fbWinWidth - 2 * FEEDBACK_BEVEL) -
	 (5 * cellW + 4 * FB_SNAP_CELL_GAP)) / 2;

    for (i = 0; i < 5; i++)
    {
	int x = rowX + i * (cellW + FB_SNAP_CELL_GAP);
	int y = row1Y;
	if (localX >= x && localX < (x + cellW) &&
	    localY >= y && localY < (y + cellH))
	{
	    switch (i)
	    {
		case 0: return FB_SNAP_MAXIMIZE;
		case 1: return FB_SNAP_HALF_LEFT;
		case 2: return FB_SNAP_HALF_RIGHT;
		case 3: return FB_SNAP_HALF_TOP;
		case 4: return FB_SNAP_HALF_BOTTOM;
	    }
	}
    }

    for (i = 0; i < 5; i++)
    {
	int x = rowX + i * (cellW + FB_SNAP_CELL_GAP);
	int y = row2Y;
	if (localX >= x && localX < (x + cellW) &&
	    localY >= y && localY < (y + cellH))
	{
	    switch (i)
	    {
		case 0: return FB_SNAP_MINIMIZE;
		case 1: return FB_SNAP_QUAD_TL;
		case 2: return FB_SNAP_QUAD_TR;
		case 3: return FB_SNAP_QUAD_BL;
		case 4: return FB_SNAP_QUAD_BR;
	    }
	}
    }

    return FB_SNAP_NONE;
}

static GC
GetFeedbackSnapGC (WmScreenData *pSD, Boolean active)
{
    Pixel color = active ? pSD->clientAppearance.activeBackground
			 : pSD->clientAppearance.background;
    GC *gcPtr = active ? &pSD->fbSnapActiveGC : &pSD->fbSnapInactiveGC;
    Pixel *colorPtr = active ? &pSD->fbSnapActiveColor
			     : &pSD->fbSnapInactiveColor;

    if (!(*gcPtr) || (*colorPtr != color))
    {
	XGCValues values;
	XtGCMask mask = GCForeground | GCBackground;

	if (*gcPtr)
	{
	    XFreeGC (DISPLAY, *gcPtr);
	    *gcPtr = NULL;
	}

	values.foreground = color;
	values.background = pSD->feedbackAppearance.background;
	*gcPtr = XCreateGC (DISPLAY, pSD->rootWindow, mask, &values);
	*colorPtr = color;
    }

    return *gcPtr;
}

static GC
GetFeedbackSnapBgGC (WmScreenData *pSD)
{
    Pixel color = BlackPixel (DISPLAY, pSD->screen);

    if (!pSD->fbSnapBgGC || pSD->fbSnapBgColor != color)
    {
	XGCValues values;
	XtGCMask mask = GCForeground | GCBackground;

	if (pSD->fbSnapBgGC)
	{
	    XFreeGC (DISPLAY, pSD->fbSnapBgGC);
	    pSD->fbSnapBgGC = NULL;
	}

	values.foreground = color;
	values.background = pSD->feedbackAppearance.background;
	pSD->fbSnapBgGC = XCreateGC (DISPLAY, pSD->rootWindow, mask, &values);
	pSD->fbSnapBgColor = color;
    }

    return pSD->fbSnapBgGC;
}

static void
DrawSunkenRect (WmScreenData *pSD, int x, int y, unsigned int width,
		unsigned int height)
{
    GC topGC = pSD->feedbackAppearance.inactiveBottomShadowGC;
    GC botGC = pSD->feedbackAppearance.inactiveTopShadowGC;

    if (width < 2 || height < 2)
    {
	return;
    }

    XDrawLine (DISPLAY, pSD->feedbackWin, topGC,
	       x, y, x + (int)width - 1, y);
    XDrawLine (DISPLAY, pSD->feedbackWin, topGC,
	       x, y, x, y + (int)height - 1);
    XDrawLine (DISPLAY, pSD->feedbackWin, botGC,
	       x, y + (int)height - 1, x + (int)width - 1, y + (int)height - 1);
    XDrawLine (DISPLAY, pSD->feedbackWin, botGC,
	       x + (int)width - 1, y, x + (int)width - 1, y + (int)height - 1);
}

static void
FillSnapRect (WmScreenData *pSD, int x, int y, unsigned int width,
	      unsigned int height, int zone)
{
    GC fillGC = GetFeedbackSnapGC (pSD, (pSD->fbSnapHover == zone));
    GC bgGC = GetFeedbackSnapBgGC (pSD);
    int ux, uy;
    unsigned int uw, uh;
    int screenW = DisplayWidth (DISPLAY, pSD->screen);
    int screenH = DisplayHeight (DISPLAY, pSD->screen);
    int ix = x + 2;
    int iy = y + 2;
    int iw = (int)width - 4;
    int ih = (int)height - 4;
    int fillX, fillY, fillW, fillH;
    int usableX, usableY;
    int usableW, usableH;

    if (!fillGC || !bgGC || iw <= 0 || ih <= 0)
    {
	return;
    }

    XFillRectangle (DISPLAY, pSD->feedbackWin, bgGC,
		    ix, iy, (unsigned int)iw, (unsigned int)ih);

    FeedbackGetUsableArea (pSD, pSD->fbSnapFullScreen,
			   &ux, &uy, &uw, &uh);

    usableX = ix + (ux * iw) / screenW;
    usableY = iy + (uy * ih) / screenH;
    usableW = (int)((uw * iw) / screenW);
    usableH = (int)((uh * ih) / screenH);

    switch (zone)
    {
	case FB_SNAP_MAXIMIZE:
	    fillX = usableX;
	    fillY = usableY;
	    fillW = usableW;
	    fillH = usableH;
	    XFillRectangle (DISPLAY, pSD->feedbackWin, fillGC,
			    fillX, fillY, (unsigned int)fillW, (unsigned int)fillH);
	    break;
	case FB_SNAP_MINIMIZE:
	    if (pSD->fbSnapClient &&
		ICON_FRAME_WIN(pSD->fbSnapClient) &&
		!(pSD->fbSnapClient->pSD->useIconBox &&
		  P_ICON_BOX(pSD->fbSnapClient)))
	    {
		int iconX = ICON_X(pSD->fbSnapClient);
		int iconY = ICON_Y(pSD->fbSnapClient);
		int iconW = (int)ICON_WIDTH(pSD->fbSnapClient);
		int iconH = (int)ICON_HEIGHT(pSD->fbSnapClient);

		fillX = ix + (iconX * iw) / screenW;
		fillY = iy + (iconY * ih) / screenH;
		fillW = (iconW * iw) / screenW;
		fillH = (iconH * ih) / screenH;

		if (fillW < 4) fillW = 4;
		if (fillH < 4) fillH = 4;

		if (fillX < ix) fillX = ix;
		if (fillY < iy) fillY = iy;
		if (fillX + fillW > ix + iw)
		{
		    fillW = (ix + iw) - fillX;
		}
		if (fillY + fillH > iy + ih)
		{
		    fillH = (iy + ih) - fillY;
		}
	    }
	    else
	    {
		fillW = usableW / 5;
		if (fillW < 4) fillW = 4;
		fillH = usableH / 5;
		if (fillH < 4) fillH = 4;
		fillX = usableX + 2;
		fillY = usableY + (usableH - fillH) - 2;
	    }
	    XFillRectangle (DISPLAY, pSD->feedbackWin, fillGC,
			    fillX, fillY, (unsigned int)fillW, (unsigned int)fillH);
	    XDrawRectangle (DISPLAY, pSD->feedbackWin,
			    pSD->feedbackAppearance.inactiveTopShadowGC,
			    fillX, fillY,
			    (unsigned int)(fillW - 1),
			    (unsigned int)(fillH - 1));
	    break;
	case FB_SNAP_HALF_LEFT:
	    fillX = usableX;
	    fillY = usableY;
	    fillW = usableW / 2;
	    fillH = usableH;
	    XFillRectangle (DISPLAY, pSD->feedbackWin, fillGC,
			    fillX, fillY, (unsigned int)fillW, (unsigned int)fillH);
	    break;
	case FB_SNAP_HALF_RIGHT:
	    fillX = usableX + (usableW / 2);
	    fillY = usableY;
	    fillW = usableW - (usableW / 2);
	    fillH = usableH;
	    XFillRectangle (DISPLAY, pSD->feedbackWin, fillGC,
			    fillX, fillY, (unsigned int)fillW, (unsigned int)fillH);
	    break;
	case FB_SNAP_HALF_TOP:
	    fillX = usableX;
	    fillY = usableY;
	    fillW = usableW;
	    fillH = usableH / 2;
	    XFillRectangle (DISPLAY, pSD->feedbackWin, fillGC,
			    fillX, fillY, (unsigned int)fillW, (unsigned int)fillH);
	    break;
	case FB_SNAP_HALF_BOTTOM:
	    fillX = usableX;
	    fillY = usableY + (usableH / 2);
	    fillW = usableW;
	    fillH = usableH - (usableH / 2);
	    XFillRectangle (DISPLAY, pSD->feedbackWin, fillGC,
			    fillX, fillY, (unsigned int)fillW, (unsigned int)fillH);
	    break;
	case FB_SNAP_QUAD_TL:
	    fillX = usableX;
	    fillY = usableY;
	    fillW = usableW / 2;
	    fillH = usableH / 2;
	    XFillRectangle (DISPLAY, pSD->feedbackWin, fillGC,
			    fillX, fillY, (unsigned int)fillW, (unsigned int)fillH);
	    break;
	case FB_SNAP_QUAD_TR:
	    fillX = usableX + (usableW / 2);
	    fillY = usableY;
	    fillW = usableW - (usableW / 2);
	    fillH = usableH / 2;
	    XFillRectangle (DISPLAY, pSD->feedbackWin, fillGC,
			    fillX, fillY, (unsigned int)fillW, (unsigned int)fillH);
	    break;
	case FB_SNAP_QUAD_BL:
	    fillX = usableX;
	    fillY = usableY + (usableH / 2);
	    fillW = usableW / 2;
	    fillH = usableH - (usableH / 2);
	    XFillRectangle (DISPLAY, pSD->feedbackWin, fillGC,
			    fillX, fillY, (unsigned int)fillW, (unsigned int)fillH);
	    break;
	case FB_SNAP_QUAD_BR:
	    fillX = usableX + (usableW / 2);
	    fillY = usableY + (usableH / 2);
	    fillW = usableW - (usableW / 2);
	    fillH = usableH - (usableH / 2);
	    XFillRectangle (DISPLAY, pSD->feedbackWin, fillGC,
			    fillX, fillY, (unsigned int)fillW, (unsigned int)fillH);
	    break;
	default:
	    break;
    }
}

/*************************************<->*************************************
 *
 *  ShowFeedbackWindow(pSD, x, y, width, height, style)
 *
 *
 *  Description:
 *  -----------
 *  Pop up the window for moving and sizing feedback
 *
 *
 *  Inputs:
 *  ------
 *  pSD		- pointer to screen data
 *  x		- initial x-value
 *  y		- initial y-value
 *  width 	- initial width value
 *  height	- initial height value
 *  style	- show size, position, or both
 *  
 * 
 *  Outputs:
 *  -------
 *
 *
 *  Comments:
 *  --------
 *************************************<->***********************************/
void ShowFeedbackWindow (WmScreenData *pSD, int x, int y, unsigned int width, unsigned int height, unsigned long style)
{
    unsigned long        mask = 0;
    XSetWindowAttributes win_attribs;
    XWindowChanges       win_changes;
    int                  direction, ascent, descent;
    XCharStruct          xcsLocation;
    int                  winX, winY;
    int                  tmpX, tmpY;

    if ( (pSD->fbStyle = style) == FB_OFF)
	return;

    pSD->fbLastX = x;
    pSD->fbLastY = y;
    pSD->fbLastWidth = width;
    pSD->fbLastHeight = height;

    /*
     * Derive the size and position of the window from the text extents
     * Set starting position of each string 
     */
    XTextExtents(pSD->feedbackAppearance.font, DEFAULT_POSITION_STRING, 
		 strlen(DEFAULT_POSITION_STRING), &direction, &ascent, 
		 &descent, &xcsLocation);
    
    pSD->fbWinWidth = xcsLocation.width + 4*FEEDBACK_BEVEL;

    switch (pSD->fbStyle) 
    {
	case FB_SIZE:
	    pSD->fbSizeY = 2*FEEDBACK_BEVEL + ascent;
	    pSD->fbWinHeight = (ascent + descent) + 4*FEEDBACK_BEVEL;
	    break;

	case FB_POSITION:
	    pSD->fbLocY = 2*FEEDBACK_BEVEL + ascent;
	    pSD->fbWinHeight = (ascent + descent) + 4*FEEDBACK_BEVEL;
	    if (FeedbackSnapEnabled(pSD))
	    {
		int cellW, cellH, row1Y, row2Y;
		int rowW;

		FeedbackSnapLayout (pSD, &cellW, &cellH, &row1Y, &row2Y);
		rowW = 5 * cellW + 4 * FB_SNAP_CELL_GAP;
		if (pSD->fbWinWidth < (unsigned int)(rowW + 2 * FEEDBACK_BEVEL))
		{
		    pSD->fbWinWidth = rowW + 2 * FEEDBACK_BEVEL;
		}
		pSD->fbLocY = FEEDBACK_BEVEL + ascent;
		pSD->fbWinHeight = row2Y + cellH + FEEDBACK_BEVEL;
	    }
	    break;

	default:
	case (FB_SIZE | FB_POSITION):
	    pSD->fbLocY = 2*FEEDBACK_BEVEL + ascent;
	    pSD->fbSizeY = pSD->fbLocY + ascent + descent;
	    pSD->fbWinHeight = 2*(ascent + descent) + 4*FEEDBACK_BEVEL;
	    break;
    }

    if (pSD->feedbackGeometry) /* set by user */
    {
	unsigned int junkWidth, junkHeight;

	mask = XParseGeometry(pSD->feedbackGeometry, &tmpX, &tmpY,
			      &junkWidth, &junkHeight);
    }

    if (mask & (XValue|YValue))
    {
	winX = (mask & XNegative) ? 
	    DisplayWidth(DISPLAY, pSD->screen)  + tmpX - pSD->fbWinWidth : tmpX;
	winY = (mask & YNegative) ? 
	    DisplayHeight(DISPLAY, pSD->screen) + tmpY -pSD->fbWinHeight : tmpY;
    }
    else
    {
	winX = (DisplayWidth(DISPLAY, pSD->screen) - pSD->fbWinWidth)/2;
	winY = (DisplayHeight(DISPLAY, pSD->screen) -pSD->fbWinHeight)/2;
    }

    pSD->fbWinX = winX;
    pSD->fbWinY = winY;

    /* 
     * Put new text into the feedback strings
     */
    UpdateFeedbackText (pSD, x, y, width, height);

    /*
     * bevel the window border for a 3-D look
     */
    if ( (pSD->fbTop && pSD->fbBottom) ||
	 ((pSD->fbTop = AllocateRList((unsigned)2*FEEDBACK_BEVEL)) &&
	  (pSD->fbBottom = AllocateRList((unsigned)2*FEEDBACK_BEVEL))) )
    {
	pSD->fbTop->used = 0;
	pSD->fbBottom->used = 0;
	BevelRectangle (pSD->fbTop,
			pSD->fbBottom,
			0, 0, 
			pSD->fbWinWidth, pSD->fbWinHeight,
			FEEDBACK_BEVEL, FEEDBACK_BEVEL,
			FEEDBACK_BEVEL, FEEDBACK_BEVEL);
    }

    /*
     * Create window if not yet created, otherwise fix size and position
     */

    if (!pSD->feedbackWin)
    {

	/*
	 * Create the window
	 */

	mask = CWEventMask | CWOverrideRedirect | CWSaveUnder;
	win_attribs.event_mask = ExposureMask;
	win_attribs.override_redirect = TRUE;
	win_attribs.save_under = TRUE;

	/* 
	 * Use background pixmap if one is specified, otherwise set the
	 * appropriate background color. 
	 */

	if (pSD->feedbackAppearance.backgroundPixmap)
	{
	    mask |= CWBackPixmap;
	    win_attribs.background_pixmap =
				pSD->feedbackAppearance.backgroundPixmap;
	}
	else
	{
	    mask |= CWBackPixel;
	    win_attribs.background_pixel =
				pSD->feedbackAppearance.background;
	}

	pSD->feedbackWin = XCreateWindow (DISPLAY, pSD->rootWindow, 
					  winX, winY,
					  pSD->fbWinWidth, 
					  pSD->fbWinHeight,
					  0, CopyFromParent, 
					  InputOutput, CopyFromParent, 
					  mask, &win_attribs);
    }
    else
    {
	win_changes.x = winX;
	win_changes.y = winY;
	win_changes.width = pSD->fbWinWidth;
	win_changes.height = pSD->fbWinHeight;
	win_changes.stack_mode = Above;

	mask = CWX | CWY | CWWidth | CWHeight | CWStackMode;

	XConfigureWindow(DISPLAY, pSD->feedbackWin, (unsigned int) mask, 
	    &win_changes);
	pSD->fbWinX = winX;
	pSD->fbWinY = winY;
    }


    /*
     * Make the feedback window visible (map it)
     */

    if (pSD && pSD->feedbackWin)
    {
	/* Make sure the feedback window doesn't get buried */
	XRaiseWindow(DISPLAY, pSD->feedbackWin);
	XMapWindow (DISPLAY, pSD->feedbackWin);
	PaintFeedbackWindow(pSD);
    }

} /* END OF FUNCTION ShowFeedbackWindow */



/*************************************<->*************************************
 *
 *  PaintFeedbackWindow(pSD)
 *
 *
 *  Description:
 *  -----------
 *  Repaints the feedback window in response to exposure events
 *
 *
 *  Inputs:
 *  ------
 *  pSD		- pointer to screen data
 * 
 *  Outputs:
 *  -------
 *
 *
 *  Comments:
 *  --------
 *************************************<->***********************************/
void PaintFeedbackWindow (WmScreenData *pSD)
{
    if (pSD->feedbackWin)
    {
	/* 
	 * draw beveling 
	 */
	if (pSD->fbTop && pSD->fbTop->used > 0) 
	{
	    XFillRectangles (DISPLAY, pSD->feedbackWin, 
			     pSD->feedbackAppearance.inactiveTopShadowGC,
			     pSD->fbTop->prect, pSD->fbTop->used);
	}
	if (pSD->fbBottom && pSD->fbBottom->used > 0) 
	{
	    XFillRectangles (DISPLAY, pSD->feedbackWin, 
			     pSD->feedbackAppearance.inactiveBottomShadowGC,
			     pSD->fbBottom->prect, 
			     pSD->fbBottom->used);
	}

	/*
	 * clear old text 
	 */
	XClearArea (DISPLAY, pSD->feedbackWin, 
		    FEEDBACK_BEVEL, FEEDBACK_BEVEL,
		    pSD->fbWinWidth-2*FEEDBACK_BEVEL, 
		    pSD->fbWinHeight-2*FEEDBACK_BEVEL,
		    FALSE);

	/*
	 * put up new text
	 */
	if (pSD->fbStyle & FB_POSITION) 
	{
	    WmDrawString (DISPLAY, pSD->feedbackWin, 
			 pSD->feedbackAppearance.inactiveGC,
			 pSD->fbLocX, pSD->fbLocY, 
			 pSD->fbLocation, strlen(pSD->fbLocation));
	}
	if (pSD->fbStyle & FB_SIZE) 
	{
	    WmDrawString (DISPLAY, pSD->feedbackWin, 
			 pSD->feedbackAppearance.inactiveGC,
			 pSD->fbSizeX, pSD->fbSizeY, 
			 pSD->fbSize, strlen(pSD->fbSize));
	}

	if (FeedbackSnapEnabled(pSD))
	{
	    int cellW, cellH, row1Y, row2Y, rowX;
	    int i;

	    FeedbackSnapLayout (pSD, &cellW, &cellH, &row1Y, &row2Y);
	    rowX = FEEDBACK_BEVEL +
		(((int)pSD->fbWinWidth - 2 * FEEDBACK_BEVEL) -
		 (5 * cellW + 4 * FB_SNAP_CELL_GAP)) / 2;

	    for (i = 0; i < 5; i++)
	    {
		int x = rowX + i * (cellW + FB_SNAP_CELL_GAP);
		int y = row1Y;
		int zone = FB_SNAP_MAXIMIZE;
		switch (i)
		{
		    case 0: zone = FB_SNAP_MAXIMIZE; break;
		    case 1: zone = FB_SNAP_HALF_LEFT; break;
		    case 2: zone = FB_SNAP_HALF_RIGHT; break;
		    case 3: zone = FB_SNAP_HALF_TOP; break;
		    case 4: zone = FB_SNAP_HALF_BOTTOM; break;
		}
		DrawSunkenRect (pSD, x, y, (unsigned int)cellW,
				(unsigned int)cellH);
		FillSnapRect (pSD, x, y, (unsigned int)cellW,
			      (unsigned int)cellH, zone);
	    }

	    for (i = 0; i < 5; i++)
	    {
		int x = rowX + i * (cellW + FB_SNAP_CELL_GAP);
		int y = row2Y;
		int zone = FB_SNAP_MINIMIZE;
		switch (i)
		{
		    case 0: zone = FB_SNAP_MINIMIZE; break;
		    case 1: zone = FB_SNAP_QUAD_TL; break;
		    case 2: zone = FB_SNAP_QUAD_TR; break;
		    case 3: zone = FB_SNAP_QUAD_BL; break;
		    case 4: zone = FB_SNAP_QUAD_BR; break;
		}
		DrawSunkenRect (pSD, x, y, (unsigned int)cellW,
				(unsigned int)cellH);
		FillSnapRect (pSD, x, y, (unsigned int)cellW,
			      (unsigned int)cellH, zone);
	    }
	}
    }
}



/*************************************<->*************************************
 *
 *  HideFeedbackWindow (pSD)
 *
 *
 *  Description:
 *  -----------
 *  Hide the feedback window
 *
 *
 *  Inputs:
 *  ------
 *  pDS		- pointer to screen data
 * 
 *  Outputs:
 *  -------
 *
 *
 *  Comments:
 *  --------
 * 
 *************************************<->***********************************/
void HideFeedbackWindow (WmScreenData *pSD)
{
    if (pSD->feedbackWin)
    {
	XUnmapWindow (DISPLAY, pSD->feedbackWin);
	ForceColormapFocus (ACTIVE_PSD, ACTIVE_PSD->colormapFocus);
    }
    pSD->fbStyle = FB_OFF;
    pSD->fbSnapHover = FB_SNAP_NONE;
    pSD->fbSnapEnabled = False;
    pSD->fbSnapFullScreen = False;
    pSD->fbSnapClient = NULL;
}




/*************************************<->*************************************
 *
 *  UpdateFeedbackInfo (pSD, x, y, width, height)
 *
 *
 *  Description:
 *  -----------
 *  Update the information in the feedback window
 *
 *
 *  Inputs:
 *  ------
 *  pSD		- pointer to screen info
 *  x		- x-value
 *  y		- y-value
 *  width 	- width value
 *  height	- height value
 *
 * 
 *  Outputs:
 *  -------
 *
 *
 *  Comments:
 *  --------
 * 
 *************************************<->***********************************/
void UpdateFeedbackInfo (WmScreenData *pSD, int x, int y, unsigned int width, unsigned int height)
{
    /*
     * Currently the feedback window must always be redrawn to (potentially)
     * repair damage done by moving the configuration outline.  The feedback
     * repainting generally only needs to be done when the information
     * changes or the feedback window is actually overwritten by the
     * configuration outline.
     */

	pSD->fbLastX = x;
	pSD->fbLastY = y;
	pSD->fbLastWidth = width;
	pSD->fbLastHeight = height;

	UpdateFeedbackText (pSD, x, y, width, height);

	PaintFeedbackWindow(pSD);
}




/*************************************<->*************************************
 *
 *  UpdateFeedbackText (pSD, x, y, width, height)
 *
 *
 *  Description:
 *  -----------
 *  Update the information in the feedback strings
 *
 *
 *  Inputs:
 *  ------
 *  pSD		- pointer to screen data
 *  x		- x-value
 *  y		- y-value
 *  width 	- width value
 *  height	- height value
 *
 * 
 *  Outputs:
 *  -------
 *
 *
 *  Comments:
 *  --------
 * 
 *************************************<->***********************************/
void UpdateFeedbackText (WmScreenData *pSD, int x, int y, unsigned int width, unsigned int height)
{
    int         direction, ascent, descent;
    XCharStruct xcs;

    if (pSD->fbStyle & FB_POSITION) 
    {
	sprintf (pSD->fbLocation, "(%4d,%-4d)", x, y);
	XTextExtents(pSD->feedbackAppearance.font, pSD->fbLocation,
		 strlen(pSD->fbLocation), &direction, &ascent, 
		 &descent, &xcs);
	pSD->fbLocX = (pSD->fbWinWidth - xcs.width)/2;
    }

    if (pSD->fbStyle & FB_SIZE) 
    {
	sprintf (pSD->fbSize,     "%4dx%-4d", width, height);
	XTextExtents(pSD->feedbackAppearance.font, pSD->fbSize,
		 strlen(pSD->fbSize), &direction, &ascent, 
		 &descent, &xcs);
	pSD->fbSizeX = (pSD->fbWinWidth - xcs.width)/2;
    }
}

Boolean FeedbackUpdateSnapHover (WmScreenData *pSD, int rootX, int rootY)
{
    int zone;

    if (!FeedbackSnapEnabled(pSD))
    {
	if (pSD->fbSnapHover != FB_SNAP_NONE)
	{
	    pSD->fbSnapHover = FB_SNAP_NONE;
	    PaintFeedbackWindow (pSD);
	    return True;
	}
	return False;
    }

    zone = FeedbackGetSnapZone (pSD, rootX, rootY);
    if (zone != pSD->fbSnapHover)
    {
	pSD->fbSnapHover = zone;
	PaintFeedbackWindow (pSD);
	return True;
    }

    return False;
}

Boolean FeedbackGetSnapGeometry (WmScreenData *pSD, int rootX, int rootY,
				 int *pX, int *pY, unsigned int *pWidth,
				 unsigned int *pHeight)
{
    int zone = FeedbackGetSnapZone (pSD, rootX, rootY);
    int areaX;
    int areaY;
    unsigned int areaW;
    unsigned int areaH;

    if (zone == FB_SNAP_NONE)
    {
	return False;
    }

    FeedbackGetUsableArea (pSD, pSD->fbSnapFullScreen,
			   &areaX, &areaY, &areaW, &areaH);

    switch (zone)
    {
	case FB_SNAP_MAXIMIZE:
	    *pX = areaX;
	    *pY = areaY;
	    *pWidth = areaW;
	    *pHeight = areaH;
	    break;
	case FB_SNAP_HALF_LEFT:
	    *pX = areaX;
	    *pY = areaY;
	    *pWidth = areaW / 2;
	    *pHeight = areaH;
	    break;
	case FB_SNAP_HALF_RIGHT:
	    *pX = areaX + (int)(areaW / 2);
	    *pY = areaY;
	    *pWidth = areaW - (unsigned int)(areaW / 2);
	    *pHeight = areaH;
	    break;
	case FB_SNAP_HALF_TOP:
	    *pX = areaX;
	    *pY = areaY;
	    *pWidth = areaW;
	    *pHeight = areaH / 2;
	    break;
	case FB_SNAP_HALF_BOTTOM:
	    *pX = areaX;
	    *pY = areaY + (int)(areaH / 2);
	    *pWidth = areaW;
	    *pHeight = areaH - (unsigned int)(areaH / 2);
	    break;
	case FB_SNAP_QUAD_TL:
	    *pX = areaX;
	    *pY = areaY;
	    *pWidth = areaW / 2;
	    *pHeight = areaH / 2;
	    break;
	case FB_SNAP_QUAD_TR:
	    *pX = areaX + (int)(areaW / 2);
	    *pY = areaY;
	    *pWidth = areaW - (unsigned int)(areaW / 2);
	    *pHeight = areaH / 2;
	    break;
	case FB_SNAP_QUAD_BL:
	    *pX = areaX;
	    *pY = areaY + (int)(areaH / 2);
	    *pWidth = areaW / 2;
	    *pHeight = areaH - (unsigned int)(areaH / 2);
	    break;
	case FB_SNAP_QUAD_BR:
	    *pX = areaX + (int)(areaW / 2);
	    *pY = areaY + (int)(areaH / 2);
	    *pWidth = areaW - (unsigned int)(areaW / 2);
	    *pHeight = areaH - (unsigned int)(areaH / 2);
	    break;
	default:
	    return False;
    }

    return True;
}

int FeedbackGetSnapAction (WmScreenData *pSD, int rootX, int rootY)
{
    return FeedbackGetSnapZone (pSD, rootX, rootY);
}



/*************************************<->*************************************
 *
 *  static void
 *  OkCB (w, client_data, call_data)
 *
 *
 *  Description:
 *  -----------
 *  QuestionBox Ok callback.
 *
 *
 *  Inputs:
 *  ------
 *  None.
 *
 * 
 *  Outputs:
 *  -------
 *  None.
 *
 *
 *  Comments:
 *  --------
 *  None.
 * 
 *************************************<->***********************************/

static void OkCB (Widget w, caddr_t client_data, caddr_t call_data)
{
    WithdrawDialog (w);

    confirm_func[((WmScreenData *)client_data)->actionNbr] (False);

    wmGD.confirmDialogMapped = False;

} /* END OF FUNCTION OkCB */


/*************************************<->*************************************
 *
 *  static void
 *  CancelCB (w, client_data, call_data)
 *
 *
 *  Description:
 *  -----------
 *  QuestionBox Cancel callback.
 *
 *
 *  Inputs:
 *  ------
 *  None.
 *
 * 
 *  Outputs:
 *  -------
 *  None.
 *
 *
 *  Comments:
 *  --------
 *  None.
 * 
 *************************************<->***********************************/

static void CancelCB (Widget w, caddr_t client_data, caddr_t call_data)
{
    WithdrawDialog (w);

    wmGD.confirmDialogMapped = False;

} /* END OF FUNCTION CancelCB */



/*************************************<->*************************************
 *
 *  void
 *  ConfirmAction (pSD,nbr)
 *
 *
 *  Description:
 *  -----------
 *  Post a QuestionBox and ask for confirmation.  If so, executes the
 *  appropriate action.
 *
 *
 *  Inputs:
 *  ------
 *  nbr = action number
 *  pSD->screen
 *  pSD->screenTopLevel
 *
 * 
 *  Outputs:
 *  -------
 *  actionNbr = current QuestionBox widget index.
 *  confirmW[actionNbr]  = QuestionBox widget.
 *
 *
 *  Comments:
 *  --------
 * 
 *************************************<->***********************************/

void ConfirmAction (WmScreenData *pSD, int nbr)
{
    Arg           args[8];
    int  n;
    int           x, y;
    Dimension     width, height;
    Widget        dialogShellW = NULL;
    XmString	  messageString;
    static XmString	  defaultMessageString = NULL;


    /*
     * If there is a system modal window, don't post another
     * one.  We need to think about a way to let a new system
     * modal window be posted, and when unposted, restore the
     * modal state of the current system modal window.  
     */

    if(wmGD.systemModalActive)
    {
	return ;
    }

    if (pSD->confirmboxW[nbr] == NULL)
    /* First time for this one */
    {
#ifndef NO_MESSAGE_CATALOG
	/*
	 * Initialize messages
	 */
	initMesg();
#endif

        /* 
         * Create a dialog popup shell with explicit keyboard policy.
         */

        n = 0;
        XtSetArg(args[n], XmNx, (XtArgVal)
	         (DisplayWidth (DISPLAY, pSD->screen)/2)); n++;
        XtSetArg(args[n], XmNy, (XtArgVal)
	         (DisplayHeight (DISPLAY, pSD->screen)/2)); n++;
        XtSetArg(args[n], XtNallowShellResize, (XtArgVal) TRUE);  n++;
        XtSetArg(args[n], XtNkeyboardFocusPolicy, (XtArgVal) XmEXPLICIT);  n++;
        XtSetArg(args[n], XtNdepth, 
		(XtArgVal) DefaultDepth(DISPLAY, pSD->screen));  n++;
        XtSetArg(args[n], XtNscreen, 
		(XtArgVal) ScreenOfDisplay(DISPLAY, pSD->screen));  n++;

        dialogShellW =
    	        XtCreatePopupShell ((String) WmNfeedback, 
				    transientShellWidgetClass,
		                    pSD->screenTopLevelW, args, n);

        /* 
         * Create a QuestionBox as a child of the popup shell.
	 * Set traversalOn and add callbacks for the OK and CANCEL buttons.
	 * Unmanage the HELP button.
         */

        n = 0;
        XtSetArg(args[n], XmNdialogType, (XtArgVal) XmDIALOG_QUESTION); n++;
        XtSetArg(args[n], XmNmessageAlignment, (XtArgVal) XmALIGNMENT_CENTER);
	   n++;
        XtSetArg(args[n], XmNtraversalOn, (XtArgVal) TRUE); n++;

	/*
	 * In 1.2 confirmbox's widget name changed from the generic
	 * WmNconfirmbox (ie. 'confirmbox') to a more descriptive name
	 * so that each confirm dialog can be customized separately (e.g.
	 * "Mwm*confirmRestart*messageString: restart it?").
	 */

        pSD->confirmboxW[nbr] = 
	    XtCreateManagedWidget (confirm_widget[nbr], xmMessageBoxWidgetClass,
                                   dialogShellW, args, n);

        n = 0;
        XtSetArg(args[n], XmNmessageString, &messageString); n++;
        XtGetValues(pSD->confirmboxW[nbr], (ArgList) args, n);

	if (defaultMessageString == NULL)
	{
	    defaultMessageString = XmStringCreateLocalized ("");
	}

        n = 0;

	/*
	 * If the message string is the default, then put something
	 * 'reasonable' in instead.
	 */

	if (XmStringCompare( messageString, defaultMessageString ))
	{
            messageString = XmStringCreateLocalized(confirm_mesg[nbr]);
	    XtSetArg(args[n], XmNmessageString, (XtArgVal) messageString); n++;
	    XtSetValues(pSD->confirmboxW[nbr], (ArgList) args, n);
            XmStringFree(messageString);
	}

        n = 0;
        XtSetArg (args[n], XmNtraversalOn, (XtArgVal) TRUE); n++;
        XtSetArg (args[n], XmNhighlightThickness, 
		  (XtArgVal) CB_HIGHLIGHT_THICKNESS); n++;
#ifndef NO_MESSAGE_CATALOG
	XtSetArg(args[n], XmNlabelString, wmGD.okLabel); n++;
#endif
        XtSetValues ( XmMessageBoxGetChild (pSD->confirmboxW[nbr], 
			    XmDIALOG_OK_BUTTON), args, n);
#ifndef NO_MESSAGE_CATALOG
	n--;
	XtSetArg(args[n], XmNlabelString, wmGD.cancelLabel); n++;
#endif
        XtSetValues ( XmMessageBoxGetChild (pSD->confirmboxW[nbr], 
			    XmDIALOG_CANCEL_BUTTON), args, n);
        XtAddCallback (pSD->confirmboxW[nbr], XmNokCallback, 
	    (XtCallbackProc)OkCB, (XtPointer)pSD); 
        XtAddCallback (pSD->confirmboxW[nbr], XmNcancelCallback, 
	    (XtCallbackProc)CancelCB, (XtPointer)NULL); 

        XtUnmanageChild
	    (XmMessageBoxGetChild (pSD->confirmboxW[nbr], 
		XmDIALOG_HELP_BUTTON));

        XtRealizeWidget (dialogShellW);

        /* 
         * Center the DialogShell in the display.
         */

        n = 0;
        XtSetArg(args[n], XmNheight, &height); n++;
        XtSetArg(args[n], XmNwidth, &width); n++;
        XtGetValues (dialogShellW, (ArgList) args, n);

        x = (DisplayWidth (DISPLAY, pSD->screen) - ((int) width))/2;
        y = (DisplayHeight (DISPLAY, pSD->screen) - ((int) height))/2;
        n = 0;
        XtSetArg(args[n], XmNx, (XtArgVal) x); n++;
        XtSetArg(args[n], XmNy, (XtArgVal) y); n++;
        XtSetValues (dialogShellW, (ArgList) args, n);

        ManageWindow (pSD, XtWindow(dialogShellW), MANAGEW_CONFIRM_BOX);
    }
    else
    {
        ReManageDialog (pSD, pSD->confirmboxW[nbr]);
    }

    pSD->actionNbr = nbr;

    XFlush(DISPLAY);

    wmGD.confirmDialogMapped = True;

} /* END OF FUNCTION ConfirmAction */



/*************************************<->*************************************
 *
 *  EnterWaitState (void)
 *  LeaveWatState (void)
 *
 *  Description:
 *  -----------
 *  Enter the wait state.
 *  Leave the wait state.
 *
 *  Inputs:
 *  ------
 *  None.
 *
 * 
 *  Outputs:
 *  -------
 *  None.
 *
 *
 *  Comments:
 *  --------
 *  None.
 * 
 *************************************<->***********************************/

void EnterWaitState(void)
{
    if (!waitCursor)
	waitCursor = _DtGetHourGlassCursor(DISPLAY);

    XGrabPointer (DISPLAY, DefaultRootWindow(DISPLAY), FALSE,
		0, GrabModeAsync, GrabModeAsync, None,
		waitCursor, CurrentTime);
    XGrabKeyboard (DISPLAY, DefaultRootWindow(DISPLAY), FALSE,
		GrabModeAsync, GrabModeAsync, CurrentTime);
}

void LeaveWaitState(void)
{
    XUngrabPointer (DISPLAY, CurrentTime);
    XUngrabKeyboard (DISPLAY, CurrentTime);
}



/*************************************<->*************************************
 *
 *  InitCursorInfo ()
 *
 *
 *  Description:
 *  -----------
 *  This function determines whether a server supports large cursors.  It it
 *  does large feedback cursors are used in some cases (wait state and
 *  system modal state); otherwise smaller (16x16) standard cursors are used.
 *
 *  Outputs:
 *  -------
 *  wmGD.useLargeCusors = set to True if larger cursors are supported.
 * 
 *************************************<->***********************************/

void InitCursorInfo (void)
{
    unsigned int cWidth;
    unsigned int cHeight;

    TaskSwitcherLog("TaskSwitcherLog init");

    wmGD.useLargeCursors = False;

    if (XQueryBestCursor (DISPLAY, DefaultRootWindow(DISPLAY), 
	32, 32, &cWidth, &cHeight))
    {
	if ((cWidth >= 32) && (cHeight >= 32))
	{
	    wmGD.useLargeCursors = True;
	}
    }

} /* END OF FUNCTION InitCursorInfo */


/* ===================================================================== */
/* Task switcher (Alt+Tab) */

static void
FreeTaskSwitchList (WmScreenData *pSD)
{
    TaskSwitcherLog("FreeTaskSwitchList enter list=%p count=%d",
                    (void *)pSD->taskSwitchList, pSD->taskSwitchCount);
    if (pSD->taskSwitchTitles)
    {
        int i;
        for (i = 0; i < pSD->taskSwitchCount; i++)
        {
            if (pSD->taskSwitchTitles[i])
                XmStringFree (pSD->taskSwitchTitles[i]);
        }
        XtFree ((char *)pSD->taskSwitchTitles);
        pSD->taskSwitchTitles = NULL;
    }
    if (pSD->taskSwitchList)
    {
        XtFree ((char *)pSD->taskSwitchList);
        pSD->taskSwitchList = NULL;
    }
    pSD->taskSwitchCount = 0;
    pSD->taskSwitchIndex = -1;
    TaskSwitcherLog("FreeTaskSwitchList done");
}

static void
BuildTaskSwitchList (WmScreenData *pSD)
{
    ClientListEntry *pEntry;
    ClientData *pCD;
    int count = 0;
    int capacity;

    TaskSwitcherLog("BuildTaskSwitchList start pSD=%p", (void *)pSD);

    FreeTaskSwitchList (pSD);

    capacity = (int)pSD->clientCounter + 4;
    if (capacity < 8)
        capacity = 8;

    pSD->taskSwitchList = (ClientData **)XtCalloc (capacity, sizeof(ClientData *));
    pSD->taskSwitchTitles = (XmString *)XtCalloc (capacity, sizeof(XmString));

    for (pEntry = pSD->clientList; pEntry; pEntry = pEntry->nextSibling)
    {
        int i;
        Boolean duplicate = False;

        pCD = pEntry->pCD;
        if (!pCD)
            continue;
        if (pCD->clientName)
        {
            if (!strncmp(pCD->clientName, "popup_name", 10) ||
                !strncmp(pCD->clientName, "popup_", 6))
            {
                continue;
            }
        }
        if (pSD->taskSwitchWin)
        {
            if ((pCD->client == pSD->taskSwitchWin) ||
                (pCD->clientFrameWin == pSD->taskSwitchWin) ||
                (pCD->clientBaseWin == pSD->taskSwitchWin) ||
                (pCD->clientTitleWin == pSD->taskSwitchWin) ||
                (ICON_FRAME_WIN(pCD) == pSD->taskSwitchWin))
            {
                continue;
            }
        }
        if ((pCD->clientState & ~UNSEEN_STATE) == WITHDRAWN_STATE)
            continue;
        if (pCD->clientState & UNSEEN_STATE)
            continue;
        if (!ClientInWorkspace (pSD->pActiveWS, pCD))
            continue;

        for (i = 0; i < count; i++)
        {
            if (pSD->taskSwitchList[i] == pCD)
            {
                duplicate = True;
                break;
            }
        }
        if (duplicate)
            continue;

        if (count >= capacity)
        {
            capacity *= 2;
            pSD->taskSwitchList = (ClientData **)XtRealloc (
                (char *)pSD->taskSwitchList, capacity * sizeof(ClientData *));
            pSD->taskSwitchTitles = (XmString *)XtRealloc (
                (char *)pSD->taskSwitchTitles, capacity * sizeof(XmString));
        }

        pSD->taskSwitchList[count++] = pCD;
        if (CLIENT_DISPLAY_TITLE(pCD))
            pSD->taskSwitchTitles[count - 1] = XmStringCopy (CLIENT_DISPLAY_TITLE(pCD));
        else if (ICON_DISPLAY_TITLE(pCD))
            pSD->taskSwitchTitles[count - 1] = XmStringCopy (ICON_DISPLAY_TITLE(pCD));
        else
            pSD->taskSwitchTitles[count - 1] = XmStringCreateLocalized ("(untitled)");
    }

    pSD->taskSwitchCount = count;
    TaskSwitcherLog("BuildTaskSwitchList done count=%d", count);
}

static void
ComputeTaskSwitchLayout (WmScreenData *pSD)
{
    int screenW;
    int screenH;
    int extraGap = 0;
    int bodyH = TaskSwitchBodyHeight (pSD);
    int gridW = 0;
    int gridH = 0;
    double ratio = 1.0;

    if (!pSD)
        return;

    TaskSwitcherLog("ComputeTaskSwitchLayout count=%d", pSD->taskSwitchCount);

    screenW = DisplayWidth (DISPLAY, pSD->screen);
    screenH = DisplayHeight (DISPLAY, pSD->screen);
    if (screenH > 0)
        ratio = (double)screenW / (double)screenH;

    if (pSD->iconPlacementMargin > 0)
        extraGap = pSD->iconPlacementMargin;

    if (pSD->taskSwitchCount > 0)
    {
        int cols;
        int rows;
        int baseIconW;
        int baseIconH;
        int maxIconW;
        int maxIconH;
        int minIconW = 24;
        int minIconH = 24;
        double maxGridW;
        double maxGridH;
        int labelH = 0;

        if (pSD->iconAppearance.font)
            labelH = TEXT_HEIGHT (pSD->iconAppearance.font);
        minIconH = labelH + 8;

        baseIconW = pSD->iconWidth;
        baseIconH = pSD->iconHeight;

        if (pSD->taskSwitchCount <= 5)
        {
            cols = pSD->taskSwitchCount;
            rows = 1;
        }
        else
        {
            cols = (int)ceil (sqrt ((double)pSD->taskSwitchCount * ratio));
            if (cols < 1)
                cols = 1;
            if (cols > pSD->taskSwitchCount)
                cols = pSD->taskSwitchCount;
            rows = (pSD->taskSwitchCount + cols - 1) / cols;
        }

        pSD->taskSwitchCols = cols;
        pSD->taskSwitchRows = rows;

        maxGridW = 0.6 * (double)screenW;
        maxGridH = 0.6 * (double)screenH;

        maxIconW = (int)(maxGridW / cols) - (ICON_GRID_EXTRA(pSD) + IB_SPACING + extraGap + (2 * IB_MARGIN_WIDTH));
        maxIconH = (int)(maxGridH / rows) - (IB_SPACING + extraGap + (2 * IB_MARGIN_HEIGHT));

        if (maxIconW < 8)
            maxIconW = 8;
        if (maxIconH < 8)
            maxIconH = 8;

        if (maxIconW < minIconW)
            minIconW = maxIconW;
        if (maxIconH < minIconH)
            minIconH = maxIconH;

        pSD->taskSwitchIconW = baseIconW;
        pSD->taskSwitchIconH = baseIconH;

        if (pSD->taskSwitchIconW > maxIconW)
            pSD->taskSwitchIconW = maxIconW;
        if (pSD->taskSwitchIconH > maxIconH)
            pSD->taskSwitchIconH = maxIconH;

        if (pSD->taskSwitchIconW < minIconW)
            pSD->taskSwitchIconW = minIconW;
        if (pSD->taskSwitchIconH < minIconH)
            pSD->taskSwitchIconH = minIconH;

        pSD->taskSwitchCellW = pSD->taskSwitchIconW +
            ICON_GRID_EXTRA(pSD) + IB_SPACING + extraGap +
            (2 * IB_MARGIN_WIDTH);
        pSD->taskSwitchCellH = pSD->taskSwitchIconH +
            IB_SPACING + extraGap + (2 * IB_MARGIN_HEIGHT);

        gridW = cols * pSD->taskSwitchCellW;
        gridH = rows * pSD->taskSwitchCellH;
    }
    else
    {
        pSD->taskSwitchCols = 0;
        pSD->taskSwitchRows = 0;
        pSD->taskSwitchCellW = 0;
        pSD->taskSwitchCellH = 0;
        pSD->taskSwitchIconW = 0;
        pSD->taskSwitchIconH = 0;
    }

    pSD->taskSwitchTitleH = 0;
    if (pSD->clientTitleAppearance.font)
    {
        pSD->taskSwitchTitleH =
            TEXT_HEIGHT (pSD->clientTitleAppearance.font) + (2 * TASK_SWITCH_TITLE_PAD);
    }
    else if (pSD->feedbackAppearance.font)
    {
        pSD->taskSwitchTitleH =
            TEXT_HEIGHT (pSD->feedbackAppearance.font) + (2 * TASK_SWITCH_TITLE_PAD);
    }

    pSD->taskSwitchWidth = (2 * TASK_SWITCH_MARGIN) + gridW;
    if (pSD->taskSwitchWidth < TASK_SWITCH_MIN_WIDTH)
        pSD->taskSwitchWidth = TASK_SWITCH_MIN_WIDTH;

    pSD->taskSwitchHeight = (2 * TASK_SWITCH_MARGIN) + pSD->taskSwitchTitleH + bodyH;
    if (gridH > 0)
        pSD->taskSwitchHeight += TASK_SWITCH_TEXT_GAP + gridH;
    if (pSD->taskSwitchHeight < TASK_SWITCH_MIN_HEIGHT)
        pSD->taskSwitchHeight = TASK_SWITCH_MIN_HEIGHT;

    pSD->taskSwitchX = (screenW - pSD->taskSwitchWidth) / 2;
    pSD->taskSwitchY = (screenH - pSD->taskSwitchHeight) / 2;
    TaskSwitcherLog("ComputeTaskSwitchLayout cols=%d rows=%d cell=%dx%d win=%dx%d pos=%d,%d",
                    pSD->taskSwitchCols, pSD->taskSwitchRows,
                    pSD->taskSwitchCellW, pSD->taskSwitchCellH,
                    pSD->taskSwitchWidth, pSD->taskSwitchHeight,
                    pSD->taskSwitchX, pSD->taskSwitchY);
}

static void
EnsureTaskSwitchWindow (WmScreenData *pSD)
{
    XSetWindowAttributes win_attribs;
    unsigned long mask = CWEventMask | CWOverrideRedirect | CWSaveUnder;

    win_attribs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                             PointerMotionMask | KeyPressMask | KeyReleaseMask;
    win_attribs.override_redirect = True;
    win_attribs.save_under = True;

    mask |= CWBackPixel;
    win_attribs.background_pixel = pSD->iconAppearance.background;

    pSD->taskSwitchWin = XCreateWindow (DISPLAY, pSD->rootWindow,
                                        pSD->taskSwitchX, pSD->taskSwitchY,
                                        pSD->taskSwitchWidth,
                                        pSD->taskSwitchHeight,
                                        0, CopyFromParent, InputOutput,
                                        CopyFromParent, mask, &win_attribs);

    if (pSD->taskSwitchWin)
    {
        XClassHint classHint;
        XStoreName (DISPLAY, pSD->taskSwitchWin, "Task Switcher");
        classHint.res_name = "dtwm-task-switcher";
        classHint.res_class = "DtwmTaskSwitcher";
        XSetClassHint (DISPLAY, pSD->taskSwitchWin, &classHint);
        TaskSwitcherEnsureCursors ();
        TaskSwitcherSetCursor (pSD, taskSwitchCursorNormal);
    }
}

static void
TaskSwitcherTimeout (XtPointer client_data, XtIntervalId *id)
{
    WmScreenData *pSD = (WmScreenData *)client_data;
    Window rootRet, childRet;
    int rootX, rootY;
    int winX, winY;
    unsigned int mask = 0;

    taskSwitchTimer = (XtIntervalId)0;
    TaskSwitcherLog("TaskSwitcherTimeout tick");

    if (!pSD || !pSD->taskSwitchActive)
        return;
    if (pSD->taskSwitchPinned && !taskSwitchPinnedAlt)
        return;
    if (pSD->taskSwitchPinned)
        return;

    if (XQueryPointer (DISPLAY, pSD->rootWindow, &rootRet, &childRet,
                       &rootX, &rootY, &winX, &winY, &mask))
    {
        if (!(mask & Mod1Mask))
        {
            TaskSwitcherLog("TaskSwitcherTimeout mod1 up -> finish");
            FinishTaskSwitcher (pSD, CurrentTime, True);
            return;
        }
    }
    else
    {
        TaskSwitcherLog("TaskSwitcherTimeout XQueryPointer failed -> finish");
        FinishTaskSwitcher (pSD, CurrentTime, False);
        return;
    }

    taskSwitchTimer = XtAppAddTimeOut (XtDisplayToApplicationContext (DISPLAY),
                                       TASK_SWITCH_TIMEOUT_MS,
                                       TaskSwitcherTimeout, (XtPointer)pSD);
}

Boolean
TaskSwitcherActive (WmScreenData *pSD)
{
    static WmScreenData *lastPsd = NULL;
    static int lastActive = -1;
    int active = (pSD && pSD->taskSwitchActive) ? 1 : 0;

    if (pSD != lastPsd || active != lastActive)
    {
        TaskSwitcherLog("TaskSwitcherActive query pSD=%p active=%d",
                        (void *)pSD, active);
        lastPsd = pSD;
        lastActive = active;
    }
    return (pSD && pSD->taskSwitchActive);
}

void
PaintTaskSwitcher (WmScreenData *pSD)
{
    XRectangle titleBox;
    XRectangle bodyBox;
    XmString titleString = NULL;
    XmString bodyString = NULL;
    Boolean bodyNeedsFree = False;
    Boolean bodyEllipsized = False;
    int bodyTextH = TaskSwitchBodyHeight (pSD);
    int labelH = 0;
    GC activeBorderGC;
    GC inactiveBorderGC;
    GC activeFillGC;
    GC inactiveFillGC;
    int startX;
    int startY;
    int i;

    if (!pSD || !pSD->taskSwitchWin)
        return;

    TaskSwitcherLog("PaintTaskSwitcher enter count=%d index=%d win=%lu",
                    pSD->taskSwitchCount, pSD->taskSwitchIndex,
                    (unsigned long)pSD->taskSwitchWin);

    EnsureTaskSwitchBorderGCs (pSD);
    activeBorderGC = taskSwitchActiveBorderGC ?
        taskSwitchActiveBorderGC : pSD->feedbackAppearance.inactiveGC;
    inactiveBorderGC = taskSwitchInactiveBorderGC ?
        taskSwitchInactiveBorderGC : pSD->feedbackAppearance.inactiveGC;
    activeFillGC = taskSwitchActiveFillGC ?
        taskSwitchActiveFillGC : pSD->iconAppearance.inactiveGC;
    inactiveFillGC = taskSwitchInactiveFillGC ?
        taskSwitchInactiveFillGC : pSD->iconAppearance.inactiveGC;

    TaskSwitcherLog("PaintTaskSwitcher XClearWindow start");
    XClearWindow (DISPLAY, pSD->taskSwitchWin);
    TaskSwitcherLog("PaintTaskSwitcher XClearWindow done");

    /* bevel border */
    TaskSwitcherLog("PaintTaskSwitcher border start");
    {
        GC borderGC = pSD->taskSwitchPinned ? activeBorderGC : inactiveBorderGC;
        XDrawLine (DISPLAY, pSD->taskSwitchWin, borderGC,
                   0, 0, pSD->taskSwitchWidth - 1, 0);
        XDrawLine (DISPLAY, pSD->taskSwitchWin, borderGC,
                   0, 0, 0, pSD->taskSwitchHeight - 1);
        XDrawLine (DISPLAY, pSD->taskSwitchWin, borderGC,
                   pSD->taskSwitchWidth - 1, 0,
                   pSD->taskSwitchWidth - 1, pSD->taskSwitchHeight - 1);
        XDrawLine (DISPLAY, pSD->taskSwitchWin, borderGC,
                   0, pSD->taskSwitchHeight - 1,
                   pSD->taskSwitchWidth - 1, pSD->taskSwitchHeight - 1);
    }
    TaskSwitcherLog("PaintTaskSwitcher border done");

    /* title */
    if (pSD->taskSwitchTitleH > 0 &&
        (pSD->clientTitleAppearance.fontList || pSD->feedbackAppearance.fontList))
    {
        XmFontList titleFontList = pSD->clientTitleAppearance.fontList ?
            pSD->clientTitleAppearance.fontList :
            pSD->feedbackAppearance.fontList;
        GC titleGC = pSD->iconAppearance.inactiveGC;
        GC titleFillGC = (GC)0L;
        if (pSD->taskSwitchPinned)
        {
            titleFillGC = taskSwitchPinnedTitleFillGC ?
                taskSwitchPinnedTitleFillGC : taskSwitchActiveFillGC;
            titleGC = taskSwitchPinnedTitleTextGC ?
                taskSwitchPinnedTitleTextGC : pSD->iconAppearance.inactiveGC;
            if (titleFillGC)
            {
                XFillRectangle (DISPLAY, pSD->taskSwitchWin, titleFillGC,
                                TASK_SWITCH_MARGIN, TASK_SWITCH_MARGIN,
                                pSD->taskSwitchWidth - (2 * TASK_SWITCH_MARGIN),
                                pSD->taskSwitchTitleH);
                if (activeBorderGC && inactiveBorderGC)
                {
                    int tx = TASK_SWITCH_MARGIN;
                    int ty = TASK_SWITCH_MARGIN;
                    int tw = pSD->taskSwitchWidth - (2 * TASK_SWITCH_MARGIN);
                    int th = pSD->taskSwitchTitleH;
                    XDrawLine (DISPLAY, pSD->taskSwitchWin, activeBorderGC,
                               tx, ty, tx + tw - 1, ty);
                    XDrawLine (DISPLAY, pSD->taskSwitchWin, activeBorderGC,
                               tx, ty, tx, ty + th - 1);
                    XDrawLine (DISPLAY, pSD->taskSwitchWin, inactiveBorderGC,
                               tx + tw - 1, ty, tx + tw - 1, ty + th - 1);
                    XDrawLine (DISPLAY, pSD->taskSwitchWin, inactiveBorderGC,
                               tx, ty + th - 1, tx + tw - 1, ty + th - 1);
                }
            }
        }
        titleString = XmStringCreateLocalized ("Task Switcher");
        titleBox.x = TASK_SWITCH_MARGIN;
        titleBox.y = TASK_SWITCH_MARGIN;
        titleBox.width = pSD->taskSwitchWidth - (2 * TASK_SWITCH_MARGIN);
        titleBox.height = pSD->taskSwitchTitleH;
        XmStringDraw (DISPLAY, pSD->taskSwitchWin,
                      titleFontList,
                      titleString,
                      titleGC,
                      titleBox.x, titleBox.y + TASK_SWITCH_TITLE_PAD,
                      titleBox.width, XmALIGNMENT_CENTER,
                      XmSTRING_DIRECTION_L_TO_R, &titleBox);
        XmStringFree (titleString);
    }

    /* body (simulated next window title) */
    if (pSD->feedbackAppearance.fontList)
    {
        TaskSwitcherLog("PaintTaskSwitcher title/body start");
        int bodyIndex = pSD->taskSwitchIndex;
        if (pSD->taskSwitchHoverIndex >= 0 &&
            pSD->taskSwitchHoverIndex < pSD->taskSwitchCount)
        {
            bodyIndex = pSD->taskSwitchHoverIndex;
        }

        if (bodyIndex >= 0 &&
            bodyIndex < pSD->taskSwitchCount)
        {
            bodyString = pSD->taskSwitchTitles ?
                pSD->taskSwitchTitles[bodyIndex] : NULL;
        }

        if (!bodyString)
        {
            bodyString = XmStringCreateLocalized ("(none)");
            bodyNeedsFree = True;
        }

        bodyBox.x = TASK_SWITCH_MARGIN;
        bodyBox.y = TASK_SWITCH_MARGIN + pSD->taskSwitchTitleH;
        bodyBox.width = pSD->taskSwitchWidth - (2 * TASK_SWITCH_MARGIN);
        bodyBox.height = bodyTextH;

        if (bodyBox.height > 0)
        {
            XmString bodyDraw = TaskSwitchEllipsize (pSD->feedbackAppearance.fontList,
                                                     bodyString, bodyBox.width,
                                                     &bodyEllipsized);
            Dimension textWidth = XmStringWidth (pSD->feedbackAppearance.fontList,
                                                 bodyDraw);
            int alignment = (textWidth >= bodyBox.width) ?
                XmALIGNMENT_BEGINNING : XmALIGNMENT_CENTER;

            XmStringDraw (DISPLAY, pSD->taskSwitchWin,
                          pSD->feedbackAppearance.fontList,
                          bodyDraw,
                          pSD->iconAppearance.inactiveGC,
                          bodyBox.x, bodyBox.y + TASK_SWITCH_TITLE_PAD,
                          bodyBox.width, alignment,
                          XmSTRING_DIRECTION_L_TO_R, &bodyBox);
            if (bodyEllipsized)
                XmStringFree (bodyDraw);
        }

        if (bodyNeedsFree)
            XmStringFree (bodyString);
    }
    TaskSwitcherLog("PaintTaskSwitcher title/body done");

    if (pSD->iconAppearance.font)
        labelH = TEXT_HEIGHT (pSD->iconAppearance.font);

    if (pSD->taskSwitchCount > 0 && pSD->taskSwitchCols > 0)
    {
        TaskSwitcherLog("PaintTaskSwitcher grid start count=%d", pSD->taskSwitchCount);
        startX = TASK_SWITCH_MARGIN;
        startY = TASK_SWITCH_MARGIN + pSD->taskSwitchTitleH + bodyTextH +
            TASK_SWITCH_TEXT_GAP;

        for (i = 0; i < pSD->taskSwitchCount; i++)
        {
            TaskSwitcherLog("PaintTaskSwitcher icon %d start", i);
            int row = i / pSD->taskSwitchCols;
            int col = i % pSD->taskSwitchCols;
            int cellX = startX + col * pSD->taskSwitchCellW;
            int cellY = startY + row * pSD->taskSwitchCellH;
            int frameX = cellX + IB_MARGIN_WIDTH;
            int frameY = cellY + IB_MARGIN_HEIGHT;
            int frameW = pSD->taskSwitchIconW + ICON_GRID_EXTRA(pSD);
            int frameH = pSD->taskSwitchIconH;

            TaskSwitcherLog("PaintTaskSwitcher icon %d frame x=%d y=%d w=%d h=%d",
                            i, frameX, frameY, frameW, frameH);
            if (i == pSD->taskSwitchIndex)
            {
                TaskSwitcherLog("PaintTaskSwitcher icon %d frame draw active start", i);
                FillRoundedRect (pSD->taskSwitchWin, activeFillGC,
                                 frameX, frameY, frameW, frameH, 4);
                TaskSwitcherLog("PaintTaskSwitcher icon %d frame draw active done", i);
            }
            else
            {
                TaskSwitcherLog("PaintTaskSwitcher icon %d frame draw inactive start", i);
                FillRoundedRect (pSD->taskSwitchWin, inactiveFillGC,
                                 frameX, frameY, frameW, frameH, 4);
                TaskSwitcherLog("PaintTaskSwitcher icon %d frame draw inactive done", i);
            }

            if (pSD->taskSwitchList && pSD->taskSwitchList[i])
            {
                DrawTaskSwitchIconPixmapScaled (pSD->taskSwitchWin, pSD, pSD->taskSwitchList[i],
                                                frameX, frameY, frameW, frameH,
                                                labelH);
            }
            if (labelH > 0 &&
                pSD->iconAppearance.fontList &&
                pSD->taskSwitchTitles &&
                pSD->taskSwitchTitles[i])
            {
                XRectangle textBox;
                XRectangle labelBox;
                Dimension textWidth;
                int alignment;
                XmString labelDraw;
                Boolean labelEllipsized = False;
                int pad = 2;
                int labelBoxH = labelH + (2 * pad);

                labelBox.x = frameX + pad;
                labelBox.width = frameW - (2 * pad);
                labelBox.height = labelBoxH;
                labelBox.y = frameY + frameH - labelBoxH - pad;
                if (labelBox.y < frameY + pad)
                    labelBox.y = frameY + pad;
                if (labelBox.width < 1)
                    labelBox.width = 1;

                textBox.x = labelBox.x + pad;
                textBox.width = labelBox.width - (2 * pad);
                textBox.height = labelH;
                textBox.y = labelBox.y + pad;
                if (textBox.width < 1)
                    textBox.width = 1;

                TaskSwitcherLog("PaintTaskSwitcher icon %d label width calc start", i);
                labelDraw = TaskSwitchEllipsize (pSD->iconAppearance.fontList,
                                                 pSD->taskSwitchTitles[i],
                                                 textBox.width,
                                                 &labelEllipsized);
                textWidth = XmStringWidth (pSD->iconAppearance.fontList,
                                           labelDraw);
                TaskSwitcherLog("PaintTaskSwitcher icon %d label width=%u", i, (unsigned int)textWidth);
                alignment = (textWidth >= textBox.width) ?
                    XmALIGNMENT_BEGINNING : XmALIGNMENT_CENTER;

                TaskSwitcherLog("PaintTaskSwitcher icon %d label draw start", i);
                FillRoundedRect (pSD->taskSwitchWin,
                                 (i == pSD->taskSwitchIndex) ? activeFillGC : inactiveFillGC,
                                 labelBox.x, labelBox.y,
                                 labelBox.width, labelBox.height, 3);
                XmStringDraw (DISPLAY, pSD->taskSwitchWin,
                              pSD->iconAppearance.fontList,
                              labelDraw,
                              (i == pSD->taskSwitchIndex) ? activeBorderGC : pSD->iconAppearance.inactiveGC,
                              textBox.x, textBox.y, textBox.width,
                              alignment, XmSTRING_DIRECTION_L_TO_R, &textBox);
                TaskSwitcherLog("PaintTaskSwitcher icon %d label draw done", i);
                if (labelEllipsized)
                    XmStringFree (labelDraw);
            }
            TaskSwitcherLog("PaintTaskSwitcher icon %d done", i);
        }
        TaskSwitcherLog("PaintTaskSwitcher grid done");
    }
    TaskSwitcherLog("PaintTaskSwitcher exit");
}

static void
TaskSwitcherComputeGeometry (WmScreenData *pSD, int *startX, int *startY,
                             int *labelH, int *bodyTextH)
{
    int bodyH = 0;
    int label = 0;

    if (pSD)
    {
        if (pSD->iconAppearance.font)
            label = TEXT_HEIGHT (pSD->iconAppearance.font);
        bodyH = TaskSwitchBodyHeight (pSD);
    }

    if (startX)
        *startX = TASK_SWITCH_MARGIN;
    if (startY)
    {
        if (pSD)
            *startY = TASK_SWITCH_MARGIN + pSD->taskSwitchTitleH + bodyH +
                TASK_SWITCH_TEXT_GAP;
        else
            *startY = 0;
    }
    if (labelH)
        *labelH = label;
    if (bodyTextH)
        *bodyTextH = bodyH;
}

static void
PaintTaskSwitcherBody (WmScreenData *pSD)
{
    XRectangle bodyBox;
    XmString bodyString = NULL;
    Boolean bodyNeedsFree = False;
    Boolean bodyEllipsized = False;
    int bodyTextH = TaskSwitchBodyHeight (pSD);

    if (!pSD || !pSD->taskSwitchWin || !pSD->feedbackAppearance.fontList)
        return;

    bodyBox.x = TASK_SWITCH_MARGIN;
    bodyBox.y = TASK_SWITCH_MARGIN + pSD->taskSwitchTitleH;
    bodyBox.width = pSD->taskSwitchWidth - (2 * TASK_SWITCH_MARGIN);
    bodyBox.height = bodyTextH;

    if (bodyBox.height <= 0 || bodyBox.width <= 0)
        return;

    if (taskSwitchBackgroundGC)
    {
        XFillRectangle (DISPLAY, pSD->taskSwitchWin, taskSwitchBackgroundGC,
                        bodyBox.x, bodyBox.y,
                        bodyBox.width, bodyBox.height);
    }

    {
        int bodyIndex = pSD->taskSwitchIndex;
        if (pSD->taskSwitchHoverIndex >= 0 &&
            pSD->taskSwitchHoverIndex < pSD->taskSwitchCount)
        {
            bodyIndex = pSD->taskSwitchHoverIndex;
        }

        if (bodyIndex >= 0 &&
            bodyIndex < pSD->taskSwitchCount)
        {
            bodyString = pSD->taskSwitchTitles ?
                pSD->taskSwitchTitles[bodyIndex] : NULL;
        }

        if (!bodyString)
        {
            bodyString = XmStringCreateLocalized ("(none)");
            bodyNeedsFree = True;
        }

        {
            XmString bodyDraw = TaskSwitchEllipsize (pSD->feedbackAppearance.fontList,
                                                     bodyString, bodyBox.width,
                                                     &bodyEllipsized);
            Dimension textWidth = XmStringWidth (pSD->feedbackAppearance.fontList,
                                                 bodyDraw);
            int alignment = (textWidth >= bodyBox.width) ?
                XmALIGNMENT_BEGINNING : XmALIGNMENT_CENTER;

            XmStringDraw (DISPLAY, pSD->taskSwitchWin,
                          pSD->feedbackAppearance.fontList,
                          bodyDraw,
                          pSD->iconAppearance.inactiveGC,
                          bodyBox.x, bodyBox.y + TASK_SWITCH_TITLE_PAD,
                          bodyBox.width, alignment,
                          XmSTRING_DIRECTION_L_TO_R, &bodyBox);
            if (bodyEllipsized)
                XmStringFree (bodyDraw);
        }

        if (bodyNeedsFree)
            XmStringFree (bodyString);
    }
}

static Boolean
TaskSwitcherIconRect (WmScreenData *pSD, int index, int startX, int startY,
                      int labelH, int *frameX, int *frameY,
                      int *frameW, int *frameH, int *iconAreaH)
{
    int row;
    int col;
    int cellX;
    int cellY;
    int fX;
    int fY;
    int fW;
    int fH;
    int iconH;

    if (!pSD || pSD->taskSwitchCols <= 0 ||
        index < 0 || index >= pSD->taskSwitchCount)
        return False;

    row = index / pSD->taskSwitchCols;
    col = index % pSD->taskSwitchCols;
    cellX = startX + col * pSD->taskSwitchCellW;
    cellY = startY + row * pSD->taskSwitchCellH;
    fX = cellX + IB_MARGIN_WIDTH;
    fY = cellY + IB_MARGIN_HEIGHT;
    fW = pSD->taskSwitchIconW + ICON_GRID_EXTRA(pSD);
    fH = pSD->taskSwitchIconH;
    iconH = fH - labelH;
    if (iconH < 0)
        iconH = 0;

    if (frameX) *frameX = fX;
    if (frameY) *frameY = fY;
    if (frameW) *frameW = fW;
    if (frameH) *frameH = fH;
    if (iconAreaH) *iconAreaH = iconH;
    return True;
}

static void
PaintTaskSwitcherIcon (WmScreenData *pSD, int index, int startX, int startY,
                       int labelH, GC activeFillGC, GC inactiveFillGC,
                       GC activeBorderGC)
{
    int row;
    int col;
    int cellX;
    int cellY;
    int frameX;
    int frameY;
    int frameW;
    int frameH;
    int frameXrel;
    int frameYrel;
    Drawable target;
    Pixmap buffer = (Pixmap)0L;
    XWindowAttributes attr;
    int depth;
    GC backgroundGC;

    if (!pSD || !pSD->taskSwitchWin || index < 0 ||
        index >= pSD->taskSwitchCount)
        return;

    row = index / pSD->taskSwitchCols;
    col = index % pSD->taskSwitchCols;
    cellX = startX + col * pSD->taskSwitchCellW;
    cellY = startY + row * pSD->taskSwitchCellH;
    frameX = cellX + IB_MARGIN_WIDTH;
    frameY = cellY + IB_MARGIN_HEIGHT;
    frameW = pSD->taskSwitchIconW + ICON_GRID_EXTRA(pSD);
    frameH = pSD->taskSwitchIconH;

    frameXrel = frameX - cellX;
    frameYrel = frameY - cellY;

    depth = DefaultDepth (DISPLAY, pSD->screen);
    if (XGetWindowAttributes (DISPLAY, pSD->taskSwitchWin, &attr))
        depth = attr.depth;

    buffer = XCreatePixmap (DISPLAY, pSD->taskSwitchWin,
                            pSD->taskSwitchCellW, pSD->taskSwitchCellH,
                            (unsigned int)depth);
    if (buffer)
    {
        target = buffer;
    }
    else
    {
        target = pSD->taskSwitchWin;
    }

    backgroundGC = taskSwitchBackgroundGC ?
        taskSwitchBackgroundGC : pSD->feedbackAppearance.inactiveGC;
    XFillRectangle (DISPLAY, target, backgroundGC,
                    0, 0,
                    (unsigned int)pSD->taskSwitchCellW,
                    (unsigned int)pSD->taskSwitchCellH);

    if (index == pSD->taskSwitchIndex)
    {
        FillRoundedRect (target, activeFillGC,
                         frameXrel, frameYrel, frameW, frameH, 4);
    }
    else
    {
        FillRoundedRect (target, inactiveFillGC,
                         frameXrel, frameYrel, frameW, frameH, 4);
    }

    if (pSD->taskSwitchList && pSD->taskSwitchList[index])
    {
        DrawTaskSwitchIconPixmapScaled (target, pSD, pSD->taskSwitchList[index],
                                        frameXrel, frameYrel, frameW, frameH,
                                        labelH);
    }

    if (labelH > 0 &&
        pSD->iconAppearance.fontList &&
        pSD->taskSwitchTitles &&
        pSD->taskSwitchTitles[index])
    {
        XRectangle textBox;
        XRectangle labelBox;
        Dimension textWidth;
        int alignment;
        XmString labelDraw;
        Boolean labelEllipsized = False;
        int pad = 2;
        int labelBoxH = labelH + (2 * pad);

        labelBox.x = frameXrel + pad;
        labelBox.width = frameW - (2 * pad);
        labelBox.height = labelBoxH;
        labelBox.y = frameYrel + frameH - labelBoxH - pad;
        if (labelBox.y < frameYrel + pad)
            labelBox.y = frameYrel + pad;
        if (labelBox.width < 1)
            labelBox.width = 1;

        textBox.x = labelBox.x + pad;
        textBox.width = labelBox.width - (2 * pad);
        textBox.height = labelH;
        textBox.y = labelBox.y + pad;
        if (textBox.width < 1)
            textBox.width = 1;

        labelDraw = TaskSwitchEllipsize (pSD->iconAppearance.fontList,
                                         pSD->taskSwitchTitles[index],
                                         textBox.width,
                                         &labelEllipsized);
        textWidth = XmStringWidth (pSD->iconAppearance.fontList,
                                   labelDraw);
        alignment = (textWidth >= textBox.width) ?
            XmALIGNMENT_BEGINNING : XmALIGNMENT_CENTER;

        FillRoundedRect (target,
                         (index == pSD->taskSwitchIndex) ? activeFillGC : inactiveFillGC,
                         labelBox.x, labelBox.y,
                         labelBox.width, labelBox.height, 3);
        XmStringDraw (DISPLAY, target,
                      pSD->iconAppearance.fontList,
                      labelDraw,
                      (index == pSD->taskSwitchIndex) ? activeBorderGC : pSD->iconAppearance.inactiveGC,
                      textBox.x, textBox.y, textBox.width,
                      alignment, XmSTRING_DIRECTION_L_TO_R, &textBox);
        if (labelEllipsized)
            XmStringFree (labelDraw);
    }

    if (buffer)
    {
        XCopyArea (DISPLAY, buffer, pSD->taskSwitchWin,
                   inactiveFillGC ? inactiveFillGC : pSD->feedbackAppearance.inactiveGC,
                   0, 0,
                   (unsigned int)pSD->taskSwitchCellW,
                   (unsigned int)pSD->taskSwitchCellH,
                   cellX, cellY);
        XFreePixmap (DISPLAY, buffer);
    }
}

static void
PaintTaskSwitcherIconOnly (WmScreenData *pSD, int index, int startX, int startY,
                           int labelH, GC activeFillGC, GC inactiveFillGC)
{
    int row;
    int col;
    int cellX;
    int cellY;
    int frameX;
    int frameY;
    int frameW;
    int frameH;
    int iconAreaH;
    Drawable target;
    Pixmap buffer = (Pixmap)0L;
    XWindowAttributes attr;
    int depth;
    GC backgroundGC;

    if (!pSD || !pSD->taskSwitchWin || index < 0 ||
        index >= pSD->taskSwitchCount)
        return;

    row = index / pSD->taskSwitchCols;
    col = index % pSD->taskSwitchCols;
    cellX = startX + col * pSD->taskSwitchCellW;
    cellY = startY + row * pSD->taskSwitchCellH;
    frameX = cellX + IB_MARGIN_WIDTH;
    frameY = cellY + IB_MARGIN_HEIGHT;
    frameW = pSD->taskSwitchIconW + ICON_GRID_EXTRA(pSD);
    frameH = pSD->taskSwitchIconH;
    iconAreaH = frameH - labelH;
    if (iconAreaH <= 0)
        return;

    depth = DefaultDepth (DISPLAY, pSD->screen);
    if (XGetWindowAttributes (DISPLAY, pSD->taskSwitchWin, &attr))
        depth = attr.depth;

    buffer = XCreatePixmap (DISPLAY, pSD->taskSwitchWin,
                            (unsigned int)frameW, (unsigned int)iconAreaH,
                            (unsigned int)depth);
    if (buffer)
        target = buffer;
    else
        target = pSD->taskSwitchWin;

    backgroundGC = (index == pSD->taskSwitchIndex) ? activeFillGC : inactiveFillGC;
    if (!backgroundGC)
        backgroundGC = pSD->iconAppearance.inactiveGC;
    XFillRectangle (DISPLAY, target, backgroundGC, 0, 0,
                    (unsigned int)frameW, (unsigned int)iconAreaH);

    if (pSD->taskSwitchList && pSD->taskSwitchList[index])
    {
        DrawTaskSwitchIconPixmapScaled (target, pSD, pSD->taskSwitchList[index],
                                        0, 0, frameW, iconAreaH, 0);
    }

    if (buffer)
    {
        XCopyArea (DISPLAY, buffer, pSD->taskSwitchWin,
                   inactiveFillGC ? inactiveFillGC : pSD->feedbackAppearance.inactiveGC,
                   0, 0, (unsigned int)frameW, (unsigned int)iconAreaH,
                   frameX, frameY);
        XFreePixmap (DISPLAY, buffer);
    }
}

void
TaskSwitcherIconUpdated (WmScreenData *pSD, ClientData *pCD)
{
    int i;
    int startX;
    int startY;
    int labelH = 0;
    int frameX;
    int frameY;
    int frameW;
    int frameH;
    int iconAreaH;
    Boolean labelChanged = False;
    XmString newTitle = NULL;

    if (!pSD || !pCD || !TaskSwitcherActive (pSD))
        return;
    if (!pSD->taskSwitchWin || !pSD->taskSwitchList || pSD->taskSwitchCount <= 0)
        return;

    for (i = 0; i < pSD->taskSwitchCount; i++)
    {
        if (pSD->taskSwitchList[i] == pCD)
            break;
    }
    if (i >= pSD->taskSwitchCount)
        return;

    TaskSwitcherComputeGeometry (pSD, &startX, &startY, &labelH, NULL);
    if (!TaskSwitcherIconRect (pSD, i, startX, startY, labelH,
                               &frameX, &frameY, &frameW, &frameH, &iconAreaH))
        return;
    if (iconAreaH <= 0 || frameW <= 0)
        return;

    if (pSD->taskSwitchTitles)
    {
        if (CLIENT_DISPLAY_TITLE(pCD))
            newTitle = XmStringCopy (CLIENT_DISPLAY_TITLE(pCD));
        else if (ICON_DISPLAY_TITLE(pCD))
            newTitle = XmStringCopy (ICON_DISPLAY_TITLE(pCD));
        else
            newTitle = XmStringCreateLocalized ("(untitled)");

        if (newTitle)
        {
            if (!pSD->taskSwitchTitles[i] ||
                !XmStringCompare (pSD->taskSwitchTitles[i], newTitle))
            {
                if (pSD->taskSwitchTitles[i])
                    XmStringFree (pSD->taskSwitchTitles[i]);
                pSD->taskSwitchTitles[i] = newTitle;
                labelChanged = True;
            }
            else
            {
                XmStringFree (newTitle);
            }
        }
    }

    {
        XEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.xexpose.type = Expose;
        ev.xexpose.display = DISPLAY;
        ev.xexpose.window = pSD->taskSwitchWin;
        ev.xexpose.x = frameX;
        ev.xexpose.y = frameY;
        ev.xexpose.width = (unsigned int)frameW;
        ev.xexpose.height = (unsigned int)iconAreaH;
        ev.xexpose.count = 0;
        XSendEvent (DISPLAY, pSD->taskSwitchWin, False, ExposureMask, &ev);
    }

    if (labelChanged && labelH > 0)
    {
        int pad = 2;
        int labelBoxH = labelH + (2 * pad);
        int labelY = frameY + frameH - labelBoxH - pad;
        XEvent ev;

        if (labelY < frameY + pad)
            labelY = frameY + pad;

        memset(&ev, 0, sizeof(ev));
        ev.xexpose.type = Expose;
        ev.xexpose.display = DISPLAY;
        ev.xexpose.window = pSD->taskSwitchWin;
        ev.xexpose.x = frameX;
        ev.xexpose.y = labelY;
        ev.xexpose.width = (unsigned int)frameW;
        ev.xexpose.height = (unsigned int)labelBoxH;
        ev.xexpose.count = 0;
        XSendEvent (DISPLAY, pSD->taskSwitchWin, False, ExposureMask, &ev);
    }
}

void
HandleTaskSwitcherExpose (WmScreenData *pSD, XExposeEvent *event)
{
    int startX;
    int startY;
    int labelH;
    int bodyTextH;
    int gridTop;
    int i;
    GC activeBorderGC;
    GC activeFillGC;
    GC inactiveFillGC;

    if (!pSD || !event)
        return;

    if (event->count > 0)
        return;

    TaskSwitcherComputeGeometry (pSD, &startX, &startY, &labelH, &bodyTextH);
    gridTop = startY;

    if (event->y < gridTop)
    {
        int bodyTop = TASK_SWITCH_MARGIN + pSD->taskSwitchTitleH;
        int bodyBottom = bodyTop + TaskSwitchBodyHeight (pSD);
        int eventBottom = event->y + event->height;

        if (event->y >= bodyTop && event->y < bodyBottom)
        {
            PaintTaskSwitcherBody (pSD);
            return;
        }

        if (eventBottom > bodyTop && event->y < bodyBottom)
        {
            PaintTaskSwitcherBody (pSD);
            return;
        }

        PaintTaskSwitcher (pSD);
        return;
    }

    EnsureTaskSwitchBorderGCs (pSD);
    activeBorderGC = taskSwitchActiveBorderGC ?
        taskSwitchActiveBorderGC : pSD->feedbackAppearance.inactiveGC;
    activeFillGC = taskSwitchActiveFillGC ?
        taskSwitchActiveFillGC : pSD->iconAppearance.inactiveGC;
    inactiveFillGC = taskSwitchInactiveFillGC ?
        taskSwitchInactiveFillGC : pSD->iconAppearance.inactiveGC;

    for (i = 0; i < pSD->taskSwitchCount; i++)
    {
        int frameX;
        int frameY;
        int frameW;
        int frameH;
        int iconAreaH;
        int labelTop;
        int rx1, ry1, rx2, ry2;
        int ix1, iy1, ix2, iy2;

        if (!TaskSwitcherIconRect (pSD, i, startX, startY, labelH,
                                   &frameX, &frameY, &frameW, &frameH, &iconAreaH))
            continue;

        rx1 = event->x;
        ry1 = event->y;
        rx2 = event->x + event->width;
        ry2 = event->y + event->height;

        ix1 = frameX;
        iy1 = frameY;
        ix2 = frameX + frameW;
        iy2 = frameY + frameH;

        if (rx2 <= ix1 || rx1 >= ix2 || ry2 <= iy1 || ry1 >= iy2)
            continue;

        labelTop = frameY + iconAreaH;
        if (ry1 >= labelTop || ry2 > labelTop)
        {
            PaintTaskSwitcherIcon (pSD, i, startX, startY, labelH,
                                   activeFillGC, inactiveFillGC, activeBorderGC);
        }
        else
        {
            PaintTaskSwitcherIconOnly (pSD, i, startX, startY, labelH,
                                       activeFillGC, inactiveFillGC);
        }
    }
}

void
StartTaskSwitcher (WmScreenData *pSD, Time time, int direction)
{
    int i;
    int currentIndex = -1;
    ClientData *current = wmGD.keyboardFocus;
    Window rootRet;
    Window childRet;
    int rootX, rootY;
    int winX, winY;
    unsigned int mask = 0;

    if (!pSD || !pSD->showTaskSwitcher)
    {
        TaskSwitcherLog("StartTaskSwitcher ignored showTaskSwitcher=%d pSD=%p",
                        pSD ? pSD->showTaskSwitcher : 0, (void *)pSD);
        XAllowEvents (DISPLAY, AsyncKeyboard, time);
        return;
    }
    TaskSwitcherLog("StartTaskSwitcher dir=%d time=%lu active=%d",
                    direction, (unsigned long)time, pSD->taskSwitchActive);

    if (!pSD->taskSwitchActive)
    {
        TaskSwitcherLog("StartTaskSwitcher inactive -> build list");
        FreeTaskSwitchList (pSD);
        BuildTaskSwitchList (pSD);

        if (pSD->taskSwitchCount > 0)
        {
            for (i = 0; i < pSD->taskSwitchCount; i++)
            {
                if (pSD->taskSwitchList[i] == current)
                {
                    currentIndex = i;
                    break;
                }
            }

            if (currentIndex < 0)
                currentIndex = 0;

            pSD->taskSwitchIndex =
                (currentIndex + direction + pSD->taskSwitchCount) %
                pSD->taskSwitchCount;
        }
        else
        {
            pSD->taskSwitchIndex = -1;
        }

        ComputeTaskSwitchLayout (pSD);
        TaskSwitcherLog("StartTaskSwitcher layout done");
        if (pSD->taskSwitchWin)
        {
            TaskSwitcherLog("StartTaskSwitcher destroying old window %lu",
                            (unsigned long)pSD->taskSwitchWin);
            XDestroyWindow (DISPLAY, pSD->taskSwitchWin);
            pSD->taskSwitchWin = (Window)0L;
        }
        EnsureTaskSwitchWindow (pSD);
        if (!pSD->taskSwitchWin)
        {
            TaskSwitcherLog("StartTaskSwitcher failed to create window");
            XAllowEvents (DISPLAY, AsyncKeyboard, time);
            return;
        }

    pSD->taskSwitchActive = True;
    TaskSwitcherLog("StartTaskSwitcher mapped win=%lu index=%d count=%d",
                    (unsigned long)pSD->taskSwitchWin,
                    pSD->taskSwitchIndex, pSD->taskSwitchCount);
    TaskSwitcherLog("StartTaskSwitcher XRaiseWindow");
    XRaiseWindow (DISPLAY, pSD->taskSwitchWin);
    XMapWindow (DISPLAY, pSD->taskSwitchWin);
    TaskSwitcherLog("StartTaskSwitcher XMapWindow");
    PaintTaskSwitcher (pSD);
    TaskSwitcherLog("StartTaskSwitcher PaintTaskSwitcher done");

    TaskSwitcherLog("StartTaskSwitcher XAllowEvents");
    XAllowEvents (DISPLAY, AsyncKeyboard, time);

    if (XQueryPointer (DISPLAY, pSD->rootWindow, &rootRet, &childRet,
                       &rootX, &rootY, &winX, &winY, &mask))
    {
        if (!(mask & Mod1Mask))
        {
            TaskSwitcherLog("StartTaskSwitcher mod1 up immediately -> finish");
            FinishTaskSwitcher (pSD, time, False);
            return;
        }
    }
    else
    {
        TaskSwitcherLog("StartTaskSwitcher XQueryPointer failed -> finish");
        FinishTaskSwitcher (pSD, time, False);
        return;
    }

    if (!taskSwitchTimer)
    {
        taskSwitchTimer = XtAppAddTimeOut (
            XtDisplayToApplicationContext (DISPLAY),
            TASK_SWITCH_TIMEOUT_MS, TaskSwitcherTimeout, (XtPointer)pSD);
        TaskSwitcherLog("StartTaskSwitcher timer started id=%lu",
                        (unsigned long)taskSwitchTimer);
    }
    }
    else
    {
        TaskSwitcherLog("StartTaskSwitcher already active -> advance");
        if (pSD->taskSwitchPinned)
            taskSwitchPinnedAlt = True;
        AdvanceTaskSwitcher (pSD, direction);
        XAllowEvents (DISPLAY, AsyncKeyboard, time);
    }
}

void
AdvanceTaskSwitcher (WmScreenData *pSD, int direction)
{
    int oldIndex = -1;
    if (!pSD || !pSD->taskSwitchActive)
        return;
    if (taskSwitchDragging && taskSwitchDragSD == pSD)
    {
        taskSwitchDragging = False;
        taskSwitchDragSD = NULL;
        XUngrabPointer (DISPLAY, CurrentTime);
        TaskSwitcherLog("AdvanceTaskSwitcher drag cancelled");
        TaskSwitcherEnsureCursors ();
        TaskSwitcherSetCursor (pSD, taskSwitchCursorNormal);
    }
    if (pSD->taskSwitchCount > 0)
    {
        oldIndex = pSD->taskSwitchIndex;
        pSD->taskSwitchIndex =
            (pSD->taskSwitchIndex + direction + pSD->taskSwitchCount) %
            pSD->taskSwitchCount;
    }
    TaskSwitcherLog("AdvanceTaskSwitcher dir=%d index=%d", direction, pSD->taskSwitchIndex);
    if (pSD->taskSwitchWin && pSD->taskSwitchCount > 0)
    {
        int startX;
        int startY;
        int labelH;
        int frameX;
        int frameY;
        int frameW;
        int frameH;
        int iconAreaH;
        XEvent ev;

        TaskSwitcherComputeGeometry (pSD, &startX, &startY, &labelH, NULL);

        if (oldIndex >= 0 &&
            TaskSwitcherIconRect (pSD, oldIndex, startX, startY, labelH,
                                  &frameX, &frameY, &frameW, &frameH, &iconAreaH))
        {
            memset(&ev, 0, sizeof(ev));
            ev.xexpose.type = Expose;
            ev.xexpose.display = DISPLAY;
            ev.xexpose.window = pSD->taskSwitchWin;
            ev.xexpose.x = frameX;
            ev.xexpose.y = frameY;
            ev.xexpose.width = (unsigned int)frameW;
            ev.xexpose.height = (unsigned int)frameH;
            ev.xexpose.count = 0;
            XSendEvent (DISPLAY, pSD->taskSwitchWin, False, ExposureMask, &ev);
        }

        if (TaskSwitcherIconRect (pSD, pSD->taskSwitchIndex, startX, startY, labelH,
                                  &frameX, &frameY, &frameW, &frameH, &iconAreaH))
        {
            memset(&ev, 0, sizeof(ev));
            ev.xexpose.type = Expose;
            ev.xexpose.display = DISPLAY;
            ev.xexpose.window = pSD->taskSwitchWin;
            ev.xexpose.x = frameX;
            ev.xexpose.y = frameY;
            ev.xexpose.width = (unsigned int)frameW;
            ev.xexpose.height = (unsigned int)frameH;
            ev.xexpose.count = 0;
            XSendEvent (DISPLAY, pSD->taskSwitchWin, False, ExposureMask, &ev);
        }
    }

    if (pSD->taskSwitchPinned && !taskSwitchTimer)
    {
        taskSwitchTimer = XtAppAddTimeOut (
            XtDisplayToApplicationContext (DISPLAY),
            TASK_SWITCH_TIMEOUT_MS, TaskSwitcherTimeout, (XtPointer)pSD);
        TaskSwitcherLog("AdvanceTaskSwitcher timer started id=%lu",
                        (unsigned long)taskSwitchTimer);
    }
}

void
FinishTaskSwitcher (WmScreenData *pSD, Time time, Boolean activate)
{
    ClientData *pCD = NULL;

    if (!pSD || !pSD->taskSwitchActive)
        return;
    if (activate &&
        pSD->taskSwitchIndex >= 0 &&
        pSD->taskSwitchIndex < pSD->taskSwitchCount)
    {
        pCD = pSD->taskSwitchList[pSD->taskSwitchIndex];
    }

    TaskSwitcherLog("FinishTaskSwitcher begin activate=%d", activate);
    if (taskSwitchTimer)
    {
        XtRemoveTimeOut (taskSwitchTimer);
        taskSwitchTimer = (XtIntervalId)0;
        TaskSwitcherLog("FinishTaskSwitcher timer removed");
    }
    taskSwitchPinnedAlt = False;

    XUngrabKeyboard (DISPLAY, time);
    XUngrabPointer (DISPLAY, time);
    TaskSwitcherLog("FinishTaskSwitcher ungrabbed");

    if (taskSwitchDragging && taskSwitchDragSD == pSD)
    {
        taskSwitchDragging = False;
        taskSwitchDragSD = NULL;
    }

    if (pSD->taskSwitchWin)
    {
        XUnmapWindow (DISPLAY, pSD->taskSwitchWin);
        XDestroyWindow (DISPLAY, pSD->taskSwitchWin);
        pSD->taskSwitchWin = (Window)0L;
        TaskSwitcherLog("FinishTaskSwitcher destroyed window");
    }

    pSD->taskSwitchActive = False;
    pSD->taskSwitchPinned = False;
    FreeTaskSwitchList (pSD);
    pSD->taskSwitchIndex = -1;
    pSD->taskSwitchCols = 0;
    pSD->taskSwitchRows = 0;
    pSD->taskSwitchCellW = 0;
    pSD->taskSwitchCellH = 0;
    pSD->taskSwitchIconW = 0;
    pSD->taskSwitchIconH = 0;
    pSD->taskSwitchTitleH = 0;
    pSD->taskSwitchHoverIndex = -1;
    pSD->taskSwitchX = 0;
    pSD->taskSwitchY = 0;
    pSD->taskSwitchWidth = 0;
    pSD->taskSwitchHeight = 0;

    TaskSwitcherLog("FinishTaskSwitcher activate=%d focus=%p",
                    activate, (void *)pCD);

    if (activate && pCD)
    {
        TaskSwitcherActivateSelection (pSD, pCD, time);
        TaskSwitcherLog("FinishTaskSwitcher activate selection done");
    }
}

Boolean
HandleTaskSwitcherButtonPress (WmScreenData *pSD, XButtonEvent *event)
{
    int startX;
    int startY;
    int bodyTextH;
    int labelH = 0;
    int i;
    Boolean onIcon = False;

    if (!pSD || !pSD->taskSwitchActive || !event)
        return False;
    TaskSwitcherLog("HandleTaskSwitcherButtonPress x=%d y=%d button=%u",
                    event->x, event->y, event->button);

    if (pSD->iconAppearance.font)
        labelH = TEXT_HEIGHT (pSD->iconAppearance.font);

    bodyTextH = TaskSwitchBodyHeight (pSD);
    startX = TASK_SWITCH_MARGIN;
    startY = TASK_SWITCH_MARGIN + pSD->taskSwitchTitleH + bodyTextH +
        TASK_SWITCH_TEXT_GAP;

    if (pSD->taskSwitchCount > 0)
    {
        for (i = 0; i < pSD->taskSwitchCount; i++)
        {
            int row = i / pSD->taskSwitchCols;
            int col = i % pSD->taskSwitchCols;
            int cellX = startX + col * pSD->taskSwitchCellW;
            int cellY = startY + row * pSD->taskSwitchCellH;
            int frameX = cellX + IB_MARGIN_WIDTH;
            int frameY = cellY + IB_MARGIN_HEIGHT;
            int frameW = pSD->taskSwitchIconW + ICON_GRID_EXTRA(pSD);
            int frameH = pSD->taskSwitchIconH;

            if (event->x >= frameX && event->x < (frameX + frameW) &&
                event->y >= frameY && event->y < (frameY + frameH))
            {
                onIcon = True;
                break;
            }
        }
    }

    if (!onIcon)
    {
        TaskSwitcherEnsureCursors ();
        TaskSwitcherSetCursor (pSD, taskSwitchCursorHand);
        pSD->taskSwitchPinned = True;
        TaskSwitcherLog("HandleTaskSwitcherButtonPress pinned");
        if (taskSwitchTimer)
        {
            XtRemoveTimeOut (taskSwitchTimer);
            taskSwitchTimer = (XtIntervalId)0;
            TaskSwitcherLog("HandleTaskSwitcherButtonPress timer removed");
        }

        XSetWindowBorderWidth (DISPLAY, pSD->taskSwitchWin, 2);
        XSetWindowBorder (DISPLAY, pSD->taskSwitchWin,
                          pSD->feedbackAppearance.activeForeground);
        PaintTaskSwitcher (pSD);

        if (event->button == Button1)
        {
            taskSwitchDragging = True;
            taskSwitchDragSD = pSD;
            taskSwitchDragStartX = event->x_root;
            taskSwitchDragStartY = event->y_root;
            taskSwitchDragWinX = pSD->taskSwitchX;
            taskSwitchDragWinY = pSD->taskSwitchY;
            TaskSwitcherEnsureCursors ();
            TaskSwitcherSetCursor (pSD, taskSwitchCursorDrag);
            XGrabPointer (DISPLAY, pSD->taskSwitchWin, False,
                          ButtonReleaseMask | PointerMotionMask,
                          GrabModeAsync, GrabModeAsync, None, None,
                          event->time);
        }
    }
    else
    {
        TaskSwitcherEnsureCursors ();
        TaskSwitcherSetCursor (pSD, taskSwitchCursorHand);
        if (event->button == Button1 && !pSD->taskSwitchPinned)
        {
            pSD->taskSwitchIndex = i;
            TaskSwitcherLog("HandleTaskSwitcherButtonPress activate index=%d", i);
            if (taskSwitchDragging && taskSwitchDragSD == pSD)
            {
                taskSwitchDragging = False;
                taskSwitchDragSD = NULL;
                XUngrabPointer (DISPLAY, event->time);
            }
            FinishTaskSwitcher (pSD, event->time, True);
            return True;
        }
        if (event->button == Button1 && pSD->taskSwitchPinned)
        {
            pSD->taskSwitchIndex = i;
            TaskSwitcherLog("HandleTaskSwitcherButtonPress pinned activate index=%d", i);
            if (taskSwitchDragging && taskSwitchDragSD == pSD)
            {
                taskSwitchDragging = False;
                taskSwitchDragSD = NULL;
                XUngrabPointer (DISPLAY, event->time);
            }
            FinishTaskSwitcher (pSD, event->time, True);
            return True;
        }
    }

    return True;
}

Boolean
HandleTaskSwitcherButtonRelease (WmScreenData *pSD, XButtonEvent *event)
{
    if (!pSD || !event)
        return False;

    if (taskSwitchDragging && taskSwitchDragSD == pSD &&
        event->button == Button1)
    {
        taskSwitchDragging = False;
        taskSwitchDragSD = NULL;
        XUngrabPointer (DISPLAY, event->time);
        TaskSwitcherLog("HandleTaskSwitcherButtonRelease drag end");
        TaskSwitcherEnsureCursors ();
        TaskSwitcherSetCursor (pSD, taskSwitchCursorNormal);
        return True;
    }

    return False;
}

Boolean
HandleTaskSwitcherMotion (WmScreenData *pSD, XMotionEvent *event)
{
    int startX;
    int startY;
    int bodyTextH;
    int labelH = 0;
    int i;
    Boolean onIcon = False;

    if (!pSD || !event)
        return False;

    if (taskSwitchDragging && taskSwitchDragSD == pSD)
    {
        int dx = event->x_root - taskSwitchDragStartX;
        int dy = event->y_root - taskSwitchDragStartY;
        int newX = taskSwitchDragWinX + dx;
        int newY = taskSwitchDragWinY + dy;
        int screenW = DisplayWidth (DISPLAY, pSD->screen);
        int screenH = DisplayHeight (DISPLAY, pSD->screen);
        int maxX = screenW - (int)pSD->taskSwitchWidth;
        int maxY = screenH - (int)pSD->taskSwitchHeight;

        if (newX < 0) newX = 0;
        if (newY < 0) newY = 0;
        if (newX > maxX) newX = maxX;
        if (newY > maxY) newY = maxY;

        pSD->taskSwitchX = newX;
        pSD->taskSwitchY = newY;
        XMoveWindow (DISPLAY, pSD->taskSwitchWin, newX, newY);
        return True;
    }

    if (pSD->taskSwitchCount > 0)
    {
        int hoverIndex = -1;
        if (pSD->iconAppearance.font)
            labelH = TEXT_HEIGHT (pSD->iconAppearance.font);

        bodyTextH = TaskSwitchBodyHeight (pSD);
        startX = TASK_SWITCH_MARGIN;
        startY = TASK_SWITCH_MARGIN + pSD->taskSwitchTitleH + bodyTextH +
            TASK_SWITCH_TEXT_GAP;

        for (i = 0; i < pSD->taskSwitchCount; i++)
        {
            int row = i / pSD->taskSwitchCols;
            int col = i % pSD->taskSwitchCols;
            int cellX = startX + col * pSD->taskSwitchCellW;
            int cellY = startY + row * pSD->taskSwitchCellH;
            int frameX = cellX + IB_MARGIN_WIDTH;
            int frameY = cellY + IB_MARGIN_HEIGHT;
            int frameW = pSD->taskSwitchIconW + ICON_GRID_EXTRA(pSD);
            int frameH = pSD->taskSwitchIconH;

            if (event->x >= frameX && event->x < (frameX + frameW) &&
                event->y >= frameY && event->y < (frameY + frameH))
            {
                onIcon = True;
                hoverIndex = i;
                break;
            }
        }

        if (hoverIndex != pSD->taskSwitchHoverIndex)
        {
            pSD->taskSwitchHoverIndex = hoverIndex;
            if (pSD->taskSwitchWin)
            {
                XEvent ev;
                memset(&ev, 0, sizeof(ev));
                ev.xexpose.type = Expose;
                ev.xexpose.display = DISPLAY;
                ev.xexpose.window = pSD->taskSwitchWin;
                ev.xexpose.x = TASK_SWITCH_MARGIN;
                ev.xexpose.y = TASK_SWITCH_MARGIN + pSD->taskSwitchTitleH;
                ev.xexpose.width = pSD->taskSwitchWidth - (2 * TASK_SWITCH_MARGIN);
                ev.xexpose.height = TaskSwitchBodyHeight (pSD);
                ev.xexpose.count = 0;
                XSendEvent (DISPLAY, pSD->taskSwitchWin, False, ExposureMask, &ev);
            }
        }
    }

    TaskSwitcherEnsureCursors ();
    TaskSwitcherSetCursor (pSD, onIcon ? taskSwitchCursorHand : taskSwitchCursorNormal);

    return False;
}
