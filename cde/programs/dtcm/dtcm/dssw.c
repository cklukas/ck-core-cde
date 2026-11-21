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
**  dssw.c
**
**  $XConsortium: dssw.c /main/7 1996/10/14 16:06:20 barstow $
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
static  char sccsid[] = "@(#)dssw.c 1.58 95/08/07 Copyr 1993 Sun Microsystems, Inc.";
#endif

#include <EUSCompat.h>
#include <stdint.h>
#include <stdlib.h>
#include <Xm/Xm.h>
#include <Xm/Form.h>
#include <Xm/RowColumn.h>
#include <Xm/Text.h>
#include <Xm/TextF.h>
#include <Xm/LabelG.h>
#include <Xm/PushB.h>
#include <Xm/PushBG.h>
#include <Xm/ToggleBG.h>
#include <Xm/ToggleB.h>
#include "dssw.h"
#include "ics_io.h"
#include "ux_helpers.h"
#include "misc.h"
#include "getdate.h"
#include "props_pu.h"
#include "util.h"
#ifdef SVR4
#include <sys/param.h>
#endif /* SVR4 */

extern Dimension ComputeMaxWidth(Widget, Widget, Widget, Widget);
// misc.c
extern void _i18n_WidestWidget(int num, Widget *ret, Dimension *dim, ...);
extern void _i18n_HighestWidgetAdjust(int num, Widget *ret,
                                      Dimension *dim, ...);


/*******************************************************************************
**
**  Static functions visible to dssw.c only
**
*******************************************************************************/
static void
dssw_get_non_times(DSSW *dssw, time_t t) {
	char		*str;
	char		*loc_str = nullptr;
	Props		*p = (Props *)dssw->cal->properties;
	OrderingType	ot = get_int_prop(p, CP_DATEORDERING);
	SeparatorType	st = get_int_prop(p, CP_DATESEPARATOR);
	const char	*cat_name = "";

	cm_strcpy(dssw->date_val,
		get_date_from_widget(t, dssw->date_text, ot, st));

	str = XmTextGetString(dssw->what_text);
	if (dssw->location_text)
		loc_str = XmTextFieldGetString(dssw->location_text);

	if (dssw->category_idx > 0 && dssw->category_idx < DTCM_CAT_COUNT)
		cat_name = dtcm_category_name((DtcmCategory)dssw->category_idx);

	dssw->location_val[0] = '\0';
	if (loc_str) {
		snprintf(dssw->location_val, sizeof(dssw->location_val), "%s", loc_str);
		XtFree(loc_str);
	}

	if (dssw->location_val[0] || cat_name[0]) {
		DtcmIcsEvent ev;
		memset(&ev, 0, sizeof(ev));
		const char *nl = strchr(str ? str : "", '\n');
		if (nl) {
			size_t slen = (size_t)(nl - str);
			if (slen >= sizeof(ev.summary)) slen = sizeof(ev.summary) - 1;
			memcpy(ev.summary, str, slen);
			ev.summary[slen] = '\0';
			snprintf(ev.description, sizeof(ev.description), "%s", nl + 1);
		} else {
			snprintf(ev.summary, sizeof(ev.summary), "%s", str ? str : "");
		}
		snprintf(ev.location, sizeof(ev.location), "%s", dssw->location_val);
		snprintf(ev.category, sizeof(ev.category), "%s", cat_name);
		dtcm_ics_compose_what(&ev, dssw->what_val, sizeof(dssw->what_val));
	} else {
		snprintf(dssw->what_val, sizeof(dssw->what_val), "%s", str ? str : "");
	}
	XtFree(str);
}

static void
dssw_set_non_times(DSSW *dssw, time_t t) {
	Props		*p = (Props *)dssw->cal->properties;
	OrderingType	ot = get_int_prop(p, CP_DATEORDERING);
	SeparatorType	st = get_int_prop(p, CP_DATESEPARATOR);
	char		sum[512], loc[512], cat[64], desc[2048];

	set_date_in_widget(t, dssw->date_text, ot, st);

	dtcm_ics_decompose_what_ex(dssw->what_val,
		sum, sizeof(sum), loc, sizeof(loc),
		cat, sizeof(cat), desc, sizeof(desc));

	if (loc[0] || cat[0]) {
		char combined[WHAT_LEN];
		if (desc[0])
			snprintf(combined, sizeof(combined), "%s\n%s", sum, desc);
		else
			snprintf(combined, sizeof(combined), "%s", sum);
		XmTextSetString(dssw->what_text, combined);
	} else {
		XmTextSetString(dssw->what_text, dssw->what_val);
	}

	snprintf(dssw->location_val, sizeof(dssw->location_val), "%s", loc);
	if (dssw->location_text)
		XmTextFieldSetString(dssw->location_text, loc);

	DtcmCategory c_enum = dtcm_category_from_name(cat);
	dssw->category_idx = (int)c_enum;
	if (dssw->category_menu && c_enum >= 0 && c_enum < DTCM_CAT_COUNT &&
	    dssw->category_btns[c_enum]) {
		XtVaSetValues(dssw->category_menu,
			XmNmenuHistory, dssw->category_btns[c_enum], nullptr);
	}
}

static void
dssw_category_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
	(void)call_data;
	DSSW *dssw = (DSSW *)client_data;
	long idx = 0;
	XtVaGetValues(w, XmNuserData, &idx, nullptr);
	if (idx >= 0 && idx < DTCM_CAT_COUNT)
		dssw->category_idx = (int)idx;
}

static void
dssw_preset_duration_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
	(void)call_data;
	DSSW *dssw = (DSSW *)client_data;
	Props *p = (Props *)dssw->cal->properties;
	DisplayType dt = get_int_prop(p, CP_DEFAULTDISP);
	long mins = 0;

	XtVaGetValues(w, XmNuserData, &mins, nullptr);
	if (mins <= 0) {
		/* All Day / No Time */
		dssw->start_val.val[0] = '\0';
		dssw->stop_val.val[0] = '\0';
		dssw->start_val.block = dssw->stop_val.block = TIME_AM;
		set_dssw_times(dssw);
		return;
	}

	get_dssw_times(dssw);
	int start_hrs = 9, start_mins = 0;
	if (!blank_buf(dssw->start_val.val)) {
		int h = 0, m = 0;
		if (strchr(dssw->start_val.val, ':')) {
			(void)sscanf(dssw->start_val.val, "%d:%d", &h, &m);
		} else {
			int v = atoi(dssw->start_val.val);
			h = v / 100;
			m = v % 100;
		}
		if (dt == HOUR12) {
			if (dssw->start_val.block == TIME_PM && h < 12) h += 12;
			if (dssw->start_val.block == TIME_AM && h == 12) h = 0;
		}
		if (h >= 0 && h <= 23 && m >= 0 && m <= 59) {
			start_hrs = h;
			start_mins = m;
		}
	} else {
		if (dt == HOUR12) {
			snprintf(dssw->start_val.val, sizeof(dssw->start_val.val), "9:00");
			dssw->start_val.block = TIME_AM;
		} else {
			snprintf(dssw->start_val.val, sizeof(dssw->start_val.val), "0900");
			dssw->start_val.block = TIME_AM;
		}
	}

	int total = start_hrs * 60 + start_mins + (int)mins;
	if (total >= 24 * 60) total = 23 * 60 + 59;
	int stop_hrs = total / 60;
	int stop_mins = total % 60;

	if (dt == HOUR12) {
		int disp_h = stop_hrs;
		if (disp_h >= 12) {
			dssw->stop_val.block = TIME_PM;
			if (disp_h > 12) disp_h -= 12;
		} else {
			dssw->stop_val.block = TIME_AM;
			if (disp_h == 0) disp_h = 12;
		}
		snprintf(dssw->stop_val.val, sizeof(dssw->stop_val.val), "%d:%02d", disp_h, stop_mins);
	} else {
		snprintf(dssw->stop_val.val, sizeof(dssw->stop_val.val), "%02d%02d", stop_hrs, stop_mins);
		dssw->stop_val.block = (stop_hrs >= 12) ? TIME_PM : TIME_AM;
	}
	set_dssw_times(dssw);
}

/*
**  Callback from start time and stop time to set text fields accordingly
*/
static void
dssw_set_start_hour(Widget w, XtPointer client_data, XtPointer cbs) {
	int		start_hrs, stop_hrs, dur, start_mins, stop_mins;
	long		user_data;
	DSSW		*dssw = (DSSW *)client_data;
	Props		*p = (Props *)dssw->cal->properties;
	DisplayType	dt = get_int_prop(p, CP_DEFAULTDISP);

	XtVaGetValues(w, XmNuserData, &user_data, nullptr);

	if (user_data == NO_TIME) {
		dssw->start_val.val[0] = '\0';
		dssw->stop_val.val[0]  = '\0';
		dssw->start_val.block = dssw->stop_val.block = TIME_AM;
        } else if (user_data == ALL_DAY) {
		if (dt == HOUR12) {
			sprintf(dssw->start_val.val, "12:00");
			sprintf(dssw->stop_val.val, "11:59");
		} else {
			sprintf(dssw->start_val.val, "0000");
			sprintf(dssw->stop_val.val, "2359");
		}
		dssw->start_val.block = TIME_AM;
		dssw->stop_val.block = TIME_PM;
	} else {
		start_hrs = user_data / hrsec;
		dur = get_int_prop(p, CP_APPTDURATION) * minsec;
		stop_hrs = (user_data + dur) / hrsec;
		start_mins = (user_data - (start_hrs * hrsec)) / minsec;
		stop_mins = ((user_data + dur) - (stop_hrs * hrsec)) / minsec;
		stop_hrs = stop_hrs % 24;

		if (dt == HOUR12) {
			dssw->start_val.block =
				(adjust_hour(&start_hrs)) ? TIME_AM : TIME_PM;
			dssw->stop_val.block =
				(adjust_hour(&stop_hrs)) ? TIME_AM : TIME_PM;
			sprintf(dssw->start_val.val, "%2d:%02d",
				start_hrs, start_mins);
			sprintf(dssw->stop_val.val, "%2d:%02d",
				stop_hrs, stop_mins);
		} else {
			dssw->start_val.block =
				(start_hrs > 12) ? TIME_PM : TIME_AM;
			dssw->stop_val.block =
				(stop_hrs > 12) ? TIME_PM : TIME_AM;
			sprintf(dssw->start_val.val, "%02d%02d",
				start_hrs, start_mins);
			sprintf(dssw->stop_val.val, "%02d%02d",
				stop_hrs, stop_mins);
		}
	}
	set_dssw_times(dssw);
}

static void
dssw_set_stop_hour(Widget w, XtPointer client_data, XtPointer cbs) {
	long		user_data;
	int             hrs, mins;
	DSSW		*dssw = (DSSW *)client_data;
	Props		*p = (Props *)dssw->cal->properties;
	DisplayType	dt = get_int_prop(p, CP_DEFAULTDISP);

	get_dssw_times(dssw);
	XtVaGetValues(w, XmNuserData, &user_data, nullptr);

	if (user_data == NO_TIME) {
		dssw->stop_val.val[0] = '\0';
		dssw->stop_val.block = TIME_AM;
	} else {
		hrs = user_data / hrsec;
		mins = (user_data - (hrs * hrsec)) / minsec;

		if (dt == HOUR12) {
			dssw->stop_val.block =
				(adjust_hour(&hrs)) ? TIME_AM : TIME_PM;
			sprintf(dssw->stop_val.val, "%2d:%02d", hrs, mins);
		} else {
			dssw->stop_val.block = (hrs > 12) ? TIME_PM : TIME_AM;
			sprintf(dssw->stop_val.val, "%02d%02d", hrs, mins);
		}
	}
	set_dssw_times(dssw);
}

/*******************************************************************************
**
**  External functions
**
*******************************************************************************/

extern void
set_dssw_menus(DSSW *dssw, Props *p) {

	set_time_submenu(dssw->dssw_form_mgr, dssw->start_menu, p, 
			 dssw_set_start_hour, (XtPointer)dssw, 
			 dssw->show_notime_selection, dssw->show_allday_selection,
			 &dssw->start_menu_widget_list, &dssw->start_menu_widget_count);

	set_time_submenu(dssw->dssw_form_mgr, dssw->stop_menu, p, 
			 dssw_set_stop_hour, (XtPointer)dssw, dssw->show_notime_selection, False,
			 &dssw->stop_menu_widget_list, &dssw->stop_menu_widget_count);

	set_dssw_defaults(dssw, calendar->view->date, True);

}

#define GAP 	5

extern void
build_dssw(
	DSSW 		*dssw, 
	Calendar 	*c, 
	Widget 		 parent, 
	Boolean 	 show_notime, 
	Boolean 	 show_allday)
{
	Props		*p;
	DisplayType	 dt;
	Arg		 args[20];
	int		 n;
	Dimension	max_left_label_width;
	Widget		child[2];
	XmString	label_str;
	Dimension	highest, widest;
	Widget		prev, curr;
	Dimension	_toLabel, _toText, _toMenu, _toRC;

	dssw->cal = c;
	dssw->parent = parent;
	p = (Props *)c->properties;

	dt = get_int_prop(p, CP_DEFAULTDISP);

	dssw->show_notime_selection = show_notime;
	dssw->show_allday_selection = show_allday;

	/*
	**  One form manager for the widget
	*/
	dssw->dssw_form_mgr = XtVaCreateWidget("dssw_form_mgr",
		xmFormWidgetClass, 	parent,
		XmNautoUnmanage, 	False,
		nullptr);

	label_str = XmStringCreateLocalized(CATGETS(c->DT_catd, 1, 828, "Date:"));
        dssw->date_label = XtVaCreateWidget("date_label",
		xmLabelGadgetClass, 	dssw->dssw_form_mgr,
		XmNlabelString,		label_str,
                nullptr);
	XmStringFree(label_str);

	label_str = XmStringCreateLocalized(CATGETS(c->DT_catd, 1, 829, "Start:"));
        dssw->start_label = XtVaCreateWidget("start_label",
		xmLabelGadgetClass, 	dssw->dssw_form_mgr,
		XmNlabelString,		label_str,
                nullptr);
	XmStringFree(label_str);

	label_str = XmStringCreateLocalized(CATGETS(c->DT_catd, 1, 832, "End:"));
        dssw->stop_label = XtVaCreateWidget("stop_label",
		xmLabelGadgetClass, 	dssw->dssw_form_mgr,
		XmNlabelString,		label_str,
                nullptr);
	XmStringFree(label_str);

	label_str = XmStringCreateLocalized(CATGETS(c->DT_catd, 1, 835, "What:"));
        dssw->what_label = XtVaCreateWidget("what_label",
		xmLabelGadgetClass, 	dssw->dssw_form_mgr,
		XmNlabelString,		label_str,
                nullptr);
	XmStringFree(label_str);
 
        dssw->date_text = XtVaCreateWidget("date_text",
		xmTextWidgetClass, 	dssw->dssw_form_mgr,
                XmNeditMode, 		XmSINGLE_LINE_EDIT,
		XmNmaxLength, 		DATE_LEN - 1,
                nullptr);
 
	/*
	**  Radio button behavior for AM/PM selection for start time
	*/
	dssw->start_text = XtVaCreateWidget("start_text",
		xmTextWidgetClass, 	dssw->dssw_form_mgr,
		XmNeditMode, 		XmSINGLE_LINE_EDIT,
		XmNmaxLength, 		START_STOP_LEN - 1,
		nullptr);

        dssw->start_menu = create_start_stop_time_menu(dssw->dssw_form_mgr,
		nullptr, dssw_set_start_hour, (XtPointer)dssw, p, show_notime, 
		show_allday, &dssw->start_menu_widget_list, 
		&dssw->start_menu_widget_count);

	XtVaSetValues(dssw->start_menu,
		XmNnavigationType, 	XmTAB_GROUP,
		nullptr);

        dssw->start_ampm_rc_mgr = XtVaCreateWidget("start_ampm_rc_mgr",
		xmRowColumnWidgetClass, dssw->dssw_form_mgr,
		XmNpacking, 		XmPACK_COLUMN,
		XmNorientation, 	XmHORIZONTAL,
		XmNradioBehavior, 	True,
		XmNisHomogeneous, 	True,
		XmNentryClass, 		xmToggleButtonGadgetClass,
		nullptr);

	label_str = XmStringCreateLocalized(CATGETS(c->DT_catd, 1, 830, "AM"));
        child[0] = dssw->start_am = XtVaCreateWidget("start_am",
		xmToggleButtonGadgetClass, dssw->start_ampm_rc_mgr,
		XmNlabelString,		label_str,
		XmNsensitive, 		(dt == HOUR12) ? True : False,
		nullptr);      
	XmStringFree(label_str);

	label_str = XmStringCreateLocalized(CATGETS(c->DT_catd, 1, 831, "PM"));
        child[1] = dssw->start_pm = XtVaCreateWidget("start_pm",
		xmToggleButtonGadgetClass, dssw->start_ampm_rc_mgr,
		XmNlabelString,		label_str,
		XmNsensitive, 		(dt == HOUR12) ? True : False,
		nullptr);      
	XmStringFree(label_str);

	XtManageChildren(child, 2);

	/*
	**  Last and near the end, the stop stuff
	**  Radio button behavior for AM/PM selection for stop time
	*/
	dssw->stop_text = XtVaCreateWidget("stop_text",
		xmTextWidgetClass, 	dssw->dssw_form_mgr,
		XmNeditMode, 		XmSINGLE_LINE_EDIT,
		XmNmaxLength, 		START_STOP_LEN - 1,
		nullptr);

        dssw->stop_menu = create_start_stop_time_menu(dssw->dssw_form_mgr,
		nullptr, dssw_set_stop_hour, (XtPointer)dssw, p, show_notime, 
		False, &dssw->stop_menu_widget_list, 
		&dssw->stop_menu_widget_count);

	XtVaSetValues(dssw->stop_menu,
		XmNnavigationType, 	XmTAB_GROUP,
		nullptr);

        dssw->stop_ampm_rc_mgr = XtVaCreateWidget("stop_ampm_rc_mgr",
		xmRowColumnWidgetClass, dssw->dssw_form_mgr,
		XmNpacking, 		XmPACK_COLUMN,
		XmNorientation, 	XmHORIZONTAL,
		XmNradioBehavior, 	True,
		XmNisHomogeneous, 	True,
		XmNentryClass, 		xmToggleButtonGadgetClass,
		nullptr);

	label_str = XmStringCreateLocalized(CATGETS(c->DT_catd, 1, 833, "AM"));
        child[0] = dssw->stop_am = XtVaCreateWidget("stop_am",
		xmToggleButtonGadgetClass, dssw->stop_ampm_rc_mgr,
		XmNlabelString,		label_str,
		XmNsensitive, 		(dt == HOUR12) ? True : False,
		nullptr);      
	XmStringFree(label_str);

	label_str = XmStringCreateLocalized(CATGETS(c->DT_catd, 1, 834, "PM"));
        child[1] = dssw->stop_pm = XtVaCreateWidget("stop_pm",
		xmToggleButtonGadgetClass, dssw->stop_ampm_rc_mgr,
		XmNlabelString,		label_str,
		XmNsensitive, 		(dt == HOUR12) ? True : False,
		nullptr);      
	XmStringFree(label_str);

	XtManageChildren(child, 2);


	/*
	**  Finally the what text field
	*/
	n = 0;
        XtSetArg(args[n], XmNeditMode, 		XmMULTI_LINE_EDIT), n++;
/*	XtSetArg(args[n], XmNscrollVertical, 	True), n++;  */
	XtSetArg(args[n], XmNscrollHorizontal,	False), n++; 
	XtSetArg(args[n], XmNscrollingPolicy, 	XmAUTOMATIC), n++; 
	XtSetArg(args[n], XmNscrollBarDisplayPolicy, XmAS_NEEDED), n++;
	XtSetArg(args[n], XmNrows, 		3), n++;
	XtSetArg(args[n], XmNmaxLength, 	WHAT_LEN - 1), n++;

	dssw->what_text = XmCreateScrolledText(dssw->dssw_form_mgr, 
					       "what_text", args, n);

	dssw->what_scrollwindow = XtParent(dssw->what_text);
	XtManageChild(dssw->what_text);

	label_str = XmStringCreateLocalized("Location:");
	dssw->location_label = XtVaCreateManagedWidget("location_label",
		xmLabelGadgetClass, dssw->dssw_form_mgr,
		XmNlabelString, label_str,
		nullptr);
	XmStringFree(label_str);

	label_str = XmStringCreateLocalized("Category:");
	dssw->category_label = XtVaCreateManagedWidget("category_label",
		xmLabelGadgetClass, dssw->dssw_form_mgr,
		XmNlabelString, label_str,
		nullptr);
	XmStringFree(label_str);

	/*
	 * Do a layout
	 */
	_i18n_WidestWidget( 6, &prev, &max_left_label_width,
		dssw->date_label, dssw->start_label, dssw->stop_label,
		dssw->location_label, dssw->category_label, dssw->what_label );
	_toText = max_left_label_width + 2 * GAP;

	/*
	 * dssw->date_label, dssw->date_text
	 */
	_i18n_WidestWidget( 1, &curr, &widest, dssw->date_label );
	_toLabel = _toText - GAP - widest;
	_i18n_HighestWidgetAdjust( 2, &curr, &highest, dssw->date_label,
					dssw->date_text );
	XtVaSetValues( curr,
			XmNtopAttachment, XmATTACH_FORM,
			XmNtopOffset, GAP,
			nullptr );
	XtVaSetValues( dssw->date_label,
			XmNleftAttachment, XmATTACH_FORM,
			XmNleftOffset, _toLabel,
			nullptr );
	XtVaSetValues( dssw->date_text,
			XmNleftAttachment, XmATTACH_FORM,
			XmNleftOffset, _toText,
			XmNrightAttachment, XmATTACH_FORM,
			nullptr );
	prev = curr;

	/*
	 * dssw->start_label, dssw->start_text, dssw->start_menu,
	 * dssw->start_ampm_rc_mgr
	 */
	_i18n_WidestWidget( 1, &curr, &widest, dssw->start_label );
	_toLabel = _toText - GAP - widest;
	_i18n_WidestWidget( 1, &curr, &widest, dssw->start_text );
	_toMenu = _toText + widest + GAP;
	_i18n_WidestWidget( 1, &curr, &widest, dssw->start_menu );
	_toRC = _toMenu + widest + GAP;
	_i18n_HighestWidgetAdjust( 4, &curr, &highest, dssw->start_label,
		dssw->start_text, dssw->start_menu, dssw->start_ampm_rc_mgr );
	XtVaSetValues( curr,
			XmNtopAttachment, XmATTACH_WIDGET,
			XmNtopWidget, prev,
			XmNtopOffset, GAP,
			nullptr );
	XtVaSetValues( dssw->start_label,
			XmNleftAttachment, XmATTACH_FORM,
			XmNleftOffset, _toLabel,
			nullptr );
	XtVaSetValues( dssw->start_text,
			XmNleftAttachment, XmATTACH_FORM,
			XmNleftOffset, _toText,
			nullptr );
	XtVaSetValues( dssw->start_menu,
			XmNleftAttachment, XmATTACH_FORM,
			XmNleftOffset, _toMenu,
			nullptr );
	XtVaSetValues( dssw->start_ampm_rc_mgr,
			XmNleftAttachment, XmATTACH_FORM,
			XmNleftOffset, _toRC,
			nullptr );
	prev = curr;

	/*
	 * dssw->stop_label, dssw->stop_text, dssw->stop_menu,
	 * dssw->stop_ampm_rc_mgr
	 */
	_i18n_WidestWidget( 1, &curr, &widest, dssw->stop_label );
	_toLabel = _toText - GAP - widest;
	_i18n_WidestWidget( 1, &curr, &widest, dssw->stop_text );
	_toMenu = _toText + widest + GAP;
	_i18n_WidestWidget( 1, &curr, &widest, dssw->stop_menu );
	_toRC = _toMenu + widest + GAP;
	_i18n_HighestWidgetAdjust( 4, &curr, &highest, dssw->stop_label,
		dssw->stop_text, dssw->stop_menu, dssw->stop_ampm_rc_mgr );
	XtVaSetValues( curr,
			XmNtopAttachment, XmATTACH_WIDGET,
			XmNtopWidget, prev,
			XmNtopOffset, GAP,
			nullptr );
	XtVaSetValues( dssw->stop_label,
			XmNleftAttachment, XmATTACH_FORM,
			XmNleftOffset, _toLabel,
			nullptr );
	XtVaSetValues( dssw->stop_text,
			XmNleftAttachment, XmATTACH_FORM,
			XmNleftOffset, _toText,
			nullptr );
	XtVaSetValues( dssw->stop_menu,
			XmNleftAttachment, XmATTACH_FORM,
			XmNleftOffset, _toMenu,
			nullptr );
	XtVaSetValues( dssw->stop_ampm_rc_mgr,
			XmNleftAttachment, XmATTACH_FORM,
			XmNleftOffset, _toRC,
			nullptr );
	prev = curr;

	/*
	 * Quick duration preset buttons: 15m, 30m, 1h, All Day
	 */
	dssw->dur_rc = XtVaCreateWidget("dur_rc",
		xmRowColumnWidgetClass, dssw->dssw_form_mgr,
		XmNorientation, XmHORIZONTAL,
		XmNspacing, 4,
		XmNmarginWidth, 0,
		XmNmarginHeight, 0,
		XmNtopAttachment, XmATTACH_WIDGET,
		XmNtopWidget, prev,
		XmNtopOffset, GAP / 2,
		XmNleftAttachment, XmATTACH_FORM,
		XmNleftOffset, _toText,
		nullptr);
	{
		static const struct { const char *lbl; long mins; } presets[] = {
			{ "15m", 15 }, { "30m", 30 }, { "1h", 60 }, { "All Day", 0 }
		};
		for (int pi = 0; pi < 4; pi++) {
			XmString ps = XmStringCreateLocalized((char *)presets[pi].lbl);
			Widget pb = XtVaCreateManagedWidget("durPreset",
				xmPushButtonWidgetClass, dssw->dur_rc,
				XmNlabelString, ps,
				XmNuserData, (XtPointer)(intptr_t)presets[pi].mins,
				nullptr);
			XtAddCallback(pb, XmNactivateCallback, dssw_preset_duration_cb, (XtPointer)dssw);
			XmStringFree(ps);
		}
	}
	XtManageChild(dssw->dur_rc);
	prev = dssw->dur_rc;

	/*
	 * Dedicated Location field + Category dropdown
	 */
	{
		_i18n_WidestWidget(1, &curr, &widest, dssw->location_label);
		_toLabel = _toText - GAP - widest;
		XtVaSetValues(dssw->location_label,
			XmNtopAttachment, XmATTACH_WIDGET,
			XmNtopWidget, prev,
			XmNtopOffset, GAP,
			XmNleftAttachment, XmATTACH_FORM,
			XmNleftOffset, _toLabel,
			nullptr);

		dssw->location_text = XtVaCreateManagedWidget("location_text",
			xmTextFieldWidgetClass, dssw->dssw_form_mgr,
			XmNcolumns, 22,
			XmNtopAttachment, XmATTACH_WIDGET,
			XmNtopWidget, prev,
			XmNtopOffset, GAP / 2,
			XmNleftAttachment, XmATTACH_FORM,
			XmNleftOffset, _toText,
			XmNrightAttachment, XmATTACH_FORM,
			nullptr);
		prev = dssw->location_text;

		_i18n_WidestWidget(1, &curr, &widest, dssw->category_label);
		_toLabel = _toText - GAP - widest;
		XtVaSetValues(dssw->category_label,
			XmNtopAttachment, XmATTACH_WIDGET,
			XmNtopWidget, prev,
			XmNtopOffset, GAP,
			XmNleftAttachment, XmATTACH_FORM,
			XmNleftOffset, _toLabel,
			nullptr);

		Widget cat_pane = XmCreatePulldownMenu(dssw->dssw_form_mgr, "catPane", nullptr, 0);
		static const char *cat_labels[DTCM_CAT_COUNT] = {
			"None", "Work", "Personal", "Important", "Travel"
		};
		for (int ci = 0; ci < DTCM_CAT_COUNT; ci++) {
			XmString cs = XmStringCreateLocalized((char *)cat_labels[ci]);
			dssw->category_btns[ci] = XtVaCreateManagedWidget("catBtn",
				xmPushButtonGadgetClass, cat_pane,
				XmNlabelString, cs,
				XmNuserData, (XtPointer)(intptr_t)ci,
				nullptr);
			XtAddCallback(dssw->category_btns[ci], XmNactivateCallback,
				dssw_category_cb, (XtPointer)dssw);
			XmStringFree(cs);
		}
		dssw->category_idx = 0;
		Arg cat_args[6];
		int cat_ac = 0;
		XtSetArg(cat_args[cat_ac], XmNsubMenuId, cat_pane); cat_ac++;
		XtSetArg(cat_args[cat_ac], XmNtopAttachment, XmATTACH_WIDGET); cat_ac++;
		XtSetArg(cat_args[cat_ac], XmNtopWidget, prev); cat_ac++;
		XtSetArg(cat_args[cat_ac], XmNtopOffset, GAP / 2); cat_ac++;
		XtSetArg(cat_args[cat_ac], XmNleftAttachment, XmATTACH_FORM); cat_ac++;
		XtSetArg(cat_args[cat_ac], XmNleftOffset, _toText); cat_ac++;
		dssw->category_menu = XmCreateOptionMenu(dssw->dssw_form_mgr, "catMenu", cat_args, cat_ac);
		XtManageChild(dssw->category_menu);
		prev = dssw->category_menu;
	}

	/*
	 * dssw->what_label, dssw->what_scrollwindow
	 */
	_i18n_WidestWidget( 1, &curr, &widest, dssw->what_label );
	_toLabel = _toText - GAP - widest;
	XtVaSetValues( dssw->what_label,
			XmNtopAttachment, XmATTACH_WIDGET,
			XmNtopWidget, prev,
			XmNtopOffset, GAP,
			XmNleftAttachment, XmATTACH_FORM,
			XmNleftOffset, _toLabel,
			nullptr );
	XtVaSetValues( dssw->what_scrollwindow,
			XmNtopAttachment, XmATTACH_WIDGET,
			XmNtopWidget, prev,
			XmNtopOffset, GAP,
			XmNleftAttachment, XmATTACH_FORM,
			XmNleftOffset, _toText,
			XmNrightAttachment, XmATTACH_FORM,
			XmNbottomAttachment, XmATTACH_FORM,
			nullptr );
}

/*
**  This function will consume form values and stuff them into an appointment.
*/
extern Boolean
dssw_form_to_appt(DSSW *dssw, Dtcm_appointment *a, char *name, Tick t)
{
  return dssw_form_flags_to_appt(dssw, a, name, t, (int *)nullptr);
}

extern Boolean
dssw_form_flags_to_appt(DSSW *dssw, Dtcm_appointment *a, char *name, Tick t, int *flagsP)
{
	time_t		start_tick, stop_tick;
	char		ampm_buf[BUFSIZ], buf[BUFSIZ];
	Props		*p = (Props *)dssw->cal->properties;
	Props_pu	*pu = (Props_pu *)dssw->cal->properties_pu;
	DisplayType	dt = get_int_prop(p, CP_DEFAULTDISP);
	int		flags = 0;

	if (flagsP == (int *)nullptr)
	  flagsP = &flags;

	/*
	**  If neither start nor end times exist, then the start time is set
	**  to 3:41am (magic time) and the end time to 3:41 plus one minute.
	*/
	get_dssw_vals(dssw, t);
	if (blank_buf(dssw->date_val)) {
		editor_err_msg(dssw->parent, name, MISSING_DATE,
			       pu->xm_error_pixmap);
		return False;
	}

	if (!blank_buf(dssw->start_val.val)) {
		if (!valid_time(p, dssw->start_val.val)) {
			if (a->type->value->item.sint32_value == CSA_TYPE_TODO)
				editor_err_msg(dssw->parent, name, 
					       INVALID_TIME_DUE,
					       pu->xm_error_pixmap);
			else
				editor_err_msg(dssw->parent, name, 
					       INVALID_START,
					       pu->xm_error_pixmap);
			return False;
		}

		if (dt == HOUR12) {
			/* am and pm should not be translated.  They are only
			 * used in the date parsing code and never shown to
			 * the user.
			 */
			if (dssw->start_val.block == TIME_AM)
				sprintf(ampm_buf, "am");
			else
				sprintf(ampm_buf, "pm");
		} else
			ampm_buf[0] = '\0';

		snprintf(buf, sizeof(buf), "%s %s%s",
			dssw->date_val, dssw->start_val.val, ampm_buf); 

		if (!blank_buf(dssw->stop_val.val)
			&& !valid_time(p, dssw->stop_val.val)) {
			editor_err_msg(dssw->parent, name, INVALID_STOP,
				pu->xm_error_pixmap);
               		return False;
		}
	} else if (!blank_buf(dssw->stop_val.val)) {
		editor_err_msg(dssw->parent, name, MISSING_START,
			pu->xm_error_pixmap);
		return False;
	} else
		sprintf(buf, "%s 3:41am", dssw->date_val);

	start_tick = cm_getdate(buf, nullptr);
	if(start_tick < 0) {
		editor_err_msg(dssw->parent, name, INVALID_DATE,
			pu->xm_error_pixmap);
		return False;
	}

	if (blank_buf(dssw->what_val) && blank_buf(dssw->start_val.val)
		&& blank_buf(dssw->stop_val.val)) {
		editor_err_msg(dssw->parent, name, INVALID_NOTIME_APPT,
			pu->xm_error_pixmap);
		return False;
	}

	a->time->value->item.date_time_value = (char *) XtMalloc(BUFSIZ);
	_csa_tick_to_iso8601(start_tick, a->time->value->item.date_time_value);
	a->what->value->item.string_value = (char *)cm_strdup(dssw->what_val);
	a->show_time->value->item.sint32_value = True;

	if (!blank_buf(dssw->stop_val.val)) {
		if (dt == HOUR12) {
			/* am and pm should not be translated.  They are only
			 * used in the date parsing code and never shown to
			 * the user.
			 */
			if (dssw->stop_val.block == TIME_AM)
				sprintf(ampm_buf, "am");
			else
				sprintf(ampm_buf, "pm");
		} else
			ampm_buf[0] = '\0';

		snprintf(buf, sizeof(buf), "%s %s%s", dssw->date_val,
			dssw->stop_val.val, ampm_buf); 
		if ((stop_tick = cm_getdate(buf, nullptr)) <= 0) {
			editor_err_msg(dssw->parent, name, INVALID_DATE,
				       pu->xm_error_pixmap);
			return False;
		}
		if (stop_tick < start_tick) {
		    if (*flagsP == 0)
		    {
			char *title = XtNewString(CATGETS(calendar->DT_catd, 1, 248,
					"Calendar : Schedule Appointment"));
			char *ident1 = XtNewString(CATGETS(calendar->DT_catd, 1,
					923, "Cancel"));
			char *ident2 = XtNewString(CATGETS(calendar->DT_catd, 1,
					250, "Next Day"));
			sprintf(buf, "%s", CATGETS(calendar->DT_catd, 1, 247,
				"This appointment has an end time earlier than\nits begin time.  Do you want to\nschedule it into the next day?"));
			*flagsP = dialog_popup(dssw->parent,
				DIALOG_TITLE, title,
				DIALOG_TEXT, buf,
				BUTTON_IDENT, -1, ident1,
				BUTTON_IDENT, DSSW_NEXT_DAY, ident2,
				DIALOG_IMAGE, pu->xm_question_pixmap,
				nullptr);
			XtFree(ident2);
			XtFree(ident1);
			XtFree(title);
		    }
		    switch (*flagsP) {
		    case DSSW_NEXT_DAY:
			stop_tick += daysec;
			break;

		    default:
			*flagsP = 0;
		        return False;
		    }
		}

		a->end_time->value->item.date_time_value = (char *) XtMalloc(BUFSIZ);
		_csa_tick_to_iso8601(stop_tick, a->end_time->value->item.date_time_value);
	} else if (blank_buf(dssw->start_val.val)) {
		a->end_time->value->item.date_time_value = (char *) XtMalloc(BUFSIZ);
		_csa_tick_to_iso8601(start_tick + minsec, a->end_time->value->item.date_time_value);
		a->show_time->value->item.sint32_value = False;
	} else {
		free(a->end_time->value);
		a->end_time->value = nullptr;
	}

	return True;
}
/*
**  This function will consume form values and stuff them into an appointment.
*/
extern Boolean
dssw_form_to_todo(DSSW *dssw, Dtcm_appointment *a, char *name, Tick t)
{
	time_t		start_tick, stop_tick;
	char		ampm_buf[BUFSIZ], buf[BUFSIZ];
	Props		*p = (Props *)dssw->cal->properties;
	Props_pu	*pu = (Props_pu *)dssw->cal->properties_pu;
	DisplayType	dt = get_int_prop(p, CP_DEFAULTDISP);

	get_dssw_vals(dssw, t);
	/*
	 * Todo does not have end time.  So to distinguish between this
	 * and the editor, zero this out.
	 */
	dssw->stop_val.val[0] = '\0';
	if (blank_buf(dssw->date_val)) {
		editor_err_msg(dssw->parent, name, MISSING_DATE,
			       pu->xm_error_pixmap);
		return False;
	}

	if (!blank_buf(dssw->start_val.val)) {
		if (!valid_time(p, dssw->start_val.val)) {
			editor_err_msg(dssw->parent, name, INVALID_TIME,
				pu->xm_error_pixmap);
			return False;
		}

		if (dt == HOUR12) {
			if (dssw->start_val.block == TIME_AM)
				sprintf(ampm_buf, "am");
			else
				sprintf(ampm_buf, "pm");
		} else
			ampm_buf[0] = '\0';

		snprintf(buf, sizeof(buf), "%s %s%s",
			dssw->date_val, dssw->start_val.val, ampm_buf); 

		/* 
		 * No check here for stop time.
		 */
	} else {
		editor_err_msg(dssw->parent, name, MISSING_TIME,
			pu->xm_error_pixmap);
		return False;
	}

	start_tick = cm_getdate(buf, nullptr);
	if(start_tick < 0) {
		editor_err_msg(dssw->parent, name, INVALID_DATE,
			pu->xm_error_pixmap);
		return False;
	}

	a->time->value->item.date_time_value = (char *) XtMalloc(BUFSIZ);
	_csa_tick_to_iso8601(start_tick, a->time->value->item.date_time_value);
	a->what->value->item.string_value = (char *)cm_strdup(dssw->what_val);
	a->show_time->value->item.sint32_value = True;

	free(a->end_time->value);
	a->end_time->value = nullptr;

	return True;
}

/*
**  The next two functions will take appointment values and stuff them into
**  a form.
*/
extern Boolean
dssw_appt_to_form(DSSW *dssw, CSA_entry_handle entry) {
	Boolean			ret_val;
	CSA_return_code		stat;
	Dtcm_appointment	*appt;

	appt = allocate_appt_struct(appt_read,
				    dssw->cal->general->version,
				    CSA_ENTRY_ATTR_START_DATE_I,
				    CSA_ENTRY_ATTR_SUMMARY_I,
				    CSA_ENTRY_ATTR_END_DATE_I,
				    CSA_X_DT_ENTRY_ATTR_SHOWTIME_I,
				    nullptr);
	stat = query_appt_struct(dssw->cal->cal_handle, entry, appt);
	backend_err_msg(dssw->cal->frame, dssw->cal->view->current_calendar,
		stat, ((Props_pu *)dssw->cal->properties_pu)->xm_error_pixmap);
	if (stat != CSA_SUCCESS) {
		free_appt_struct(&appt);
		return False;
	}

	ret_val = dssw_attrs_to_form(dssw, appt);
	free_appt_struct(&appt);

	return ret_val;
}

extern Boolean
dssw_attrs_to_form(DSSW *dssw, Dtcm_appointment *appt) {
	int		start_hr, stop_hr;
	char		*what;
	time_t		tick, end_tick = 0;
	Boolean		showtime;
	Props		*p = (Props *)dssw->cal->properties;
	DisplayType	dt = get_int_prop(p, CP_DEFAULTDISP);

	_csa_iso8601_to_tick(appt->time->value->item.date_time_value, &tick);
	showtime = showtime_set(appt);
	if (appt->end_time)
		_csa_iso8601_to_tick(appt->end_time->value->item.\
			date_time_value, &end_tick);
	what = appt->what->value->item.string_value;

	/*
	**  Set the date, start, stop, and what fields
	*/
	if (!showtime || magic_time(tick)) {
		dssw->start_val.val[0] = '\0';
		dssw->stop_val.val[0] = '\0';
	} else {
		if ((start_hr = hour(tick)) < 12)
			dssw->start_val.block = TIME_AM;
		else
			dssw->start_val.block = TIME_PM;

		if (dt == HOUR12) {
			if (start_hr == 0)
				start_hr = 12;
			else if (start_hr > 12)
				start_hr -= 12;
			sprintf(dssw->start_val.val, "%2d:%02d", start_hr,
				minute(tick));
		} else
			sprintf(dssw->start_val.val, "%02d%02d", start_hr,
				minute(tick));

		if (end_tick) {
			if ((stop_hr = hour(end_tick)) < 12)
				dssw->stop_val.block = TIME_AM;
			else
				dssw->stop_val.block = TIME_PM;

			if (dt == HOUR12) {
				if (stop_hr == 0)
					stop_hr = 12;
				else if (stop_hr > 12)
					stop_hr -= 12;
				sprintf(dssw->stop_val.val, "%2d:%02d", stop_hr,
					minute(end_tick));
			} else
				sprintf(dssw->stop_val.val, "%02d%02d", stop_hr,
					minute(end_tick));
		} else {
			/* no end time */
			dssw->stop_val.val[0] = '\0';
			dssw->stop_val.block = TIME_AM;
		}
	}

	/* limit the size of the text pulled out of the appointment. */

	if (what) {
		strncpy(dssw->what_val, what, WHAT_LEN - 1);
		dssw->what_val[WHAT_LEN-1] = '\0';
	}

	set_dssw_vals(dssw, tick);

	return True;
}

extern void
get_dssw_times(DSSW *dssw) {
	char		*str;

	str = XmTextGetString(dssw->start_text);
	snprintf(dssw->start_val.val, sizeof(dssw->start_val.val), "%s", str);
	XtFree(str);
	dssw->start_val.block =
		XmToggleButtonGetState(dssw->start_am) ? TIME_AM : TIME_PM;

	str = XmTextGetString(dssw->stop_text);
	snprintf(dssw->stop_val.val, sizeof(dssw->stop_val.val), "%s", str);
	XtFree(str);
	dssw->stop_val.block =
		XmToggleButtonGetState(dssw->stop_am) ? TIME_AM : TIME_PM;
}

extern void
get_dssw_vals(DSSW *dssw, Tick t) {
	dssw_get_non_times(dssw, t);
	get_dssw_times(dssw);
}

extern void
set_dssw_times(DSSW *dssw) {
	XmString	xmstr;
	Props		*p = (Props *)dssw->cal->properties;
	DisplayType	dt = get_int_prop(p, CP_DEFAULTDISP);

	XtVaSetValues(dssw->start_text, XmNvalue, dssw->start_val.val,
		nullptr);
	XtVaSetValues(dssw->stop_text, XmNvalue, dssw->stop_val.val,
		nullptr);

	xmstr = XmStringCreateLocalized(dssw->start_val.val);
	XtVaSetValues(XmOptionButtonGadget(dssw->start_menu),
		XmNlabelString, xmstr,
		nullptr);
	XmStringFree(xmstr);
	xmstr = XmStringCreateLocalized(dssw->stop_val.val);
	XtVaSetValues(XmOptionButtonGadget(dssw->stop_menu),
		XmNlabelString, xmstr,
		nullptr);
	XmStringFree(xmstr);

	if (dt == HOUR12) {
		if (dssw->start_val.block == TIME_AM)
			XmToggleButtonSetState(dssw->start_am, True, True);
		else
			XmToggleButtonSetState(dssw->start_pm, True, True);
		if (dssw->stop_val.block == TIME_AM)
			XmToggleButtonSetState(dssw->stop_am, True, True);
		else
			XmToggleButtonSetState(dssw->stop_pm, True, True);
	}
}

extern void
load_dssw_times(DSSW *dssw, Tick start, Tick stop, Boolean set_no_time) {
	char		buf[10];
	Props		*p = (Props *)dssw->cal->properties;
	DisplayType	dt;

	/*
	 * If start and stop times were specified, do the right thing to the
	 * dssw widget
	 */
	dt = get_int_prop(p, CP_DEFAULTDISP);

	if (set_no_time == True){
		strcpy(dssw->start_val.val, " ");
		strcpy(dssw->stop_val.val, " ");
		return;
	}

	if (start > 0) {
		format_time(start, dt, buf);
		if (dt == HOUR12) {
			strncpy(dssw->start_val.val, buf, 5);
			dssw->start_val.val[5] = '\0';
			dssw->start_val.block = (buf[5] == 'a') ?
				TIME_AM : TIME_PM;
		} else {
			snprintf(dssw->start_val.val, sizeof(dssw->start_val.val), "%s", buf);
			dssw->start_val.block = TIME_AM;
		}
	}
	if (stop > 0) {
		format_time(stop, dt, buf);
		if (dt == HOUR12) {
			strncpy(dssw->stop_val.val, buf, 5);
			dssw->stop_val.val[5] = '\0';
			dssw->stop_val.block = (buf[5] == 'a') ?
				TIME_AM : TIME_PM;
		} else {
                        snprintf(dssw->stop_val.val, START_STOP_LEN, "%s", buf);
			dssw->stop_val.block = TIME_AM;
		}
	}
}

extern void
set_dssw_vals(DSSW *dssw, Tick t) {
	dssw_set_non_times(dssw, t);
	set_dssw_times(dssw);
}

extern void
set_dssw_defaults(DSSW *dssw, Tick t, Boolean set_times) {
	int			appt_beg, appt_end, beg_hr, end_hr;
	Props			*p = (Props *)dssw->cal->properties;
	DisplayType		dt;
	Time_scope_menu_op	dur_scope;

	if (set_times) {
		dt = get_int_prop(p, CP_DEFAULTDISP);
		appt_beg = get_int_prop(p, CP_APPTBEGIN);
		beg_hr = appt_beg / minsec;
		appt_end = get_int_prop(p, CP_APPTDURATION) + appt_beg;
		end_hr = appt_end / minsec;
		end_hr = end_hr % 24;

		if (dt == HOUR12) {
			dssw->start_val.block =	(adjust_hour(&beg_hr)) ?
				TIME_AM : TIME_PM;
			dssw->stop_val.block =	(adjust_hour(&end_hr)) ?
				TIME_AM : TIME_PM;
			sprintf(dssw->start_val.val, "%2d:%02d",
				beg_hr, appt_beg % minsec);
			sprintf(dssw->stop_val.val, "%2d:%02d",
				end_hr, appt_end % minsec);
		} else {
			sprintf(dssw->start_val.val, "%02d%02d",
				beg_hr, appt_beg % minsec);
			sprintf(dssw->stop_val.val, "%02d%02d",
				end_hr, appt_end % minsec);
			dssw->start_val.block = dssw->stop_val.block = TIME_AM;
		}
	}
	dssw->what_val[0] = '\0';

	set_dssw_vals(dssw, t);
}
