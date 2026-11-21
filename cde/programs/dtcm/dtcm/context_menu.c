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
#include <Xm/MessageB.h>
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
#include "undo_stack.h"
#include "ux_helpers.h"
#include "weekglance.h"

static Widget g_ctx_menu = nullptr;
static Widget g_ctx_switch_view = nullptr;
static Widget g_ctx_edit_appt = nullptr;
static Widget g_ctx_copy_appt = nullptr;
static Widget g_ctx_paste_appt = nullptr;
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
    editor_select_appt(g_ctx_cal, pc->start_time, pc->summary);
}

typedef struct {
    Calendar *c;
    CSA_entry_handle target;
    CSA_entry_handle *list;
    Tick start_time;
    Tick end_time;
    int show_time;
    char summary[512];
} RecurDeleteCtx;

static void
finish_delete_entry(Calendar *c, CSA_entry_handle target, CSA_entry_handle *list,
                    CSA_enum scope, Tick start_time, Tick end_time,
                    int show_time, const char *summary)
{
    CSA_return_code stat = csa_delete_entry(c->cal_handle, target, scope, nullptr);
    if (list) csa_free(list);

    if (stat == CSA_SUCCESS) {
        if (scope == CSA_SCOPE_ONE)
            dtcm_undo_push_delete(start_time, end_time, show_time, summary);
        invalidate_cache(c);
        if (c->view->glance == monthGlance)
            prepare_to_paint_monthview(c, nullptr);
        else if (c->view->glance == weekGlance)
            paint_weekview(c, nullptr);
        else if (c->view->glance == dayGlance)
            paint_dayview(c, True, nullptr, False);
        set_message(c->message_text,
            scope == CSA_SCOPE_ALL
                ? "All occurrences deleted."
                : "Appointment deleted (Ctrl+Z to undo).");
    } else {
        set_message(c->message_text, "Could not delete appointment.");
    }
}

static void
recur_del_response_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    RecurDeleteCtx *ctx = (RecurDeleteCtx *)cd;
    XmAnyCallbackStruct *cbs = (XmAnyCallbackStruct *)call_data;
    if (ctx && cbs) {
        if (cbs->reason == XmCR_OK) {
            finish_delete_entry(ctx->c, ctx->target, ctx->list, CSA_SCOPE_ONE,
                                ctx->start_time, ctx->end_time, ctx->show_time, ctx->summary);
        } else if (cbs->reason == XmCR_HELP) {
            finish_delete_entry(ctx->c, ctx->target, ctx->list, CSA_SCOPE_ALL,
                                ctx->start_time, ctx->end_time, ctx->show_time, ctx->summary);
        } else {
            if (ctx->list) csa_free(ctx->list);
        }
        free(ctx);
    }
    XtDestroyWidget(XtParent(w));
}

static void
delete_appt_by_ref(Calendar *c, Tick start_time, Tick end_time, int show_time, const char *summary)
{
    if (!c || c->cal_handle == 0)
        return;

    Tick start = lowerbound(start_time);
    Tick stop = next_ndays(start, 1) - 1;
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
        set_message(c->message_text, "Could not locate appointment to delete.");
        return;
    }

    CSA_entry_handle target = 0;
    Boolean is_recurring = False;
    Dtcm_appointment *appt = allocate_appt_struct(appt_read,
                                                  c->general->version,
                                                  CSA_ENTRY_ATTR_START_DATE_I,
                                                  CSA_ENTRY_ATTR_SUMMARY_I,
                                                  CSA_X_DT_ENTRY_ATTR_REPEAT_TYPE_I,
                                                  nullptr);
    for (CSA_uint32 i = 0; i < n_entries; i++) {
        if (query_appt_struct(c->cal_handle, list[i], appt) == CSA_SUCCESS) {
            Tick st = 0;
            if (appt->time && appt->time->value && appt->time->value->item.date_time_value)
                _csa_iso8601_to_tick(appt->time->value->item.date_time_value, &st);
            const char *sum = (appt->what && appt->what->value && appt->what->value->item.string_value)
                              ? appt->what->value->item.string_value : "";
            if (st == start_time &&
                strcmp(sum, summary ? summary : "") == 0) {
                target = list[i];
                if (appt->repeat_type && appt->repeat_type->value &&
                    appt->repeat_type->value->item.sint32_value != CSA_X_DT_REPEAT_ONETIME) {
                    is_recurring = True;
                }
                break;
            }
        }
    }
    free_appt_struct(&appt);

    if (target == 0) {
        csa_free(list);
        set_message(c->message_text, "Could not locate appointment to delete.");
        return;
    }

    if (is_recurring) {
        RecurDeleteCtx *ctx = (RecurDeleteCtx *)calloc(1, sizeof(RecurDeleteCtx));
        if (!ctx) {
            csa_free(list);
            return;
        }
        ctx->c = c;
        ctx->target = target;
        ctx->list = list;
        ctx->start_time = start_time;
        ctx->end_time = end_time;
        ctx->show_time = show_time;
        snprintf(ctx->summary, sizeof(ctx->summary), "%s", summary ? summary : "");

        XmString msg = XmStringCreateLocalized(
            "This is a recurring appointment.\nDelete only this occurrence or all occurrences?");
        XmString ok_lbl = XmStringCreateLocalized("This Occurrence");
        XmString all_lbl = XmStringCreateLocalized("All Occurrences");
        XmString cancel_lbl = XmStringCreateLocalized("Cancel");
        XmString title = XmStringCreateLocalized("Delete Recurring Appointment");
        Widget dlg = XmCreateQuestionDialog(c->frame, "recurDeleteDlg", nullptr, 0);
        XtVaSetValues(dlg,
            XmNdialogTitle, title,
            XmNmessageString, msg,
            XmNokLabelString, ok_lbl,
            XmNhelpLabelString, all_lbl,
            XmNcancelLabelString, cancel_lbl,
            XmNdialogStyle, XmDIALOG_PRIMARY_APPLICATION_MODAL,
            nullptr);
        XtAddCallback(dlg, XmNokCallback, recur_del_response_cb, ctx);
        XtAddCallback(dlg, XmNhelpCallback, recur_del_response_cb, ctx);
        XtAddCallback(dlg, XmNcancelCallback, recur_del_response_cb, ctx);
        XmStringFree(msg);
        XmStringFree(ok_lbl);
        XmStringFree(all_lbl);
        XmStringFree(cancel_lbl);
        XmStringFree(title);
        XtManageChild(dlg);
        return;
    }

    dtcm_selected_appt_clear();
    finish_delete_entry(c, target, list, CSA_SCOPE_ONE,
                        start_time, end_time, show_time,
                        summary ? summary : "");
}

void
dtcm_copy_selected_appt(Calendar *c)
{
    if (!c) return;
    time_t st = 0, en = 0;
    int show_time = 1;
    char sum[1024];
    if (!dtcm_selected_appt_get(nullptr, &st, &en, &show_time, sum, sizeof(sum))) {
        set_message(c->message_text, "Select an appointment first to copy.");
        return;
    }
    dtcm_clipboard_copy(st, en, show_time, sum);
    set_message(c->message_text, "Appointment copied (select a time/day and press Ctrl+V to paste).");
}

void
dtcm_paste_appt(Calendar *c, Tick target_time)
{
    if (!c || c->cal_handle == 0) return;
    time_t st = 0, en = 0;
    int show_time = 1;
    char sum[1024];
    if (!dtcm_clipboard_get(&st, &en, &show_time, sum, sizeof(sum))) {
        set_message(c->message_text, "Clipboard is empty (copy an appointment first with Ctrl+C).");
        return;
    }
    Tick dur = (en > st) ? (Tick)(en - st) : 3600;
    Tick new_start = target_time > 0 ? target_time : c->view->date;
    Tick new_end = new_start + dur;

    Dtcm_appointment *appt = allocate_appt_struct(appt_write, c->general->version, nullptr);
    load_appt_defaults(appt, (Props *)c->properties);
    if (appt->time && appt->time->value) {
        free(appt->time->value->item.date_time_value);
        appt->time->value->item.date_time_value = ckalloc(64);
        _csa_tick_to_iso8601(new_start, appt->time->value->item.date_time_value);
    }
    if (appt->end_time && appt->end_time->value) {
        free(appt->end_time->value->item.date_time_value);
        appt->end_time->value->item.date_time_value = ckalloc(64);
        _csa_tick_to_iso8601(new_end, appt->end_time->value->item.date_time_value);
    }
    if (appt->show_time && appt->show_time->value)
        appt->show_time->value->item.sint32_value = show_time ? B_TRUE : B_FALSE;
    if (appt->what && appt->what->value) {
        free(appt->what->value->item.string_value);
        appt->what->value->item.string_value = cm_strdup(sum);
    }
    CSA_entry_handle new_entry = 0;
    CSA_return_code rc = csa_add_entry(c->cal_handle, appt->count, appt->attrs, &new_entry, nullptr);
    if (rc == CSA_SUCCESS) {
        csa_free((CSA_buffer)new_entry);
        dtcm_selected_appt_set(-1, new_start, new_end, show_time, sum);
        invalidate_cache(c);
        if (c->view->glance == monthGlance)
            prepare_to_paint_monthview(c, nullptr);
        else if (c->view->glance == weekGlance)
            paint_weekview(c, nullptr);
        else if (c->view->glance == dayGlance)
            paint_dayview(c, True, nullptr, False);
        set_message(c->message_text, "Appointment pasted.");
    } else {
        set_message(c->message_text, "Could not paste appointment.");
    }
    free_appt_struct(&appt);
}

void
dtcm_delete_selected_appt(Calendar *c)
{
    if (!c) return;
    time_t st = 0, en = 0;
    int show_time = 1;
    char sum[1024];
    if (!dtcm_selected_appt_get(nullptr, &st, &en, &show_time, sum, sizeof(sum))) {
        set_message(c->message_text, "Select an appointment first to delete.");
        return;
    }
    delete_appt_by_ref(c, (Tick)st, (Tick)en, show_time, sum);
}

static void
ctx_copy_appt_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)cd; (void)call_data;
    if (!g_ctx_cal || g_ctx_appt_idx < 0 || !g_ctx_cal->paint_cache ||
        g_ctx_appt_idx >= g_ctx_cal->paint_cache_size)
        return;
    Paint_cache *pc = &g_ctx_cal->paint_cache[g_ctx_appt_idx];
    dtcm_clipboard_copy(pc->start_time, pc->end_time, pc->show_time, pc->summary);
    set_message(g_ctx_cal->message_text, "Appointment copied (select a time/day and press Ctrl+V to paste).");
}

static void
ctx_paste_appt_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)cd; (void)call_data;
    dtcm_paste_appt(g_ctx_cal, g_ctx_day);
}

static void
ctx_delete_appt_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)cd; (void)call_data;
    if (!g_ctx_cal || g_ctx_appt_idx < 0 || !g_ctx_cal->paint_cache ||
        g_ctx_appt_idx >= g_ctx_cal->paint_cache_size || g_ctx_cal->cal_handle == 0)
        return;

    Paint_cache *pc = &g_ctx_cal->paint_cache[g_ctx_appt_idx];
    delete_appt_by_ref(g_ctx_cal, pc->start_time, pc->end_time, pc->show_time, pc->summary);
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
        XmString s_copy = XmStringCreateLocalized("Copy Appointment");
        XmString s_paste = XmStringCreateLocalized("Paste Appointment");
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
        g_ctx_copy_appt = XmCreatePushButtonGadget(g_ctx_menu, "ctxCopyAppt",
                                                   (Arg[]){{XmNlabelString, (XtArgVal)s_copy}}, 1);
        g_ctx_paste_appt = XmCreatePushButtonGadget(g_ctx_menu, "ctxPasteAppt",
                                                    (Arg[]){{XmNlabelString, (XtArgVal)s_paste}}, 1);
        g_ctx_del_appt = XmCreatePushButtonGadget(g_ctx_menu, "ctxDelAppt",
                                                  (Arg[]){{XmNlabelString, (XtArgVal)s_del}}, 1);

        XtAddCallback(b_new, XmNactivateCallback, ctx_new_appt_cb, nullptr);
        XtAddCallback(b_todo, XmNactivateCallback, ctx_new_todo_cb, nullptr);
        XtAddCallback(g_ctx_switch_view, XmNactivateCallback, ctx_day_view_cb, nullptr);
        XtAddCallback(b_today, XmNactivateCallback, ctx_today_cb, nullptr);
        XtAddCallback(g_ctx_edit_appt, XmNactivateCallback, ctx_edit_appt_cb, nullptr);
        XtAddCallback(g_ctx_copy_appt, XmNactivateCallback, ctx_copy_appt_cb, nullptr);
        XtAddCallback(g_ctx_paste_appt, XmNactivateCallback, ctx_paste_appt_cb, nullptr);
        XtAddCallback(g_ctx_del_appt, XmNactivateCallback, ctx_delete_appt_cb, nullptr);

        XtManageChild(b_new);
        XtManageChild(b_todo);
        XtManageChild(b_sep1);
        XtManageChild(g_ctx_switch_view);
        XtManageChild(b_today);
        XtManageChild(b_sep2);
        XtManageChild(g_ctx_edit_appt);
        XtManageChild(g_ctx_copy_appt);
        XtManageChild(g_ctx_paste_appt);
        XtManageChild(g_ctx_del_appt);

        XmStringFree(s_new);
        XmStringFree(s_todo);
        XmStringFree(s_day);
        XmStringFree(s_today);
        XmStringFree(s_edit);
        XmStringFree(s_copy);
        XmStringFree(s_paste);
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
    XtSetSensitive(g_ctx_copy_appt, has_appt);
    XtSetSensitive(g_ctx_paste_appt, dtcm_clipboard_has_item() ? True : False);
    XtSetSensitive(g_ctx_del_appt, has_appt);

    XmMenuPosition(g_ctx_menu, ev);
    XtManageChild(g_ctx_menu);
}
