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
/*******************************************************************************
**
**  dayglance.c
**
**  $XConsortium: dayglance.c /main/10 1996/11/21 19:42:19 drk $
**
**  RESTRICTED CONFIDENTIAL INFORMATION:
**
**  The information in this document is subject to special
**  restrictions in a confidential disclosure agreement between
**  HP, IBM, Sun, USL, SCO and Univel.  Do not distribute this
**  document outside HP, IBM, Sun, USL, SCO, or Univel without
**  Sun's specific written approval.  This document and all copies
**  and derivative works thereof must be returned or destroyed at
**  Sun's request.
**
**  Copyright 1993 Sun Microsystems, Inc.  All rights reserved.
**
*******************************************************************************/

/*									*
 * (c) Copyright 1993, 1994 Hewlett-Packard Company			*
 * (c) Copyright 1993, 1994 International Business Machines Corp.	*
 * (c) Copyright 1993, 1994 Sun Microsystems, Inc.			*
 * (c) Copyright 1993, 1994 Novell, Inc. 				*
 */

#ifndef lint
static char sccsid[] = "@(#)dayglance.c 1.76 95/04/24 Copyr 1991 Sun Microsystems, Inc.";
#endif

#include <EUSCompat.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <X11/cursorfont.h>
#include <Xm/Xm.h>
#include <Xm/Form.h>
#include <Xm/Text.h>
#include <Xm/ToggleBG.h>
#include <Dt/HourGlass.h>
#include "util.h"
#include "stdlib.h"
#include "calendar.h"
#include "timeops.h"
#include "datefield.h"
#include "props.h"
#include "help.h"
#include "x_graphics.h"
#include "format.h"
#include "weekglance.h"
#include "monthglance.h"
#include "yearglance.h"
#include "MonthPanel.h"
#include "getdate.h"
#include "select.h"
#include "dayglance.h"
#include "editor.h"
#include "todo.h"
#include "print.h"
#include "group_editor.h"
#include "ics_io.h"
#include "ux_helpers.h"
#include "context_menu.h"
#include "undo_stack.h"
#include "agenda_ui.h"
#include "overlay_store.h"

#define XOS_USE_XT_LOCKING
#define X_INCLUDE_TIME_H
#if defined(__linux__)
#undef SVR4
#endif
#include <X11/Xos_r.h>

extern int debug;


/* private function prototypes */
static Boolean _print_day(Calendar*, int, void *,
			  Tick, Props*, Boolean);
static void day_btn_cb(Widget, XtPointer, XtPointer);
static void create_month_panels(Calendar *);
static void display_monthpanels(Calendar *);
static void update_quarter(Calendar *);
static Boolean in_moboxes(Calendar *, int, int);

#define INSIDE_MARGIN 6

extern int
dtcm_day_mobox_width(Calendar *c)
{
	if (!c || !c->view)
		return 240;
	int winw = c->view->winw;
	int def_w = (int)(winw * 0.4);
	Day *d = (Day *)c->view->day_info;
	if (!d || d->split_x <= 0)
		return def_w;
	int min_w = 180;
	int max_w = (winw > 420) ? (winw - 220) : def_w;
	if (max_w < min_w)
		return def_w;
	if (d->split_x < min_w) return min_w;
	if (d->split_x > max_w) return max_w;
	return d->split_x;
}

extern void
paint_day_header(Calendar *c, Tick date, void *rect)
{
	Props *p = (Props*)c->properties;
	OrderingType ot = get_int_prop(p, CP_DATEORDERING);
	int pfy, x;
	char buf[100];
	Boolean inrange = False;
	Colormap cmap;
	Pixel foreground_pixel;
	XFontSetExtents fontextents;
	struct tm *tm;
	_Xltimeparams localtime_buf;

	tm = _XLocaltime(&date, localtime_buf);

	CalFontExtents(c->fonts->labelfont, &fontextents);
	pfy = fontextents.max_logical_extent.height;
        XtVaGetValues(c->canvas, XmNcolormap, &cmap, nullptr);
        XtVaGetValues(c->canvas, XmNforeground, &foreground_pixel, nullptr);
        buf [0] = '\0';
	/* NL_COMMENT
	   Attention Translator:

	   This string is used in the calendar day view.  In the C locale
	   it has the form:

		Monday, January 16, 1995

	   strftime conversion string: "%A, %B %e, %Y" is used.  

	   Use the appropriate strftime conversion for your locale.
	*/
	strftime(buf, 100, CATGETS(c->DT_catd, 1, 992, "%A, %B %e, %Y"), tm);
	inrange = today_inrange(c, date);
	x = gr_center(c->view->winw-(int)MOBOX_AREA_WIDTH, buf,
			c->fonts->labelfont) + (int)MOBOX_AREA_WIDTH;
	if (c->xcontext->screen_depth >= 8 && inrange) 
        	gr_text_rgb(c->xcontext, x, c->view->topoffset - (pfy/2),
                        c->fonts->labelfont, buf, foreground_pixel, 
			cmap, rect);
	else
        	gr_text(c->xcontext, x, c->view->topoffset - (pfy/2),
                        c->fonts->labelfont, buf, rect);
}
	
extern int
morning(int hr)
{
	return(hr<12);
}

static void
paint_dayview_appts(Calendar *c, Paint_cache *cache, int a_total, void *rect)
{
	int w = c->view->boxw;
	int h = c->view->boxh;
	int begin_time, end_time;
	int x, x2, y, y2, num_hrs, i, last_hr, hr, x_off;
	Cal_Font *pf = c->fonts->boldfont;
	Cal_Font *pf2 = c->fonts->viewfont;
	XFontSetExtents fontextents;
	XFontSetExtents fontextents2;
	Props *p = (Props*)c->properties;
	Boolean am = True;
	char buf[16], *appt_str;
	int pfy, curr_line, maxlines;
	Lines *lines = nullptr, *headlines = nullptr;
	DisplayType disp_t;
	Colormap cmap;
	Pixel fg;
	Tick start_tick, end_tick;
	int	nop, hrbox_margin;

	CalFontExtents(pf, &fontextents);
	CalFontExtents(pf2, &fontextents2);


	XtVaGetValues(c->canvas, XmNcolormap, &cmap, XmNforeground, &fg, nullptr);

	/* draw horizontal lines */
	begin_time = get_int_prop(p, CP_DAYBEGIN);
	end_time = get_int_prop(p, CP_DAYEND);
	disp_t = get_int_prop(p, CP_DEFAULTDISP);
	num_hrs = end_time - begin_time + 1;

	if (disp_t == HOUR12)
		CalTextExtents(pf, "12pm", 4, &nop, &nop, &hrbox_margin, &nop);
	else
		CalTextExtents(pf, "24 ", 3, &nop, &nop, &hrbox_margin, &nop);

	x = MOBOX_AREA_WIDTH+2;
	x2 = x + w;
	y = c->view->topoffset;
	for (i = 0; i <= num_hrs; i++) {
		gr_draw_line(c->xcontext, x, y, x2, y, gr_solid, rect);
		y += h;
	}
	/* draw vertical line */
	y = c->view->topoffset;
	y2 = y + num_hrs * h;
	x += hrbox_margin;
	gr_draw_line(c->xcontext, x, y, x, y2, gr_solid, rect);

	x = MOBOX_AREA_WIDTH+3;
	y += h/2+4;

	/* draw in hours */
	for (i = begin_time - 1; i < end_time; i++) {
		hr = i;
		if (i < begin_time)
			buf[0] = '\0';
		else if (disp_t == HOUR12) {
			am = adjust_hour(&hr);
			(void) sprintf(buf, "%d%s", hr, am ? "a" : "p");
		}
		else
			(void) sprintf(buf, "%02d", hr);
		x_off = gr_center(hrbox_margin, buf, pf); 

/* REVISIT: unclear why we're still distinguishing between gr_text[_rgb]
		if (c->xcontext->screen_depth >= 8) 
			gr_text_rgb(c->xcontext, x+x_off, y, pf,
				buf, fg, cmap, rect);
		else
*/
			gr_text(c->xcontext, x+x_off, y, pf, buf, rect);

		y += h;
	}

	/* draw in appointments */
	dtcm_hittest_clear();

	x = MOBOX_AREA_WIDTH + hrbox_margin + 6;
	pfy = fontextents2.max_logical_extent.height;

	maxlines = (h - 6) / pfy;
	curr_line = last_hr = 0;

	/* loop thru, getting out the "no time" appointments */

        for (i = 0; i < a_total; i++) {
		if (i != a_total)
			last_hr = hr;
		hr = begin_time;
		if (cache[i].show_time == 0) {
			if (last_hr != hr) curr_line = 0;
			y = c->view->topoffset + 2 + pfy;
			if (curr_line < maxlines) {
				y += (curr_line * pfy) + h * (hr - begin_time);
				headlines = lines = text_to_lines(cache[i].summary, 4);

				start_tick = cache[i].start_time;
				end_tick = cache[i].end_time;
				if (lines != nullptr && lines->s != nullptr) { 
					appt_str = ckalloc(cm_strlen(lines->s)+18);
					format_line(start_tick, lines->s, 
						appt_str, end_tick, 
						cache[i].show_time, disp_t);
					lines = lines->next;
				}
				else {
					appt_str = ckalloc(15);
					format_line(start_tick, (char*)nullptr, 
						appt_str, end_tick, 
						cache[i].show_time, disp_t);
				}
				appt_str[cm_strlen(appt_str)] = '\0';

/* REVISIT: unclear why we're still distinguishing between gr_text[_rgb]
				if (c->xcontext->screen_depth >= 8) 
					gr_text_rgb(c->xcontext, x, y,
					   pf2, appt_str, fg, cmap, rect);
				else
*/
					gr_text(c->xcontext, x, y,
					   pf2, appt_str, rect);

				int entry_top = y - pfy + 2;
				int entry_lines = 1;
				free(appt_str); appt_str = nullptr;
				curr_line++;
				if (curr_line < maxlines && lines != nullptr) {
				 	appt_str = ckalloc(324);
					cm_strcpy(appt_str, "    ");
					while (lines != nullptr) { 
						if (lines->s != nullptr) 
							cm_strcat(appt_str, lines->s);
						lines = lines->next;
						if (lines != nullptr && lines->s != nullptr)
							cm_strcat(appt_str, " - ");
					}
					y += pfy;

/* REVISIT: unclear why we're still distinguishing between gr_text[_rgb]

					if (c->xcontext->screen_depth >= 8) 
						gr_text_rgb(c->xcontext, x, y,
						   pf2, appt_str, fg,
						   cmap, rect);
					else
*/
						gr_text(c->xcontext, x, y,
						   pf2, appt_str, rect);

					curr_line++;
					entry_lines++;
					free(appt_str); appt_str = nullptr;
				}
				dtcm_hittest_add(MOBOX_AREA_WIDTH + 3, entry_top,
					w > 6 ? w - 6 : w, entry_lines * pfy + 4,
					i, c->view->date);
				destroy_lines(headlines); lines=nullptr;
			}
		}
	}

	/* Proportional, pastel-tinted event blocks with side-by-side overlap layout */
	{
		DtcmDayBlock *blocks = nullptr;
		int bcount = 0, bcap = 0;

		for (i = 0; i < a_total; i++) {
			if (!dtcm_filter_matches(cache[i].summary))
				continue;
			start_tick = cache[i].start_time;
			end_tick = cache[i].end_time;
			if (!cache[i].show_time || magic_time(start_tick))
				continue;
			struct tm ts, te;
			localtime_r(&start_tick, &ts);
			int smin = ts.tm_hour * 60 + ts.tm_min;
			int emin = smin + 60;
			if (end_tick > start_tick) {
				int dur_min = (int)((end_tick - start_tick) / 60);
				if (dur_min < 15) dur_min = 15;
				emin = smin + dur_min;
			}
			if (emin <= begin_time * 60 || smin >= end_time * 60)
				continue;
			if (bcount == bcap) {
				int ncap = bcap ? bcap * 2 : 32;
				DtcmDayBlock *nb = (DtcmDayBlock *)realloc(blocks, (size_t)ncap * sizeof(DtcmDayBlock));
				if (!nb) break;
				blocks = nb;
				bcap = ncap;
			}
			blocks[bcount].cache_idx = i;
			blocks[bcount].start_min = smin;
			blocks[bcount].end_min = emin;
			blocks[bcount].col = 0;
			blocks[bcount].num_cols = 1;
			bcount++;
		}

		/* Merge enabled overlaid calendar occurrences for this day */
		Tick day_lo = lowerbound(c->view->date);
		Tick day_hi = next_ndays(day_lo, 1) - 1;
		DtcmOverlayOccurrence ovl[64];
		int novl = dtcm_overlay_query_range((time_t)day_lo, (time_t)day_hi, ovl, 64);
		for (int oi = 0; oi < novl; oi++) {
			if (!dtcm_filter_matches(ovl[oi].summary))
				continue;
			if (ovl[oi].all_day) {
				/* Render all-day overlay pill in top no-time band */
				new_XContext *xc2 = c->xcontext;
				Colormap cmap2 = DefaultColormap(xc2->display, DefaultScreen(xc2->display));
				unsigned short pr2, pg2, pb2, ar2, ag2, ab2;
				dtcm_category_pastel_rgb(ovl[oi].color, &pr2, &pg2, &pb2);
				dtcm_category_rgb(ovl[oi].color, &ar2, &ag2, &ab2);
				XColor pc2 = { .red = pr2, .green = pg2, .blue = pb2, .flags = DoRed | DoGreen | DoBlue };
				XColor ac2 = { .red = ar2, .green = ag2, .blue = ab2, .flags = DoRed | DoGreen | DoBlue };
				Pixel fp2 = XAllocColor(xc2->display, cmap2, &pc2) ? pc2.pixel : xc2->background;
				Pixel ap2 = XAllocColor(xc2->display, cmap2, &ac2) ? ac2.pixel : xc2->foreground;
				int ax = MOBOX_AREA_WIDTH + hrbox_margin + 5;
				int aw = (MOBOX_AREA_WIDTH + 2 + w) - ax - 3;
				int ay = c->view->topoffset + 2 + oi * (pfy + 2);
				if (ay + pfy < c->view->topoffset + h && aw > 20) {
					XSetForeground(xc2->display, xc2->gc, fp2);
					XFillRectangle(xc2->display, xc2->xid, xc2->gc, ax, ay, (unsigned int)aw, (unsigned int)(pfy + 1));
					XSetForeground(xc2->display, xc2->gc, ap2);
					XFillRectangle(xc2->display, xc2->xid, xc2->gc, ax, ay, 4, (unsigned int)(pfy + 1));
					XSetForeground(xc2->display, xc2->gc, xc2->foreground);
					char olbl[256];
					snprintf(olbl, sizeof(olbl), "%s (%s)", ovl[oi].summary, ovl[oi].source_name);
					int mc = gr_nchars(aw - 10, olbl, pf2);
					if ((int)strlen(olbl) > mc && mc >= 0) olbl[mc] = '\0';
					gr_text(xc2, ax + 7, ay + pfy - 1, pf2, olbl, nullptr);
				}
				continue;
			}
			int smin = hour((Tick)ovl[oi].start_utc) * 60 + minute((Tick)ovl[oi].start_utc);
			int emin = (ovl[oi].end_utc > ovl[oi].start_utc)
				   ? (hour((Tick)ovl[oi].end_utc) * 60 + minute((Tick)ovl[oi].end_utc))
				   : (smin + 60);
			if (emin <= smin) emin = smin + 30;
			if (emin <= begin_time * 60 || smin >= end_time * 60)
				continue;
			if (bcount == bcap) {
				int ncap = bcap ? bcap * 2 : 32;
				DtcmDayBlock *nb = (DtcmDayBlock *)realloc(blocks, (size_t)ncap * sizeof(DtcmDayBlock));
				if (!nb) break;
				blocks = nb;
				bcap = ncap;
			}
			blocks[bcount].cache_idx = -(oi + 1);
			blocks[bcount].start_min = smin;
			blocks[bcount].end_min = emin;
			blocks[bcount].col = 0;
			blocks[bcount].num_cols = 1;
			bcount++;
		}

		dtcm_layout_day_events(blocks, bcount);

		new_XContext *xc = c->xcontext;
		Colormap def_cmap = DefaultColormap(xc->display, DefaultScreen(xc->display));
		int area_x = MOBOX_AREA_WIDTH + hrbox_margin + 5;
		int area_w = (MOBOX_AREA_WIDTH + 2 + w) - area_x - 3;
		if (area_w < 40) area_w = 40;
		int grid_top = c->view->topoffset + h;
		int grid_bot = c->view->topoffset + (end_time - begin_time + 1) * h;

		for (int bi = 0; bi < bcount; bi++) {
			int idx = blocks[bi].cache_idx;
			int smin = blocks[bi].start_min;
			int emin = blocks[bi].end_min;
			if (smin < begin_time * 60) smin = begin_time * 60;
			if (emin > end_time * 60) emin = end_time * 60;

			int top_y = grid_top + ((smin - begin_time * 60) * h) / 60 + 1;
			int bot_y = grid_top + ((emin - begin_time * 60) * h) / 60 - 1;
			if (bot_y > grid_bot - 1) bot_y = grid_bot - 1;
			int blk_h = bot_y - top_y;
			if (blk_h < pfy + 4) blk_h = pfy + 4;
			if (top_y + blk_h > grid_bot) blk_h = grid_bot - top_y;
			if (blk_h < 10) continue;

			int col_w = area_w / blocks[bi].num_cols;
			int blk_x = area_x + blocks[bi].col * col_w + 1;
			int blk_w = col_w - 3;
			if (blk_w < 24) blk_w = 24;

			char clean_sum[256] = "", loc_buf[128] = "", cat_buf[64] = "";
			DtcmCategory cat = DTCM_CAT_NONE;
			if (idx >= 0) {
				dtcm_ics_decompose_what_ex(cache[idx].summary,
					clean_sum, sizeof(clean_sum),
					loc_buf, sizeof(loc_buf),
					cat_buf, sizeof(cat_buf),
					nullptr, 0);
				cat = dtcm_category_from_name(cat_buf);
			} else {
				int oi = -idx - 1;
				snprintf(clean_sum, sizeof(clean_sum), "%s (%s)", ovl[oi].summary, ovl[oi].source_name);
				snprintf(loc_buf, sizeof(loc_buf), "%s", ovl[oi].location);
				cat = ovl[oi].color;
			}

			unsigned short pr, pg, pb, ar, ag, ab;
			dtcm_category_pastel_rgb(cat, &pr, &pg, &pb);
			dtcm_category_rgb(cat, &ar, &ag, &ab);

			XColor pcol = { .red = pr, .green = pg, .blue = pb, .flags = DoRed | DoGreen | DoBlue };
			XColor acol = { .red = ar, .green = ag, .blue = ab, .flags = DoRed | DoGreen | DoBlue };
			Pixel fill_pix = XAllocColor(xc->display, def_cmap, &pcol) ? pcol.pixel : xc->background;
			Pixel acc_pix = XAllocColor(xc->display, def_cmap, &acol) ? acol.pixel : xc->foreground;

			XRectangle clip_r = { (short)blk_x, (short)top_y, (unsigned short)blk_w, (unsigned short)blk_h };
			XSetClipRectangles(xc->display, xc->gc, 0, 0, &clip_r, 1, Unsorted);

			Boolean is_selected = (idx >= 0 &&
				dtcm_selected_appt_matches(cache[idx].start_time, cache[idx].summary))
				? True : False;

			/* Soft pastel card fill */
			XSetForeground(xc->display, xc->gc, fill_pix);
			XFillRectangle(xc->display, xc->xid, xc->gc, blk_x, top_y, (unsigned int)blk_w, (unsigned int)blk_h);

			/* Bold category accent bar on left */
			XSetForeground(xc->display, xc->gc, acc_pix);
			XFillRectangle(xc->display, xc->xid, xc->gc, blk_x, top_y, is_selected ? 5 : 4, (unsigned int)blk_h);

			/* Crisp border (thick highlight ring when this event card is selected) */
			if (is_selected) {
				XSetForeground(xc->display, xc->gc, xc->hilight_color);
				XDrawRectangle(xc->display, xc->xid, xc->gc, blk_x, top_y,
					(unsigned int)(blk_w - 1), (unsigned int)(blk_h - 1));
				if (blk_w > 3 && blk_h > 3) {
					XDrawRectangle(xc->display, xc->xid, xc->gc, blk_x + 1, top_y + 1,
						(unsigned int)(blk_w - 3), (unsigned int)(blk_h - 3));
				}
			} else {
				XDrawRectangle(xc->display, xc->xid, xc->gc, blk_x, top_y,
					(unsigned int)(blk_w - 1), (unsigned int)(blk_h - 1));
			}

			XSetForeground(xc->display, xc->gc, xc->foreground);
			XSetClipMask(xc->display, xc->gc, None);

			/* Format line 1: time + clean summary */
			char line1[320];
			if (idx >= 0) {
				char *s_txt = clean_sum[0] ? clean_sum : (cache[idx].summary ? cache[idx].summary : "");
				format_line(cache[idx].start_time, s_txt, line1, cache[idx].end_time,
					    cache[idx].show_time, disp_t);
			} else {
				int oi = -idx - 1;
				format_line((Tick)ovl[oi].start_utc, clean_sum, line1, (Tick)ovl[oi].end_utc,
					    1, disp_t);
			}
			int text_x = blk_x + 7;
			int text_w = blk_w - 10;
			if (text_w > 10) {
				int mc = gr_nchars(text_w, line1, pf2);
				if ((int)strlen(line1) > mc && mc >= 0) line1[mc] = '\0';
				gr_text(xc, text_x, top_y + pfy + 1, pf2, line1, &clip_r);

				if (loc_buf[0] && blk_h >= pfy * 2 + 6) {
					char line2[160];
					snprintf(line2, sizeof(line2), "@ %s", loc_buf);
					int mc2 = gr_nchars(text_w, line2, pf2);
					if ((int)strlen(line2) > mc2 && mc2 >= 0) line2[mc2] = '\0';
					gr_text(xc, text_x, top_y + pfy * 2 + 2, pf2, line2, &clip_r);
				}
			}

			if (idx >= 0)
				dtcm_hittest_add(blk_x, top_y, blk_w, blk_h, idx, c->view->date);
		}
		free(blocks);
	}
}

extern void
init_mo(Calendar *c)
{
	Day *day_info = (Day *)c->view->day_info;
	day_info->month1 = previousmonth(c->view->date);
	day_info->month2 = c->view->date;
	day_info->month3 = nextmonth(c->view->date);
}

extern void 
init_dayview(Calendar *c)
{
        int  tot_rows, wks_1, wks_2, wks_3;
	Dimension w, h;
	Day *day_info = (Day *) c->view->day_info;

	/*
	 * Create month panels if they're not already there
	 */
	if (day_info->month_panels == (Widget *) nullptr) {
		day_info->month_panels = (Widget *)ckalloc(3 * sizeof(Widget));

		create_month_panels(c);
	}


        XtVaGetValues(c->canvas, XmNwidth, &w, XmNheight, &h, nullptr);
	(void)cache_dims(c, w, h);

	day_info->day_selected = -1;
	day_info->mobox_width = (int)MOBOX_AREA_WIDTH - 
			2*c->view->outside_margin;
	/* col width of day number in month boxes */
        day_info->col_w = 
		(day_info->mobox_width-INSIDE_MARGIN*2)/7;
	/* width of all of the month boxes */
        day_info->mobox_width = 7 * day_info->col_w + 
			2 * INSIDE_MARGIN;

	wks_1 = numwks(day_info->month1);
	wks_2 = numwks(day_info->month2);
	wks_3 = numwks(day_info->month3); 
	/* total rows in three months */ 
	tot_rows = wks_1 + wks_2 + wks_3 + 3; 

	/* row height of day number in month boxes */
        day_info->row_h = (c->view->winh - 3*c->view->topoffset-c->view->outside_margin) 
			/ tot_rows;

	/* height of 1st month */
        day_info->mobox_height1 = day_info->row_h * 
			(wks_1+1)+1;
	/* height of 2nd month */
        day_info->mobox_height2 = day_info->row_h * 
			(wks_2+1)+1;
	/* height of 2rd month */
        day_info->mobox_height3 = day_info->row_h * 
			(wks_3+1)+1;

	day_info->month1_y = c->view->topoffset;
	day_info->month2_y = 2*c->view->topoffset + 
			day_info->mobox_height1; 
	day_info->month3_y = 3*c->view->topoffset + 
			day_info->mobox_height1 +
			day_info->mobox_height2;
	((Selection*)(c->view->current_selection))->row = 0;
}

extern void
monthbox_xytodate(Calendar *c, int x, int y)
{
	char str[5];
	Day *day_info = (Day *)c->view->day_info;
	XFontSetExtents fontextents;
	int pfy;
	int col_w = day_info->col_w;
        int row_h = day_info->row_h;
	int row, col, x_off;
	int day_selected, tmpx;

	CalFontExtents(c->fonts->labelfont, &fontextents);
	pfy = fontextents.max_ink_extent.height;

	col = (x-c->view->outside_margin-INSIDE_MARGIN) / col_w;
	if (col < 0) return;
	tmpx = c->view->outside_margin + INSIDE_MARGIN + col * col_w;

	if (y < (day_info->month1_y + 
			day_info->mobox_height1)) {
		row = (y-day_info->month1_y-row_h) / row_h;
        	day_selected = (7 * (row+1)) - fdom(day_info->month1)
				 - (6 - col);
		if (day_selected <= 0 || day_selected >
			 monthlength(day_info->month1)) 
				return;
		day_info->day_selected = day_selected;
		day_info->day_selected_y = 
			day_info->month1_y + (row+1)*row_h; 
		c->view->olddate = c->view->date;
		c->view->date = next_ndays(first_dom(day_info->month1),
			 day_info->day_selected);
	}
	else if (y < (day_info->month2_y +
                        day_info->mobox_height2)) {
		row = (y-day_info->month2_y-row_h) / row_h;
        	day_selected = (7 * (row+1)) - fdom(day_info->month2)
				 - (6 - col);
		if (day_selected <= 0 || day_selected >
			 monthlength(day_info->month2)) 
				return;
		day_info->day_selected = day_selected;
		day_info->day_selected_y = 
			day_info->month2_y + (row+1)*row_h; 
		c->view->olddate = c->view->date;
		c->view->date = next_ndays(first_dom(day_info->month2),
			 day_info->day_selected);
	}
	else if (y < (day_info->month3_y +
                        day_info->mobox_height3)) {
		row = (y-day_info->month3_y-row_h) / row_h;
        	day_selected = (7 * (row+1)) - fdom(day_info->month3) 
				- (6 - col);
		if (day_selected <= 0 || day_selected > 
				monthlength(day_info->month3)) 
				return;
		day_info->day_selected = day_selected;
		day_info->day_selected_y = 
			day_info->month3_y + (row+1)*row_h; 
		c->view->olddate = c->view->date;
		c->view->date = next_ndays(first_dom(day_info->month3),
			 day_info->day_selected);
	}
	day_info->day_selected_x = tmpx;
	sprintf(str, "%d", day_info->day_selected);
	x_off = gr_center(col_w, str, c->fonts->labelfont);
	day_info->day_selected_x2 =
		day_info->day_selected_x+x_off;
	day_info->day_selected_y2 =
		day_info->day_selected_y + pfy;

}

extern void
monthbox_datetoxy(Calendar *c)
{
	char str[5];
	int week, x_off, dayw, daym, mo;
	Day *day_info = (Day *) c->view->day_info;
	XFontSetExtents fontextents;
	int pfy;
	int col_w = day_info->col_w;
	int row_h = day_info->row_h;
	struct tm tm;
	_Xltimeparams localtime_buf;

	CalFontExtents(c->fonts->labelfont, &fontextents);
	pfy = fontextents.max_logical_extent.height;

	tm = *_XLocaltime(&c->view->date, localtime_buf);
	mo = tm.tm_mon+1;
	dayw = tm.tm_wday;
	daym = tm.tm_mday;
	week = (12+tm.tm_mday-tm.tm_wday)/7;
	day_info->day_selected_x = c->view->outside_margin + 
			INSIDE_MARGIN + col_w*dayw;

	if (mo == month(day_info->month1)) 
		day_info->day_selected_y = 
			day_info->month1_y + 
			row_h*week;
	else if (mo == month(day_info->month2))  
		day_info->day_selected_y = 
			day_info->month2_y + 
			row_h*week;
	else if (mo == month(day_info->month3)) 
		day_info->day_selected_y = 
			day_info->month3_y + 
			row_h*week;
	sprintf(str, "%d", daym);
	x_off = gr_center(col_w, str, c->fonts->labelfont);
	day_info->day_selected_x2 = 
			day_info->day_selected_x+x_off;
	day_info->day_selected_y2 = 
			day_info->day_selected_y + pfy;
	day_info->day_selected = daym;
}

static Boolean
in_moboxes(Calendar *c, int x, int y)
{
	int margin = c->view->outside_margin;
	int topoff = c->view->topoffset;
	Boolean in_mobox = False;
	Day *day_info = (Day *)c->view->day_info;
	int row_h = day_info->row_h;

	if (x < (MOBOX_AREA_WIDTH-margin-2*INSIDE_MARGIN) && 
	   	x > margin && y > topoff && 
		( (y < (day_info->month3_y+
			day_info->mobox_height3) &&
			y > (day_info->month3_y+row_h)) || 
		  (y < (day_info->month2_y+
                	day_info->mobox_height2) &&
			y > (day_info->month2_y+row_h)) ||
		  (y < (day_info->month1_y+
			day_info->mobox_height1) &&
			y > day_info->month1_y+row_h) ) )
		in_mobox = True;

	return in_mobox;
}

extern void
paint_dayview(Calendar *c, Boolean repaint, XRectangle *rect, Boolean update_months)
{
	Props 		*p = (Props*)c->properties;
	int 		num_hrs;
	int 		beg = get_int_prop(p, CP_DAYBEGIN);
	int 		end = get_int_prop(p, CP_DAYEND);
	time_t 		start, stop;
	CSA_attribute 		*range_attrs;
        CSA_entry_handle 	*list;
	CSA_enum 	*ops;
	int 		i, j;
	CSA_uint32 	a_total;
	int		panel0_year, panel0_month;
	int		panel1_year, panel1_month;
	int		panel2_year, panel2_month;
	int		year_num, month_num;
	Boolean		day_not_on_panel = False;
	Day 		*d = (Day *)c->view->day_info;
	int		top_panel = 0;
	int		bottom_panel = 2;

	num_hrs = end - beg + 1;
	if (c->paint_cache == nullptr) {
		start = (time_t) lower_bound(0, c->view->date);
        	stop = (time_t) next_nhours(start, end+1) - 1;
		setup_range(&range_attrs, &ops, &j, start, stop, CSA_TYPE_EVENT, 0,
		    	B_FALSE, c->general->version);
        	csa_list_entries(c->cal_handle, j, range_attrs, ops, &a_total, &list, nullptr);
	
		free_range(&range_attrs, &ops, j);
		allocate_paint_cache(list, a_total, &c->paint_cache);
		c->paint_cache_size = a_total;
		csa_free(list);
	}

	year_num = year(c->view->date);
	month_num = month(c->view->date);
	XtVaGetValues(d->month_panels[1],
		XmNyear, &panel1_year,
		XmNmonth, &panel1_month,
		nullptr);

	if ((panel1_year == year(get_bot())) && (panel1_month == 1))
		top_panel = 1;
	else if ((panel1_year == year(get_eot())) && (panel1_month == 12))
		bottom_panel = 1;

	XtVaGetValues(d->month_panels[top_panel],
		XmNyear, &panel0_year,
		XmNmonth, &panel0_month,
		nullptr);

	if ((year_num < panel0_year) || 
	    ((year_num == panel0_year) && (month_num < panel0_month)))
		day_not_on_panel = True;
	
	XtVaGetValues(d->month_panels[bottom_panel],
		XmNyear, &panel2_year,
		XmNmonth, &panel2_month,
		nullptr);

	if ((year_num > panel2_year) || 
	    ((year_num == panel2_year) && (month_num > panel2_month)))
		day_not_on_panel = True;

	/* set up month panels */
	if (update_months || day_not_on_panel)
		update_quarter(c);

	/* Highlight the active day button as pressed down + bold across the 3 mini-panels */
	{
		int sel_dom = dom(c->view->date);
		for (int pk = 0; pk < 3; pk++) {
			if (!d->month_panels[pk]) continue;
			int py = 0, pm = 0;
			XtVaGetValues(d->month_panels[pk],
				XmNyear, &py,
				XmNmonth, &pm,
				nullptr);
			XmMonthPanelSetSelectedDay(d->month_panels[pk],
				(py == year_num && pm == month_num) ? sel_dom : 0);
		}
	}

	/* no need to do this on a damage event */

	if (!rect)
		display_monthpanels(c);

	/* repaint appointment area */
	if (repaint) {
		int	line_length;

		gr_clear_area(c->xcontext, 0, 0, 
			c->view->winw, c->view->winh);
		line_length = c->view->topoffset + 
			      ((end - beg + 1) * c->view->boxh);
		/* draw line separating mo. boxes and appts. */
        	gr_draw_line(c->xcontext, (int)MOBOX_AREA_WIDTH+1,
			 0, (int)MOBOX_AREA_WIDTH+1,
                         line_length, gr_solid, rect);
        	gr_draw_line(c->xcontext, (int)MOBOX_AREA_WIDTH+2, 
			 0, (int)MOBOX_AREA_WIDTH+2,
                         line_length, gr_solid, rect);
		/* Subtle sash handle centered on the resizable divider */
		{
			int mid_y = line_length / 2;
			int sx = (int)MOBOX_AREA_WIDTH - 1;
			gr_draw_box(c->xcontext, sx, mid_y - 12, 5, 24, rect);
		}
        	gr_draw_line(c->xcontext, (int)MOBOX_AREA_WIDTH+2, 
			 c->view->topoffset-1, c->view->winw,
                         c->view->topoffset-1,  gr_solid, rect);
        	gr_draw_line(c->xcontext, (int)MOBOX_AREA_WIDTH+2, 
			 c->view->topoffset, c->view->winw,
                         c->view->topoffset, gr_solid, rect);
		paint_dayview_appts(c, c->paint_cache, c->paint_cache_size, rect);
	}
	/* just repaint schedule area */
	else {
		gr_clear_area(c->xcontext, 
				(int)MOBOX_AREA_WIDTH+4, 0, 
				c->view->winw - (int)MOBOX_AREA_WIDTH+4,
				c->view->winh);
        	gr_draw_line(c->xcontext, (int)MOBOX_AREA_WIDTH+2, 
			 c->view->topoffset, c->view->winw,
                         c->view->topoffset,  gr_solid, rect);
        	gr_draw_line(c->xcontext, (int)MOBOX_AREA_WIDTH+2, 
			 c->view->topoffset+1, c->view->winw,
                         c->view->topoffset+1, gr_solid, rect);
		paint_dayview_appts(c, c->paint_cache, c->paint_cache_size, rect);
	}

	paint_day_header(c, c->view->date, rect);

	/* Live current-time indicator line and Today header accent when viewing Today */
	if (dtcm_is_same_day(c->view->date, now())) {
		new_XContext *xc = c->xcontext;
		XColor col;
		col.red = 0xc000; col.green = 0x3900; col.blue = 0x2b00;
		col.flags = DoRed | DoGreen | DoBlue;
		Colormap cmap = DefaultColormap(xc->display, DefaultScreen(xc->display));
		Pixel line_pix = XAllocColor(xc->display, cmap, &col) ? col.pixel : xc->hilight_color;

		if (rect)
			XSetClipRectangles(xc->display, xc->gc, 0, 0, rect, 1, Unsorted);
		XSetForeground(xc->display, xc->gc, line_pix);

		/* Subtle Today accent bar along top of schedule pane */
		XFillRectangle(xc->display, xc->xid, xc->gc,
			(int)MOBOX_AREA_WIDTH + 3, c->view->topoffset - 4,
			c->view->boxw > 2 ? c->view->boxw - 2 : c->view->winw - (int)MOBOX_AREA_WIDTH - 4, 3);

		time_t cur = now();
		struct tm *now_tm = localtime(&cur);
		if (now_tm && now_tm->tm_hour >= beg && now_tm->tm_hour < end && c->view->boxh > 0) {
			int now_y = c->view->topoffset + c->view->boxh +
				(now_tm->tm_hour - beg) * c->view->boxh +
				(now_tm->tm_min * c->view->boxh) / 60;
			int x1 = (int)MOBOX_AREA_WIDTH + 3;
			int x2 = x1 + c->view->boxw - 2;
			XFillArc(xc->display, xc->xid, xc->gc, x1, now_y - 3, 7, 7, 0, 360 * 64);
			XFillRectangle(xc->display, xc->xid, xc->gc, x1 + 4, now_y - 1,
				x2 > x1 + 4 ? x2 - x1 - 4 : 1, 2);
		}

		XSetForeground(xc->display, xc->gc, xc->foreground);
		if (rect)
			XSetClipMask(xc->display, xc->gc, None);
	}
}

extern void
paint_day(Calendar *c)
{
	Day *day_info = (Day *) c->view->day_info;

        c->view->glance = dayGlance;
	XmToggleButtonGadgetSetState(c->day_scope, True, False);
	gr_clear_area(c->xcontext, 0, 0, c->view->winw, c->view->winh);
	if (day_info->month_panels == (Widget *) nullptr)
		(void)init_dayview(c);
	paint_dayview(c, True, nullptr, True); 
	calendar_select(c, hourSelect, (caddr_t)nullptr);
}
	
/*
 * handler for button to switch to day view.
 */
extern void
day_button (Widget widget, XtPointer data, XtPointer cbs)
{
        Calendar *c = calendar;

	if (c->view->glance == dayGlance)
		return;

	XtUnmapWidget(c->canvas);
	invalidate_cache(c);

	switch (c->view->glance) {
		case weekGlance:
			c->view->glance = dayGlance;
			cleanup_after_weekview(c);
			break;
		case yearGlance:
			c->view->glance = dayGlance;
			cleanup_after_yearview(c);
			break;
		case monthGlance:
			c->view->glance = dayGlance;
			cleanup_after_monthview(c);
			break;
		case agendaGlance:
			c->view->glance = dayGlance;
			cleanup_after_agenda(c);
			break;
		default:
			break;	
	}

	init_mo(c);
	(void)init_dayview(c);

	XtMapWidget(c->canvas);
}

extern void
print_day_range(Calendar *c, Tick start_tick, Tick end_tick)
{
	int 		n;
	Tick 	first_date = start_tick;
        Boolean 	done = False, first = True;
        int 		num_page = 1;
	Props 		*pr = (Props*)c->properties;
	void *xp = (void *)nullptr;

	n = (end_tick - start_tick)/daysec + 1;

	if (n <= 0) n = 1;

	if ((xp = x_open_file(c)) == (void *)nullptr)
	  return;

	for (; n > 0; n--) {
	  while (!done) {
	    done = _print_day(c, num_page, xp,
			      first_date, pr, first);
	    num_page++;
	    first = False;
	  }
	  done = False;
	  num_page = 1;
	}

	x_print_file(xp, c);
}

static int
count_day_pages(Calendar *c, int lines_per_page, Tick tick)
{
	int	n, i, j, timeslots, num_appts, pages, max = 0; 
	Props *p = (Props *)c->properties;
	int       daybegin = get_int_prop(p, CP_DAYBEGIN);
	int       dayend   = get_int_prop(p, CP_DAYEND);
        char 	*location;
	time_t 	start, end;
        CSA_return_code stat;
        CSA_entry_handle *list;
        CSA_attribute *range_attrs;
	CSA_enum *ops;
	CSA_uint32 a_total;

	for (i=daybegin; i < dayend; i++) {
		start = (time_t) lower_bound(i, tick);
		end = (time_t) next_nhours(start+1, 1) - 1;
		setup_range(&range_attrs, &ops, &j, start, end,
			    CSA_TYPE_EVENT, 0, B_FALSE, c->general->version);
        	csa_list_entries(c->cal_handle, j, range_attrs, ops, &a_total, &list, nullptr);
		free_range(&range_attrs, &ops, j);

                num_appts = count_multi_appts(list, a_total, c);

		if (num_appts > max)
			max = num_appts;

                csa_free(list);
	}
 
       	pages = max / lines_per_page;
        if ((max % lines_per_page) > 0)
                pages++;
 
        return(pages);
}

static Boolean
_print_day(Calendar *c,
    int num_page,
    void *xp,
    Tick first_date,
    Props *p,
    Boolean first)
{
    char buf[100];
    int n, i, j, timeslots, num_appts, pages;
    int max = 0;
    int daybegin = get_int_prop(p, CP_DAYBEGIN);
    int dayend   = get_int_prop(p, CP_DAYEND);
    OrderingType ord_t = get_int_prop(p, CP_DATEORDERING);
    Boolean more, done = False, all_done = True;
    char *location;
    CSA_return_code stat;
    CSA_entry_handle *list;
    CSA_attribute *range_attrs;
    CSA_enum *ops;
    CSA_uint32 a_total;
    time_t start, stop;
    int lines_per_page;
    static Tick tick = 0;
    static int total_pages = 0;

    /*
     * Need to find the max number of timeslots which will be shown
     * in one column, for later calculation of box height.
     */  
    if ((!morning(daybegin)) || dayend <= 12) 
    	timeslots = dayend - daybegin;
    else
    	timeslots = ((12-daybegin) > (dayend-12)) ?
    		     (12-daybegin) : (dayend-12);

    x_init_printer(xp, PORTRAIT);
    x_init_day(xp, timeslots);
    lines_per_page = x_get_day_lines_per_page(xp);

    if (first)
      tick = first_date;

    if (num_page > 1)
      tick = prevday(tick);
    else
      total_pages = (lines_per_page > 0) ?
	count_day_pages(c, lines_per_page, tick) : 1;

    format_date(tick, ord_t, buf, 1, 0, 0);

    x_print_header(xp, buf, num_page, total_pages);
    x_day_header(xp);

    for (i=daybegin; i < dayend; i++) {
      start = (time_t) lower_bound(i, tick);
      stop = (time_t) next_nhours(start+1, 1) - 1;
      setup_range(&range_attrs, &ops, &j, start, stop,
		  CSA_TYPE_EVENT, 0, B_FALSE, c->general->version);

      csa_list_entries(c->cal_handle, j, range_attrs,
		       ops, &a_total, &list, nullptr);
      free_range(&range_attrs, &ops, j);
 
      num_appts = count_multi_appts(list, a_total, c);

      if ((lines_per_page > 0) && (num_appts > (lines_per_page * num_page)))
	more = True;
      else
	more = False;

      x_day_timeslots (xp, i, more);

      if (lines_per_page > 0)
	done = x_print_multi_appts (xp, list, a_total,
				    num_page, dayGlance);
      else done = True;

      if (!done)
	all_done = False;
      csa_free(list);
    }
 
    x_finish_printer(xp);
    tick = nextday(tick); 

    return(all_done);
}

static int
day_xytoclock(Calendar *c, int x, int y, Tick t)
{
	int daybegin, hr, val;
	char buf[36];
	struct tm tm;
	Props *p;
	_Xltimeparams localtime_buf;

	p	= (Props *)c->properties;
	daybegin = get_int_prop(p, CP_DAYBEGIN);
	tm	= *_XLocaltime(&t, localtime_buf);

	hr = (x == 1) ? (12 + y) : (y + daybegin - 1);

	/* 
	 * If y == 0 then the user is clicking on the no-time area.  There
	 * is no hour associated with no-time events.
	 */
	if (y == 0) hr = 0;

	(void)sprintf(buf, "%d/%d/%d", tm.tm_mon+1, tm.tm_mday, tm.tm_year+1900);
	val	=cm_getdate(buf, nullptr);
	val	= val+(hr*(int)hrsec);
	adjust_dst(t, val);
	return(val);
}

static int
day_y_to_minutes(Calendar *c, int y)
{
	Props *p = (Props *)c->properties;
	int beg = get_int_prop(p, CP_DAYBEGIN);
	int end = get_int_prop(p, CP_DAYEND);
	int h = c->view->boxh;
	if (h <= 0)
		return beg * 60;
	int grid_top = c->view->topoffset + h;
	int raw_mins = beg * 60 + ((y - grid_top) * 60) / h;
	int snapped = dtcm_snap_minutes(raw_mins, 15);
	if (snapped < beg * 60) snapped = beg * 60;
	if (snapped > end * 60) snapped = end * 60;
	return snapped;
}

static Boolean
day_update_appt_times(Calendar *c, int cache_idx, Tick new_start, Tick new_end)
{
	if (!c || c->cal_handle == 0 || !c->paint_cache ||
	    cache_idx < 0 || cache_idx >= c->paint_cache_size)
		return False;

	Paint_cache *pc = &c->paint_cache[cache_idx];
	if (new_start == pc->start_time && new_end == pc->end_time)
		return False;

	Tick day_start = lowerbound(pc->start_time);
	Tick day_stop = next_ndays(day_start, 1) - 1;
	CSA_attribute *range_attrs = nullptr;
	CSA_enum *ops = nullptr;
	int rcount = 0;
	CSA_uint32 n_entries = 0;
	CSA_entry_handle *list = nullptr;

	setup_range(&range_attrs, &ops, &rcount, day_start, day_stop, CSA_TYPE_EVENT,
		    0, B_FALSE, c->general->version);
	CSA_return_code stat = csa_list_entries(c->cal_handle, rcount,
						range_attrs, ops, &n_entries, &list, nullptr);
	free_range(&range_attrs, &ops, rcount);
	if (stat != CSA_SUCCESS || !list || n_entries == 0) {
		if (list) csa_free(list);
		return False;
	}

	CSA_entry_handle target = 0;
	Dtcm_appointment *appt = allocate_appt_struct(appt_read,
						      c->general->version,
						      CSA_ENTRY_ATTR_START_DATE_I,
						      CSA_ENTRY_ATTR_SUMMARY_I,
						      nullptr);
	for (CSA_uint32 i = 0; i < n_entries; i++) {
		if (query_appt_struct(c->cal_handle, list[i], appt) == CSA_SUCCESS) {
			Tick st = 0;
			if (appt->time && appt->time->value && appt->time->value->item.date_time_value)
				_csa_iso8601_to_tick(appt->time->value->item.date_time_value, &st);
			const char *sum = (appt->what && appt->what->value && appt->what->value->item.string_value)
					  ? appt->what->value->item.string_value : "";
			if (st == pc->start_time && strcmp(sum, pc->summary ? pc->summary : "") == 0) {
				target = list[i];
				break;
			}
		}
	}
	free_appt_struct(&appt);
	if (target == 0) {
		csa_free(list);
		return False;
	}

	char start_iso[BUFSIZ], end_iso[BUFSIZ];
	_csa_tick_to_iso8601(new_start, start_iso);
	_csa_tick_to_iso8601(new_end, end_iso);

	CSA_attribute_value vals[2];
	CSA_attribute attrs[2];
	memset(vals, 0, sizeof(vals));
	memset(attrs, 0, sizeof(attrs));
	vals[0].type = CSA_VALUE_DATE_TIME;
	vals[0].item.date_time_value = start_iso;
	attrs[0].name = CSA_ENTRY_ATTR_START_DATE;
	attrs[0].value = &vals[0];
	vals[1].type = CSA_VALUE_DATE_TIME;
	vals[1].item.date_time_value = end_iso;
	attrs[1].name = CSA_ENTRY_ATTR_END_DATE;
	attrs[1].value = &vals[1];

	CSA_entry_handle updated = 0;
	stat = csa_update_entry_attributes(c->cal_handle, target, CSA_SCOPE_ONE,
					   CSA_FALSE, 2, attrs, &updated, nullptr);
	if (updated)
		csa_free((CSA_buffer)(uintptr_t)updated);
	csa_free(list);
	if (stat == CSA_SUCCESS) {
		dtcm_undo_push_reschedule(pc->start_time, pc->end_time,
					  new_start, new_end,
					  pc->show_time, pc->summary);
		return True;
	}
	return False;
}

extern void
day_event(XEvent *event)
{
        static int lastcol, lastrow;
        static XEvent lastevent;
        static Boolean divider_dragging = False;
        static Cursor divider_cursor = None;
        static Cursor move_cursor = None;
        static Cursor resize_cursor = None;
        enum { DAY_DRAG_NONE = 0, DAY_DRAG_MOVE, DAY_DRAG_RESIZE, DAY_DRAG_CREATE };
        static int day_drag_mode = DAY_DRAG_NONE;
        static Boolean day_drag_active = False;
        static int day_drag_idx = -1;
        static int day_drag_start_y = 0;
        static int day_drag_start_min = 0;
        pr_pos xy;
        int x, y, i, j;
        int boxw, boxh, margin, id;
        Calendar *c = calendar;
        Tick date = c->view->date;
        Boolean in_mbox = False; /* in month boxes ? */
        Editor *e = (Editor*)c->editor;
        ToDo *t = (ToDo*)c->todo;
        GEditor *ge = (GEditor*)c->geditor;
        Day *day_info = (Day *)c->view->day_info;
 
        boxw    = c->view->boxw;
        boxh    = c->view->boxh;
        margin  = c->view->outside_margin;
        x       = event->xbutton.x;
        y       = event->xbutton.y;

        /* Resizable divider between left 3-month panel and right schedule pane */
        if (event->type == ButtonPress && event->xbutton.button == Button1 &&
            abs(x - ((int)MOBOX_AREA_WIDTH + 1)) <= 6) {
                divider_dragging = True;
                if (divider_cursor == None)
                        divider_cursor = XCreateFontCursor(XtDisplay(c->canvas), XC_sb_h_double_arrow);
                XDefineCursor(XtDisplay(c->canvas), XtWindow(c->canvas), divider_cursor);
                return;
        }
        if (divider_dragging) {
                if (event->type == MotionNotify) {
                        int min_w = 180;
                        int max_w = (c->view->winw > 420) ? (c->view->winw - 220) : min_w;
                        int nx = x;
                        if (nx < min_w) nx = min_w;
                        if (nx > max_w) nx = max_w;
                        if (nx != day_info->split_x) {
                                day_info->split_x = nx;
                                cache_dims(c, c->view->winw, c->view->winh);
                                init_mo(c);
                                display_monthpanels(c);
                                paint_dayview(c, True, nullptr, False);
                                calendar_select(c, hourSelect, (caddr_t)nullptr);
                        }
                } else if (event->type == ButtonRelease) {
                        divider_dragging = False;
                        XUndefineCursor(XtDisplay(c->canvas), XtWindow(c->canvas));
                }
                return;
        }
 
        /* boundary conditions */
        if ((!(in_mbox = in_moboxes(c, x, y)) && x < MOBOX_AREA_WIDTH+2)
||
                (x > MOBOX_AREA_WIDTH+2 && y < c->view->topoffset)) {
                if (event->type == ButtonRelease && day_drag_mode != DAY_DRAG_NONE) {
                        XUndefineCursor(XtDisplay(c->canvas), XtWindow(c->canvas));
                        day_drag_mode = DAY_DRAG_NONE;
                        day_drag_active = False;
                        day_drag_idx = -1;
                        set_message(c->message_text, "");
                }
                lastcol=0; lastrow=0;
                return;
        }
        if (in_mbox) {
                xy.x = (x - margin)/ day_info->col_w;
                xy.y = (x - c->view->topoffset)/ day_info->row_h;
        }
        else {
                xy.x    = boxw;
                xy.y    = (y - c->view->topoffset)/boxh;
        }
        switch(event->type) {
        case MotionNotify:
                if (!in_mbox && day_drag_mode != DAY_DRAG_NONE) {
                        if (!day_drag_active && abs(y - day_drag_start_y) >= 8) {
                                day_drag_active = True;
                                if (day_drag_mode == DAY_DRAG_RESIZE) {
                                        if (resize_cursor == None)
                                                resize_cursor = XCreateFontCursor(XtDisplay(c->canvas), XC_sb_v_double_arrow);
                                        XDefineCursor(XtDisplay(c->canvas), XtWindow(c->canvas), resize_cursor);
                                } else if (day_drag_mode == DAY_DRAG_MOVE) {
                                        if (move_cursor == None)
                                                move_cursor = XCreateFontCursor(XtDisplay(c->canvas), XC_fleur);
                                        XDefineCursor(XtDisplay(c->canvas), XtWindow(c->canvas), move_cursor);
                                }
                        }
                        if (day_drag_active) {
                                int cur_min = day_y_to_minutes(c, y);
                                char msg[128];
                                if (day_drag_mode == DAY_DRAG_RESIZE) {
                                        snprintf(msg, sizeof(msg), "Resize end to %02d:%02d...", cur_min / 60, cur_min % 60);
                                } else if (day_drag_mode == DAY_DRAG_MOVE) {
                                        snprintf(msg, sizeof(msg), "Move appointment to %02d:%02d...", cur_min / 60, cur_min % 60);
                                } else {
                                        int s = day_drag_start_min < cur_min ? day_drag_start_min : cur_min;
                                        int en = day_drag_start_min < cur_min ? cur_min : day_drag_start_min;
                                        if (en < s + 15) en = s + 15;
                                        snprintf(msg, sizeof(msg), "New appointment: %02d:%02d - %02d:%02d", s/60, s%60, en/60, en%60);
                                }
                                set_message(c->message_text, msg);
                                break;
                        }
                }
                if (!in_mbox) {
                        if (xy.x !=lastcol || xy.y !=lastrow) {
                                calendar_deselect(c);
                                j = day_xytoclock(c, xy.x, xy.y, date);
                                if (j >= 0) {
                                        c->view->olddate = c->view->date;                                        c->view->date = j;
                                        calendar_select(c, hourSelect, (caddr_t)&xy);
                                }
                                lastcol=xy.x;
                                lastrow=xy.y;
                        }
                }
                break;
        case ButtonRelease:
                if (day_drag_active && day_drag_mode != DAY_DRAG_NONE) {
                        XUndefineCursor(XtDisplay(c->canvas), XtWindow(c->canvas));
                        int cur_min = day_y_to_minutes(c, y);
                        Tick day_base = lowerbound(c->view->date);
                        if (day_drag_mode == DAY_DRAG_RESIZE && day_drag_idx >= 0 &&
                            c->paint_cache && day_drag_idx < c->paint_cache_size) {
                                Paint_cache *pc = &c->paint_cache[day_drag_idx];
                                Tick new_end = day_base + (Tick)cur_min * 60;
                                if (new_end < pc->start_time + 15 * 60)
                                        new_end = pc->start_time + 15 * 60;
                                if (day_update_appt_times(c, day_drag_idx, pc->start_time, new_end)) {
                                        invalidate_cache(c);
                                        paint_dayview(c, True, nullptr, False);
                                        set_message(c->message_text, "Appointment duration updated (Ctrl+Z to undo).");
                                }
                        } else if (day_drag_mode == DAY_DRAG_MOVE && day_drag_idx >= 0 &&
                                   c->paint_cache && day_drag_idx < c->paint_cache_size) {
                                Paint_cache *pc = &c->paint_cache[day_drag_idx];
                                Tick dur = (pc->end_time > pc->start_time) ? (pc->end_time - pc->start_time) : 3600;
                                Tick new_start = day_base + (Tick)cur_min * 60;
                                Tick new_end = new_start + dur;
                                if (day_update_appt_times(c, day_drag_idx, new_start, new_end)) {
                                        invalidate_cache(c);
                                        paint_dayview(c, True, nullptr, False);
                                        set_message(c->message_text, "Appointment moved (Ctrl+Z to undo).");
                                }
                        } else if (day_drag_mode == DAY_DRAG_CREATE) {
                                int s = day_drag_start_min < cur_min ? day_drag_start_min : cur_min;
                                int en = day_drag_start_min < cur_min ? cur_min : day_drag_start_min;
                                if (en < s + 15) en = s + 15;
                                set_message(c->message_text, "");
                                show_editor(c, day_base + (Tick)s * 60, day_base + (Tick)en * 60, False);
                        }
                }
                day_drag_mode = DAY_DRAG_NONE;
                day_drag_active = False;
                day_drag_idx = -1;
                break;
        case ButtonPress:
		if (event->xbutton.button == Button3 && !in_mbox) {
			j = day_xytoclock(c, xy.x, xy.y, date);
			const DtcmHitBox *hb = dtcm_hittest_find(x, y);
			if (hb && c->paint_cache && hb->cache_idx >= 0 &&
			    hb->cache_idx < c->paint_cache_size) {
				Paint_cache *pc = &c->paint_cache[hb->cache_idx];
				calendar_deselect(c);
				dtcm_selected_appt_set(hb->cache_idx, pc->start_time, pc->end_time,
						       pc->show_time, pc->summary);
				paint_dayview(c, False, nullptr, False);
			}
			dtcm_context_menu_popup(c, &event->xbutton, j >= 0 ? j : c->view->date,
						hb ? hb->cache_idx : -1);
			break;
		}
		day_drag_mode = DAY_DRAG_NONE;
		day_drag_active = False;
		day_drag_idx = -1;
		if (event->xbutton.button == Button1 && !in_mbox && y >= c->view->topoffset + boxh) {
			const DtcmHitBox *hb = dtcm_hittest_find(x, y);
			day_drag_start_y = y;
			day_drag_start_min = day_y_to_minutes(c, y);
			if (hb) {
				day_drag_idx = hb->cache_idx;
				day_drag_mode = (y >= hb->y + hb->h - 6) ? DAY_DRAG_RESIZE : DAY_DRAG_MOVE;
			} else {
				day_drag_mode = DAY_DRAG_CREATE;
			}
		}
		if (ds_is_double_click(&lastevent, event)) {
			day_drag_mode = DAY_DRAG_NONE;
			day_drag_active = False;
			day_drag_idx = -1;
			_DtTurnOnHourGlass(c->frame);
			j = day_xytoclock(c, xy.x, xy.y, date);
			const DtcmHitBox *hb = !in_mbox ? dtcm_hittest_find(x, y) : nullptr;
			if (hb && c->paint_cache && hb->cache_idx >= 0 &&
			    hb->cache_idx < c->paint_cache_size) {
				Paint_cache *pc = &c->paint_cache[hb->cache_idx];
				show_editor(c, pc->start_time, pc->end_time,
					    pc->show_time ? False : True);
				editor_select_appt(c, pc->start_time, pc->summary);
			} else if (lastcol == xy.x && lastrow == xy.y && !in_mbox) {
				show_editor(c, j, next_nhours(j, 1), xy.y == 0 ? True : False);
			} else if (in_mbox) {
				monthbox_deselect(c);
				monthbox_xytodate(c, x, y);
				monthbox_select(c);
			} else {
				show_editor(c, j, next_nhours(j, 1), xy.y == 0 ? True : False);
			}
			_DtTurnOffHourGlass(c->frame);
		} else {
			if (in_mbox) {
				monthbox_deselect(c);
				monthbox_xytodate(c, x, y);
				paint_dayview(c, False, nullptr, True);
				calendar_select(c, hourSelect, (caddr_t)nullptr);
				monthbox_select(c);
				j = day_xytoclock(c, xy.x, xy.y, date);
				if (editor_showing(e)) {
					set_editor_defaults(e, j,
							    next_nhours(j, 1), False);
					add_all_appt(e);
				}
				if (todo_showing(t)) {
					set_todo_defaults(t);
					add_all_todo(t);
				}
				if (geditor_showing(ge)) {
					set_geditor_defaults(ge, j,
							     next_nhours(j, 1));
					add_all_gappt(ge);
				}
			} else {
				const DtcmHitBox *hb = dtcm_hittest_find(x, y);
				calendar_deselect(c);
				if (hb && c->paint_cache && hb->cache_idx >= 0 &&
				    hb->cache_idx < c->paint_cache_size) {
					Paint_cache *pc = &c->paint_cache[hb->cache_idx];
					c->view->olddate = c->view->date;
					c->view->date = pc->start_time;
					dtcm_selected_appt_set(hb->cache_idx, pc->start_time, pc->end_time,
							       pc->show_time, pc->summary);
					paint_dayview(c, False, nullptr, False);
					char csum[256], cloc[128], ccat[64], smsg[384];
					dtcm_ics_decompose_what_ex(pc->summary, csum, sizeof(csum),
						cloc, sizeof(cloc), ccat, sizeof(ccat), nullptr, 0);
					snprintf(smsg, sizeof(smsg),
						 "Selected: %s  (Ctrl+C Copy, Del Delete, Enter Edit)",
						 csum[0] ? csum : (pc->summary ? pc->summary : ""));
					set_message(c->message_text, smsg);
					if (editor_showing(e)) {
						editor_select_appt(c, pc->start_time, pc->summary);
					}
				} else {
					Boolean had_sel = dtcm_selected_appt_is_set() ? True : False;
					dtcm_selected_appt_clear();
					if (had_sel)
						paint_dayview(c, False, nullptr, False);
					j = day_xytoclock(c, xy.x, xy.y, date);
					if (j >= 0) {
						c->view->olddate = c->view->date;
						c->view->date = j;
						calendar_select(c, hourSelect,
								(caddr_t)&xy);
						if (editor_showing(e)) {
							set_editor_defaults(e, j,
								next_nhours(j, 1), xy.y == 0 ? True : False);
							add_all_appt(e);
						}
						if (todo_showing(t)) {
							set_todo_defaults(t);
							add_all_todo(t);
						}
						if (geditor_showing(ge)) {
							set_geditor_defaults(ge, j,
								next_nhours(j, 1));
							add_all_gappt(ge);
						}
					}
				}
			}
		}
		lastcol=xy.x;
		lastrow=xy.y;
                break;
        default:
        	break;
        };            /* switch */
        if (event->type == ButtonPress && event->xbutton.button == Button1)
                lastevent = *event;
}

static void
sync_panel_form_width(Calendar *c)
{
	Day *d = (Day *)c->view->day_info;
	if (!d || !d->panel_form)
		return;
	int w = MOBOX_AREA_WIDTH - 2;
	if (w < 160) w = 160;
	XtVaSetValues(d->panel_form,
		XmNrightAttachment, XmATTACH_NONE,
		XmNwidth, (Dimension)w,
		nullptr);
}

static void
display_monthpanels(Calendar *c)
{
	Day 	*d = (Day *)c->view->day_info;
	char 	buf[BUFSIZ];
	int	lastyear = year(get_eot());
	int	firstyear = year(get_bot());
	int	panel_year;
	int	panel_month;

	sync_panel_form_width(c);
	XtManageChild(d->panel_form);

	XtVaGetValues(d->month_panels[1],
		XmNyear, &panel_year,
		XmNmonth, &panel_month,
		nullptr);

	if ((panel_year == firstyear) && (panel_month == 1)) {
		XtUnmapWidget(d->month_panels[0]);
		XtMapWidget(d->month_panels[2]);
		sprintf(buf, "%s", CATGETS(c->DT_catd, 1, 623, "Calendar does not display dates prior to January 1, 1970"));
		set_message(c->message_text, buf);
	}
	else if ((panel_year == lastyear) && (panel_month == 12)) {
		XtMapWidget(d->month_panels[0]);
		XtUnmapWidget(d->month_panels[2]);
		sprintf(buf, "%s", CATGETS(c->DT_catd, 1, 624, "Calendar does not display dates after December 31, 2037"));
		set_message(c->message_text, buf);
	}
	else {
		XtMapWidget(d->month_panels[0]);
		XtMapWidget(d->month_panels[2]);
		set_message(c->message_text, "");
	}

	/* switch canvas to attach using its XmNleftPosition value */
/* THIS IS THE RIGHT THIS TO DO, BUT ONLY AFTER REMOVING THE
   CODE THAT DRAWS THE OLD MO_BOXES.  FOR NOW, WE JUST MANAGE THE
   MONTHPANELS ON TOP OF THAT PART OF THE CANVAS - dac
	XtVaSetValues(c->canvas,
		XmNleftAttachment, XmATTACH_POSITION,
		XmNleftPosition, 40,
		NULL);
*/

}

extern void
cleanup_after_dayview(Calendar *c)
{
	Day *d = (Day *)c->view->day_info;

	invalidate_cache(c);

	XtUnmanageChild(d->panel_form);

	XmToggleButtonGadgetSetState(c->day_scope, False, False);

	/* make canvas attach to form, and ignore its XmNleftPosition */
/* DITTO ABOVE: THIS ISN'T NEEDED TILL WE ACTUALLY DO RESIZE THE
   CANVAS TO ADJOIN THE MONTHPANELS WHILE DAY VIEW IS UP
	XtVaSetValues(c->canvas, XmNleftAttachment, XmATTACH_FORM, NULL);
*/

	gr_clear_area(calendar->xcontext, 0, 0, calendar->view->winw,
		       calendar->view->winh);

	set_message(c->message_text, "");
}

static void
create_month_panels(Calendar *c)
{
	Day *d = (Day *)c->view->day_info;
	Arg al[20];
	int ac;

	ac=0;
	XtSetArg(al[ac], XmNtopAttachment, XmATTACH_OPPOSITE_WIDGET); ac++;
        XtSetArg(al[ac], XmNtopWidget, c->canvas); ac++;
	XtSetArg(al[ac], XmNrightAttachment, XmATTACH_POSITION); ac++;
        XtSetArg(al[ac], XmNrightPosition, 40); ac++;
	XtSetArg(al[ac], XmNleftAttachment, XmATTACH_FORM); ac++;
        XtSetArg(al[ac], XmNleftOffset, 2); ac++;
        XtSetArg(al[ac], XmNbottomAttachment, XmATTACH_POSITION); ac++;
        XtSetArg(al[ac], XmNbottomPosition, 95); ac++;
        XtSetArg(al[ac], XmNleftAttachment, XmATTACH_FORM); ac++;
	d->panel_form = XmCreateForm(c->form, "quarter", al, ac);


	ac=0;
        XtSetArg(al[ac], XmNtopAttachment, XmATTACH_FORM); ac++;
        XtSetArg(al[ac], XmNrightAttachment, XmATTACH_FORM); ac++;
	XtSetArg(al[ac], XmNbottomAttachment, XmATTACH_POSITION); ac++;
        XtSetArg(al[ac], XmNbottomPosition, 33); ac++;
        XtSetArg(al[ac], XmNleftAttachment, XmATTACH_FORM); ac++;
	XtSetArg(al[ac], XmNmonth, month(previousmonth(c->view->date))); ac++;
	XtSetArg(al[ac], XmNyear, year(previousmonth(c->view->date))); ac++;
        d->month_panels[0] = XmCreateMonthPanel(d->panel_form, "lastMonth", al, ac);
        XtAddCallback(d->month_panels[0], XmNactivateCallback,
                        day_btn_cb, (XtPointer) c);
	XtAddCallback(d->month_panels[0], XmNhelpCallback, 
			help_view_cb, nullptr);

	ac=0;
        XtSetArg(al[ac], XmNtopAttachment, XmATTACH_POSITION); ac++;
	XtSetArg(al[ac], XmNtopPosition, 33); ac++;
        XtSetArg(al[ac], XmNrightAttachment, XmATTACH_FORM); ac++;
	XtSetArg(al[ac], XmNbottomAttachment, XmATTACH_POSITION); ac++;
        XtSetArg(al[ac], XmNbottomPosition, 66); ac++;
        XtSetArg(al[ac], XmNleftAttachment, XmATTACH_FORM); ac++;
	XtSetArg(al[ac], XmNmonth, month(c->view->date)); ac++;
	XtSetArg(al[ac], XmNyear, year(c->view->date)); ac++;
        d->month_panels[1] = XmCreateMonthPanel(d->panel_form, "thisMonth", al, ac);
        XtAddCallback(d->month_panels[1], XmNactivateCallback,
                        day_btn_cb, (XtPointer) c);
	XtAddCallback(d->month_panels[1], XmNhelpCallback, 
			help_view_cb, nullptr);

	ac=0;
        XtSetArg(al[ac], XmNtopAttachment, XmATTACH_POSITION); ac++;
        XtSetArg(al[ac], XmNtopPosition, 66); ac++;
        XtSetArg(al[ac], XmNrightAttachment, XmATTACH_FORM); ac++;
        XtSetArg(al[ac], XmNbottomAttachment, XmATTACH_FORM); ac++;
        XtSetArg(al[ac], XmNleftAttachment, XmATTACH_FORM); ac++;
	XtSetArg(al[ac], XmNmonth, month(nextmonth(c->view->date))); ac++;
	XtSetArg(al[ac], XmNyear, year(nextmonth(c->view->date))); ac++;
        d->month_panels[2] = XmCreateMonthPanel(d->panel_form, "nextMonth", al, ac);
        XtAddCallback(d->month_panels[2], XmNactivateCallback,
                        day_btn_cb, (XtPointer) c);
	XtAddCallback(d->month_panels[2], XmNhelpCallback, 
			help_view_cb, nullptr);

	ManageChildren(d->panel_form);

}

/*
 * This redisplays the 3 month panels 2 show the correct 3-month
 * window, centred around the month containing the "current" date.
 */
static void
update_quarter(Calendar *c)
{
	Day *d = (Day *)c->view->day_info;
        int year_num, month_num;

	/* previous month */
	year_num = year(previousmonth(c->view->date));
	month_num = month(previousmonth(c->view->date));
	XtVaSetValues(d->month_panels[0],
		XmNyear, year_num,
		XmNmonth, month_num,
		nullptr);
	
	/* current month */
	year_num = year(c->view->date);
	month_num = month(c->view->date);
	XtVaSetValues(d->month_panels[1],
		XmNyear, year_num,
		XmNmonth, month_num,
		nullptr);
	
	/* next month */
	year_num = year(nextmonth(c->view->date));
	month_num = month(nextmonth(c->view->date));
	XtVaSetValues(d->month_panels[2],
		XmNyear, year_num,
		XmNmonth, month_num,
		nullptr);
	
}

static void
day_btn_cb(Widget w, XtPointer client, XtPointer call)
{
        XmMonthPanelCallbackStruct *cbs = (XmMonthPanelCallbackStruct *) call;
        Calendar *c = (Calendar *)client;
        int monthno, year;
        int date = c->view->date;
 
	invalidate_cache(c);

        XtVaGetValues(w, XmNmonth, &monthno, XmNyear, &year, nullptr);
 
        if (cbs->type == MONTH_SELECTION) {

		XtUnmapWidget(c->canvas);

		/*
		 * Date update:
		 *	if month chosen is not current month
		 *	set date to 1st of new month, otherwise
		 *	don't change date.
		 */
		if (month(c->view->date) != monthno) {
			calendar_deselect (c);
			c->view->olddate = c->view->date;
			c->view->date = monthdayyear(monthno, 1, year);
			c->view->nwks = numwks(c->view->date);
			calendar_select (c, monthSelect, nullptr);
		}
 
                /* switch to month view */
                c->view->glance = monthGlance;
                cleanup_after_dayview(c);
                prepare_to_paint_monthview(c, nullptr);

		XtMapWidget(c->canvas);
        }
        else {	/* type == DAY_SELECTION */
                c->view->olddate = c->view->date;
                c->view->date = monthdayyear(monthno, cbs->day, year);
		gr_clear_area(c->xcontext, 0, 0, c->view->winw, c->view->winh);
		paint_dayview(c, True, nullptr, False); 
		calendar_select(c, hourSelect, (caddr_t)nullptr);
        }
}
