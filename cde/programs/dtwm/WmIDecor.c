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
#include <Xm/Xm.h>
#include <Xm/DrawP.h>   	/* for XmeClearBorder */
/*
 * include extern functions
 */
#include "WmIDecor.h"
#include "WmError.h"
#include "WmGraphics.h"
#include "WmIconBox.h"
#include "WmMenu.h"
#include "WmWinInfo.h"
#include "WmWrkspace.h"



/*
 * Global Variables:
 */
static unsigned int activeIconTextWidth = 1;
static unsigned int activeIconTextHeight = 1;
static RList *pActiveIconTopRects = NULL;
static RList *pActiveIconBotRects = NULL;
static const unsigned long kMinimizeBlinkIntervalMs = 320;
static const int kMinimizeBlinkToggles = 2;

typedef struct _IconBlinkData
{
    ClientData *pCD;
    int remainingToggles;
    Boolean showing;
    XtIntervalId timerId;
    unsigned int restoreWidth;
    unsigned int restoreHeight;
    Boolean resized;
    struct _IconBlinkData *next;
} IconBlinkData;

static IconBlinkData *iconBlinkList = NULL;


static GC GetOpenIconFillGC (ClientData *pcd)
{
    WmScreenData *pSD = pcd->pSD;
    Pixel color = ICON_APPEARANCE(pcd).activeBackground;
    Pixmap pixmap = ICON_APPEARANCE(pcd).activeBackgroundPixmap;

    /* Prefer palette highlight/select color when available. */
    if (pSD->pSecondaryPixelSet)
    {
	color = pSD->pSecondaryPixelSet->sc;
	pixmap = (Pixmap)NULL;
    }

    if (!pSD->openIconFillGC ||
	pSD->openIconFillColor != color ||
	pSD->openIconFillPixmap != pixmap)
    {
	XGCValues values;
	XtGCMask mask = GCForeground | GCBackground;

	if (pSD->openIconFillGC)
	{
	    XFreeGC (DISPLAY, pSD->openIconFillGC);
	    pSD->openIconFillGC = NULL;
	}

	values.foreground = color;
	values.background = ICON_APPEARANCE(pcd).background;
	pSD->openIconFillGC = XCreateGC (DISPLAY, pSD->rootWindow, mask, &values);

	if (pixmap != (Pixmap)NULL)
	{
	    XSetTile (DISPLAY, pSD->openIconFillGC, pixmap);
	    XSetFillStyle (DISPLAY, pSD->openIconFillGC, FillTiled);
	}
	else
	{
	    XSetFillStyle (DISPLAY, pSD->openIconFillGC, FillSolid);
	}

	pSD->openIconFillColor = color;
	pSD->openIconFillPixmap = pixmap;
    }

    return pSD->openIconFillGC;
}

static GC GetOpenIconBarGC (ClientData *pcd, Boolean active)
{
    WmScreenData *pSD = pcd->pSD;
    Pixel color = active ? pSD->clientAppearance.activeBackground
			 : pSD->clientAppearance.background;
    GC *gcPtr = active ? &pSD->openIconBarActiveGC
		       : &pSD->openIconBarInactiveGC;
    Pixel *colorPtr = active ? &pSD->openIconBarActiveColor
			     : &pSD->openIconBarInactiveColor;

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
	values.background = ICON_APPEARANCE(pcd).background;
	*gcPtr = XCreateGC (DISPLAY, pSD->rootWindow, mask, &values);
	*colorPtr = color;
    }

    return *gcPtr;
}

static Boolean OpenIconIndicatorShouldShow (ClientData *pcd, Boolean ignoreState)
{
    if (!pcd || !ICON_FRAME_WIN(pcd))
    {
	return False;
    }

    if (wmGD.movingIcon && (wmGD.movingIconClient == pcd))
    {
	return False;
    }

    if (!ignoreState)
    {
	if (!pcd->pSD->showOpenWindowIcons ||
	    (pcd->clientState == MINIMIZED_STATE))
	{
	    return False;
	}
    }

    return True;
}

static Window CreateOpenIconIndicatorWindow (ClientData *pcd)
{
    Window parent;
    Window root;
    Window *children = NULL;
    unsigned int nchildren = 0;
    XSetWindowAttributes attrs;
    unsigned long mask;
    Window win;

    if (!ICON_FRAME_WIN(pcd))
    {
	return None;
    }

    if (!XQueryTree (DISPLAY, ICON_FRAME_WIN(pcd),
		     &root, &parent, &children, &nchildren))
    {
	return None;
    }
    if (children)
    {
	XFree (children);
    }

    mask = CWEventMask | CWCursor | CWBackPixel;
    attrs.event_mask = ButtonPressMask | ButtonReleaseMask |
		       ButtonMotionMask | ExposureMask;
    attrs.cursor = wmGD.workspaceCursor;
    attrs.background_pixel = ICON_APPEARANCE(pcd).background;

    win = XCreateWindow (DISPLAY, parent,
			 0, 0, 1, 1,
			 0, CopyFromParent, InputOutput,
			 CopyFromParent, mask, &attrs);
    if (win)
    {
	XSaveContext (DISPLAY, win, wmGD.windowContextType, (caddr_t)pcd);
	if (pcd->iconWindow)
	{
	    XGrabButton (DISPLAY, AnyButton, AnyModifier, win, True,
			 ButtonPressMask|ButtonReleaseMask|
			     ButtonMotionMask,
			 GrabModeAsync, GrabModeAsync, None,
			 wmGD.workspaceCursor);
	}
    }

    return win;
}

static Boolean GetOpenIconIndicatorGeometry (ClientData *pcd, int *x, int *y,
					     unsigned int *w, unsigned int *h)
{
    XWindowAttributes attr;
    int xOffset = 0;
    int yOffset = 0;

    if (!ICON_FRAME_WIN(pcd) ||
	!XGetWindowAttributes (DISPLAY, ICON_FRAME_WIN(pcd), &attr))
    {
	return False;
    }

    if (P_ICON_BOX(pcd))
    {
	xOffset = IB_MARGIN_WIDTH;
	yOffset = IB_MARGIN_HEIGHT;
    }

    *x = attr.x + xOffset + ICON_WIDTH(pcd) + OPEN_ICON_BAR_GAP;
    *w = OPEN_ICON_BAR_WIDTH;
    *h = (unsigned int)((ICON_HEIGHT(pcd) * 3) / 8);
    *y = attr.y + yOffset + ((int)ICON_HEIGHT(pcd) - (int)(*h)) / 2;

    return True;
}

static Boolean UseSharedIconWindows (ClientData *pcd)
{
    if (!pcd)
    {
	return False;
    }

    if (!pcd->pSD->useIconBox)
    {
	return True;
    }

    if (pcd->clientFlags & (CLIENT_WM_CLIENTS | FRONT_PANEL_BOX))
    {
	return True;
    }

    return False;
}

static int OpenIconExtraWidth (ClientData *pcd)
{
    return ICON_OPEN_EXTRA(pcd);
}

static int IconContentWidth (ClientData *pcd)
{
    return ICON_WIDTH(pcd);
}

static void DrawOpenIconBarInternal (ClientData *pcd, int xOffset, int yOffset,
                                     unsigned int openWidth, Boolean ignoreState,
                                     int activeOverride)
{
    (void)xOffset;
    (void)yOffset;
    (void)openWidth;
    UpdateOpenIconIndicator(pcd, ignoreState, activeOverride);
}

static void DrawOpenIconBar (ClientData *pcd, int xOffset, int yOffset)
{
    DrawOpenIconBarInternal(pcd, xOffset, yOffset,
                            ICON_WIDTH(pcd), False, -1);
}

void UpdateOpenIconIndicator (ClientData *pcd, Boolean ignoreState, int activeOverride)
{
    Window win;
    int x;
    int y;
    unsigned int w;
    unsigned int h;
    GC barGC;
    GC topGC;
    GC botGC;
    Boolean isActive;

    if (!OpenIconIndicatorShouldShow(pcd, ignoreState))
    {
	if (ICON_INDICATOR_WIN(pcd))
	{
	    XUnmapWindow (DISPLAY, ICON_INDICATOR_WIN(pcd));
	}
	return;
    }

    if (!ICON_INDICATOR_WIN(pcd))
    {
	Window newWin = CreateOpenIconIndicatorWindow(pcd);
	if (!newWin)
	{
	    return;
	}
	if (UseSharedIconWindows(pcd))
	{
	    int i;
	    for (i = 0; i < pcd->sizeWsList; i++)
	    {
		pcd->pWsList[i].iconIndicatorWin = newWin;
	    }
	}
	else
	{
	    ICON_INDICATOR_WIN(pcd) = newWin;
	}
    }

    if (!GetOpenIconIndicatorGeometry(pcd, &x, &y, &w, &h))
    {
	return;
    }

    win = ICON_INDICATOR_WIN(pcd);
    XMoveResizeWindow (DISPLAY, win, x, y, w, h);
    XMapWindow (DISPLAY, win);

    if (activeOverride < 0)
    {
	/* Active state follows keyboard focus unless explicitly overridden. */
	isActive = (wmGD.keyboardFocus == pcd);
    }
    else
    {
	isActive = (activeOverride != 0);
    }
    /*
     * Bar color is derived from the client's appearance:
     * - active uses clientAppearance.activeBackground and active shadows
     * - inactive uses clientAppearance.background and inactive shadows
     */
    barGC = GetOpenIconBarGC(pcd, isActive);
    if (!barGC)
    {
	return;
    }

    topGC = isActive ? ICON_APPEARANCE(pcd).activeTopShadowGC
		     : ICON_APPEARANCE(pcd).inactiveTopShadowGC;
    botGC = isActive ? ICON_APPEARANCE(pcd).activeBottomShadowGC
		     : ICON_APPEARANCE(pcd).inactiveBottomShadowGC;

    XFillRectangle (DISPLAY, win, barGC, 0, 0, w, h);
    XDrawLine (DISPLAY, win, topGC, 0, 0, (int)w - 1, 0);
    XDrawLine (DISPLAY, win, topGC, 0, 0, 0, (int)h - 1);
    XDrawLine (DISPLAY, win, botGC, 0, (int)h - 1, (int)w - 1, (int)h - 1);
    XDrawLine (DISPLAY, win, botGC, (int)w - 1, 0, (int)w - 1, (int)h - 1);
}

static IconBlinkData *FindIconBlink (ClientData *pcd)
{
    IconBlinkData *cur = iconBlinkList;
    while (cur)
    {
	if (cur->pCD == pcd)
	{
	    return cur;
	}
	cur = cur->next;
    }
    return NULL;
}

static void RemoveIconBlink (IconBlinkData *blink)
{
    IconBlinkData **pp = &iconBlinkList;
    while (*pp)
    {
	if (*pp == blink)
	{
	    *pp = blink->next;
	    if (blink->timerId)
	    {
		XtRemoveTimeOut(blink->timerId);
	    }
	    XtFree((char *)blink);
	    return;
	}
	pp = &(*pp)->next;
    }
}

static void ShowMinimizeBlinkBar (ClientData *pcd, Boolean active)
{
    int xOffset;
    int yOffset;

    if (!ICON_FRAME_WIN(pcd))
    {
	return;
    }

    if (P_ICON_BOX(pcd))
    {
	xOffset = IB_MARGIN_WIDTH;
	yOffset = IB_MARGIN_HEIGHT;
    }
    else
    {
	xOffset = 0;
	yOffset = 0;
    }

    DrawOpenIconBarInternal(pcd, xOffset, yOffset,
                            ICON_WIDTH(pcd),
                            True, active ? 1 : 0);
    XSync(DISPLAY, False);
}

static void MinimizeBlinkTimeout (XtPointer client_data, XtIntervalId *id)
{
    IconBlinkData *blink = (IconBlinkData *)client_data;

    if (!blink || !blink->pCD)
    {
        return;
    }

    blink->timerId = 0;

    if (blink->pCD->clientState != MINIMIZED_STATE ||
	!ClientInWorkspace(blink->pCD->pSD->pActiveWS, blink->pCD))
    {
	IconExposureProc(blink->pCD, True);
	if (blink->resized)
	{
	    XResizeWindow(DISPLAY, ICON_FRAME_WIN(blink->pCD),
			  blink->restoreWidth, blink->restoreHeight);
	}
	RemoveIconBlink(blink);
	return;
    }

    if (blink->remainingToggles <= 0)
    {
	IconExposureProc(blink->pCD, True);
	if (blink->resized)
	{
	    XResizeWindow(DISPLAY, ICON_FRAME_WIN(blink->pCD),
			  blink->restoreWidth, blink->restoreHeight);
	}
	RemoveIconBlink(blink);
	return;
    }

    blink->showing = !blink->showing;
    ShowMinimizeBlinkBar(blink->pCD, blink->showing);
    blink->remainingToggles--;

    blink->timerId = XtAppAddTimeOut(wmGD.mwmAppContext,
				     kMinimizeBlinkIntervalMs,
				     MinimizeBlinkTimeout,
				     (XtPointer)blink);
}

void StartMinimizeIconBlink (ClientData *pcd)
{
    IconBlinkData *blink;
    XWindowAttributes attr;

    if (!pcd || !ICON_FRAME_WIN(pcd))
    {
	return;
    }

    if (!ClientInWorkspace(pcd->pSD->pActiveWS, pcd))
    {
	return;
    }

    blink = FindIconBlink(pcd);
    if (blink)
    {
	if (blink->timerId)
	{
	    XtRemoveTimeOut(blink->timerId);
	}
	blink->remainingToggles = kMinimizeBlinkToggles;
	blink->showing = False;
    }
    else
    {
	blink = (IconBlinkData *)XtMalloc(sizeof(IconBlinkData));
	blink->pCD = pcd;
	blink->remainingToggles = kMinimizeBlinkToggles;
	blink->showing = False;
	blink->timerId = 0;
	blink->restoreWidth = ICON_WIDTH(pcd);
	blink->restoreHeight = ICON_HEIGHT(pcd);
	blink->resized = False;
	blink->next = iconBlinkList;
	iconBlinkList = blink;
    }

    if (!P_ICON_BOX(pcd) && XGetWindowAttributes(DISPLAY, ICON_FRAME_WIN(pcd), &attr))
    {
	blink->restoreWidth = (unsigned int)attr.width;
	blink->restoreHeight = (unsigned int)attr.height;
    }

    ShowMinimizeBlinkBar(pcd, True);
    blink->showing = True;

    blink->timerId = XtAppAddTimeOut(wmGD.mwmAppContext,
				     kMinimizeBlinkIntervalMs,
				     MinimizeBlinkTimeout,
				     (XtPointer)blink);
}


/*************************************<->*************************************
 *
 *  MakeIcon (pWS, pcd)
 *
 *
 *  Description:
 *  -----------
 *  Create an icon frame and fill it in as appropriate for the client.
 *
 *
 *  Inputs:
 *  ------
 *  pWS		- pointer to workspace data
 *  pcd		- pointer to client data
 *
 * 
 *  Outputs:
 *  -------
 *  Return 	- TRUE if success, FALSE if failure.
 *
 *
 *  Comments:
 *  --------
 *  
 * 
 *************************************<->***********************************/

Boolean MakeIcon (WmWorkspaceData *pWS, ClientData *pcd)
{
    XSetWindowAttributes window_attribs;
    unsigned long attr_mask;
    int xOffset;
    int yOffset;
    WsClientData *pWsc = GetWsClientData (pWS, pcd);


    /*
     * Common to all icons
     */

    /* compute dimensions of outer icon frame */
    /* create icon frame window */

    attr_mask = CWEventMask | CWCursor;
    window_attribs.event_mask = (ButtonPressMask | ButtonReleaseMask |
				 SELECT_BUTTON_MOTION_MASK |
				 DMANIP_BUTTON_MOTION_MASK |
				 ExposureMask |
				 SubstructureRedirectMask |
				 FocusChangeMask);
    window_attribs.cursor = wmGD.workspaceCursor;

    if ((wmGD.keyboardFocusPolicy == KEYBOARD_FOCUS_POINTER) ||
	(wmGD.colormapFocusPolicy == CMAP_FOCUS_POINTER))
    {
	window_attribs.event_mask |= EnterWindowMask | LeaveWindowMask;
    }

    /* 
     * Use background pixmap if one is specified, otherwise set the
     * appropriate background color. 
     */

    if (ICON_APPEARANCE(pcd).backgroundPixmap)
    {
	attr_mask |= CWBackPixmap;
	window_attribs.background_pixmap = 
			    ICON_APPEARANCE(pcd).backgroundPixmap;
    }
    else
    {
	attr_mask |= CWBackPixel;
	window_attribs.background_pixel = 
				ICON_APPEARANCE(pcd).background;
    }
   
    if ((!pcd->pSD->useIconBox) || 
	(pcd->clientFlags & (CLIENT_WM_CLIENTS | FRONT_PANEL_BOX)))
    {
	pWsc->iconFrameWin = XCreateWindow (DISPLAY,
			       ROOT_FOR_CLIENT(pcd),	/* parent */
			       pWsc->iconX,
			       pWsc->iconY,
			       (unsigned int) ICON_WIDTH(pcd),
			       (unsigned int) ICON_HEIGHT(pcd),
			       0,		/* border width */
			       CopyFromParent,	/* depth */
			       InputOutput,	/* class */
			       CopyFromParent,	/* visual */
			       attr_mask,
			       &window_attribs);

    }
    else
    {
        /*
         * Insert the icon into the icon box.
         * Don't make icon in the box for any icon box (or any WM window)
	 * OR any client that doesn't have the MWM_FUNC_MINIMIZE bit set
	 * in pcd->clientFunctions
         */

        if ((pcd->pSD->useIconBox) && 
	    (!(pcd->clientFlags & CLIENT_WM_CLIENTS)) &&
	    (pcd->clientFunctions & MWM_FUNC_MINIMIZE) )
        {
            if (!InsertIconIntoBox(pWS->pIconBox, pcd))
		Warning(((char *)GETMESSAGE(30, 1, "Could not make icon to go in icon box")));

	}

    }


    /* make space for the top/bottom changing shadow rectangles */

    if ((pcd->piconTopShadows = 
	    AllocateRList ((unsigned)NUM_BOTH_TOP_RECTS)) == NULL)
    {
	/* Out of memory! */
	Warning (((char *)GETMESSAGE(30, 2, "Insufficient memory for icon creation")));
	return(FALSE);
    }
    
    if ((pcd->piconBottomShadows = 
	 AllocateRList ((unsigned)NUM_BOTH_BOTTOM_RECTS)) == NULL)
    {
	/* Out of memory! */
	Warning (((char *)GETMESSAGE(30, 3, "Insufficient memory for icon creation")));
	return(FALSE);
    }


    /*
     * Adjust for icons in the box 
     * Don't adjust the icon for the icon box itself
     */

    if (pWS->pIconBox && (pWS->pIconBox->pCD_iconBox != pcd) &&
	!(pcd->clientFlags & FRONT_PANEL_BOX))
    {
	xOffset = IB_MARGIN_WIDTH;
	yOffset = IB_MARGIN_HEIGHT;
    }
    else
    {
	xOffset = 0;
        yOffset = 0;
    }


    /*
     * Reparent the icon window if there is one
     */
    if ((ICON_DECORATION(pcd) & ICON_IMAGE_PART) && 
	(pcd->iconWindow))
    {
	ReparentIconWindow (pcd, xOffset, yOffset);
    }

    if (pcd->piconTopShadows->used == 0)
    MakeIconShadows (pcd, xOffset, yOffset);

    return(TRUE);

} /* END OF FUNCTION MakeIcon */



/*************************************<->*************************************
 *
 *  MakeIconShadows (pcd, xOffset, yOffset)
 *
 *  Comments:
 *  --------
 * 
 *************************************<->***********************************/

void MakeIconShadows (ClientData *pcd, int xOffset, int yOffset)
{        

    /*
     * Handle different icon styles
     */
    
    switch (ICON_DECORATION(pcd) & (ICON_LABEL_PART | ICON_IMAGE_PART)) 
    {
	case ICON_LABEL_PART:
	    BevelRectangle (pcd->piconTopShadows, 	/* label */
			    pcd->piconBottomShadows, 
			    0 + xOffset, (int)ICON_IMAGE_HEIGHT(pcd) + yOffset,
			    (unsigned int) IconContentWidth(pcd), 
			    (unsigned int) ICON_LABEL_HEIGHT(pcd),
			    ICON_EXTERNAL_SHADOW_WIDTH,
			    ICON_EXTERNAL_SHADOW_WIDTH,
			    ICON_EXTERNAL_SHADOW_WIDTH,
			    ICON_EXTERNAL_SHADOW_WIDTH);
	    break;

	case ICON_IMAGE_PART:
	    BevelRectangle (pcd->piconTopShadows, 	/* image outside */
			    pcd->piconBottomShadows, 
			    0 + xOffset, 0 + yOffset,
			    (unsigned int) IconContentWidth(pcd), 
			    (unsigned int) ICON_IMAGE_HEIGHT(pcd),
			    ICON_EXTERNAL_SHADOW_WIDTH,
			    ICON_EXTERNAL_SHADOW_WIDTH,
			    ICON_EXTERNAL_SHADOW_WIDTH,
			    ICON_EXTERNAL_SHADOW_WIDTH);

	    if (wmGD.frameStyle == WmRECESSED)
		BevelRectangle (pcd->piconBottomShadows, /* image inside */
			    pcd->piconTopShadows, 
			    ICON_INNER_X_OFFSET + xOffset,
			    ICON_INNER_Y_OFFSET + yOffset,
			    (unsigned int) (ICON_IMAGE_MAXIMUM(pcd).width + 
				4*ICON_INTERNAL_SHADOW_WIDTH),
			    (unsigned int) (ICON_IMAGE_MAXIMUM(pcd).height + 
				4*ICON_INTERNAL_SHADOW_WIDTH),
			    ICON_INTERNAL_SHADOW_WIDTH,
			    ICON_INTERNAL_SHADOW_WIDTH,
			    ICON_INTERNAL_SHADOW_WIDTH,
			    ICON_INTERNAL_SHADOW_WIDTH);

	    break;

	case (ICON_IMAGE_PART | ICON_LABEL_PART):
	    if (wmGD.frameStyle == WmSLAB)
	    {
		BevelRectangle (pcd->piconTopShadows, 	/* image outside */
			    pcd->piconBottomShadows, 
			    0 + xOffset, 0 + yOffset,
			    (unsigned int) IconContentWidth(pcd), 
			    (unsigned int) (ICON_IMAGE_HEIGHT(pcd) +
					    ICON_LABEL_HEIGHT(pcd)),
			    ICON_EXTERNAL_SHADOW_WIDTH,
			    ICON_EXTERNAL_SHADOW_WIDTH,
			    ICON_EXTERNAL_SHADOW_WIDTH,
			    ICON_EXTERNAL_SHADOW_WIDTH);
	    }
	    else
	    {
		BevelRectangle (pcd->piconTopShadows, 	/* image outside */
			    pcd->piconBottomShadows, 
			    0 + xOffset, 0 + yOffset, 
			    (unsigned int) IconContentWidth(pcd), 
			    (unsigned int) ICON_IMAGE_HEIGHT(pcd),
			    ICON_EXTERNAL_SHADOW_WIDTH,
			    ICON_EXTERNAL_SHADOW_WIDTH,
			    ICON_INTERNAL_SHADOW_WIDTH,
			    ICON_EXTERNAL_SHADOW_WIDTH);
	    }

	    if (wmGD.frameStyle == WmRECESSED)
		BevelRectangle (pcd->piconBottomShadows, /* image inside */
			    pcd->piconTopShadows, 
			    ICON_INNER_X_OFFSET + xOffset,
			    ICON_INNER_Y_OFFSET + yOffset,
			    (unsigned int) (ICON_IMAGE_MAXIMUM(pcd).width + 
				4*ICON_INTERNAL_SHADOW_WIDTH),
			    (unsigned int) (ICON_IMAGE_MAXIMUM(pcd).height + 
				4*ICON_INTERNAL_SHADOW_WIDTH),
			    ICON_INTERNAL_SHADOW_WIDTH,
			    ICON_INTERNAL_SHADOW_WIDTH,
			    ICON_INTERNAL_SHADOW_WIDTH,
			    ICON_INTERNAL_SHADOW_WIDTH);

	    if (wmGD.frameStyle == WmRECESSED)
		BevelRectangle (pcd->piconTopShadows, 	/* label */
			    pcd->piconBottomShadows, 
			    0 + xOffset, (int)ICON_IMAGE_HEIGHT(pcd) + yOffset,
			    (unsigned int) IconContentWidth(pcd), 
			    (unsigned int) ICON_LABEL_HEIGHT(pcd),
			    ICON_INTERNAL_SHADOW_WIDTH,
			    ICON_EXTERNAL_SHADOW_WIDTH,
			    ICON_EXTERNAL_SHADOW_WIDTH,
			    ICON_EXTERNAL_SHADOW_WIDTH);

	    break;
    }

} /* END OF FUNCTION MakeIconShadows */






/*************************************<->*************************************
 *
 *  IconExposureProc (pcd, expose)
 *
 *
 *  Description:
 *  -----------
 *  Repaint the icon.
 *
 *
 *  Inputs:
 *  ------
 *  pcd		- pointer to client data
 *
 * 
 *  Outputs:
 *  -------
 *
 *  Comments:
 *  --------
 * 
 *************************************<->***********************************/

void IconExposureProc (ClientData *pcd, Boolean expose)
{

    Pixmap image;
    int dest_x, dest_y;
    int xOffset;
    int yOffset;
    unsigned int width, height;
    GC iconGC, topGC, botGC;



    /*
     * Adjust for icons in the iconBox
     */

    if (P_ICON_BOX(pcd))
    {
	xOffset = IB_MARGIN_WIDTH;
	yOffset = IB_MARGIN_HEIGHT;
    }
    else
    {
	xOffset = 0;
        yOffset = 0;
    }
    
    /* adjust icon window background to allow transparent gap for open icons */
    if (pcd->pSD->showOpenWindowIcons &&
	(pcd->clientState != MINIMIZED_STATE))
    {
	XSetWindowBackgroundPixmap (DISPLAY, ICON_FRAME_WIN(pcd),
				    ParentRelative);
    }
    else if (ICON_APPEARANCE(pcd).backgroundPixmap)
    {
	XSetWindowBackgroundPixmap (DISPLAY, ICON_FRAME_WIN(pcd),
				    ICON_APPEARANCE(pcd).backgroundPixmap);
    }
    else
    {
	XSetWindowBackground (DISPLAY, ICON_FRAME_WIN(pcd),
			      ICON_APPEARANCE(pcd).background);
    }

    /* get appropriate GCs */

    if ((wmGD.keyboardFocus == pcd) && (pcd->clientState == MINIMIZED_STATE))
    {
	iconGC = ICON_APPEARANCE(pcd).activeGC;
	topGC = ICON_APPEARANCE(pcd).activeTopShadowGC;
	botGC = ICON_APPEARANCE(pcd).activeBottomShadowGC;
    }
    else
    {
	iconGC = ICON_APPEARANCE(pcd).inactiveGC;
	topGC = ICON_APPEARANCE(pcd).inactiveTopShadowGC;
	botGC = ICON_APPEARANCE(pcd).inactiveBottomShadowGC;
    }

    if (ACTIVE_PSD->useIconBox && P_ICON_BOX(pcd))
    {
	/* draw shadowing */

	if (expose)
	{
	    XClearArea (DISPLAY, 
			    ICON_FRAME_WIN(pcd), 
			    IB_MARGIN_WIDTH, 
			    IB_MARGIN_HEIGHT, 
			    (unsigned int) ICON_WIDTH(pcd), 
			    (unsigned int) ICON_HEIGHT(pcd), False);
	}

	if (pcd->clientState == MINIMIZED_STATE)
	{
	    /*
	     * This is the "raised" icon appearance
	     */

	    if (pcd->piconTopShadows)
	    {

	        XFillRectangles (DISPLAY, 
				    ICON_FRAME_WIN(pcd), 
				    topGC,
				    pcd->piconTopShadows->prect,
				    pcd->piconTopShadows->used);
	    }

	    if (pcd->piconBottomShadows)
	    { 
	        XFillRectangles (DISPLAY,
				    ICON_FRAME_WIN(pcd), 
				    botGC,
				    pcd->piconBottomShadows->prect,
				    pcd->piconBottomShadows->used);
	    }
	}
	else 
	{
	    GC fillGC = GetOpenIconFillGC(pcd);
	    if (fillGC)
	    {
		XFillRectangle (DISPLAY,
				ICON_FRAME_WIN(pcd),
				fillGC,
				xOffset, yOffset,
				(unsigned int) IconContentWidth(pcd),
				(unsigned int) ICON_HEIGHT(pcd));
	    }

	    if (pcd->piconTopShadows)
	    {
		XFillRectangles (DISPLAY,
				 ICON_FRAME_WIN(pcd),
				 botGC,
				 pcd->piconTopShadows->prect,
				 pcd->piconTopShadows->used);
	    }

	    if (pcd->piconBottomShadows)
	    {
		XFillRectangles (DISPLAY,
				 ICON_FRAME_WIN(pcd),
				 topGC,
				 pcd->piconBottomShadows->prect,
				 pcd->piconBottomShadows->used);
	    }
			    
	}

    }
    else 
    {
	/* draw shadowing */

	if (pcd->clientState == MINIMIZED_STATE)
	{
	    /*
	     * This is the "raised" icon appearance
	     */

	    if (pcd->piconTopShadows->prect)
	    {

		XFillRectangles (DISPLAY, 
				ICON_FRAME_WIN(pcd), 
				topGC,
				pcd->piconTopShadows->prect,
				pcd->piconTopShadows->used);
	    }

	    if (pcd->piconBottomShadows->prect)
	    { 
		XFillRectangles (DISPLAY,
				ICON_FRAME_WIN(pcd), 
				botGC,
				pcd->piconBottomShadows->prect,
				pcd->piconBottomShadows->used);
	    }

	}
	else
	{
	    GC fillGC = GetOpenIconFillGC(pcd);
	    if (fillGC)
	    {
		XFillRectangle (DISPLAY,
				ICON_FRAME_WIN(pcd),
				fillGC,
				0, 0,
				(unsigned int) IconContentWidth(pcd),
				(unsigned int) ICON_HEIGHT(pcd));
	    }

	    if (pcd->piconTopShadows->prect)
	    {
		XFillRectangles (DISPLAY,
				ICON_FRAME_WIN(pcd),
				botGC,
				pcd->piconTopShadows->prect,
				pcd->piconTopShadows->used);
	    }

	    if (pcd->piconBottomShadows->prect)
	    {
		XFillRectangles (DISPLAY,
				ICON_FRAME_WIN(pcd),
				topGC,
				pcd->piconBottomShadows->prect,
				pcd->piconBottomShadows->used);
	    }
	}
    }


    /* draw icon text */
/*
    if ((ICON_DECORATION(pcd) & ICON_LABEL_PART) &&
	(expose || !(ICON_DECORATION(pcd) & ICON_ACTIVE_LABEL_PART)))
*/
    if (ICON_DECORATION(pcd) & ICON_LABEL_PART)
    {
	DrawIconTitle (pcd);
    }

    /* 
     * Draw image if no icon window (client has to redraw that!) 
     *  OR if using the iconbox, draw the default image where
     *  the icon window was.
     */

    if (expose &&
	((!pcd->iconWindow && (ICON_DECORATION(pcd) & ICON_IMAGE_PART)) ||
         (ACTIVE_PSD->useIconBox && P_ICON_BOX(pcd) &&
		pcd->iconWindow && 
		pcd->clientState != MINIMIZED_STATE &&
		(ICON_DECORATION(pcd) & ICON_IMAGE_PART))))
    {
	if (pcd->iconWindow)
	{
	    image = DEFAULT_PIXMAP(pcd);
	}
	else
	{
	    image = pcd->iconPixmap;
	}

	if (image)
	{

	    if ((ACTIVE_PSD->useIconBox) && (P_ICON_BOX(pcd)))
	    {

		if (pcd->clientState != MINIMIZED_STATE)
		{
		    dest_x = ICON_IMAGE_X_OFFSET 
			    + ICON_INTERNAL_SHADOW_WIDTH
			    + xOffset;

		    dest_y = ICON_IMAGE_Y_OFFSET 
			    + ICON_INTERNAL_SHADOW_WIDTH
			    + yOffset;
		    if (wmGD.frameStyle == WmSLAB)
		    {
			/* less beveling in this style */
			dest_x -= ICON_INTERNAL_SHADOW_WIDTH;
			dest_y -= ICON_INTERNAL_SHADOW_WIDTH;
		    } 

		    width = ICON_IMAGE_MAXIMUM(pcd).width;
		    height= ICON_IMAGE_MAXIMUM(pcd).height;
		    if (wmGD.frameStyle == WmSLAB)
		    {
			width += 2;
			height += 2;
		    } 
		    XCopyArea (DISPLAY, image, 
				ICON_FRAME_WIN(pcd), 
				iconGC,
		                ICON_INTERNAL_SHADOW_WIDTH, 
				ICON_INTERNAL_SHADOW_WIDTH, 
				width, height, dest_x, dest_y);

		    if (FADE_NORMAL_ICON(pcd))
		    {
			iconGC = FADE_ICON_GC(pcd);
			XFillRectangle (DISPLAY, 
				    ICON_FRAME_WIN(pcd), 
				    iconGC,
				    dest_x, dest_y,
				    width, height);
		    }

		}
		else
		{
		    dest_x = ICON_IMAGE_X_OFFSET 
			    + xOffset;

		    dest_y = ICON_IMAGE_Y_OFFSET 
			    + yOffset;
		    if (wmGD.frameStyle == WmSLAB)
		    {
			/* less beveling in this style */
			dest_x -= ICON_INTERNAL_SHADOW_WIDTH;
			dest_y -= ICON_INTERNAL_SHADOW_WIDTH;
		    } 

		    width = ICON_IMAGE_MAXIMUM(pcd).width
		    	    + (2 * ICON_INTERNAL_SHADOW_WIDTH);
		    height= ICON_IMAGE_MAXIMUM(pcd).height
		    	    + (2 * ICON_INTERNAL_SHADOW_WIDTH);

		    if (wmGD.frameStyle == WmSLAB)
		    {
			width += 2;
			height += 2;
		    } 
	            XCopyArea (DISPLAY, image, 
				ICON_FRAME_WIN(pcd), 
				iconGC, 0, 0, width, height, 
				dest_x, dest_y);

		}
	    }
	    else

	    {
		width = ICON_IMAGE_MAXIMUM(pcd).width +
			2 * ICON_INTERNAL_SHADOW_WIDTH;

		height= ICON_IMAGE_MAXIMUM(pcd).height +
			2 * ICON_INTERNAL_SHADOW_WIDTH;

		if (wmGD.frameStyle == WmSLAB)
		{
		    dest_x = ICON_INNER_X_OFFSET;
		    dest_y = ICON_INNER_Y_OFFSET;
		    width += 2;
		    height += 2;
		} 
		else 
		{
		    dest_x = ICON_INNER_X_OFFSET + ICON_INTERNAL_SHADOW_WIDTH;
		    dest_y = ICON_INNER_Y_OFFSET + ICON_INTERNAL_SHADOW_WIDTH;
		}
		XCopyArea (DISPLAY, image, 
			    ICON_FRAME_WIN(pcd), 
			    iconGC, 0, 0, width, height, 
			    dest_x, dest_y);


	    }

	}
    }

    DrawOpenIconBar (pcd, xOffset, yOffset);

} /* END OF FUNCTION IconExposureProc */




/*************************************<->*************************************
 *
 *  GetIconTitleBox (pcd, pBox)
 *
 *
 *  Description:
 *  -----------
 *  Returns a rectangle containing the icon text box
 *
 *
 *  Inputs:
 *  ------
 *  pcd - pointer to client data
 *  pBox - pointer to an XRectangle structure that gets returned data
 *
 *  Outputs:
 *  -------
 *  pBox - returned data
 *
 *  Comments:
 *  --------
 * 
 *************************************<->***********************************/

void GetIconTitleBox (ClientData *pcd, XRectangle *pBox)
{
    int xOffset;
    int yOffset;


    /*
     * Adjust for icons in the iconBox
     */

    if (P_ICON_BOX(pcd))
    {
	xOffset = IB_MARGIN_WIDTH;
	yOffset = IB_MARGIN_HEIGHT;
    }
    else
    {
	xOffset = 0;
        yOffset = 0;
    }

    if ((P_ICON_BOX(pcd)) && (pcd->clientState != MINIMIZED_STATE))
    {
	/* move label up to enhance shrink wrap effect */
	pBox->x = ICON_EXTERNAL_SHADOW_WIDTH 
		+ ICON_IMAGE_LEFT_PAD
		+ (2 * ICON_INTERNAL_SHADOW_WIDTH)
		+ ICON_IMAGE_LEFT_PAD
		+ xOffset;

	pBox->y = ICON_IMAGE_HEIGHT(pcd) 
		+  yOffset
                + ((ICON_IMAGE_HEIGHT(pcd) > 0) 
			? - ICON_IMAGE_BOTTOM_PAD
			: ICON_EXTERNAL_SHADOW_WIDTH)
		+  ((ICON_IMAGE_HEIGHT(pcd) >0)
			? 0
			: WM_TOP_TITLE_PADDING );

	if (wmGD.frameStyle == WmSLAB)
	{
	    /* account for less beveling in this style */
	    pBox->x -= ICON_INTERNAL_SHADOW_WIDTH;
	    pBox->y -= ICON_INTERNAL_SHADOW_WIDTH;
	}



	pBox->width = ICON_IMAGE_MAXIMUM(pcd).width 
			+ ((wmGD.frameStyle == WmSLAB) ? 2 : 0) 
			- ICON_IMAGE_LEFT_PAD
			- ICON_EXTERNAL_SHADOW_WIDTH;

	pBox->height = TEXT_HEIGHT(ICON_APPEARANCE(pcd).font);

    }
    else if ((P_ICON_BOX(pcd)) && (pcd->clientState == MINIMIZED_STATE))
    {
	pBox->x = ICON_EXTERNAL_SHADOW_WIDTH 
		+ ICON_IMAGE_LEFT_PAD
		+ (2 * ICON_INTERNAL_SHADOW_WIDTH)
		+ ICON_IMAGE_LEFT_PAD
		+ xOffset;

	pBox->y = ICON_IMAGE_HEIGHT(pcd) 
		+  yOffset
                + ((ICON_IMAGE_HEIGHT(pcd) > 0) 
			? ICON_INTERNAL_SHADOW_WIDTH 
			: ICON_EXTERNAL_SHADOW_WIDTH)
		+  WM_TOP_TITLE_PADDING ;

	pBox->width = ICON_IMAGE_MAXIMUM(pcd).width 
			+ ((wmGD.frameStyle == WmSLAB) ? 2 : 0) 
			- ICON_IMAGE_LEFT_PAD;

	pBox->height = TEXT_HEIGHT(ICON_APPEARANCE(pcd).font);

	if (wmGD.frameStyle == WmSLAB)
	{
	    /* account for less beveling in this style */
	    pBox->x -= ICON_INTERNAL_SHADOW_WIDTH + 2*ICON_IMAGE_LEFT_PAD;
	    pBox->y -= 3 * ICON_INTERNAL_SHADOW_WIDTH;
	    pBox->width += ICON_IMAGE_LEFT_PAD + 2;
	}

    }
    else
    {
	pBox->x = ICON_EXTERNAL_SHADOW_WIDTH 
		+ WM_TOP_TITLE_PADDING
		+ xOffset;

	if (wmGD.frameStyle == WmSLAB)
	{
	    /* account for less beveling in this style */
	    yOffset -= ICON_INTERNAL_SHADOW_WIDTH;
	}
	pBox->y = ICON_IMAGE_HEIGHT(pcd) 
		+  WM_TOP_TITLE_PADDING 
		+  yOffset
                + ((ICON_IMAGE_HEIGHT(pcd) > 0) 
			? ICON_INTERNAL_SHADOW_WIDTH 
			: ICON_EXTERNAL_SHADOW_WIDTH);

	pBox->width = ICON_WIDTH(pcd) - 2 * ICON_EXTERNAL_SHADOW_WIDTH - 
		  WM_TOP_TITLE_PADDING - WM_BOTTOM_TITLE_PADDING;
	pBox->height = TEXT_HEIGHT(ICON_APPEARANCE(pcd).font);

    }

    if (pcd->pSD->showOpenWindowIcons &&
	(pcd->clientState != MINIMIZED_STATE))
    {
	if ((int)pBox->width > ICON_OPEN_EXTRA(pcd))
	{
	    pBox->width -= ICON_OPEN_EXTRA(pcd);
	}
    }

} /* END OF FUNCTION GetIconTitleBox */



/*************************************<->*************************************
 *
 *  DrawIconTitle (pcd)
 *
 *
 *  Description:
 *  -----------
 *  Draws the title in the Icon title area
 *
 *
 *  Inputs:
 *  ------
 *  pcd - pointer to client data
 *
 *  Outputs:
 *  -------
 *
 *  Comments:
 *  --------
 * 
 *************************************<->***********************************/

void DrawIconTitle (ClientData *pcd)
{
    XRectangle textBox;
    GC iconGC;
    Boolean useOpenFill = False;
    
    
    GetIconTitleBox (pcd, &textBox);

    /* use a consistent icon text color */
    iconGC = ICON_APPEARANCE(pcd).inactiveGC;

    /* 
     * Dim text if this is in the icon box and the client is mapped 
     */

    if ((ACTIVE_PSD->useIconBox) &&
	(P_ICON_BOX(pcd)) &&
	(FADE_NORMAL_ICON(pcd)) &&
	(!(pcd->clientState == MINIMIZED_STATE)))
    {
	iconGC = FADE_ICON_TEXT_GC(pcd);
    }




    if (pcd->pSD->showOpenWindowIcons &&
	(pcd->clientState != MINIMIZED_STATE))
    {
	GC fillGC = GetOpenIconFillGC(pcd);
	if (fillGC)
	{
	    XFillRectangle (DISPLAY,
			    ICON_FRAME_WIN(pcd),
			    fillGC,
			    textBox.x, textBox.y,
			    (unsigned int) textBox.width,
			    (unsigned int) textBox.height);
	    useOpenFill = True;
	}
    }

    if (!useOpenFill)
    {
	XClearArea (DISPLAY,
		    ICON_FRAME_WIN(pcd),
		    textBox.x, textBox.y,
		    (unsigned int) textBox.width,
		    (unsigned int) textBox.height,
		    False);
    }

    {
	Dimension textWidth;
	int alignment;

	textWidth = XmStringWidth(ICON_APPEARANCE(pcd).fontList,
				  ICON_DISPLAY_TITLE(pcd));
	alignment = (textWidth >= textBox.width) ?
	    XmALIGNMENT_BEGINNING : XmALIGNMENT_CENTER;

	if ((alignment == XmALIGNMENT_CENTER) &&
	    (pcd->pSD->showOpenWindowIcons) &&
	    (pcd->clientState != MINIMIZED_STATE))
	{
	    XRectangle drawBox = textBox;
	    drawBox.x += (ICON_OPEN_EXTRA(pcd) / 2);
	    XmStringDraw (DISPLAY, ICON_FRAME_WIN(pcd),
			  ICON_APPEARANCE(pcd).fontList,
			  ICON_DISPLAY_TITLE(pcd), iconGC,
			  drawBox.x, drawBox.y, drawBox.width,
			  alignment, XmSTRING_DIRECTION_L_TO_R, &drawBox);
	}
	else
	{
	    XmStringDraw (DISPLAY, ICON_FRAME_WIN(pcd), ICON_APPEARANCE(pcd).fontList,
			  ICON_DISPLAY_TITLE(pcd), iconGC,
			  textBox.x, textBox.y, textBox.width,
			  alignment, XmSTRING_DIRECTION_L_TO_R, &textBox);
	}
    }

} /* END OF FUNCTION DrawIconTitle */




/*************************************<->*************************************
 *
 *  RedisplayIconTitle (pcd)
 *
 *
 *  Description:
 *  -----------
 *  Draws the title in the Icon title area
 *
 *
 *  Inputs:
 *  ------
 *  pcd - pointer to client data
 *
 *  Outputs:
 *  -------
 *
 *  Comments:
 *  --------
 * 
 *************************************<->***********************************/

void RedisplayIconTitle (ClientData *pcd)
{
    XRectangle textBox;
    GC iconGC;
    Boolean useOpenFill = False;

    /*
     * only proceed if we've got the right icon parts to work on
     */

    if (ICON_DECORATION(pcd) & ICON_LABEL_PART && ICON_FRAME_WIN(pcd))
    {

	/* nothing to do if no labels */
	if (!(ICON_DECORATION(pcd) & ICON_LABEL_PART))
	    return;

	/* get the box that the text sits in */
	GetIconTitleBox (pcd, &textBox);

	/* 
	 * Get appropriate GCs 
	 * Dim text if this is in the icon box and the client is mapped 
	 */
	iconGC = ICON_APPEARANCE(pcd).inactiveGC;

	if ((ACTIVE_PSD->useIconBox) && 
	    (P_ICON_BOX(pcd)) &&
	    (FADE_NORMAL_ICON(pcd)) && 
	    (!(pcd->clientState == MINIMIZED_STATE)))
	{
	    iconGC = FADE_ICON_TEXT_GC(pcd);
	}

	if (pcd->pSD->showOpenWindowIcons &&
	    (pcd->clientState != MINIMIZED_STATE))
	{
	    GC fillGC = GetOpenIconFillGC(pcd);
	    if (fillGC)
	    {
		XFillRectangle (DISPLAY,
				ICON_FRAME_WIN(pcd),
				fillGC,
				textBox.x, textBox.y,
				(unsigned int) textBox.width,
				(unsigned int) textBox.height);
		useOpenFill = True;
	    }
	}

	/* out with the old */
	if (!useOpenFill)
	{
	    XClearArea (DISPLAY,
		ICON_FRAME_WIN(pcd),
		textBox.x, textBox.y,
		(unsigned int) textBox.width, (unsigned int) textBox.height,
		FALSE);
	}

	{
	    Dimension textWidth;
	    int alignment;

	    textWidth = XmStringWidth(ICON_APPEARANCE(pcd).fontList,
				      ICON_DISPLAY_TITLE(pcd));
	    alignment = (textWidth >= textBox.width) ?
		XmALIGNMENT_BEGINNING : XmALIGNMENT_CENTER;

	    if ((alignment == XmALIGNMENT_CENTER) &&
		(pcd->pSD->showOpenWindowIcons) &&
		(pcd->clientState != MINIMIZED_STATE))
	    {
		XRectangle drawBox = textBox;
		drawBox.x += (ICON_OPEN_EXTRA(pcd) / 2);
		XmStringDraw (DISPLAY, ICON_FRAME_WIN(pcd),
			      ICON_APPEARANCE(pcd).fontList,
			      ICON_DISPLAY_TITLE(pcd), iconGC,
			      drawBox.x, drawBox.y, drawBox.width,
			      alignment, XmSTRING_DIRECTION_L_TO_R,
			      &drawBox);
	    }
	    else
	    {
		XmStringDraw (DISPLAY, ICON_FRAME_WIN(pcd),
			      ICON_APPEARANCE(pcd).fontList,
			      ICON_DISPLAY_TITLE(pcd), iconGC,
			      textBox.x, textBox.y, textBox.width,
			      alignment, XmSTRING_DIRECTION_L_TO_R,
			      &textBox);
	    }
	}

	/* 
	 * Erase & paint text in the active icon text window
	 */
	if ((wmGD.keyboardFocus == pcd) && 
	    (ICON_DECORATION(pcd) & ICON_ACTIVE_LABEL_PART))
	{
	    PaintActiveIconText (pcd, True);
	}
    }
} /* END OF FUNCTION  RedisplayIconTitle */



/*************************************<->*************************************
 *
 *  GetIconDimensions (pSD, &pWidth, &pLabelHeight, &pImageHeight)
 *
 *
 *  Description:
 *  -----------
 *  returns dimensions of icon frame parts
 *
 *
 *  Inputs:
 *  ------
 *  pSD		  - pointer to screen data
 *  pWidth	  - pointer to width of frame
 *  pLabelHeight  - pointer to height of label part of icon
 *  pImageHeight  - pointer to height of image part of icon 
 *
 * 
 *  Outputs:
 *  -------
 *  *pWidth	  - width of frame
 *  *pLabelHeight - height of label part of icon
 *  *pImageHeight - height of image part of icon 
 *
 *
 *  Comments:
 *  --------
 * 
 *************************************<->***********************************/
void GetIconDimensions (WmScreenData *pSD, unsigned int *pWidth, unsigned int *pLabelHeight, unsigned int *pImageHeight)
{
    /*
     * The icon width is always keyed to the icon image maximum regardless
     * of whether an icon image part appears or not.
     */
    *pWidth = pSD->iconImageMaximum.width + 
		((wmGD.frameStyle == WmSLAB) ? 2 : 0) +
		ICON_IMAGE_LEFT_PAD +
		ICON_IMAGE_RIGHT_PAD +
		2 * ICON_EXTERNAL_SHADOW_WIDTH +
		4 * ICON_INTERNAL_SHADOW_WIDTH;
    if (wmGD.frameStyle == WmSLAB)
    {
	/* less beveling in this style */
	*pWidth -= 2 * ICON_INTERNAL_SHADOW_WIDTH;
    }

    switch (pSD->iconDecoration & (ICON_IMAGE_PART | ICON_LABEL_PART)) 
    {
	case ICON_LABEL_PART:
	    *pImageHeight = 0;

	    *pLabelHeight = ICON_EXTERNAL_SHADOW_WIDTH            +
			    WM_TOP_TITLE_PADDING                  +
			    TEXT_HEIGHT(pSD->iconAppearance.font) +
			    WM_BOTTOM_TITLE_PADDING               +
			    ICON_EXTERNAL_SHADOW_WIDTH;
	    break;

	case ICON_IMAGE_PART:
	    *pImageHeight = ICON_EXTERNAL_SHADOW_WIDTH   +
			    ICON_IMAGE_TOP_PAD           +
			    ICON_INTERNAL_SHADOW_WIDTH   +
			    ICON_INTERNAL_SHADOW_WIDTH   +
			    pSD->iconImageMaximum.height + 
			    ((wmGD.frameStyle == WmSLAB) ? 2 : 0) +
			    ICON_INTERNAL_SHADOW_WIDTH   +
			    ICON_INTERNAL_SHADOW_WIDTH   +
			    ICON_IMAGE_BOTTOM_PAD        +
			    ICON_EXTERNAL_SHADOW_WIDTH;
	    if (wmGD.frameStyle == WmSLAB)
	    {
		/* less beveling in this style */
		*pImageHeight -= 2 * ICON_INTERNAL_SHADOW_WIDTH;
	    }


	    *pLabelHeight = 0;

	    break;

	case (ICON_IMAGE_PART | ICON_LABEL_PART):
	    *pImageHeight = ICON_EXTERNAL_SHADOW_WIDTH   +
			    ICON_IMAGE_TOP_PAD           +
			    ICON_INTERNAL_SHADOW_WIDTH   +
			    ICON_INTERNAL_SHADOW_WIDTH   +
			    pSD->iconImageMaximum.height + 
			    ((wmGD.frameStyle == WmSLAB) ? 2 : 0) +
			    ICON_INTERNAL_SHADOW_WIDTH   +
			    ICON_INTERNAL_SHADOW_WIDTH   +
			    ICON_IMAGE_BOTTOM_PAD        +
			    ICON_INTERNAL_SHADOW_WIDTH;

	    *pLabelHeight = ICON_INTERNAL_SHADOW_WIDTH            +
			    WM_TOP_TITLE_PADDING                  +
			    TEXT_HEIGHT(pSD->iconAppearance.font) +
			    WM_BOTTOM_TITLE_PADDING               +
			    ICON_EXTERNAL_SHADOW_WIDTH;
	    if (wmGD.frameStyle == WmSLAB)
	    {
		/*
		 * In this style there is less beveling and no 
		 * etching between the icon image and label.
		 */
		*pImageHeight -= 3 * ICON_INTERNAL_SHADOW_WIDTH;
	        *pLabelHeight -= ICON_INTERNAL_SHADOW_WIDTH;
	    }

	    break;

	default:
	    *pLabelHeight = *pImageHeight = 0;
	    break;
	    
    }
}


/*************************************<->*************************************
 *
 *  InitIconSize (pSD)
 *
 *
 *  Description:
 *  -----------
 *  set global icon size variables
 *
 *
 *  Inputs:
 *  ------
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
void InitIconSize (WmScreenData *pSD)
{
    Cardinal label, image;

    GetIconDimensions (pSD, (unsigned int *)&(pSD->iconWidth), 
	&label, &image);
    
    pSD->iconHeight = label+image;

    pSD->iconImageHeight = image;
    pSD->iconLabelHeight = label;


    /* shrink-wrap metrics no longer used for open icon rendering */
} /* END OF FUNCTION InitIconSize */


/*************************************<->*************************************
 *
 *  ShowActiveIcon (pcd)
 *
 *
 *  Description:
 *  -----------
 *  Paint the icon to indicate an "active" state
 *
 *
 *  Inputs:
 *  ------
 *  pcd		- pointer to client data
 *
 * 
 *  Outputs:
 *  -------
 *
 *
 *  Comments:
 *  --------
 *  
 * 
 *************************************<->***********************************/

void ShowActiveIcon (ClientData *pcd)
{
    unsigned long attr_mask;
    XSetWindowAttributes window_attribs;

    if (ICON_FRAME_WIN(pcd))
    {
	/* 
	 * Use background pixmap if one is specified, otherwise set the
	 * appropriate background color. 
	 */

	if (ICON_APPEARANCE(pcd).activeBackgroundPixmap)
	{
	    attr_mask = CWBackPixmap;
	    window_attribs.background_pixmap = 
				ICON_APPEARANCE(pcd).activeBackgroundPixmap;
	}
	else
	{
	    attr_mask = CWBackPixel;
	    window_attribs.background_pixel = 
				ICON_APPEARANCE(pcd).activeBackground;
	}
	

	/* set active window attributes */
	XChangeWindowAttributes (DISPLAY, 
				ICON_FRAME_WIN(pcd), 
				attr_mask, &window_attribs);

	/* clear the frame to the right background */
	if ((!ACTIVE_PSD->useIconBox) || 
	    (P_ICON_BOX(pcd) == NULL))
	{
	    if (ICON_DECORATION(pcd) & ICON_IMAGE_PART)
	    {
		Dimension dheight, dwidth;

		dwidth = ICON_WIDTH(pcd) -
			2*ICON_EXTERNAL_SHADOW_WIDTH;
		if (ICON_DECORATION(pcd) & ICON_LABEL_PART)
		{
		    dheight =   ICON_IMAGE_HEIGHT(pcd) -
				ICON_EXTERNAL_SHADOW_WIDTH;
		}
		else
		{
		    dheight =   ICON_IMAGE_HEIGHT(pcd) -
				2*ICON_EXTERNAL_SHADOW_WIDTH;
		}
		if (wmGD.frameStyle == WmRECESSED)
		{
		    dheight -=	ICON_INTERNAL_SHADOW_WIDTH;
		}

		XmeClearBorder (DISPLAY, ICON_FRAME_WIN(pcd),
				ICON_EXTERNAL_SHADOW_WIDTH, 
				ICON_EXTERNAL_SHADOW_WIDTH,
				dwidth,
				dheight,
				ICON_IMAGE_TOP_PAD);
	    }

	    if (ICON_DECORATION(pcd) & ICON_LABEL_PART)
	    {
		XClearArea (DISPLAY, 
			    ICON_FRAME_WIN(pcd), 
			    0, 
			    ICON_IMAGE_HEIGHT(pcd),
			    (unsigned int) ICON_WIDTH(pcd), 
			    (unsigned int) ICON_HEIGHT(pcd), False);
	    }
	}
	else
	{
	    /*
	     * clear only area of real frame, not highlight area
	     */

	    XClearArea (DISPLAY, 
			    ICON_FRAME_WIN(pcd), 
			    IB_MARGIN_WIDTH,
			    IB_MARGIN_HEIGHT,
			    (unsigned int) ICON_WIDTH(pcd), 
			    (unsigned int) ICON_HEIGHT(pcd), False);
	}


	/* 
	 * Put up a big icon text label.
	 */

	if (ICON_DECORATION(pcd) & ICON_ACTIVE_LABEL_PART)
	    
	{
	    if (wmGD.activeIconTextDisplayed)
		PaintActiveIconText(pcd, True);
	    else
		ShowActiveIconText(pcd);
	}

	/* simulate exposure of window */
	IconExposureProc(pcd, False);
	UpdateOpenIconIndicator(pcd, False, 1);

    }

} /* END OF FUNCTION ShowActiveIcon */



/*************************************<->*************************************
 *
 *  ShowInactiveIcon (pcd, refresh)
 *
 *
 *  Description:
 *  -----------
 *  Make the icon appear "inactive"
 *
 *
 *  Inputs:
 *  ------
 *  pcd		- pointer to client data
 *
 *  refresh	- if True redraw the icon
 *
 *************************************<->***********************************/
void ShowInactiveIcon (ClientData *pcd, Boolean refresh)
{
    unsigned long attr_mask = 0;
    XSetWindowAttributes window_attribs;

    /* turn off the active icon text */
    if (ICON_DECORATION(pcd) & ICON_ACTIVE_LABEL_PART)
    {
	/* pass in screen to fix multiscreen bug [P3385] */
	HideActiveIconText(pcd->pSD);
    }
   
    if (ICON_FRAME_WIN(pcd))
    {
	/* 
	 * Use background pixmap if one is specified, otherwise set the
	 * appropriate background color. 
	 */

	if (ICON_APPEARANCE(pcd).backgroundPixmap)
	{
	    attr_mask |= CWBackPixmap;
	    window_attribs.background_pixmap = 
				ICON_APPEARANCE(pcd).backgroundPixmap;
	}
	else
	{
	    attr_mask |= CWBackPixel;
	    window_attribs.background_pixel = 
				ICON_APPEARANCE(pcd).background;
	}
	

	/* set active window attributes */
	XChangeWindowAttributes (DISPLAY, ICON_FRAME_WIN(pcd), attr_mask, 
				 &window_attribs);


	if (refresh)
	{
	    /* clear the frame to the right background */
	    if ((!ACTIVE_PSD->useIconBox) || 
	        (P_ICON_BOX(pcd) == NULL))
	    {
		XmeClearBorder (DISPLAY, ICON_FRAME_WIN(pcd),
				0, 0,
				ICON_WIDTH(pcd), ICON_IMAGE_HEIGHT(pcd), 4);

		XClearArea (DISPLAY, 
			    ICON_FRAME_WIN(pcd), 
			    0, ICON_IMAGE_HEIGHT(pcd),
			    (unsigned int) ICON_WIDTH(pcd), 
			    (unsigned int) ICON_HEIGHT(pcd), False);
	    }
	    else
	    {
	        /*
	         * clear only area of real frame, not highlight area
	         */

	        XClearArea (DISPLAY, 
			    ICON_FRAME_WIN(pcd), 
	    	    	    IB_MARGIN_WIDTH,
			    IB_MARGIN_HEIGHT,
			    (unsigned int) ICON_WIDTH(pcd), 
			    (unsigned int) ICON_HEIGHT(pcd), False);
	    }
	

        /* simulate exposure of window */
	    IconExposureProc(pcd, False);
	}

	UpdateOpenIconIndicator(pcd, False, 0);
    }

} /* END OF FUNTION ShowInactiveIcon  */



/*************************************<->*************************************
 *
 *  ReparentIconWindow (pcd, xOffset, yOffset)
 *
 *
 *  Description:
 *  -----------
 *  Reparent the icon window in the center of the image area
 *
 *
 *  Inputs:
 *  ------
 *  pcd 	- pointer to client data
 *  xOffset     - adjusts for icons in the iconBox
 *  yOffset     - adjusts for icons in the iconBox
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
void ReparentIconWindow (ClientData *pcd, int xOffset, int yOffset)
{
    int x, y, rpX, rpY;
    unsigned int width, height, bw, depth;
    Window root;
    XWindowChanges windowChanges;
    unsigned int mask;

    if (!pcd->pECD)
    {
    /*
     * Check if window size is too big
     */
    XGetGeometry (DISPLAY, pcd->iconWindow, &root, &x, &y, &width, &height, 
		  &bw, &depth);

    /*
     * strip off previous window border and set window geometry to 
     * fit inside icon frame
     */
    if (width != 0) {
	mask = CWBorderWidth;
	windowChanges.border_width = 0;
    }
    else
    {
	mask = 0;
    }

    if (width > ((ICON_IMAGE_MAXIMUM(pcd).width) +
		 ((wmGD.frameStyle == WmSLAB) ? 2 : 0)))
    {
	width = windowChanges.width = ICON_IMAGE_MAXIMUM(pcd).width +
				     ((wmGD.frameStyle == WmSLAB) ? 2 : 0);
	mask |= CWWidth;
    }
    else if (width < ICON_IMAGE_MINIMUM(pcd).width) {
	width = windowChanges.width = ICON_IMAGE_MINIMUM(pcd).width;
	mask |= CWWidth;
    }

    if (height > ((ICON_IMAGE_MAXIMUM(pcd).height) +
		 ((wmGD.frameStyle == WmSLAB) ? 2 : 0)))
    {
	height = windowChanges.height = ICON_IMAGE_MAXIMUM(pcd).height +
				     ((wmGD.frameStyle == WmSLAB) ? 2 : 0);
	mask |= CWHeight;
    }
    else if (height < ICON_IMAGE_MINIMUM(pcd).height) {
	height = windowChanges.height = ICON_IMAGE_MINIMUM(pcd).height;
	mask |= CWHeight;
    }

    if (mask)
	XConfigureWindow (DISPLAY, pcd->iconWindow, mask, &windowChanges);

    /*
     * Reparent the icon window to the center of the icon image frame
     */

    if (ICON_DECORATION(pcd) & ICON_LABEL_PART)
    {
        yOffset += ICON_INTERNAL_SHADOW_WIDTH;
    }

    rpX = ((IconContentWidth(pcd) - width)/2)    + xOffset;
    rpY = ((ICON_IMAGE_HEIGHT(pcd) - height)/2) + yOffset;




    XReparentWindow (DISPLAY, pcd->iconWindow, ICON_FRAME_WIN(pcd), rpX, rpY);
    pcd->clientFlags  |= ICON_REPARENTED;

    /*
     * Map the icon window when the icon frame is mapped.
     */
    } /* END if (!pcd->pECD) */
} /* END OF FUNCTION ReparentIconWindow */


/*************************************<->*************************************
 *
 *  PutBoxOnScreen (screen, px, py, width, height)
 *
 *
 *  Description:
 *  -----------
 *  Changes the position of the passed box so that it is all on screen
 *
 *
 *  Inputs:
 *  ------
 *  screen 	- screen we're talking about
 *  px		- pointer to x-coord
 *  py		- pointer to y-coord
 *  width	- width of box
 *  height	- height of box
 * 
 *  Outputs:
 *  -------
 *  *px		- new x-coord
 *  *py		- new y-coord
 *
 *
 *  Comments:
 *  --------
 * 
 *************************************<->***********************************/
void PutBoxOnScreen (int screen, int *px, int *py, unsigned int width, unsigned int height)
{
    /*
     * Place active label text nicely on screen
     */

    if (*px+width+1 > DisplayWidth (DISPLAY, screen))
	*px -= (*px+width+1) - DisplayWidth (DISPLAY, screen);

    if (*py+height+1 > DisplayHeight (DISPLAY, screen))
	*py -= (*py+height+1) - DisplayHeight (DISPLAY, screen);

    if (*px < 1) *px = 1;

    if (*py < 1) *py = 1;

} /* END OF FUNCTION PutBoxOnScreen */


/*************************************<->*************************************
 *
 *  PutBoxInIconBox (pCD, px, py, width, height)
 *
 *
 *  Description:
 *  -----------
 *  Changes the position of the passed box so that it is not
 *  clipped by the bulletin board
 *  
 *
 *
 *  Inputs:
 *  ------
 *  pCD		- pointer to client data
 *  px		- pointer to x-coord
 *  py		- pointer to y-coord
 *  width	- width of box
 *  height	- height of box
 * 
 *  Outputs:
 *  -------
 *  *px		- new x-coord
 *  *py		- new y-coord
 *
 *
 *  Comments:
 *  --------
 * 
 *************************************<->***********************************/
void PutBoxInIconBox (ClientData *pCD, int *px, int *py, unsigned int *width, unsigned int *height)
{

    int i;
    Arg getArgs[3];
    Dimension bBoardWidth;
    Dimension bBoardHeight;

    int clipWidth;
    int clipHeight;


    i=0;
    XtSetArg (getArgs[i], XmNwidth, (XtArgVal) &bBoardWidth ); i++;
    XtSetArg (getArgs[i], XmNheight, (XtArgVal) &bBoardHeight ); i++;
    XtGetValues (P_ICON_BOX(pCD)->bBoardWidget, getArgs, i);

    clipWidth = (int) bBoardWidth;
    clipHeight = (int) bBoardHeight;

    if (*px + *width-1 > clipWidth)
	*px -= (*px + *width-1) - clipWidth;

    if (*py + *height-1 > clipHeight)
	*py -= (*py + *height-1) - clipHeight;

    if (*px < 0) *px = 0;

    if (*py < 0) *py = 0;


} /* END OF FUNCTION PutBoxInIconBox */


/*************************************<->*************************************
 *
 *  CreateActiveIconTextWindow (pSD)
 *
 *
 *  Description:
 *  -----------
 *  creates the window that's popped up when an icon is activated
 *
 *  Inputs:
 *  ------
 *  pSD		- pointer to screen data
 * 
 *  Outputs:
 *  -------
 *
 *  Comments:
 *  --------
 * 
 *************************************<->***********************************/

void CreateActiveIconTextWindow (WmScreenData *pSD)
{
    XSetWindowAttributes window_attribs;
    unsigned long attr_mask;

    /* create active icon text window */
    attr_mask = CWEventMask| CWCursor;
    window_attribs.event_mask =  ExposureMask;
    window_attribs.cursor = wmGD.workspaceCursor;

    /* 
     * Use background pixmap if one is specified, otherwise set the
     * appropriate background color. 
     */

    if (pSD->iconAppearance.activeBackgroundPixmap)
    {
	attr_mask |= CWBackPixmap;
	window_attribs.background_pixmap = 
				pSD->iconAppearance.activeBackgroundPixmap;
    }
    else
    {
	attr_mask |= CWBackPixel;
	window_attribs.background_pixel = 
				pSD->iconAppearance.activeBackground;
    }
	

    pSD->activeIconTextWin = XCreateWindow (DISPLAY,
				       pSD->rootWindow,	/* parent */
				       0, 0,		/* x, y */
				       1, 1,		/* width, height */
				       0,		/* border width */
				       CopyFromParent,	/* depth */
				       InputOutput,	/* class */
				       CopyFromParent,	/* visual */
				       attr_mask,
				       &window_attribs);

    
    pSD->activeLabelParent = pSD->rootWindow;

} /* END OF FUNCTION CreateActiveIconTextWindow */



/*************************************<->*************************************
 *
 *  PaintActiveIconText (pcd, erase)
 *
 *
 *  Description:
 *  -----------
 *
 *
 *  Inputs:
 *  ------
 *  pcd		- pointer to client data
 *  erase	- if true, then erase the area before repainting
 * 
 *  Outputs:
 *  -------
 *
 *
 *  Comments:
 *  --------
 * 
 *************************************<->***********************************/
void PaintActiveIconText (ClientData *pcd, Boolean erase)
{
    XRectangle textBox;
    GC iconGC, topGC, botGC;

    if (!(ICON_DECORATION(pcd) & ICON_ACTIVE_LABEL_PART))
	return;

    /* get appropriate GCs */
    iconGC = ICON_APPEARANCE(pcd).activeGC;
    topGC = ICON_APPEARANCE(pcd).activeTopShadowGC;
    botGC = ICON_APPEARANCE(pcd).activeBottomShadowGC;

    /* draw shadowing */

    if (pActiveIconTopRects) {
	XFillRectangles (DISPLAY, 
			 pcd->pSD->activeIconTextWin,
			 topGC,
			 pActiveIconTopRects->prect,
			 pActiveIconTopRects->used);
    }

    if (pActiveIconBotRects) {
	XFillRectangles (DISPLAY,
			 pcd->pSD->activeIconTextWin,
			 botGC,
			 pActiveIconBotRects->prect,
			 pActiveIconBotRects->used);
    }

    /* paint the text */
    textBox.x = ICON_EXTERNAL_SHADOW_WIDTH;
    textBox.y = ICON_EXTERNAL_SHADOW_WIDTH;
    textBox.width = activeIconTextWidth - 2*ICON_EXTERNAL_SHADOW_WIDTH;
    textBox.height = activeIconTextHeight - 2*ICON_EXTERNAL_SHADOW_WIDTH;

    if (erase)
    {
	XClearArea (DISPLAY, pcd->pSD->activeIconTextWin, textBox.x, textBox.y,
		    (unsigned int) textBox.width, 
		    (unsigned int) textBox.height, 
		    FALSE);
    }

    WmDrawXmString(DISPLAY, pcd->pSD->activeIconTextWin, 
		   ICON_APPEARANCE(pcd).fontList,
		   ICON_DISPLAY_TITLE(pcd), iconGC,
		   textBox.x, textBox.y, textBox.width, &textBox, True);


} /* END OF FUNCTION PaintActiveIconText */


/*************************************<->*************************************
 *
 *  ShowActiveIconText (pcd)
 *
 *
 *  Description:
 *  -----------
 *
 *
 *  Inputs:
 *  ------
 * 
 *  Outputs:
 *  -------
 *
 *
 *  Comments:
 *  --------
 * 
 *************************************<->***********************************/
void ShowActiveIconText (ClientData *pcd)
{
    XWindowAttributes iconFrameAttribs;
    XSetWindowAttributes window_attribs;
    XWindowChanges windowChanges;
    unsigned int mask;
    int x, y; 
    unsigned int junk;
    Window root;
    Dimension dWidth, dHeight;


    /* 
     * put up a big icon text label
     */
    if (pcd->pSD->activeIconTextWin) {
	/* copy event mask from icon frame window */
	XGetWindowAttributes (DISPLAY, ICON_FRAME_WIN(pcd), &iconFrameAttribs);

	/* set attributes of window */
	window_attribs.event_mask =  iconFrameAttribs.your_event_mask;
	XChangeWindowAttributes (DISPLAY, pcd->pSD->activeIconTextWin,
				 CWEventMask, &window_attribs);

	/* set up geometry for the window */

	XmStringExtent (ICON_APPEARANCE(pcd).fontList, ICON_DISPLAY_TITLE(pcd),
			&dWidth, &dHeight);

	activeIconTextHeight =  (unsigned int) dHeight + 
	          WM_BOTTOM_TITLE_PADDING +
		  2*ICON_EXTERNAL_SHADOW_WIDTH;

	activeIconTextWidth = (unsigned int) dWidth;

	if (activeIconTextWidth < (1.2 * ICON_WIDTH(pcd))) 
	{
	    activeIconTextWidth = 1.2 * ICON_WIDTH(pcd);
	}

	activeIconTextWidth += 2*ICON_EXTERNAL_SHADOW_WIDTH;

	XGetGeometry (DISPLAY, 
			(Drawable) ICON_FRAME_WIN(pcd), 
			&root, &x, &y, 
		        &junk, &junk, &junk, &junk);


	y += ICON_IMAGE_HEIGHT(pcd);
	x -= (activeIconTextWidth - ICON_WIDTH(pcd))/2;



	if (!(P_ICON_BOX(pcd)))
	{
	    /* 
	     * This is a normal icon
	     */
	    PutBoxOnScreen (SCREEN_FOR_CLIENT(pcd), &x, &y, 
		    activeIconTextWidth, activeIconTextHeight);
	    if (ACTIVE_LABEL_PARENT(pcd) != root)
	    {
		XReparentWindow(DISPLAY, pcd->pSD->activeIconTextWin , 
				root, x, y );
		ACTIVE_LABEL_PARENT(pcd) = root;
	    }
	    
	}
	else
	{
	    /* 
	     * This is an icon in an icon box
	     */
	    x = x + IB_MARGIN_WIDTH;
	    y = y + IB_MARGIN_HEIGHT;

	    if(!(pcd->pSD->iconDecoration & ( ICON_LABEL_PART)))
	    {
		y -= activeIconTextHeight;
	    }

	    PutBoxInIconBox (pcd, &x, &y, 
				&activeIconTextWidth, &activeIconTextHeight);
	    if (ACTIVE_LABEL_PARENT(pcd) != pcd->client)
	    {
		XReparentWindow(DISPLAY, pcd->pSD->activeIconTextWin , 
		    XtWindow(P_ICON_BOX(pcd)->bBoardWidget),
		    x, y );
		ACTIVE_LABEL_PARENT(pcd) = pcd->client;
	    }
	}


	mask = CWX | CWY | CWWidth | CWHeight; 
	windowChanges.x = x;
	windowChanges.y = y;
	windowChanges.width = activeIconTextWidth;
	windowChanges.height = activeIconTextHeight;
	XConfigureWindow (DISPLAY, pcd->pSD->activeIconTextWin, mask, 
			  &windowChanges);

	/* bevel the rectangle around the edges */
	if ((pActiveIconTopRects && pActiveIconBotRects) || 
	    ((pActiveIconTopRects = 
		 AllocateRList((unsigned)4*ICON_EXTERNAL_SHADOW_WIDTH)) &&
	     (pActiveIconBotRects = 
		 AllocateRList((unsigned)4*ICON_EXTERNAL_SHADOW_WIDTH))))
	{
	    pActiveIconTopRects->used = 0;
	    pActiveIconBotRects->used = 0;
	    BevelRectangle (pActiveIconTopRects, 	
			    pActiveIconBotRects, 
			    0, 0, 
			    activeIconTextWidth, 
			    activeIconTextHeight,
			    ICON_EXTERNAL_SHADOW_WIDTH,
			    ICON_EXTERNAL_SHADOW_WIDTH,
			    ICON_EXTERNAL_SHADOW_WIDTH,
			    ICON_EXTERNAL_SHADOW_WIDTH);
	}

	XMapRaised (DISPLAY, pcd->pSD->activeIconTextWin);
	wmGD.activeIconTextDisplayed = True;

	/* save context for this window */
	XSaveContext (DISPLAY, pcd->pSD->activeIconTextWin, 
	    wmGD.windowContextType, (caddr_t) pcd);
    }
} /* END OF FUNCTION ShowActiveIconText */


/*************************************<->*************************************
 *
 *  HideActiveIconText ()
 *
 *
 *  Description:
 *  -----------
 *  Hides the big label shown over the active icon.
 *
 *
 *  Inputs:
 *  ------
 * 
 *  Outputs:
 *  -------
 *
 *
 *  Comments:
 *  --------
 * 
 *************************************<->***********************************/
void HideActiveIconText (WmScreenData *pSD)
{


    if ((pSD && pSD->activeIconTextWin) || ACTIVE_ICON_TEXT_WIN)
    {
	/* disassociate the big label window with this client */
	XDeleteContext (DISPLAY, 
			pSD 
			 ? pSD->activeIconTextWin
			 : ACTIVE_PSD->activeIconTextWin, 
			wmGD.windowContextType);

	/* hide the big label */
	XUnmapWindow (DISPLAY,
		      pSD 
		       ? pSD->activeIconTextWin
		       : ACTIVE_PSD->activeIconTextWin);
	wmGD.activeIconTextDisplayed = False;
    }
}


/*************************************<->*************************************
 *
 *  MoveActiveIconText (pcd)
 *
 *
 *  Description:
 *  -----------
 *
 *
 *  Inputs:
 *  ------
 * 
 *  Outputs:
 *  -------
 *
 *
 *  Comments:
 *  --------
 * 
 *************************************<->***********************************/
void MoveActiveIconText (ClientData *pcd)
{
    int x, y; 
    unsigned int junk;
    Window root;
    Dimension dWidth, dHeight;
    
    
    /* 
     * put up a big icon text label
     */
    if (pcd->pSD->activeIconTextWin && wmGD.activeIconTextDisplayed) {
	/* set up geometry for the window */

	XmStringExtent (ICON_APPEARANCE(pcd).fontList, ICON_DISPLAY_TITLE(pcd),
			&dWidth, &dHeight);

	activeIconTextHeight =  (unsigned int) dHeight +
	          WM_BOTTOM_TITLE_PADDING +
		  2 * ICON_EXTERNAL_SHADOW_WIDTH;
	
	activeIconTextWidth = (unsigned int) dWidth;

	if (activeIconTextWidth < (1.2 * ICON_WIDTH(pcd))) 
	{
	    activeIconTextWidth = 1.2 * ICON_WIDTH(pcd);
	}
	
	activeIconTextWidth += 2 * ICON_EXTERNAL_SHADOW_WIDTH;

	XGetGeometry (DISPLAY, 
			(Drawable) ICON_FRAME_WIN(pcd), 
			&root, &x, &y, 
		        &junk, &junk, &junk, &junk);

	
	y += ICON_IMAGE_HEIGHT(pcd);
        x -= (activeIconTextWidth - ICON_WIDTH(pcd))/2;

	if (!(P_ICON_BOX(pcd)))
	{
	    /* This is a normal icon */
	    PutBoxOnScreen (SCREEN_FOR_CLIENT(pcd), &x, &y, 
		activeIconTextWidth, activeIconTextHeight);
	}
	else 
	{
	    /* icon box */
	    x = x + IB_MARGIN_WIDTH;
            y = y + IB_MARGIN_HEIGHT;

	    if(!(pcd->pSD->iconDecoration & ( ICON_LABEL_PART)))
	    {
		y -= activeIconTextHeight;
	    }

	    PutBoxInIconBox (pcd, &x, &y, 
			     &activeIconTextWidth, &activeIconTextHeight);
	}
	
	XMoveWindow(DISPLAY, pcd->pSD->activeIconTextWin, x, y );

    }
}  /* END OF FUNCTION  MoveActiveIconText */
