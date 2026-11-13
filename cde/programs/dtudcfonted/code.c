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
/* code.c 1.37 - Fujitsu source for CDEnext    96/10/30 13:13:47      */
/* $XConsortium: code.c /main/6 1996/11/08 01:57:21 cde-fuj $ */
/*
 *  (c) Copyright 1995 FUJITSU LIMITED
 *  This is source code modified by FUJITSU LIMITED under the Joint
 *  Development Agreement for the CDEnext PST.
 *  This is unpublished proprietary source code of FUJITSU LIMITED
 */
#include	<stdio.h>
#include	<string.h>
#include	<X11/keysym.h>
#include 	<Xm/Xm.h>
#include 	<Xm/Label.h>
#include 	<Xm/List.h>
#include 	<Xm/RowColumn.h>
#include 	<Xm/SeparatoG.h>
#include        <Xm/Text.h>
#include        <Xm/PushB.h>
#include        <Xm/Frame.h>
#include        <Xm/Form.h>
#include	<Xm/MwmUtil.h>
#include	<X11/Intrinsic.h>
#include	"falfont.h"
#include	"xoakufont.h"

#define NUM 0x80

static Display *display=nullptr;
static int     screen;
static Boolean open_font;
static Widget toggle[NUM];
static Widget text;
static Window BaseW;
static Widget set_widget;
static Widget code_w=0;
static int font_w, font_h;
static unsigned long fg, bg;
static unsigned char *ptn, *clr;
static Pixmap no_pix;
static int len, code1;
static char *FontName=nullptr;
static FalFontID	fid;

extern FalFontID font_id;
extern int CodePoint;
extern Resource resource;

static void
OpenFont(void)
{
    int			mask;
    FalFontData     font_data;
    static FalFontDataList	*copylist;
    extern FalFontData  fullFontData;

    if(strcmp(FontName, fullFontData.xlfdname) == 0) {
	fid = font_id;
        return;
    }
    mask =  FAL_FONT_MASK_XLFDNAME | FAL_FONT_MASK_DEFINED |
					FAL_FONT_MASK_UNDEFINED;
    font_data.xlfdname = FontName;
    if (! CodePoint) {
	mask |= FAL_FONT_MASK_GLYPH_INDEX;
    }

    fid = FalOpenSysFont(&font_data, mask, &copylist);
}

static void
CloseFont(void)
{
    if (fid != font_id) {
        FalCloseFont(fid);
	fid = 0;
    }
}

static void
DrawCode(Widget w, int low_code)
{
    int hi_code;
    char *f = nullptr;
    XImage image;
    GC gc = XDefaultGC(display, 0);

    hi_code = (code1 & 0xff) << 8;
    image.width = font_w;
    image.height = font_h;
    image.xoffset = 0;
    image.format = XYBitmap;
    image.byte_order = LSBFirst;
    image.bitmap_unit = 8;
    image.bitmap_bit_order = MSBFirst;
    image.bitmap_pad = 8;
    image.depth = 1;
    image.bytes_per_line = (font_w - 1) / 8 + 1;
    XSetForeground(display, gc, fg);
    XSetBackground(display, gc, bg);

    if (open_font) {
	f = (char *)FalReadFont(fid, hi_code | low_code, font_w, font_h);
	if(f == nullptr || f == (char *)FAL_ERROR) {
	    return;
	}
	if( EXISTS_FLAG == 0 ) {
	    memcpy(ptn, f, len);
	    image.data = (char *)ptn;
	} else {
	    image.data = (char *)clr;
	}
	XPutImage(display, XtWindow(toggle[low_code]), gc, &image, 0, 0, 6, 6, font_w, font_h);
    } else {
	if (ptnGet(hi_code | low_code, ptn) == 0) {
	    image.data = (char *)ptn;
	} else {
	    image.data = (char *)clr;
	}
	XPutImage(display, XtWindow(toggle[low_code]), gc, &image, 0, 0, 6, 6, font_w, font_h);
    }
}

static void
SetPixmap(start_code)
{
    int hi_code, low_code;
    char *f = nullptr;
    XImage image;
    GC gc = XDefaultGC(display, 0);

    hi_code = (start_code & 0xff) << 8;
    image.width = font_w;
    image.height = font_h;
    image.xoffset = 0;
    image.format = XYBitmap;
    image.byte_order = LSBFirst;
    image.bitmap_unit = 8;
    image.bitmap_bit_order = MSBFirst;
    image.bitmap_pad = 8;
    image.depth = 1;
    image.bytes_per_line = (font_w - 1) / 8 + 1;
    XSetForeground(display, gc, fg);
    XSetBackground(display, gc, bg);

    if (open_font) {
	for (low_code=0; low_code < NUM; low_code++) {
	    f = (char *)FalReadFont(fid, hi_code | low_code, font_w, font_h);
	    if(f == nullptr || f == (char *)FAL_ERROR) {
		return;
	    }
	    if( EXISTS_FLAG == 0 ) {
		memcpy(ptn, f, len);
		image.data = (char *)ptn;
	    } else {
		image.data = (char *)clr;
	    }
	    XPutImage(display, XtWindow(toggle[low_code]), gc, &image, 0, 0, 6, 6, font_w, font_h);
	}
    } else {
	for (low_code=0; low_code < NUM; low_code++) {
	    if (ptnGet(hi_code | low_code, ptn) == 0) {
		image.data = (char *)ptn;
	    } else {
		image.data = (char *)clr;
	    }
	    XPutImage(display, XtWindow(toggle[low_code]), gc, &image, 0, 0, 6, 6, font_w, font_h);
	}
    }
}

static void
Code1Call(Widget w, int start_code, XmListCallbackStruct *data)
{
    code1 = start_code + data->item_position -1;
    SetPixmap(code1);
}

static void
Code2Call(Widget w, int code2, XmPushButtonCallbackStruct *data)
{
    char asc[16];
    sprintf(asc, "%2.2X%2.2X", code1, code2);
    XtVaSetValues(text, XmNvalue, asc, nullptr);
    DrawCode(w, code2);
}

static void
PrevPage(Widget w, Widget scroll)
{
    int *list;
    int num;
    if (XmListGetSelectedPos(scroll, &list, &num)) {
        if(list[0] > 1) {
	    XmListSelectPos(scroll, list[0] - 1, True);
	    XmListSetPos(scroll, list[0] - 1);
	}
    }
}

static void
NextPage(Widget w, Widget scroll)
{
    int *list;
    int num, item;
    XtVaGetValues(scroll, XmNitemCount, &item, nullptr);
    if (XmListGetSelectedPos(scroll, &list, &num)) {
        if(list[0] < item) {
	    XmListSelectPos(scroll, list[0] + 1, True);
	    XmListSetBottomPos(scroll, list[0] + 1);
	}
    }
}

static void
Cancel(void)
{
    XtUnmapWidget(XtParent(code_w));
}

static void
Apply(void)
{
    char *asc;
    XtVaGetValues(text, XmNvalue, &asc, nullptr);
    XtVaSetValues(set_widget, XmNvalue, asc, nullptr);
    XtFree(asc);
    Cancel();
}

static void
CreateItem(XmString *item, int *item_count, int *start_code)
{
    int count;
    int start, end;
    char str[5];
    FalFontinfo         finfo;
    extern int begin_code, last_code;

    if (open_font) {
	if (FalQueryFont(fid, &finfo) == FAL_ERROR) {
            *item_count = 0;
	    CloseFont();
	    return;
	}
	font_w = finfo.width;
	font_h = finfo.height;
	start = (finfo.top & 0xff00) >> 8;
	end = (finfo.bottom & 0xff00) >> 8;
    } else {
	font_w = edg.width;
	font_h = edg.height;
	start = (begin_code & 0xff00) >> 8;
	end = (last_code & 0xff00) >> 8;
    }
    if (CodePoint) {
	if (start < 0x80)
	    start = 80;
	if (end < 0x80)
	    start = 80;
    } else {
	if (start > 0x7f)
	    start = 0x7f;
	if (end > 0x7f)
	    end = 0x7f;
    }
    *start_code = start;
    for (count=0;start <= end; start++, count++) {
	sprintf(str, "%X", start);
	item[count] = XmStringCreateLocalized(str);
    }
    *item_count = count;
}

static Widget
CreateCodeWindow(Widget w)
{
    Widget top, base1, base2, base3, base4;
    Widget frame, scroll, label, sep, form;
    Widget push1, push2, push3, push4;
    Arg args[16];
    int n, i;
    XmString item[128];
    XmString xcs;
    int item_count, start_code, add_c;
    char add[9];

    n = 0;
    XtSetArg(args[n], XmNmwmFunctions, MWM_FUNC_ALL | MWM_FUNC_CLOSE |
					MWM_FUNC_RESIZE); n++;
    XtSetArg(args[n], XmNmwmDecorations, MWM_DECOR_ALL); n++;
    top = XmCreateFormDialog(w, "code", args, n);

    base1 = XtVaCreateManagedWidget("base1", xmRowColumnWidgetClass, top,
					XmNorientation, XmVERTICAL, nullptr);

    frame = XtVaCreateManagedWidget("frame", xmFrameWidgetClass, base1,
					nullptr);

    base2 = XtVaCreateManagedWidget("base2", xmRowColumnWidgetClass, frame,
					XmNorientation, XmHORIZONTAL, nullptr);

    CreateItem(item, &item_count, &start_code);
    code1 = start_code;
    n = 0 ;
    XtSetArg(args[n], XmNitems, item); n++;
    XtSetArg(args[n], XmNitemCount, item_count); n++;
    XtSetArg(args[n], XmNscrollBarDisplayPolicy, XmSTATIC); n++;
    scroll = XmCreateScrolledList(base2, "scroll", args, n);
    XtManageChild(scroll);
    XtAddCallback(scroll, XmNbrowseSelectionCallback,
			(XtCallbackProc)Code1Call, (XtPointer) (intptr_t) start_code);
    XtAddCallback(scroll, XmNdefaultActionCallback,
			(XtCallbackProc)Code1Call, (XtPointer) (intptr_t) start_code);
    XmListSelectPos(scroll, 1, False);

    base3 = XtVaCreateManagedWidget("base3", xmRowColumnWidgetClass, base2,
					XmNorientation, XmHORIZONTAL,
					XmNpacking, XmPACK_COLUMN,
					XmNradioAlwaysOne, True,
					XmNradioBehavior , True,
					XmNmarginHeight, 0,
					XmNmarginWidth, 0,
					XmNspacing, 0,
					XmNnumColumns, 9, nullptr);


    len = (font_w / 8 + 1) * font_h;
    ptn = (unsigned char *) malloc(len);
    clr = (unsigned char *) malloc(len);
    memset(clr, 0, len);
    XtVaGetValues(w, XmNforeground, &fg, XmNbackground, &bg, nullptr);
    no_pix = XCreatePixmapFromBitmapData(display, BaseW, (char *)clr,
		font_w, font_h, fg, bg, DefaultDepth(display, screen));

    XtVaCreateManagedWidget("", xmLabelWidgetClass, base3, nullptr);
    for (i=0; i < 16; i++) {
	sprintf(add, "%2.1X", i);
	XtVaCreateManagedWidget(add, xmLabelWidgetClass, base3, nullptr);
    }
    if (CodePoint)
	add_c = 8;
    else
	add_c = 0;
    for (i=0; i < NUM; i++) {
	if ((i % 16) == 0) {
	    snprintf(add, sizeof(add), "%2.1X", add_c++);
	    XtVaCreateManagedWidget(add, xmLabelWidgetClass, base3, nullptr);
	}
	toggle[i] = XtVaCreateWidget("toggle",
					xmPushButtonWidgetClass, base3,
					XmNwidth, font_w,
					XmNheight, font_h,
					XmNlabelType, XmPIXMAP,
					XmNlabelPixmap, no_pix, nullptr);
	XtAddCallback(toggle[i], XmNactivateCallback,
				(XtCallbackProc)Code2Call, (XtPointer) (intptr_t) i);
	XtAddCallback(toggle[i], XmNarmCallback,
				(XtCallbackProc)DrawCode, (XtPointer) (intptr_t) i);
	XtAddCallback(toggle[i], XmNdisarmCallback,
				(XtCallbackProc)DrawCode, (XtPointer) (intptr_t) i);
	XtAddEventHandler(toggle[i], ExposureMask, False,
				(XtEventHandler)DrawCode, (XtPointer) (intptr_t) i);
    }
    XtManageChildren(toggle, NUM);

    base4 = XtVaCreateManagedWidget("base4", xmFormWidgetClass, base1, nullptr);

    xcs = XmStringCreateLocalized(resource.l_code);
    label = XtVaCreateManagedWidget("code", xmLabelWidgetClass, base4,
					XmNlabelString, xcs,
					XmNleftAttachment, XmATTACH_POSITION,
					XmNleftPosition, 30, nullptr);

    text = XtVaCreateManagedWidget("text", xmTextWidgetClass, base4,
					XmNeditable, False,
					XmNcursorPositionVisible, False,
					XmNcolumns, 4,
					XmNleftAttachment, XmATTACH_WIDGET,
					XmNleftWidget, label, nullptr);

    sep = XtVaCreateManagedWidget("sep", xmSeparatorGadgetClass, base1,
					XmNorientation, XmHORIZONTAL, nullptr);

    form = XtVaCreateManagedWidget("sep", xmFormWidgetClass, base1, nullptr);

    xcs = XmStringCreateLocalized(resource.previous);
    push1 = XtVaCreateManagedWidget("PreviousPage", xmPushButtonWidgetClass, form,
					XmNleftAttachment, XmATTACH_POSITION,
					XmNlabelString, xcs,
					XmNleftPosition, 10, nullptr);
    XtAddCallback(push1, XmNactivateCallback,
				(XtCallbackProc)PrevPage, (XtPointer)scroll);
    XmStringFree(xcs);

    xcs = XmStringCreateLocalized(resource.next);
    push2 = XtVaCreateManagedWidget("NextPage", xmPushButtonWidgetClass, form,
					XmNleftAttachment, XmATTACH_POSITION,
					XmNlabelString, xcs,
					XmNleftPosition, 35, nullptr);
    XtAddCallback(push2, XmNactivateCallback,
				(XtCallbackProc)NextPage, (XtPointer)scroll);
    XmStringFree(xcs);

    xcs = XmStringCreateLocalized(resource.apply);
    push3 = XtVaCreateManagedWidget("Apply", xmPushButtonWidgetClass, form,
					XmNleftAttachment, XmATTACH_POSITION,
					XmNlabelString, xcs,
					XmNleftPosition, 60, nullptr);
    XtAddCallback(push3, XmNactivateCallback, (XtCallbackProc)Apply, nullptr);
    XmStringFree(xcs);

    xcs = XmStringCreateLocalized(resource.l_cancel);
    push4 = XtVaCreateManagedWidget("Cancel", xmPushButtonWidgetClass, form,
					XmNleftAttachment, XmATTACH_POSITION,
					XmNlabelString, xcs,
					XmNleftPosition, 80, nullptr);
    XtAddCallback(push4, XmNactivateCallback, (XtCallbackProc)Cancel, nullptr);
    XmStringFree(xcs);

    return(top);
}

void
CodeWindow(Widget widget, char *font_name, Boolean load_font)
{
    static Boolean	old_load_font;
    extern int	CodePoint;

    set_widget = widget;

    BaseW = XtWindow(widget);
    display = XtDisplay(widget);
    screen  = DefaultScreen(display);
    if (FontName == nullptr || strcmp(font_name, FontName) ||
				load_font != old_load_font) {
	old_load_font = load_font;
	if (code_w) {
	    XtDestroyWidget(code_w);
	    code_w = 0;
	    free(ptn);
	    free(clr);
	    if (fid != 0) {
		CloseFont();
	    }
	}
	if (FontName)
	    free(FontName);
	FontName = (char *)malloc(strlen(font_name)+1);
	strcpy(FontName, font_name);

	if (load_font) {
	    OpenFont();
	    if (fid == 0)
	        return;
	    open_font = True;
	} else {
	    open_font = False;
	}

	code_w = CreateCodeWindow(widget);
        XtManageChild(code_w);
    } else {
        XtMapWidget(XtParent(code_w));
    }
}
