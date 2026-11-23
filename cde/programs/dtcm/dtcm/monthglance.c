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
**  monthglance.c
**
**  $XConsortium: monthglance.c /main/13 1996/11/21 19:43:07 drk $
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
static  char sccsid[] = "@(#)monthglance.c 1.82 95/07/27 Copyr 1994 Sun Microsystems, Inc.";
#endif

#include <EUSCompat.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/param.h> /* MAXPATHLEN defined here */
#ifdef SVR4
#include <sys/utsname.h> /* SYS_NMLN */
#endif /* SVR4 */
#include <dirent.h>
#include <ctype.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <rpc/rpc.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <X11/Xlib.h>
#include <X11/IntrinsicP.h>
#include <X11/cursorfont.h>
#include <Xm/Xm.h>
#include <Xm/DialogS.h>
#include <Xm/Form.h>
#include <Xm/List.h>
#include <Xm/PushB.h>
#include <Xm/DrawingA.h>
#include <Xm/Label.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
#include <Xm/ToggleBG.h>
#include <Dt/HourGlass.h>
#include "calendar.h"
#include "util.h"
#include "timeops.h"
#include "datefield.h"
#include "x_graphics.h"
#include "props.h"
#include "select.h"
#include "editor.h"
#include "group_editor.h"
#include "monthglance.h"
#include "weekglance.h"
#include "format.h"
#include "browser.h"
#include "blist.h"
#include "dayglance.h"
#include "yearglance.h"
#include "todo.h"
#include "find.h"
#include "goto.h"
#include "tempbr.h"
#include "misc.h"
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

static void 	paint_day_entries(Tick, int, int, int, int,
				  Paint_cache *, CSA_uint32,
				  const DtcmOverlayOccurrence *, int,
				  XRectangle *);
static Boolean	allocated(Calendar *);
static void	allocator(Calendar *);
static void	deallocator(Calendar *);
extern void	layout_children(Calendar *);
static void	display_header(Calendar *);
static void	quick_button_cb(Widget, XtPointer, XtPointer);
static Boolean	print_month ( Calendar *, int,
			     void *, Tick, Props *, Boolean);
static int	count_month_pages(Calendar *, Tick, int);
static void	paint_daynames(Calendar *, XRectangle *);
static void	paint_month(Calendar *, Tick, XRectangle *);
static void	unmanage_children(Calendar *);
static void	manage_children(Calendar *);
static int	count_month_appts(CSA_entry_handle *, int, Calendar *);

static Boolean
alloc_rgb_pixel(new_XContext *xc, unsigned short r, unsigned short g,
                unsigned short b, Pixel *out)
{
	if (!xc || !xc->display || !out)
		return False;
	XColor col;
	col.red = r;
	col.green = g;
	col.blue = b;
	col.flags = DoRed | DoGreen | DoBlue;
	Colormap cmap = DefaultColormap(xc->display, DefaultScreen(xc->display));
	if (XAllocColor(xc->display, cmap, &col)) {
		*out = col.pixel;
		return True;
	}
	return False;
}

static Boolean
reschedule_cached_appt(Calendar *c, int cache_idx, Tick target_day)
{
	if (!c || c->cal_handle == 0 || !c->paint_cache ||
	    cache_idx < 0 || cache_idx >= c->paint_cache_size)
		return False;

	Paint_cache *pc = &c->paint_cache[cache_idx];
	Tick src_day = lowerbound(pc->start_time);
	Tick dst_day = lowerbound(target_day);
	int delta_days = (int)((dst_day - src_day + (dst_day >= src_day ? 43200 : -43200)) / 86400);
	if (delta_days == 0)
		return False;

	Tick start = src_day;
	Tick stop = next_ndays(src_day, 1) - 1;
	CSA_attribute *range_attrs = nullptr;
	CSA_enum *ops = nullptr;
	int rcount = 0;
	CSA_uint32 n_entries = 0;
	CSA_entry_handle *list = nullptr;

	setup_range(&range_attrs, &ops, &rcount, start, stop, CSA_TYPE_EVENT,
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
	                                              CSA_ENTRY_ATTR_END_DATE_I,
	                                              CSA_ENTRY_ATTR_SUMMARY_I,
	                                              nullptr);
	for (CSA_uint32 i = 0; i < n_entries; i++) {
		if (query_appt_struct(c->cal_handle, list[i], appt) == CSA_SUCCESS) {
			Tick st = 0;
			if (appt->time && appt->time->value && appt->time->value->item.date_time_value)
				_csa_iso8601_to_tick(appt->time->value->item.date_time_value, &st);
			const char *sum = (appt->what && appt->what->value && appt->what->value->item.string_value)
			                  ? appt->what->value->item.string_value : "";
			if (st == pc->start_time &&
			    strcmp(sum, pc->summary ? pc->summary : "") == 0) {
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

	Tick new_start = dtcm_shift_time_by_days(pc->start_time, delta_days);
	Tick new_end = (pc->end_time > pc->start_time)
	               ? dtcm_shift_time_by_days(pc->end_time, delta_days)
	               : new_start;

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

static int
compute_pad_width(
	Cal_Font	*pf)
{
	int 		 nop,
			 width;

	CalTextExtents(pf, "1", 1, &nop, &nop, &width, &nop);

	return width;
}

static int
is_multiday_event(Tick st, Tick en)
{
	if (en <= st)
		return 0;
	/* End strictly past midnight of the start day (allow 1s tolerance for 23:59:59) */
	Tick next_mid = next_ndays(lowerbound(st), 1);
	return (en > next_mid + 60) ? 1 : 0;
}

static void
paint_day_entries(
	Tick	day,
	int	x,
	int	y,
	int	w,
	int	h,
	Paint_cache *cache,
	CSA_uint32 appt_total,
	const DtcmOverlayOccurrence *ovl,
	int ovl_count,
	XRectangle *rect)
{
        char *buf;
        int i, loop, nlines = 0, maxchars;
        Calendar *c=calendar;
        Props *p = (Props*)c->properties;
	DisplayType dt = get_int_prop(p, CP_DEFAULTDISP);
        Lines *lines=nullptr;
	Tick start_tick;
        new_XContext *xc    = 	c->xcontext;
	Cal_Font *pf =		c->fonts->viewfont;
	XFontSetExtents fontextents;
        int pfy;
        int maxlines;
        Tick lower      = 	lowerbound(day);
        Tick upper      = 	next_ndays(day, 1);

	CalFontExtents(pf, &fontextents);
        pfy = fontextents.max_ink_extent.height;
        maxlines = h / pfy;

	/* Count total matching events on this day to detect overflow */
	int total_for_day = 0;
	for (i = 0; i < (int)appt_total; i++) {
		if (!dtcm_filter_matches(cache[i].summary)) continue;
		if (is_multiday_event(cache[i].start_time, cache[i].end_time)) continue;
		if (cache[i].start_time >= lower && cache[i].start_time < upper)
			total_for_day++;
	}
	for (i = 0; i < ovl_count; i++) {
		if (!dtcm_filter_matches(ovl[i].summary)) continue;
		if (is_multiday_event((Tick)ovl[i].start_utc, (Tick)ovl[i].end_utc)) continue;
		if ((Tick)ovl[i].start_utc >= lower && (Tick)ovl[i].start_utc < upper)
			total_for_day++;
	}
	int has_overflow = (total_for_day > maxlines && maxlines >= 2) ? 1 : 0;
	int draw_limit = has_overflow ? (maxlines - 1) : maxlines;

	/* loop thru the list of appointments twice, first displaying 
	   the no time appointments, and then the appointments with times 
	   associated. */

	for (loop = 0; loop < 2; loop++) {
		for (i = 0; i < appt_total; i++) {
			if (!dtcm_filter_matches(cache[i].summary))
				continue;
			if (is_multiday_event(cache[i].start_time, cache[i].end_time))
				continue; /* Painted as horizontal spanning bar */
			start_tick = cache[i].start_time;
	                if (start_tick >= lower && 
			    start_tick < upper && 
			    (loop == cache[i].show_time)) {                        
				if (nlines < draw_limit) {
					Boolean	pad;
					static int pad_width = 0;
					char clean_sum[256], loc_buf[128], cat_buf[64];
					DtcmCategory cat = DTCM_CAT_NONE;
					int bar_indent = 0;

					dtcm_ics_decompose_what_ex(cache[i].summary,
						clean_sum, sizeof(clean_sum),
						loc_buf, sizeof(loc_buf),
						cat_buf, sizeof(cat_buf),
						nullptr, 0);
					cat = dtcm_category_from_name(cat_buf);

					if (cat != DTCM_CAT_NONE) {
						unsigned short cr, cg, cb;
						Pixel cpix;
						dtcm_category_rgb(cat, &cr, &cg, &cb);
						if (alloc_rgb_pixel(xc, cr, cg, cb, &cpix)) {
							if (rect)
								XSetClipRectangles(xc->display, xc->gc, 0, 0, rect, 1, Unsorted);
							XSetForeground(xc->display, xc->gc, cpix);
							XFillRectangle(xc->display, xc->xid, xc->gc,
								x, y - pfy + 2, 3, pfy > 3 ? pfy - 2 : pfy);
							XSetForeground(xc->display, xc->gc, xc->foreground);
							if (rect)
								XSetClipMask(xc->display, xc->gc, None);
							bar_indent = 5;
						}
					}

					dtcm_hittest_add(x, y - pfy + 2, w, pfy, i, day);

	                                lines = 
					     text_to_lines(clean_sum[0] ? clean_sum : cache[i].summary, 1);
	                                if (lines != nullptr) {
	                                        buf = ckalloc(
						      cm_strlen(lines->s) + 18);
	                                        pad = format_line(start_tick, 
							    lines->s, buf, 
							    start_tick, 
							    cache[i].show_time, 
							    dt);
	                                }
	                                else {
	                                        buf = ckalloc(15);
	                                        pad = format_line(start_tick, 
							    (char*)nullptr, buf, 
							    start_tick, 
							    cache[i].show_time,
							    dt);
	                                }
	                                destroy_lines(lines); lines=nullptr;

					if (!pad_width) {
						pad_width = 
							compute_pad_width(pf);
					}
					if (pad) {
	                                	maxchars = gr_nchars(
								w - pad_width - bar_indent, 
								buf, pf);
	                                	if (cm_strlen(buf) > maxchars)
							buf[maxchars] = '\0';
	                                	(void)gr_text(xc, x + pad_width + bar_indent,
							      y, pf, buf, rect);
					} else {
	                                	maxchars = gr_nchars(w - bar_indent, buf, 
								     pf);
	                                	if (cm_strlen(buf) > maxchars)
							buf[maxchars] = '\0';
	                                	(void)gr_text(xc, x + bar_indent, y, pf, 
							      buf, rect);
					}
	                                free(buf); buf=nullptr;
	                                y = y + pfy;
	                                nlines++;
	                        }
	                }
	                else
				continue;
	        }
	}

	/* Paint single-day overlaid calendar occurrences in this day cell */
	for (i = 0; i < ovl_count && nlines < draw_limit; i++) {
		if (!dtcm_filter_matches(ovl[i].summary))
			continue;
		if (is_multiday_event((Tick)ovl[i].start_utc, (Tick)ovl[i].end_utc))
			continue;
		if ((Tick)ovl[i].start_utc < lower || (Tick)ovl[i].start_utc >= upper)
			continue;

		unsigned short ar, ag, ab, pr, pg, pb;
		Pixel apix, ppix;
		dtcm_category_rgb(ovl[i].color, &ar, &ag, &ab);
		dtcm_category_pastel_rgb(ovl[i].color, &pr, &pg, &pb);
		if (rect)
			XSetClipRectangles(xc->display, xc->gc, 0, 0, rect, 1, Unsorted);
		if (alloc_rgb_pixel(xc, pr, pg, pb, &ppix)) {
			XSetForeground(xc->display, xc->gc, ppix);
			XFillRectangle(xc->display, xc->xid, xc->gc,
				x, y - pfy + 2, w > 2 ? w - 1 : w, pfy > 2 ? pfy - 1 : pfy);
		}
		if (alloc_rgb_pixel(xc, ar, ag, ab, &apix)) {
			XSetForeground(xc->display, xc->gc, apix);
			XFillRectangle(xc->display, xc->xid, xc->gc,
				x, y - pfy + 2, 3, pfy > 2 ? pfy - 1 : pfy);
		}
		XSetForeground(xc->display, xc->gc, xc->foreground);
		if (rect)
			XSetClipMask(xc->display, xc->gc, None);

		char obuf[256];
		snprintf(obuf, sizeof(obuf), "%s", ovl[i].summary);
		maxchars = gr_nchars(w - 6, obuf, pf);
		if ((int)cm_strlen(obuf) > maxchars && maxchars >= 0)
			obuf[maxchars] = '\0';
		(void)gr_text(xc, x + 5, y, pf, obuf, rect);
		y += pfy;
		nlines++;
	}

	if (has_overflow && total_for_day > nlines) {
		int extra = total_for_day - nlines;
		char mbuf[32];
		snprintf(mbuf, sizeof(mbuf), "+%d more", extra);
		Pixel blue_pix;
		if (alloc_rgb_pixel(xc, 0x1a00, 0x5200, 0x9600, &blue_pix)) {
			XSetForeground(xc->display, xc->gc, blue_pix);
		}
		(void)gr_text(xc, x + 4, y, pf, mbuf, rect);
		XSetForeground(xc->display, xc->gc, xc->foreground);
		/* Register clickable hit box with cache_idx = -2 for "+N more" popover */
		dtcm_hittest_add(x, y - pfy, w, pfy + 2, -2, day);
	}
}

extern void
month_button(Widget widget, XtPointer data, XtPointer cbs)
{
	Calendar *c = calendar;

	if (c->view->glance == monthGlance)
		return;

	XtUnmapWidget(c->canvas);

	switch (c->view->glance) {
		case dayGlance:
			c->view->glance = monthGlance;
			cleanup_after_dayview(c);
			break;
		case weekGlance:
			c->view->glance = monthGlance;
			cleanup_after_weekview(c);
			break;
		case yearGlance:
			cleanup_after_yearview(c);
			c->view->glance = monthGlance;
			break;
		case agendaGlance:
			cleanup_after_agenda(c);
			c->view->glance = monthGlance;
			break;
		default:
			break;
	}
        prepare_to_paint_monthview (c, nullptr);

	XtMapWidget(c->canvas);
}

/*
 * Do the actual month grid, headers and appointments
 */
static void
paint_month(Calendar *c, Tick key, XRectangle *rect)
{
        Month    	*m = (Month *)c->view->month_info;
        int 		x, y, i, j;
	CSA_uint32 	a_total;
        int 		firstdom, boxw, boxh, dayname_height, margin, default_height;
        Tick 		day;
        struct tm 	tm;
        new_XContext 	*xc;
	Dimension 	btn_ht=0, btn_w=0;
	time_t 		start, stop;
	CSA_enum 	*ops;
	CSA_attribute 	*range_attrs;
	CSA_entry_handle *list = nullptr;
	XFontSetExtents fontextents;
	_Xltimeparams	localtime_buf;
 
        tm              = *_XLocaltime(&key, localtime_buf);
        tm.tm_mday      = 1;
#if defined(SVR4) || defined(__linux__)
        tm.tm_isdst      = -1;
        day             = mktime(&tm);
#else
        day             = timelocal(&tm);
#endif /* SVR4 */
        m->ndays        = ((tm.tm_mon==1) && leapyr(tm.tm_year+1900))? 29 :
                                monthdays[tm.tm_mon];
        tm              = *_XLocaltime(&day, localtime_buf);
        firstdom        = tm.tm_wday;
        boxw            = calendar->view->boxw;
        boxh            = calendar->view->boxh;
        margin          = calendar->view->outside_margin;
	dayname_height  = ((Month *) calendar->view->month_info)->dayname_height;
        xc              = calendar->xcontext;
 

	if (c->paint_cache == nullptr) {
        	start = (time_t) lowerbound(day);
        	stop = (time_t) last_dom(day);
		setup_range(&range_attrs, &ops, &j, start, stop, CSA_TYPE_EVENT,
		    	0, B_FALSE, c->general->version);
		csa_list_entries(c->cal_handle, j, range_attrs, ops, &a_total, &list, nullptr);
	
		free_range(&range_attrs, &ops, j);
		allocate_paint_cache(list, a_total, &c->paint_cache);
		c->paint_cache_size = a_total;
		csa_free(list);
	}

	CalFontExtents(c->fonts->viewfont, &fontextents);
	default_height = fontextents.max_logical_extent.height;
	dtcm_hittest_clear();
        Tick today_tick = now();

	/* Query enabled overlaid calendars for this month */
	DtcmOverlayOccurrence ovl[128];
	int ovl_count = dtcm_overlay_query_range((time_t)lowerbound(day),
						 (time_t)last_dom(day),
						 ovl, 128);

	/* Compute button height once for multi-day span bar placement */
	btn_ht = 18;
	if (m->ndays > 0 && m->hot_button[0]) {
		XtVaGetValues(m->hot_button[0], XmNheight, &btn_ht,
			XmNwidth, &btn_w, nullptr);
	}

	/* Paint continuous horizontal multi-day spanning bars per week row */
	int row_reserved[6] = { 0, 0, 0, 0, 0, 0 };
	int bar_h = default_height + 2;
	if (bar_h < 14) bar_h = 14;
	Tick month_first_day = lowerbound(day);
	int total_rows = (firstdom + m->ndays + 6) / 7;
	if (total_rows > 6) total_rows = 6;

	for (int row = 0; row < total_rows; row++) {
		DtcmMultiDaySpan spans[64];
		int nspans = 0;
		/* First day index (1-based) in this row, or <=0 if before month */
		int row_first_dom = row * 7 - firstdom + 1;
		int row_last_dom = row_first_dom + 6;
		int vis_start_col = (row_first_dom < 1) ? (1 - row_first_dom) : 0;
		int vis_end_col = (row_last_dom > m->ndays) ? (6 - (row_last_dom - m->ndays)) : 6;
		if (vis_start_col > vis_end_col) continue;

		Tick row_day0 = next_ndays(month_first_day, row_first_dom - 1);
		Tick row_day6_end = next_ndays(row_day0, 7) - 1;

		/* Primary calendar multi-day events */
		for (CSA_uint32 ci = 0; ci < c->paint_cache_size && nspans < 64; ci++) {
			Paint_cache *pc = &c->paint_cache[ci];
			if (!dtcm_filter_matches(pc->summary))
				continue;
			if (!is_multiday_event(pc->start_time, pc->end_time))
				continue;
			if (pc->end_time < row_day0 || pc->start_time > row_day6_end)
				continue;
			int sc = (int)((lowerbound(pc->start_time) - row_day0) / 86400);
			int ec = (int)((lowerbound(pc->end_time - 60) - row_day0) / 86400);
			int cl = (sc < vis_start_col) ? 1 : 0;
			int cr = (ec > vis_end_col) ? 1 : 0;
			if (sc < vis_start_col) sc = vis_start_col;
			if (ec > vis_end_col) ec = vis_end_col;
			if (sc > ec) continue;
			spans[nspans].cache_idx = (int)ci;
			spans[nspans].start_col = sc;
			spans[nspans].end_col = ec;
			spans[nspans].lane = 0;
			spans[nspans].continues_left = cl;
			spans[nspans].continues_right = cr;
			nspans++;
		}
		/* Overlay multi-day events (encoded as cache_idx = -(oi + 1)) */
		for (int oi = 0; oi < ovl_count && nspans < 64; oi++) {
			if (!dtcm_filter_matches(ovl[oi].summary))
				continue;
			if (!is_multiday_event((Tick)ovl[oi].start_utc, (Tick)ovl[oi].end_utc))
				continue;
			if ((Tick)ovl[oi].end_utc < row_day0 || (Tick)ovl[oi].start_utc > row_day6_end)
				continue;
			int sc = (int)((lowerbound((Tick)ovl[oi].start_utc) - row_day0) / 86400);
			int ec = (int)((lowerbound((Tick)ovl[oi].end_utc - 60) - row_day0) / 86400);
			int cl = (sc < vis_start_col) ? 1 : 0;
			int cr = (ec > vis_end_col) ? 1 : 0;
			if (sc < vis_start_col) sc = vis_start_col;
			if (ec > vis_end_col) ec = vis_end_col;
			if (sc > ec) continue;
			spans[nspans].cache_idx = -(oi + 1);
			spans[nspans].start_col = sc;
			spans[nspans].end_col = ec;
			spans[nspans].lane = 0;
			spans[nspans].continues_left = cl;
			spans[nspans].continues_right = cr;
			nspans++;
		}

		if (nspans > 0) {
			int nlanes = dtcm_compute_multiday_spans(spans, nspans, 7);
			int max_vis_lanes = (boxh - btn_ht - 8) / (bar_h + 2);
			if (max_vis_lanes > 3) max_vis_lanes = 3;
			if (max_vis_lanes < 1) max_vis_lanes = 1;
			int used = nlanes < max_vis_lanes ? nlanes : max_vis_lanes;
			row_reserved[row] = used * (bar_h + 2);

			int row_origin_y = (row * boxh) + dayname_height + c->view->topoffset;
			for (int si = 0; si < nspans; si++) {
				if (spans[si].lane >= max_vis_lanes)
					continue;
				int bx = (spans[si].start_col * boxw) + margin + 2;
				int bw = ((spans[si].end_col - spans[si].start_col + 1) * boxw) - 4;
				int by = row_origin_y + btn_ht + 2 + spans[si].lane * (bar_h + 2);
				if (bw < 8) bw = 8;

				DtcmCategory cat = DTCM_CAT_WORK;
				char label[256] = "";
				if (spans[si].cache_idx >= 0) {
					Paint_cache *pc = &c->paint_cache[spans[si].cache_idx];
					char csum[200], cloc[64], ccat[64];
					dtcm_ics_decompose_what_ex(pc->summary, csum, sizeof(csum),
						cloc, sizeof(cloc), ccat, sizeof(ccat), nullptr, 0);
					DtcmCategory parsed = dtcm_category_from_name(ccat);
					if (parsed != DTCM_CAT_NONE) cat = parsed;
					snprintf(label, sizeof(label), "%s%s%s",
						spans[si].continues_left ? "< " : "",
						csum[0] ? csum : (pc->summary ? pc->summary : ""),
						spans[si].continues_right ? " >" : "");
					dtcm_hittest_add(bx, by, bw, bar_h, spans[si].cache_idx, pc->start_time);
				} else {
					int oi = -spans[si].cache_idx - 1;
					cat = ovl[oi].color;
					snprintf(label, sizeof(label), "%s%s%s",
						spans[si].continues_left ? "< " : "",
						ovl[oi].summary,
						spans[si].continues_right ? " >" : "");
				}

				unsigned short pr, pg, pb, ar, ag, ab;
				Pixel ppix, apix;
				dtcm_category_pastel_rgb(cat, &pr, &pg, &pb);
				dtcm_category_rgb(cat, &ar, &ag, &ab);
				if (rect)
					XSetClipRectangles(xc->display, xc->gc, 0, 0, rect, 1, Unsorted);
				if (alloc_rgb_pixel(xc, pr, pg, pb, &ppix)) {
					XSetForeground(xc->display, xc->gc, ppix);
					XFillRectangle(xc->display, xc->xid, xc->gc, bx, by, (unsigned int)bw, (unsigned int)bar_h);
				}
				if (alloc_rgb_pixel(xc, ar, ag, ab, &apix)) {
					XSetForeground(xc->display, xc->gc, apix);
					XFillRectangle(xc->display, xc->xid, xc->gc, bx, by, 4, (unsigned int)bar_h);
					XDrawRectangle(xc->display, xc->xid, xc->gc, bx, by, (unsigned int)(bw - 1), (unsigned int)(bar_h - 1));
				}
				XSetForeground(xc->display, xc->gc, xc->foreground);
				if (rect)
					XSetClipMask(xc->display, xc->gc, None);

				int mc = gr_nchars(bw - 10, label, c->fonts->viewfont);
				if ((int)cm_strlen(label) > mc && mc >= 0)
					label[mc] = '\0';
				(void)gr_text(xc, bx + 7, by + default_height - 1, c->fonts->viewfont, label, rect);
			}
		}
	}

        x = firstdom; y = 0;
        for(i = 1; i <= m->ndays; i++) {
		int box_origin_x=0, box_origin_y=0;

		box_origin_x = (x * boxw) + margin;
		box_origin_y = (y * boxh) + dayname_height + c->view->topoffset;

		if (dtcm_is_same_day(day, today_tick)) {
			Pixel accent_pix;
			if (!alloc_rgb_pixel(xc, 0xc000, 0x3900, 0x2b00, &accent_pix))
				accent_pix = xc->hilight_color;
			if (rect)
				XSetClipRectangles(xc->display, xc->gc, 0, 0, rect, 1, Unsorted);
			XSetForeground(xc->display, xc->gc, accent_pix);
			XFillRectangle(xc->display, xc->xid, xc->gc,
				box_origin_x + 1, box_origin_y + 1,
				boxw > 2 ? boxw - 2 : boxw, 4);
			XSetForeground(xc->display, xc->gc, xc->foreground);
			if (rect)
				XSetClipMask(xc->display, xc->gc, None);
		}

		XtVaGetValues(m->hot_button[i-1], XmNheight, &btn_ht,
			XmNwidth, &btn_w, nullptr);

		/*
		 * Paint the list of calendar entries for this day below any multi-day bars
		 */
		int res_y = (y >= 0 && y < 6) ? row_reserved[y] : 0;
		paint_day_entries(day,
			box_origin_x + 2,
                        box_origin_y + default_height + btn_ht + res_y, 
			boxw - 4, (boxh - btn_ht - 4 - res_y) > 0 ? (boxh - btn_ht - 4 - res_y) : 0,
			c->paint_cache, c->paint_cache_size,
			ovl, ovl_count, rect);

                day = nextday(day);
                x++;
                if (x > 6 & i != m->ndays) {
                        x = 0;
                        y++;
                }
        }
}

/*
 * Set the label header for the view to current month name
 */
static void
display_header(Calendar *c)
{
	Month 		*m = (Month *) c->view->month_info;
	char 		buf[BUFSIZ];
	XmString 	str;
	Position 	x, y;
	struct tm 	*tm_ret;
	Tick 		tmptick = c->view->date;
	_Xltimeparams	localtime_buf;

/* label */
	tm_ret = _XLocaltime(&tmptick, localtime_buf);

	/* NL_COMMENT
	   Attention Translator:

	   This string is used in the calendar month view.  In the C locale
	   it has the form:

		July, 1995

	   strftime conversion string: "%B, %Y" is used.

	   Use the appropriate strftime conversion for your locale.
	*/
	sprintf( buf, "%s, %d", months[tm_ret->tm_mon+1],
		tm_ret->tm_year + 1900 );

	str = XmStringCreateLocalized(buf);
	XtVaSetValues(m->month_label, XmNlabelString, str, nullptr);
	XmStringFree(str);

/* positioning */
	x = c->view->outside_margin; y = 15;
	XtVaSetValues(m->month_label, XtNx, x, XtNy, y, nullptr);

/* now manage it */
	if (!XtIsManaged(m->month_label)) {
		XtManageChild(m->month_label);
	}
}

static void
paint_daynames(Calendar *c, XRectangle    *rect)
{
        int 		i, middle;
        int 		boxw = c->view->boxw;
        int 		margin = c->view->outside_margin;
        int 		dayname_height = 
				((Month *)c->view->month_info)->dayname_height;
        Cal_Font 	*pf = c->fonts->viewfont;
        new_XContext 	*xc = c->xcontext;

        for(i = 0; i < 7; i++) {
                gr_draw_box(xc, (boxw * i)+margin,
                        c->view->topoffset,
                        boxw, dayname_height, rect);
                middle = gr_center(boxw, days[i], pf);
                gr_text(xc, (boxw*i)+middle+margin, 
				c->view->topoffset+dayname_height-3,
                        	pf, days[i], rect);
        }
}

extern void
paint_grid(Calendar *c, XRectangle *rect)
{
        int i;
        int boxh        = c->view->boxh;
        int boxw        = c->view->boxw;
        int nrows       = c->view->nwks;
        int margin      = c->view->outside_margin;
        int dayname_height = ((Month *)c->view->month_info)->dayname_height;
        int rightmargin = margin + 7 * boxw;
        int bottomargin = c->view->topoffset+ dayname_height+boxh*nrows;
        new_XContext *xc    = c->xcontext;

        /* Subtle weekend shading on Sunday (col 0) and Saturday (col 6) */
        if (xc && xc->display && boxw > 2 && boxh > 2) {
                XColor bgcol;
                Colormap cmap = DefaultColormap(xc->display, DefaultScreen(xc->display));
                bgcol.pixel = xc->background;
                XQueryColor(xc->display, cmap, &bgcol);
                unsigned short dr = (bgcol.red > 0x0d00) ? (bgcol.red - 0x0d00) : bgcol.red;
                unsigned short dg = (bgcol.green > 0x0d00) ? (bgcol.green - 0x0d00) : bgcol.green;
                unsigned short db = (bgcol.blue > 0x0a00) ? (bgcol.blue - 0x0a00) : bgcol.blue;
                Pixel shade_pix;
                if (alloc_rgb_pixel(xc, dr, dg, db, &shade_pix)) {
                        if (rect)
                                XSetClipRectangles(xc->display, xc->gc, 0, 0, rect, 1, Unsorted);
                        XSetForeground(xc->display, xc->gc, shade_pix);
                        XFillRectangle(xc->display, xc->xid, xc->gc,
                                margin + 1, c->view->topoffset + dayname_height + 1,
                                boxw - 1, boxh * nrows - 1);
                        XFillRectangle(xc->display, xc->xid, xc->gc,
                                margin + 6 * boxw + 1, c->view->topoffset + dayname_height + 1,
                                boxw - 1, boxh * nrows - 1);
                        XSetForeground(xc->display, xc->gc, xc->foreground);
                        if (rect)
                                XSetClipMask(xc->display, xc->gc, None);
                }
        }

        /* horizontal */                   
        for (i = 1; i <= nrows; i++) {
                gr_draw_line(xc, margin,
                        (i*boxh)+c->view->topoffset+dayname_height,
                         rightmargin, i*boxh+c->view->topoffset+dayname_height,
                        gr_solid, rect);
        }
 
        /* vertical */
        for (i = 0; i < 8; i++) {
                gr_draw_line(xc, margin+(i*boxw),
                        c->view->topoffset+dayname_height,
                        margin+(i*boxw), bottomargin, gr_solid, rect);
        }
        /* embolden grid outline */
        gr_draw_box(xc, margin-1,
                c->view->topoffset-1, 7*boxw+2,
                        nrows*boxh+2+dayname_height, rect);

        /* ISO week numbers (W01-W53) in left margin */
        if (margin >= 18) {
                struct tm tm_first;
                _Xltimeparams localtime_buf;
                Tick d1 = c->view->date;
                tm_first = *_XLocaltime(&d1, localtime_buf);
                tm_first.tm_mday = 1;
                tm_first.tm_isdst = -1;
                Tick row_day = mktime(&tm_first);
                struct tm *tm_r = _XLocaltime(&row_day, localtime_buf);
                int fday_wday = tm_r->tm_wday;
                /* First Sunday of row 0 */
                row_day = dtcm_shift_time_by_days(row_day, -fday_wday);
                for (i = 0; i < nrows; i++) {
                        /* Thursday of this week (Sunday + 4) determines ISO week */
                        Tick thu = dtcm_shift_time_by_days(row_day, i * 7 + 4);
                        struct tm *tm_thu = _XLocaltime(&thu, localtime_buf);
                        int wk = dtcm_iso_week_number(tm_thu->tm_year + 1900,
                                                      tm_thu->tm_mon + 1,
                                                      tm_thu->tm_mday);
                        char wbuf[8];
                        snprintf(wbuf, sizeof(wbuf), "W%02d", wk);
                        int wy = c->view->topoffset + dayname_height + i * boxh + 18;
                        gr_text(xc, 2, wy, c->fonts->viewfont, wbuf, rect);
                }
        }
}

extern void
layout_month(
	Calendar	*c,
	Tick		 date)
{
	Month		*m = (Month *)c->view->month_info;
	int		 i, x, y,
			 firstdom,
			 boxw = calendar->view->boxw,
			 boxh = calendar->view->boxh,
			 margin = calendar->view->outside_margin,
			 dayname_height = m->dayname_height;
	struct	tm	 tm;
	Tick		 day;
	_Xltimeparams	 localtime_buf;

	tm = *_XLocaltime(&date, localtime_buf);
	tm.tm_mday = 1;
#if defined(SVR4) || defined(__linux__)
	tm.tm_isdst = -1;
	day = mktime(&tm);
#else
	day = timelocal(&tm);
#endif /* SVR4 */
	m->ndays = ((tm.tm_mon==1) && 
		    leapyr(tm.tm_year+1900))? 29 : monthdays[tm.tm_mon];
	tm = *_XLocaltime(&day, localtime_buf);
	firstdom = tm.tm_wday;

	x = firstdom;
	y = 0;
	for(i = 1; i <= m->ndays; i++) {
		int	box_origin_x = 0,
			box_origin_y = 0;

		box_origin_x = (x * boxw) + margin;
		box_origin_y = (y * boxh) + dayname_height + c->view->topoffset;

		m->button_loc[i-1].x = box_origin_x + 3;
		m->button_loc[i-1].y = box_origin_y + 3;

		x++;
		if (x > 6 & i != m->ndays) {
			x = 0;
			y++;
		}
		XtMoveWidget(m->hot_button[i-1], 
				m->button_loc[i-1].x, m->button_loc[i-1].y);
	}

	/* Unmanage any unneeded navigator buttons (ie. 29, 30, 31) */
	for (i=m->ndays; i<31; i++)
		if (XtIsManaged(m->hot_button[i]))
			XtUnmanageChild(m->hot_button[i]);
}

extern void
repaint_damaged_month(
	Calendar	*c,
	XRectangle	*rect)
{
	/* allocate Month memory & widgets if necessary */
	if (!allocated(c)) {
		XFontSetExtents 	boldfontextents;

		allocator(c);
		XmToggleButtonGadgetSetState(c->month_scope, True, False);
		CalFontExtents(c->fonts->boldfont, &boldfontextents);
		((Month *) c->view->month_info)->dayname_height =  
				boldfontextents.max_logical_extent.height + 6;
		layout_month(c, c->view->date); 
		display_header(c);
		manage_children(c);
	}

        paint_daynames(c, rect);
        paint_grid(c, rect);
        paint_month(c, c->view->date, rect);
        calendar_select(c, daySelect, (caddr_t)nullptr);
}

extern void
prepare_to_paint_monthview(Calendar *c, XRectangle *rect)
{
        Dimension 		w, h;
        c = calendar;

	XmToggleButtonGadgetSetState(c->month_scope, True, False);
        XtVaGetValues(c->canvas, XmNwidth, &w, XmNheight, &h, nullptr);
        cache_dims(c, w, h);

	/* allocate Month memory & widgets if necessary */
	if (!allocated(c)) {
		XFontSetExtents 	boldfontextents;

		allocator(c);
		CalFontExtents(c->fonts->boldfont, &boldfontextents);
		((Month *) c->view->month_info)->dayname_height = 
				boldfontextents.max_logical_extent.height + 6;
	}
 
	/*
	 * need to unmanage buttons while drawing to avoid many exposures
	 * when they are moved
	 */
	display_header(c);
	layout_month(c, c->view->date); 
        gr_clear_area(c->xcontext, 0, 0, c->view->winw, c->view->winh);
	manage_children(c);
}

/* ADDED FOR PRINTING */
extern void
get_time_str (Dtcm_appointment *appt, char *buf)
{
        Calendar 	*c = calendar;
        Props 		*p = (Props*)c->properties;
	DisplayType 	dt = get_int_prop(p, CP_DEFAULTDISP);
        struct tm 	*tm;
        int 		hr, mn;
	Tick 		start_tick;
	_Xltimeparams	localtime_buf;

	_csa_iso8601_to_tick(appt->time->value->item.date_time_value, &start_tick);
	buf[0] = '\0';

        if (appt==nullptr || !showtime_set(appt) || magic_time(start_tick))
                return;

        tm = _XLocaltime(&start_tick, localtime_buf);
        hr = tm->tm_hour;
        mn = tm->tm_min;

        if (dt == HOUR12) {
                adjust_hour(&hr);
                (void) sprintf(buf, "%2d:%02d ", hr, mn);
        }
        else
                (void) sprintf(buf, "%02d%02d ", hr, mn);
}

static void
month_overflow_close_cb(Widget w, XtPointer cd, XtPointer call_data)
{
	(void)w; (void)call_data;
	Widget shell = (Widget)cd;
	if (shell) {
		XtPopdown(shell);
		XtDestroyWidget(shell);
	}
}

static void
show_month_overflow_popover(Calendar *c, Tick day)
{
	if (!c || !c->frame) return;
	Tick lower = lowerbound(day);
	Tick upper = next_ndays(day, 1);

	char title[128];
	struct tm tm;
	time_t dt = (time_t)day;
	localtime_r(&dt, &tm);
	snprintf(title, sizeof(title), "Appointments - %04d-%02d-%02d",
		 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

	Widget shell = XmCreateDialogShell(c->frame, "monthOverflowDlg", nullptr, 0);
	XtVaSetValues(shell, XmNtitle, title, nullptr);

	Widget form = XtVaCreateWidget("moForm", xmFormWidgetClass, shell,
		XmNmarginWidth, 10, XmNmarginHeight, 10,
		XmNwidth, 360, XmNheight, 260,
		nullptr);

	XmString cls_s = XmStringCreateLocalized((char *)"Close");
	Widget cls_btn = XtVaCreateManagedWidget("moClose", xmPushButtonWidgetClass, form,
		XmNlabelString, cls_s,
		XmNbottomAttachment, XmATTACH_FORM,
		XmNrightAttachment, XmATTACH_FORM,
		nullptr);
	XmStringFree(cls_s);
	XtAddCallback(cls_btn, XmNactivateCallback, month_overflow_close_cb, (XtPointer)shell);

	Arg al[4];
	int ac = 0;
	XtSetArg(al[ac], XmNvisibleItemCount, 10); ac++;
	Widget list_w = XmCreateScrolledList(form, (char *)"moList", al, ac);
	XtManageChild(list_w);
	XtVaSetValues(XtParent(list_w),
		XmNtopAttachment, XmATTACH_FORM,
		XmNleftAttachment, XmATTACH_FORM,
		XmNrightAttachment, XmATTACH_FORM,
		XmNbottomAttachment, XmATTACH_WIDGET,
		XmNbottomWidget, cls_btn,
		XmNbottomOffset, 8,
		nullptr);

	if (c->paint_cache && c->paint_cache_size > 0) {
		for (int i = 0; i < c->paint_cache_size; i++) {
			if (c->paint_cache[i].start_time >= lower &&
			    c->paint_cache[i].start_time < upper) {
				struct tm stm;
				time_t st = (time_t)c->paint_cache[i].start_time;
				localtime_r(&st, &stm);
				char clean_sum[256], line[320];
				dtcm_ics_decompose_what_ex(c->paint_cache[i].summary,
					clean_sum, sizeof(clean_sum),
					nullptr, 0, nullptr, 0, nullptr, 0);
				if (c->paint_cache[i].show_time)
					snprintf(line, sizeof(line), "%02d:%02d  %s",
						 stm.tm_hour, stm.tm_min, clean_sum);
				else
					snprintf(line, sizeof(line), "(All Day)  %s", clean_sum);
				XmString s = XmStringCreateLocalized(line);
				XmListAddItemUnselected(list_w, s, 0);
				XmStringFree(s);
			}
		}
	}

	XtManageChild(form);
	XtPopup(shell, XtGrabNone);
}

extern void
month_event(XEvent *event)
{
        static int 	lastcol, lastrow;
        static XEvent 	lastevent;
        static int      drag_cache_idx = -1;
        static int      drag_start_x = 0, drag_start_y = 0;
        static Tick     drag_source_day = 0;
        static Boolean  drag_active = False;
        static Cursor   drag_cursor = None;
        int 		x, y, j, toffset;
        int 		boxw, boxh, row, col, margin, dayname_height;
        Tick 		date;
	Calendar 	*c = calendar;
        Editor 		*e = (Editor*)c->editor;
        ToDo 		*t = (ToDo*)c->todo;
        GEditor 	*ge = (GEditor*)c->geditor;

        boxw    = c->view->boxw;
        boxh    = c->view->boxh;
        margin  = c->view->outside_margin;
        dayname_height  = ((Month *) c->view->month_info)->dayname_height;
        date    = c->view->date;
        x       = event->xbutton.x;
        y       = event->xbutton.y;
        toffset = c->view->topoffset;

        if (boxw == 0)
                col = 0;
        else
                col     = (x-margin)/boxw;
        if  (boxh == 0)
                row = 0;
        else
                row     = (y-c->view->topoffset-dayname_height)/boxh;

        /* boundary conditions */
        if (x < margin || col > 6 || y < (dayname_height+toffset) || row > c->view->nwks-1) {
                if (event->type == ButtonRelease && drag_active) {
                        XUndefineCursor(XtDisplay(c->canvas), XtWindow(c->canvas));
                        drag_active = False;
                        drag_cache_idx = -1;
                        set_message(c->message_text, "");
                }
                calendar_deselect(c);
                lastcol=0; lastrow=0;
                return;
        }

	/* boundary check for click in invalid cell */
	if (((col < fdom(date)) && (row==0)) ||
	    ((col > ldom(date)) && (row==c->view->nwks - 1))) {
		if (event->type == ButtonRelease && drag_active) {
			XUndefineCursor(XtDisplay(c->canvas), XtWindow(c->canvas));
			drag_active = False;
			drag_cache_idx = -1;
			set_message(c->message_text, "");
		}
		calendar_deselect(c);
		lastcol=0; lastrow=0;
		return;
	}

	switch(event->type) {
	case MotionNotify:
                if (drag_cache_idx >= 0 && !drag_active) {
                        int dx = x - drag_start_x;
                        int dy = y - drag_start_y;
                        if (dx * dx + dy * dy >= 36) {
                                drag_active = True;
                                if (drag_cursor == None)
                                        drag_cursor = XCreateFontCursor(XtDisplay(c->canvas), XC_fleur);
                                XDefineCursor(XtDisplay(c->canvas), XtWindow(c->canvas), drag_cursor);
                                set_message(c->message_text, "Release on a day cell to reschedule appointment...");
                        }
                }
                if (col !=lastcol || row !=lastrow) {
                        calendar_deselect(c);
                        j = xytoclock(col+1, row+1, date);
                        if (j > 0) {
                                c->view->olddate = c->view->date;
                                c->view->date = j;
                                calendar_select(c, daySelect, (caddr_t)nullptr);
                        }
                        lastcol=col;
                        lastrow=row;
                }
                break;
      case ButtonRelease:
                if (drag_active && drag_cache_idx >= 0) {
                        XUndefineCursor(XtDisplay(c->canvas), XtWindow(c->canvas));
                        j = xytoclock(col+1, row+1, date);
                        if (j > 0 && !dtcm_is_same_day(j, drag_source_day)) {
                                if (reschedule_cached_appt(c, drag_cache_idx, j)) {
                                        invalidate_cache(c);
                                        c->view->olddate = c->view->date;
                                        c->view->date = j;
                                        prepare_to_paint_monthview(c, nullptr);
                                        set_message(c->message_text, "Appointment rescheduled.");
                                } else {
                                        set_message(c->message_text, "Could not reschedule appointment.");
                                }
                        } else {
                                set_message(c->message_text, "");
                        }
                }
                drag_active = False;
                drag_cache_idx = -1;
                break;
      case ButtonPress:
                if (event->xbutton.button == Button3) {
                        calendar_deselect(c);
                        j = xytoclock(col+1, row+1, date);
                        if (j > 0) {
                                c->view->olddate = c->view->date;
                                c->view->date = j;
                                calendar_select(c, daySelect, (caddr_t)nullptr);
                        }
                        const DtcmHitBox *hb = dtcm_hittest_find(x, y);
                        dtcm_context_menu_popup(c, &event->xbutton, j,
                                                hb ? hb->cache_idx : -1);
                        lastcol=col;
                        lastrow=row;
                        break;
                }
                drag_active = False;
                drag_cache_idx = -1;
                if (event->xbutton.button == Button1) {
                        const DtcmHitBox *hb = dtcm_hittest_find(x, y);
                        if (hb && hb->cache_idx == -2) {
                                show_month_overflow_popover(c, hb->day);
                                break;
                        }
                        if (hb) {
                                drag_cache_idx = hb->cache_idx;
                                drag_start_x = x;
                                drag_start_y = y;
                                drag_source_day = hb->day;
                        }
                }
		if (ds_is_double_click(&lastevent, event)) {
			drag_cache_idx = -1;
			_DtTurnOnHourGlass(c->frame);
			if (lastcol == col && lastrow == row) {
				const DtcmHitBox *hb = dtcm_hittest_find(x, y);
				if (hb && c->paint_cache && hb->cache_idx >= 0 &&
				    hb->cache_idx < c->paint_cache_size) {
					Paint_cache *pc = &c->paint_cache[hb->cache_idx];
					show_editor(c, pc->start_time, pc->end_time,
						    pc->show_time ? False : True);
					editor_select_appt(c, pc->start_time, pc->summary);
				} else {
					show_editor(c, 0, 0, False);
				}
			}
			_DtTurnOffHourGlass(c->frame);
		}
		else {
			calendar_deselect(c);
			j = xytoclock(col+1, row+1, date);
			if (j > 0) {
			   c->view->olddate = c->view->date;
			   c->view->date = j;
			   calendar_select(c, daySelect, (caddr_t)nullptr);
			}

			if (editor_showing(e)) {
				set_editor_defaults(e, 0, 0, False);
				add_all_appt(e);
			}
		}
		if (todo_showing(t)) {
			set_todo_defaults(t);
			add_all_todo(t);
		}
		if (geditor_showing(ge)) {
			set_geditor_defaults(ge, 0, 0);
			add_all_gappt(ge);
		}
		lastcol=col;
		lastrow=row;
                break;
        default:
        break;
        };              /* switch */
        if (event->type == ButtonPress && event->xbutton.button == Button1)
                lastevent = *event;
}
 
static int
count_month_pages(Calendar *c, Tick start_date, int lines_per_box)
{
        int 			i, j;
        int 			rows, pages;
        struct 	tm 		tm;
        int 			day, ndays, num_appts, max = 0;
	CSA_uint32 		a_total;
	time_t 			start, stop;
        CSA_entry_handle 	*list;
	CSA_attribute 		*range_attrs;
	CSA_enum 		*ops;
	_Xltimeparams		localtime_buf;

        tm    = *_XLocaltime(&start_date, localtime_buf);
        tm.tm_mday = 1;
#if defined(SVR4) || defined(__linux__)
        tm.tm_isdst = -1;
        day   = (int)mktime(&tm);
#else
        day   = (int)timelocal(&tm);
#endif /* SVR4 */
        ndays = ((tm.tm_mon==1) && leapyr(tm.tm_year+1900))? 29 :
                        monthdays[tm.tm_mon];
 
        /* print days of the week at the top */
        rows = numwks (start_date);
 
        /* need minimum 5 rows to paint miniature months */
        if (rows == 4) rows++;
 
        /* print the times and text of appts */
        for (i = 1; i <= ndays; i++)
        {
                /* setup a time limit for appts searched */
                start = (time_t) lowerbound (day);
                stop = (time_t) next_ndays(day, 1) - 1;
		setup_range(&range_attrs, &ops, &j, start, stop,
			    CSA_TYPE_EVENT, 0, B_FALSE,
			    c->general->version);
		csa_list_entries(c->cal_handle, j, range_attrs, ops,
				 &a_total, &list, nullptr);
		free_range(&range_attrs, &ops, j);

                num_appts = count_month_appts(list, a_total, c);
                if (num_appts > max)
                        max = num_appts;
 
                day = nextday(day);
                csa_free(list);
        }
 
 
        pages = max / lines_per_box;
        if ((max % lines_per_box) > 0)
                pages++;

        return(pages);   
}

 
static Boolean
print_month ( Calendar *c,
    int num_page,
    void *xp,
    Tick first_date,
    Props *p,
    Boolean first)
{
        int 		rows, i, j, lines_per_box;
        time_t 		lo_hour, hi_hour;
        char 		buf[50];
        int 		ndays, num_appts;
        Boolean 	more, done = False, all_done = True;
        Tick 		day;
	OrderingType 	ot = get_int_prop(p, CP_DATEORDERING);
        CSA_entry_handle *list;
        CSA_attribute 	*range_attrs;
	CSA_enum 	*ops;
        CSA_uint32 	a_total;
        static Tick 	tick = 0;
	static int	total_pages;

        if (first)
                tick = first_date;
        if (num_page > 1)
                tick = prevmonth_exactday(tick);
 
        /* print days of the week at the top */
        rows = numwks (tick);
 
        /* need minimum 5 rows to paint miniature months */
        if (rows == 4) rows++;

	x_init_printer(xp, LANDSCAPE);
	x_init_month(xp, rows);
	lines_per_box = x_get_month_lines_per_page(xp);

	if (num_page == 1)
	  total_pages = (lines_per_box > 0) ?
	    count_month_pages(c, tick, lines_per_box) : 1;

	day = first_dom(tick);
	ndays = monthlength(tick);
 
	/* print month & year on top */
	format_date(tick, ot, buf, 0, 0, 0);

	x_print_header(xp, buf, num_page, total_pages);
	x_month_daynames(xp, rows);
 
	/* print the times and text of appts */
	for (i = 1; i <= ndays; i++)
	{
	  /* setup a time limit for appts searched */
	  lo_hour = (time_t)lowerbound (day);
	  hi_hour = (time_t) next_ndays(day, 1) - 1;
	  setup_range(&range_attrs, &ops, &j, lo_hour, hi_hour,
		      CSA_TYPE_EVENT, 0, B_FALSE, c->general->version);
	  csa_list_entries(c->cal_handle, j, range_attrs,
			   ops, &a_total, &list, nullptr);
	  free_range(&range_attrs, &ops, j);
	  num_appts = count_month_appts(list, a_total, c);
	  if ((lines_per_box > 0) &&
	      (num_appts > (lines_per_box * num_page)))
	    more = True;
	  else
	    more = False;

	  x_month_timeslots(xp, day, more);

	  /* print out times and appts */
	  if (lines_per_box > 0)
	    done = x_print_month_appts(xp, list, a_total, num_page,
				       hi_hour, lines_per_box);
	  else done = True;

	  if (!done)
	    all_done = False;

	  day = nextday(day);
	  csa_free(list);
	}

	/* paint miniature previous & next month */
	x_print_little_months(xp, tick);
	x_finish_printer(xp);

        tick = nextmonth(tick);
         
        return(all_done);
}

extern void
print_month_range(Calendar *c, Tick start_tick, Tick end_tick)
{
        Props 		*p = (Props *)c->properties;
        Tick 	end, first_date = start_tick;
        int 		n;
        Boolean 	done = False, first = True;
        int 		num_page = 1;
	void		*xp = (void *)nullptr;

	/* counting up the month pages is a little trickier than in the day
	   and week cases.  Months will have variable numbers of seconds in 
	   them, simple dividing the difference by the number of seconds
	   per month doesn't work.  We'll brute force this by grinding thru,
	   adding seconds until we pass the end tick time. */

	end_tick = last_dom(end_tick);
	end = last_dom(start_tick);
	for (n = 1; end < end_tick; n++)
	  end = last_dom(nextmonth(end));

	if ((xp = x_open_file(c)) == (void *)nullptr)
	  return;

        for (; n > 0; n--)
	{
	  while (!done)
	  {
	    done = print_month(c, num_page, xp,
			       first_date, p, first);
	    num_page++;
	    first = False;
	  }

	  done = False;
	  num_page = 1;
        }

	x_print_file(xp, c);
}

static int 
count_month_appts(CSA_entry_handle *list, int a_total, Calendar *c)
{
        int 			count = 0, i, meoval;
        Props 			*pr = (Props*)c->properties;
	Dtcm_appointment 	*appt;
	CSA_return_code 	stat;
 
        meoval = get_int_prop(pr, CP_PRINTPRIVACY);
 
	appt = allocate_appt_struct(appt_read, 
				    	c->general->version,
					CSA_ENTRY_ATTR_CLASSIFICATION_I, 
					nullptr);
        for (i = 0; i < a_total; i++) {

		stat = query_appt_struct(c->cal_handle, list[i], appt);
		if (stat != CSA_SUCCESS) {
			free_appt_struct(&appt);
			return 0;
                }

		if ((privacy_set(appt) == CSA_CLASS_PUBLIC) && !(meoval & PRINT_PUBLIC))
			continue;
		else if ((privacy_set(appt) == CSA_CLASS_CONFIDENTIAL) && !(meoval & PRINT_SEMIPRIVATE))
			continue;
		else if ((privacy_set(appt) == CSA_CLASS_PRIVATE) && !(meoval & PRINT_PRIVATE))
			continue;


                count++;
        }
	free_appt_struct(&appt);
        return(count);
}        

/*
 * Handler for "hot" buttons to navigate to day view
 * Client data is index of activated button [0..30]
 */
static void
quick_button_cb(Widget widget, XtPointer client, XtPointer call)
{
        Calendar *c = calendar;
	int	 dom = (int) (intptr_t) client;

	c->view->olddate = c->view->date;
	c->view->date = next_ndays(first_dom(c->view->date), dom);

	calendar_select(c, daySelect, nullptr);
 
	cleanup_after_monthview(c);
	
	c->view->glance = dayGlance;
        init_mo(c);
        (void)init_dayview(c);

        paint_day(c);
}

/*
 * Handle positioning and managing of hotbuttons
 *
 * IMPORTANT: This function assumes that it is being called during the
 * 	Resize method of the canvas (XmDrawingArea), and for that reason
 *	it is effectively widget code and can legally call XtMoveWidget.
 *	Don't try this at home !    (and never from non-widget code)
 *
 * Also, this needs to be called *after* paint_month, as it relies on data
 * calculated there and stores in the month data structure.
 */
extern void
layout_children(Calendar *c)
{
	Month 	*m = (Month *)c->view->month_info;
	Widget 	*btns = m->hot_button;
	int 	i;

	/* Position required navigator buttons  - note that if they're
	   mapped when this happens, many exposes will occur */
	for (i=0; i < m->ndays; i++)
		XtMoveWidget(btns[i], m->button_loc[i].x, m->button_loc[i].y);

	/* Unmanage any unneeded navigator buttons (ie. 29, 30, 31) */
	for (i=m->ndays; i<31; i++)
		if (XtIsManaged(m->hot_button[i]))
			XtUnmanageChild(m->hot_button[i]);
}

static void
manage_children(Calendar *c)
{
	int 	i;
	Month 	*m = (Month *)c->view->month_info;
 
        /* manage the header widget */
        XtManageChild(m->month_label);
	XtManageChildren(m->hot_button, m->ndays);
}

static void
unmanage_children(Calendar *c)
{
	int 	i;
	Month 	*m = (Month *)c->view->month_info;
 
        /* unmanage the header widget */
        XtUnmanageChild(m->month_label);
	XtUnmanageChildren(m->hot_button, 31);
}

/*
 * Clean up anything left around that won't be removed on
 * switching to another view, such as the hot buttons for
 * navigation to day view.
 */
extern void
cleanup_after_monthview(Calendar *c)
{

	invalidate_cache(c);

	XtUnmapWidget(c->canvas);
	unmanage_children(c);

	XmToggleButtonGadgetSetState(c->month_scope, False, False);

	/*
	 * Deallocating everything causes all this view's memory to be
	 * freed (at least all we have allocated in this application)
	 * at the expense of rebuilding the view next time it is used.
	 * Commenting it out improves usability and avoids any risk
	 * of memory leakage from Xt and Motif.
	 */
/*
	deallocator(c);
*/

	XtMapWidget(c->canvas);
}

/*
 * check whether month view is allocated yet
 */
static Boolean
allocated(Calendar *c)
{
  return (c->view->month_info != nullptr);
}

/*
 * allocate storage & subwidgets used by month view
 */ static void 
allocator(Calendar *c)
{
	int 	n;
	char 	buf[BUFSIZ];
        Month 	*m;
	XmString str;

	/* main storage for other month data */
	calendar->view->month_info = (caddr_t) ckalloc(sizeof(Month));
        m = (Month *)c->view->month_info;

	m->button_loc = (XPoint *) ckalloc(31 * sizeof(XPoint));

	/* label for month & navigation button to year */
	m->month_label = XmCreateLabel(c->canvas, "monthLabel", nullptr, 0);
	
	/* navigation buttons to day view */
	m->hot_button = (Widget *) ckalloc(31 * sizeof(Widget));
	for (n=0; n<31; n++) {
		sprintf(buf, "%d", n+1);
		str = XmStringCreateLocalized(buf);
		sprintf(buf, "month2day%d", n);
		m->hot_button[n] =
			XmCreatePushButton(c->canvas, buf, nullptr, 0);
		XtVaSetValues(m->hot_button[n], XmNlabelString, str, nullptr);
		XtAddCallback(m->hot_button[n],XmNactivateCallback, 
			quick_button_cb, (XtPointer) (intptr_t) n);
		XmStringFree(str);
	}
}

/*
 * (Not in service)
 * allocate storage & subwidgets used by month view
 */
static void 
deallocator(Calendar *c)
{
        Month 	*m = (Month *)c->view->month_info;
	int 	n;

	/* free cache of points for buttons positions */
	free(m->button_loc);

	/* destroy widgets used for header */
	XtDestroyWidget(m->month_label);

	/* hot buttons */
	for (n=0; n<31; n++)
		XtDestroyWidget(m->hot_button[n]);

	/* array that held navigation buttons */
	free(m->hot_button);

	/* structure holding month information */
	free(m);
	c->view->month_info = nullptr;
}
