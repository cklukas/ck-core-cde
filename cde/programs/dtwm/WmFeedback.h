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
 * (c) Copyright 1989, 1990, 1991, 1992 OPEN SOFTWARE FOUNDATION, INC. 
 * ALL RIGHTS RESERVED 
*/ 
/* 
 * Motif Release 1.2
*/ 
/*
 * (c) Copyright 1987, 1988, 1989, 1990 HEWLETT-PACKARD COMPANY */

#ifndef WM_FEEDBACK_H
#define WM_FEEDBACK_H


extern void ConfirmAction (WmScreenData *pSD, int nbr);
extern void HideFeedbackWindow (WmScreenData *pSD);
extern void InitCursorInfo (void);
extern void PaintFeedbackWindow (WmScreenData *pSD);
extern void ShowFeedbackWindow (WmScreenData *pSD, int x, int y, 
				unsigned int width, unsigned int height, 
				unsigned long style);
extern void EnterWaitState(void);
extern void LeaveWaitState(void);
extern void UpdateFeedbackInfo (WmScreenData *pSD, int x, int y, 
				unsigned int width, unsigned int height);
extern void UpdateFeedbackText (WmScreenData *pSD, int x, int y, 
				unsigned int width, unsigned int height);
extern Boolean FeedbackUpdateSnapHover (WmScreenData *pSD, int rootX, int rootY);
extern Boolean FeedbackGetSnapGeometry (WmScreenData *pSD, int rootX, int rootY,
				int *pX, int *pY, unsigned int *pWidth,
				unsigned int *pHeight);
extern int FeedbackGetSnapAction (WmScreenData *pSD, int rootX, int rootY);
extern void StartTaskSwitcher (WmScreenData *pSD, Time time, int direction);
extern void AdvanceTaskSwitcher (WmScreenData *pSD, int direction);
extern void FinishTaskSwitcher (WmScreenData *pSD, Time time, Boolean activate);
extern void PaintTaskSwitcher (WmScreenData *pSD);
extern void TaskSwitcherIconUpdated (WmScreenData *pSD, ClientData *pCD);
extern Boolean TaskSwitcherActive (WmScreenData *pSD);
extern Boolean HandleTaskSwitcherButtonRelease (WmScreenData *pSD, XButtonEvent *event);
extern Boolean HandleTaskSwitcherMotion (WmScreenData *pSD, XMotionEvent *event);
extern void HandleTaskSwitcherExpose (WmScreenData *pSD, XExposeEvent *event);
extern void TaskSwitcherActivateSelection (WmScreenData *pSD, ClientData *pCD, Time time);
extern Boolean TaskSwitcherPointerInWindow (WmScreenData *pSD);
extern void TaskSwitcherSetPinnedAlt (Boolean active);
extern Boolean HandleTaskSwitcherButtonPress (WmScreenData *pSD, XButtonEvent *event);
extern void TaskSwitcherLog (const char *fmt, ...);

enum {
    FB_SNAP_NONE = 0,
    FB_SNAP_MAXIMIZE,
    FB_SNAP_MINIMIZE,
    FB_SNAP_HALF_LEFT,
    FB_SNAP_HALF_RIGHT,
    FB_SNAP_HALF_TOP,
    FB_SNAP_HALF_BOTTOM,
    FB_SNAP_QUAD_TL,
    FB_SNAP_QUAD_TR,
    FB_SNAP_QUAD_BL,
    FB_SNAP_QUAD_BR
};

#endif /* WM_FEEDBACK_H */
