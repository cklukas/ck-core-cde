/*
 * CDE - Common Desktop Environment
 *
 * Right-click (Button3) context menu for Calendar Manager (dtcm)
 * day cells and appointments.
 */

#include <EUSCompat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Xm/Xm.h>
#include <Xm/PushBG.h>
#include <Xm/RowColumn.h>
#include <Xm/SeparatoG.h>

#include "calendar.h"
#include "context_menu.h"
#include "dayglance.h"
#include "editor.h"
#include "ics_csa.h"
#include "misc.h"
#include "monthglance.h"
#include "select.h"
#include "todo.h"
#include "weekglance.h"

static Widget g_ctx_menu = nullptr;
static Widget g_ctx_switch_view = nullptr;
static Widget g_ctx_edit_appt = nullptr;
static Widget g_ctx_del_appt = nullptr;
static Calendar *g_ctx_cal = nullptr;
static Tick g_ctx_day = 0;
static int g_ctx_appt_idx = -1;

static void
ctx_new_appt_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)cd; (void)call_data;
    if (!g_ctx_cal) return;
    if (g_ctx_day > 0) {
        g_ctx_cal->view->olddate = g_ctx_cal->view->date;
        g_ctx_cal->view->date = g_ctx_day;
    }
    show_editor(g_ctx_cal, 0, 0, False);
}

static void
ctx_new_todo_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)cd; (void)call_data;
    if (!g_ctx_cal) return;
    if (g_ctx_day > 0) {
        g_ctx_cal->view->olddate = g_ctx_cal->view->date;
        g_ctx_cal->view->date = g_ctx_day;
    }
    show_todo(g_ctx_cal);
}

static void
ctx_day_view_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)cd; (void)call_data;
    if (!g_ctx_cal) return;
    if (g_ctx_day > 0) {
        g_ctx_cal->view->olddate = g_ctx_cal->view->date;
        g_ctx_cal->view->date = g_ctx_day;
    }
    if (g_ctx_cal->view->glance == dayGlance)
        month_button(g_ctx_cal->canvas, nullptr, nullptr);
    else
        day_button(g_ctx_cal->canvas, nullptr, nullptr);
}

static void
ctx_today_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)cd; (void)call_data;
    if (!g_ctx_cal) return;
    today_button(g_ctx_cal->canvas, nullptr, nullptr);
}

static void
ctx_edit_appt_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)cd; (void)call_data;
    if (!g_ctx_cal || g_ctx_appt_idx < 0 || !g_ctx_cal->paint_cache ||
        g_ctx_appt_idx >= g_ctx_cal->paint_cache_size)
        return;

    Paint_cache *pc = &g_ctx_cal->paint_cache[g_ctx_appt_idx];
    if (pc->start_time > 0) {
        g_ctx_cal->view->olddate = g_ctx_cal->view->date;
        g_ctx_cal->view->date = pc->start_time;
    }
    show_editor(g_ctx_cal, pc->start_time, pc->end_time, pc->show_time ? False : True);
}

static void
ctx_delete_appt_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)cd; (void)call_data;
    if (!g_ctx_cal || g_ctx_appt_idx < 0 || !g_ctx_cal->paint_cache ||
        g_ctx_appt_idx >= g_ctx_cal->paint_cache_size || g_ctx_cal->cal_handle == 0)
        return;

    Paint_cache *pc = &g_ctx_cal->paint_cache[g_ctx_appt_idx];
    Tick start = lowerbound(pc->start_time);
    Tick stop = next_ndays(start, 1) - 1;
    CSA_attribute *range_attrs = nullptr;
    CSA_enum *ops = nullptr;
    int rcount = 0;
    CSA_uint32 n_entries = 0;
    CSA_entry_handle *list = nullptr;

    setup_range(&range_attrs, &ops, &rcount, start, stop, CSA_TYPE_EVENT,
                0, B_FALSE, g_ctx_cal->general->version);
    CSA_return_code stat = csa_list_entries(g_ctx_cal->cal_handle, rcount,
                                            range_attrs, ops, &n_entries, &list, nullptr);
    free_range(&range_attrs, &ops, rcount);
    if (stat != CSA_SUCCESS || !list || n_entries == 0) {
        if (list) csa_free(list);
        set_message(g_ctx_cal->message_text, "Could not locate appointment to delete.");
        return;
    }

    CSA_entry_handle target = 0;
    Dtcm_appointment *appt = allocate_appt_struct(appt_read,
                                                  g_ctx_cal->general->version,
                                                  CSA_ENTRY_ATTR_START_DATE_I,
                                                  CSA_ENTRY_ATTR_SUMMARY_I,
                                                  nullptr);
    for (CSA_uint32 i = 0; i < n_entries; i++) {
        if (query_appt_struct(g_ctx_cal->cal_handle, list[i], appt) == CSA_SUCCESS) {
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
        set_message(g_ctx_cal->message_text, "Could not locate appointment to delete.");
        return;
    }

    stat = csa_delete_entry(g_ctx_cal->cal_handle, target, CSA_SCOPE_ONE, nullptr);
    csa_free(list);

    if (stat == CSA_SUCCESS) {
        invalidate_cache(g_ctx_cal);
        if (g_ctx_cal->view->glance == monthGlance)
            prepare_to_paint_monthview(g_ctx_cal, nullptr);
        else if (g_ctx_cal->view->glance == weekGlance)
            paint_weekview(g_ctx_cal, nullptr);
        else if (g_ctx_cal->view->glance == dayGlance)
            paint_dayview(g_ctx_cal, True, nullptr, False);
        set_message(g_ctx_cal->message_text, "Appointment deleted.");
    } else {
        set_message(g_ctx_cal->message_text, "Could not delete appointment.");
    }
}

void
dtcm_context_menu_popup(Calendar *c, XButtonEvent *ev, Tick day, int appt_cache_idx)
{
    if (!c || !c->canvas || !ev)
        return;

    g_ctx_cal = c;
    g_ctx_day = day;
    g_ctx_appt_idx = appt_cache_idx;

    if (!g_ctx_menu) {
        Arg al[4];
        int ac = 0;
        g_ctx_menu = XmCreatePopupMenu(c->canvas, "dtcmContextMenu", al, ac);

        XmString s_new = XmStringCreateLocalized("New Appointment...");
        XmString s_todo = XmStringCreateLocalized("New To Do...");
        XmString s_day = XmStringCreateLocalized("Switch to Day View");
        XmString s_today = XmStringCreateLocalized("Jump to Today");
        XmString s_edit = XmStringCreateLocalized("Edit Appointment...");
        XmString s_del = XmStringCreateLocalized("Delete Appointment");

        Widget b_new = XmCreatePushButtonGadget(g_ctx_menu, "ctxNewAppt",
                                                (Arg[]){{XmNlabelString, (XtArgVal)s_new}}, 1);
        Widget b_todo = XmCreatePushButtonGadget(g_ctx_menu, "ctxNewTodo",
                                                 (Arg[]){{XmNlabelString, (XtArgVal)s_todo}}, 1);
        Widget b_sep1 = XmCreateSeparatorGadget(g_ctx_menu, "ctxSep1", nullptr, 0);
        g_ctx_switch_view = XmCreatePushButtonGadget(g_ctx_menu, "ctxDayView",
                                                     (Arg[]){{XmNlabelString, (XtArgVal)s_day}}, 1);
        Widget b_today = XmCreatePushButtonGadget(g_ctx_menu, "ctxToday",
                                                  (Arg[]){{XmNlabelString, (XtArgVal)s_today}}, 1);
        Widget b_sep2 = XmCreateSeparatorGadget(g_ctx_menu, "ctxSep2", nullptr, 0);
        g_ctx_edit_appt = XmCreatePushButtonGadget(g_ctx_menu, "ctxEditAppt",
                                                   (Arg[]){{XmNlabelString, (XtArgVal)s_edit}}, 1);
        g_ctx_del_appt = XmCreatePushButtonGadget(g_ctx_menu, "ctxDelAppt",
                                                  (Arg[]){{XmNlabelString, (XtArgVal)s_del}}, 1);

        XtAddCallback(b_new, XmNactivateCallback, ctx_new_appt_cb, nullptr);
        XtAddCallback(b_todo, XmNactivateCallback, ctx_new_todo_cb, nullptr);
        XtAddCallback(g_ctx_switch_view, XmNactivateCallback, ctx_day_view_cb, nullptr);
        XtAddCallback(b_today, XmNactivateCallback, ctx_today_cb, nullptr);
        XtAddCallback(g_ctx_edit_appt, XmNactivateCallback, ctx_edit_appt_cb, nullptr);
        XtAddCallback(g_ctx_del_appt, XmNactivateCallback, ctx_delete_appt_cb, nullptr);

        XtManageChild(b_new);
        XtManageChild(b_todo);
        XtManageChild(b_sep1);
        XtManageChild(g_ctx_switch_view);
        XtManageChild(b_today);
        XtManageChild(b_sep2);
        XtManageChild(g_ctx_edit_appt);
        XtManageChild(g_ctx_del_appt);

        XmStringFree(s_new);
        XmStringFree(s_todo);
        XmStringFree(s_day);
        XmStringFree(s_today);
        XmStringFree(s_edit);
        XmStringFree(s_del);
    }

    if (g_ctx_switch_view) {
        XmString sw = XmStringCreateLocalized(
            c->view->glance == dayGlance ? "Switch to Month View" : "Switch to Day View");
        XtVaSetValues(g_ctx_switch_view, XmNlabelString, sw, nullptr);
        XmStringFree(sw);
    }

    Boolean has_appt = (appt_cache_idx >= 0) ? True : False;
    XtSetSensitive(g_ctx_edit_appt, has_appt);
    XtSetSensitive(g_ctx_del_appt, has_appt);

    XmMenuPosition(g_ctx_menu, ev);
    XtManageChild(g_ctx_menu);
}
