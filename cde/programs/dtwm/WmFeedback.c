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
#include <Xm/Label.h>
#include <Xm/DialogS.h>
#include <Xm/BulletinB.h>
#include <Xm/MessageB.h>

#include <Dt/HourGlass.h>
#include <math.h>

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

enum {
    FB_SNAP_NONE = 0,
    FB_SNAP_HALF_LEFT,
    FB_SNAP_HALF_RIGHT,
    FB_SNAP_HALF_TOP,
    FB_SNAP_HALF_BOTTOM,
    FB_SNAP_QUAD_TL,
    FB_SNAP_QUAD_TR,
    FB_SNAP_QUAD_BL,
    FB_SNAP_QUAD_BR
};

/*
 * include extern functions
 */
#include "WmFeedback.h"
#include "WmFunction.h"
#include "WmGraphics.h"
#include "WmPanelP.h"  /* for typedef in WmManage.h */
#include "WmManage.h"
#include "WmColormap.h"
#include "stdio.h"


/*
 * Global Variables:
 */
static Cursor  waitCursor = (Cursor)0L;

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

	if (pSD->iconPlacement & ICON_PLACE_LEFT_PRIMARY)
	{
	    left = iconReserveW;
	}
	if (pSD->iconPlacement & ICON_PLACE_RIGHT_PRIMARY)
	{
	    right = iconReserveW;
	}
	if (pSD->iconPlacement & ICON_PLACE_TOP_PRIMARY)
	{
	    top = iconReserveH;
	}
	if (pSD->iconPlacement & ICON_PLACE_BOTTOM_PRIMARY)
	{
	    bottom = iconReserveH;
	}

	if (pSD->wPanelist)
	{
	    Widget panelShell = XtParent (pSD->wPanelist);
	    if (panelShell && XtIsRealized (panelShell) && XtIsManaged (panelShell) &&
		(XtScreen (panelShell) == ScreenOfDisplay (DISPLAY, pSD->screen)))
	    {
		int px = XtX (panelShell);
		int py = XtY (panelShell);
		int pw = (int)XtWidth (panelShell);
		int ph = (int)XtHeight (panelShell);
		int distLeft = px;
		int distRight = screenW - (px + pw);
		int distTop = py;
		int distBottom = screenH - (py + ph);
		int minDist = distLeft;

		if (distRight < minDist) minDist = distRight;
		if (distTop < minDist) minDist = distTop;
		if (distBottom < minDist) minDist = distBottom;

		if (minDist == distTop)
		{
		    if (top < ph) top = ph;
		}
		else if (minDist == distBottom)
		{
		    if (bottom < ph) bottom = ph;
		}
		else if (minDist == distLeft)
		{
		    if (left < pw) left = pw;
		}
		else
		{
		    if (right < pw) right = pw;
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
	 (4 * cellW + 3 * FB_SNAP_CELL_GAP)) / 2;

    for (i = 0; i < 4; i++)
    {
	int x = rowX + i * (cellW + FB_SNAP_CELL_GAP);
	int y = row1Y;
	if (localX >= x && localX < (x + cellW) &&
	    localY >= y && localY < (y + cellH))
	{
	    switch (i)
	    {
		case 0: return FB_SNAP_HALF_LEFT;
		case 1: return FB_SNAP_HALF_RIGHT;
		case 2: return FB_SNAP_HALF_TOP;
		case 3: return FB_SNAP_HALF_BOTTOM;
	    }
	}
    }

    for (i = 0; i < 4; i++)
    {
	int x = rowX + i * (cellW + FB_SNAP_CELL_GAP);
	int y = row2Y;
	if (localX >= x && localX < (x + cellW) &&
	    localY >= y && localY < (y + cellH))
	{
	    switch (i)
	    {
		case 0: return FB_SNAP_QUAD_TL;
		case 1: return FB_SNAP_QUAD_TR;
		case 2: return FB_SNAP_QUAD_BL;
		case 3: return FB_SNAP_QUAD_BR;
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
		rowW = 4 * cellW + 3 * FB_SNAP_CELL_GAP;
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
		 (4 * cellW + 3 * FB_SNAP_CELL_GAP)) / 2;

	    for (i = 0; i < 4; i++)
	    {
		int x = rowX + i * (cellW + FB_SNAP_CELL_GAP);
		int y = row1Y;
		int zone = FB_SNAP_HALF_LEFT;
		switch (i)
		{
		    case 0: zone = FB_SNAP_HALF_LEFT; break;
		    case 1: zone = FB_SNAP_HALF_RIGHT; break;
		    case 2: zone = FB_SNAP_HALF_TOP; break;
		    case 3: zone = FB_SNAP_HALF_BOTTOM; break;
		}
		DrawSunkenRect (pSD, x, y, (unsigned int)cellW,
				(unsigned int)cellH);
		FillSnapRect (pSD, x, y, (unsigned int)cellW,
			      (unsigned int)cellH, zone);
	    }

	    for (i = 0; i < 4; i++)
	    {
		int x = rowX + i * (cellW + FB_SNAP_CELL_GAP);
		int y = row2Y;
		int zone = FB_SNAP_QUAD_TL;
		switch (i)
		{
		    case 0: zone = FB_SNAP_QUAD_TL; break;
		    case 1: zone = FB_SNAP_QUAD_TR; break;
		    case 2: zone = FB_SNAP_QUAD_BL; break;
		    case 3: zone = FB_SNAP_QUAD_BR; break;
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
