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
/*                                                                      *
 * (c) Copyright 1993, 1994 Hewlett-Packard Company                     *
 * (c) Copyright 1993, 1994 International Business Machines Corp.       *
 * (c) Copyright 1993, 1994 Sun Microsystems, Inc.                      *
 * (c) Copyright 1993, 1994 Novell, Inc.                                *
 */

#include "TermHeader.h"
#include <stdint.h>
#include <wchar.h>
#include "TermPrimP.h"
#include "TermPrimDebug.h"
#include "TermPrimRenderP.h"
#include "TermPrimRenderFont.h"

static void
FontRenderFunction(
    Widget		  w,
    TermFont		  font,
    Pixel		  fg,
    Pixel		  bg,
    unsigned long	  flags,
    int			  x,
    int			  y,
    unsigned char	 *string,
    int			  len
)
{
    DtTermPrimitiveWidget tw = (DtTermPrimitiveWidget) w;
    struct termData *tpd = tw->term.tpd;
    XGCValues values;
    unsigned long valueMask;
    XFontStruct *fontStruct = (XFontStruct *) font->fontInfo;

    /* set the renderGC... */
    valueMask = (unsigned long) 0;

    /* set the foreground... */
    if (TermIS_SECURE(flags)) {
	if (tpd->renderGC.foreground != bg) {
	    tpd->renderGC.foreground = bg;
	    values.foreground = bg;
	    valueMask |= GCForeground;
	}
    } else {
	if (tpd->renderGC.foreground != fg) {
	    tpd->renderGC.foreground = fg;
	    values.foreground = fg;
	    valueMask |= GCForeground;
	}
    }

    /* set background... */
    if (tpd->renderGC.background != bg) {
	tpd->renderGC.background = bg;
	values.background = bg;
	valueMask |= GCBackground;
    }

    /* set the font for renderGC if necessary */
    if (tpd->renderGC.fid != fontStruct->fid) {
	tpd->renderGC.fid = fontStruct->fid;
	values.font = fontStruct->fid;
	valueMask |= GCFont;
    }

    if (valueMask) {
	(void) XChangeGC(XtDisplay(w), tpd->renderGC.gc, valueMask,
		&values);
    }

    /* draw image string a line of text... */
    if (isDebugFSet('t', 1)) {
#ifdef	BBA
#pragma BBA_IGNORE
#endif	/*BBA*/
	/* Fill in the text area so we can see what is going to
	 * be displayed...
	 */
	(void) XFillRectangle(XtDisplay(w),
		XtWindow(w),
		tpd->renderGC.gc,
		x,
		y,
		tpd->cellWidth * len,
		tpd->cellHeight);
	(void) XSync(XtDisplay(w), False);
	(void) shortSleep(100000);
    }
			
    if (len <= 0 || !fontStruct)
	return;

    if (tpd->mbCurMax > 1) {
	/* In multibyte/UTF-8 locales (TermPrimRenderMb.c), `string` points to
	 * `len` wide characters (`wchar_t`), never single-byte `char`s.
	 * Passing `(char *)string` directly to XDrawImageString reads raw
	 * UTF-32 bytes (e.g. 'k', 0x00, 0x00, 0x00), rendering three NUL
	 * replacement boxes between every character. Convert each `wchar_t`
	 * cleanly for either 2-byte ISO10646-1 or 1-byte ISO8859-1 XFontStructs.
	 */
	const wchar_t *wsrc = (const wchar_t *) string;
	if (fontStruct->max_byte1 > 0) {
	    XChar2b stack16[256];
	    XChar2b *buf16 = (size_t)len <= (sizeof(stack16) / sizeof(stack16[0]))
		? stack16
		: (XChar2b *) XtMalloc((size_t)len * sizeof(XChar2b));
	    for (int i = 0; i < len; i++) {
		uint32_t uc = (uint32_t) wsrc[i];
		if (uc < 0x20u)
		    uc = 0x20u;
		else if (uc > 0xFFFFu)
		    uc = (uint32_t) '?';
		buf16[i].byte1 = (unsigned char) ((uc >> 8) & 0xFFu);
		buf16[i].byte2 = (unsigned char) (uc & 0xFFu);
	    }
	    (void) XDrawImageString16(XtDisplay(w),
		    XtWindow(w),
		    tpd->renderGC.gc,
		    x,
		    y + fontStruct->ascent,
		    buf16,
		    len);
	    if (TermIS_OVERSTRIKE(flags)) {
		(void) XDrawString16(XtDisplay(w),
			XtWindow(w),
			tpd->renderGC.gc,
			x + 1,
			y + fontStruct->ascent,
			buf16,
			len);
	    }
	    if (buf16 != stack16)
		XtFree((char *) buf16);
	} else {
	    char stack8[256];
	    char *buf8 = (size_t)len <= sizeof(stack8)
		? stack8
		: (char *) XtMalloc((size_t)len);
	    for (int i = 0; i < len; i++) {
		uint32_t uc = (uint32_t) wsrc[i];
		if (uc < 0x20u)
		    buf8[i] = ' ';
		else if (uc <= 0xFFu)
		    buf8[i] = (char) (unsigned char) uc;
		else
		    buf8[i] = '?';
	    }
	    (void) XDrawImageString(XtDisplay(w),
		    XtWindow(w),
		    tpd->renderGC.gc,
		    x,
		    y + fontStruct->ascent,
		    buf8,
		    len);
	    if (TermIS_OVERSTRIKE(flags)) {
		(void) XDrawString(XtDisplay(w),
			XtWindow(w),
			tpd->renderGC.gc,
			x + 1,
			y + fontStruct->ascent,
			buf8,
			len);
	    }
	    if (buf8 != stack8)
		XtFree(buf8);
	}
    } else {
	(void) XDrawImageString(XtDisplay(w),	/* Display		*/
		XtWindow(w),			/* Drawable		*/
		tpd->renderGC.gc,		/* GC			*/
		x,				/* x			*/
		y + fontStruct->ascent,		/* y			*/
		(char *) string,		/* string		*/
		len);				/* length		*/

	/* handle overstrike... */
	if (TermIS_OVERSTRIKE(flags)) {
	    (void) XDrawString(XtDisplay(w),	/* Display		*/
		    XtWindow(w),		/* Drawable		*/
		    tpd->renderGC.gc,		/* GC			*/
		    x + 1,			/* x			*/
		    y + fontStruct->ascent,	/* y			*/
		    (char *) string,		/* string		*/
		    len);			/* length		*/
	}
    }

    /* handle the underline enhancement... */
    /* draw the underline... */
    if (TermIS_UNDERLINE(flags)) {
	XDrawLine(XtDisplay(w),			/* Display		*/
		XtWindow(w),			/* Window		*/
		tpd->renderGC.gc,		/* GC			*/
		x,				/* X1			*/
		y + tpd->cellHeight - 1,		/* Y1			*/
		x + len * tpd->cellWidth,	/* X2			*/
		y + tpd->cellHeight - 1);	/* Y2			*/
    }
}

static void
FontDestroyFunction(
    Widget		  w,
    TermFont		  font
)
{
    (void) XtFree((char *) font);
}

static void
FontExtentsFunction(
    Widget		  w,
    TermFont		  font,
    unsigned char	 *string,
    int			  len,
    int			 *widthReturn,
    int			 *heightReturn,
    int			 *ascentReturn
)
{
    DtTermPrimitiveWidget tw = (DtTermPrimitiveWidget) w;
    XFontStruct *fontStruct = (XFontStruct *) font->fontInfo;

    if (widthReturn) {
	*widthReturn = len * tw->term.widthInc;
    }
    if (heightReturn) {
	*heightReturn = fontStruct->ascent + fontStruct->descent;
    }
    if (ascentReturn) {
	*ascentReturn = fontStruct->ascent;
    }
    return;
}

TermFont
_DtTermPrimRenderFontCreate(
    Widget		  w,
    XFontStruct		 *fontStruct
)
{
    TermFont termFont;

    termFont = (TermFont) XtMalloc(sizeof(TermFontRec));
    termFont->renderFunction = FontRenderFunction;
    termFont->destroyFunction = FontDestroyFunction;
    termFont->extentsFunction = FontExtentsFunction;
    termFont->fontInfo = (XtPointer) fontStruct;

    return(termFont);
}
