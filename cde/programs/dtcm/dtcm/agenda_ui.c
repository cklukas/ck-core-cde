/*
 * CDE - Common Desktop Environment
 *
 * Inline Quick-Filter bar (search + category chips) and 5th main view
 * (Agenda / Upcoming Schedule) for Calendar Manager (dtcm).
 */

#include <EUSCompat.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <Xm/Xm.h>
#include <Xm/Form.h>
#include <Xm/LabelG.h>
#include <Xm/List.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
#include <Xm/TextF.h>
#include <Xm/ToggleB.h>
#include <Xm/ToggleBG.h>

#include "agenda_ui.h"
#include "calendar.h"
#include "dayglance.h"
#include "editor.h"
#include "ics_io.h"
#include "misc.h"
#include "monthglance.h"
#include "ux_helpers.h"
#include "weekglance.h"
#include "yearglance.h"

static Calendar *g_cal = nullptr;
static Widget g_filter_form = nullptr;
static Widget g_filter_text = nullptr;
static Widget g_filter_cat_btns[DTCM_CAT_COUNT] = { nullptr };
static char g_query[256] = "";
static int g_cat_filter = -1; /* -1 = All, 1..4 = specific DtcmCategory */
static Boolean g_filter_visible = False;

/* Agenda view widgets (occupies canvas slot when glance == agendaGlance) */
static Widget g_agenda_form = nullptr;
static Widget g_agenda_list = nullptr;
static int g_agenda_days = 30;

typedef struct {
    Tick start_time;
    Tick end_time;
    int show_time;
    char summary[1024];
} AgendaRow;

static AgendaRow *g_rows = nullptr;
static int g_row_count = 0;
static int g_row_cap = 0;

static int
ci_contains(const char *hay, const char *needle)
{
    if (!needle || !needle[0])
        return 1;
    if (!hay || !hay[0])
        return 0;
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p; p++) {
        if (strncasecmp(p, needle, nlen) == 0)
            return 1;
    }
    return 0;
}

Boolean
dtcm_filter_bar_is_active(void)
{
    return (g_query[0] != '\0' || g_cat_filter > 0) ? True : False;
}

Boolean
dtcm_filter_matches(const char *what)
{
    if (!dtcm_filter_bar_is_active())
        return True;
    char sum[512], loc[256], cat[64];
    dtcm_ics_decompose_what_ex(what ? what : "",
                               sum, sizeof(sum),
                               loc, sizeof(loc),
                               cat, sizeof(cat),
                               nullptr, 0);
    if (g_cat_filter > 0) {
        DtcmCategory c = dtcm_category_from_name(cat);
        if ((int)c != g_cat_filter)
            return False;
    }
    if (g_query[0]) {
        if (!ci_contains(sum, g_query) &&
            !ci_contains(loc, g_query) &&
            !ci_contains(cat, g_query) &&
            !ci_contains(what, g_query))
            return False;
    }
    return True;
}

static void
refresh_current_view(Calendar *c)
{
    if (!c || !c->view)
        return;
    if (c->view->glance == agendaGlance) {
        paint_agendaview(c);
    } else if (c->view->glance == monthGlance) {
        prepare_to_paint_monthview(c, nullptr);
    } else if (c->view->glance == weekGlance) {
        paint_weekview(c, nullptr);
    } else if (c->view->glance == dayGlance) {
        paint_dayview(c, True, nullptr, False);
    }
}

static void
filter_text_changed_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)cd; (void)call_data;
    char *s = XmTextFieldGetString(w);
    snprintf(g_query, sizeof(g_query), "%s", s ? s : "");
    if (s) XtFree(s);
    refresh_current_view(g_cal);
}

static void
filter_cat_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)call_data;
    int idx = (int)(intptr_t)cd;
    g_cat_filter = (idx <= 0) ? -1 : idx;
    refresh_current_view(g_cal);
}

static void
filter_close_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)cd; (void)call_data;
    g_query[0] = '\0';
    g_cat_filter = -1;
    if (g_filter_text)
        XmTextFieldSetString(g_filter_text, "");
    dtcm_filter_bar_toggle(g_cal, False);
    refresh_current_view(g_cal);
}

void
dtcm_filter_bar_create(Calendar *c)
{
    if (!c || !c->form || g_filter_form)
        return;
    g_cal = c;

    g_filter_form = XtVaCreateWidget("filterBar",
        xmFormWidgetClass, c->form,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, c->toolbar,
        XmNbottomAttachment, XmATTACH_NONE,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNheight, 36,
        XmNresizable, False,
        XmNmarginWidth, 4,
        XmNmarginHeight, 2,
        nullptr);

    XmString lbl_s = XmStringCreateLocalized("Filter:");
    Widget lbl = XtVaCreateManagedWidget("filterLbl",
        xmLabelGadgetClass, g_filter_form,
        XmNlabelString, lbl_s,
        XmNleftAttachment, XmATTACH_FORM,
        XmNleftOffset, 4,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        nullptr);
    XmStringFree(lbl_s);

    g_filter_text = XtVaCreateManagedWidget("filterText",
        xmTextFieldWidgetClass, g_filter_form,
        XmNcolumns, 20,
        XmNleftAttachment, XmATTACH_WIDGET,
        XmNleftWidget, lbl,
        XmNleftOffset, 4,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        nullptr);
    XtAddCallback(g_filter_text, XmNvalueChangedCallback, filter_text_changed_cb, nullptr);

    Widget cat_rc = XtVaCreateManagedWidget("filterCatRC",
        xmRowColumnWidgetClass, g_filter_form,
        XmNorientation, XmHORIZONTAL,
        XmNradioBehavior, True,
        XmNspacing, 2,
        XmNmarginHeight, 0,
        XmNmarginWidth, 0,
        XmNleftAttachment, XmATTACH_WIDGET,
        XmNleftWidget, g_filter_text,
        XmNleftOffset, 6,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        nullptr);

    static const char *cat_names[DTCM_CAT_COUNT] = {
        "All", "Work", "Personal", "Important", "Travel"
    };
    for (int i = 0; i < DTCM_CAT_COUNT; i++) {
        XmString cs = XmStringCreateLocalized((char *)cat_names[i]);
        g_filter_cat_btns[i] = XtVaCreateManagedWidget("fcatBtn",
            xmToggleButtonWidgetClass, cat_rc,
            XmNlabelString, cs,
            XmNindicatorOn, False,
            XmNshadowThickness, 1,
            XmNset, (i == 0) ? True : False,
            nullptr);
        XtAddCallback(g_filter_cat_btns[i], XmNvalueChangedCallback,
                      filter_cat_cb, (XtPointer)(intptr_t)i);
        XmStringFree(cs);
    }

    XmString close_s = XmStringCreateLocalized("X");
    Widget close_btn = XtVaCreateManagedWidget("filterClose",
        xmPushButtonWidgetClass, g_filter_form,
        XmNlabelString, close_s,
        XmNrightAttachment, XmATTACH_FORM,
        XmNrightOffset, 4,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        nullptr);
    XtAddCallback(close_btn, XmNactivateCallback, filter_close_cb, nullptr);
    XmStringFree(close_s);
}

void
dtcm_filter_bar_toggle(Calendar *c, Boolean show_and_focus)
{
    if (!c)
        return;
    if (!g_filter_form)
        dtcm_filter_bar_create(c);
    if (!g_filter_form)
        return;

    if (show_and_focus && !g_filter_visible) {
        XtManageChild(g_filter_form);
        XtVaSetValues(c->canvas,
            XmNtopAttachment, XmATTACH_WIDGET,
            XmNtopWidget, g_filter_form,
            nullptr);
        if (g_agenda_form) {
            XtVaSetValues(g_agenda_form,
                XmNtopAttachment, XmATTACH_WIDGET,
                XmNtopWidget, g_filter_form,
                nullptr);
        }
        g_filter_visible = True;
        XmProcessTraversal(g_filter_text, XmTRAVERSE_CURRENT);
    } else if (!show_and_focus && g_filter_visible) {
        XtVaSetValues(c->canvas,
            XmNtopAttachment, XmATTACH_WIDGET,
            XmNtopWidget, c->toolbar,
            nullptr);
        if (g_agenda_form) {
            XtVaSetValues(g_agenda_form,
                XmNtopAttachment, XmATTACH_WIDGET,
                XmNtopWidget, c->toolbar,
                nullptr);
        }
        XtUnmanageChild(g_filter_form);
        g_filter_visible = False;
    } else if (show_and_focus && g_filter_visible) {
        XmProcessTraversal(g_filter_text, XmTRAVERSE_CURRENT);
    }
}

static void
agenda_range_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)call_data;
    g_agenda_days = (int)(intptr_t)cd;
    paint_agendaview(g_cal);
}

static void
agenda_activate_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)cd;
    XmListCallbackStruct *cbs = (XmListCallbackStruct *)call_data;
    if (!cbs || !g_cal)
        return;
    int idx = cbs->item_position - 1;
    if (idx < 0 || idx >= g_row_count)
        return;
    AgendaRow *r = &g_rows[idx];
    if (r->start_time <= 0)
        return;
    g_cal->view->olddate = g_cal->view->date;
    g_cal->view->date = r->start_time;
    show_editor(g_cal, r->start_time, r->end_time, r->show_time ? False : True);
    editor_select_appt(g_cal, r->start_time, r->summary);
}

static void
ensure_agenda_widgets(Calendar *c)
{
    if (g_agenda_form)
        return;
    g_cal = c;

    g_agenda_form = XtVaCreateWidget("agendaForm",
        xmFormWidgetClass, c->form,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, g_filter_visible ? g_filter_form : c->toolbar,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_WIDGET,
        XmNbottomWidget, c->message_text,
        XmNmarginWidth, 6,
        XmNmarginHeight, 6,
        nullptr);

    Widget hdr_rc = XtVaCreateManagedWidget("agendaHdr",
        xmRowColumnWidgetClass, g_agenda_form,
        XmNorientation, XmHORIZONTAL,
        XmNspacing, 6,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        nullptr);

    XmString title_s = XmStringCreateLocalized("Upcoming Schedule:");
    XtVaCreateManagedWidget("agendaTitle", xmLabelGadgetClass, hdr_rc,
                            XmNlabelString, title_s, nullptr);
    XmStringFree(title_s);

    static const struct { const char *lbl; int days; } ranges[] = {
        { "14 Days", 14 }, { "30 Days", 30 }, { "90 Days", 90 }
    };
    for (int i = 0; i < 3; i++) {
        XmString rs = XmStringCreateLocalized((char *)ranges[i].lbl);
        Widget rb = XtVaCreateManagedWidget("agendaRangeBtn",
            xmPushButtonWidgetClass, hdr_rc,
            XmNlabelString, rs,
            nullptr);
        XtAddCallback(rb, XmNactivateCallback, agenda_range_cb,
                      (XtPointer)(intptr_t)ranges[i].days);
        XmStringFree(rs);
    }

    Arg al[8];
    int ac = 0;
    XtSetArg(al[ac], XmNselectionPolicy, XmBROWSE_SELECT); ac++;
    XtSetArg(al[ac], XmNscrollBarDisplayPolicy, XmAS_NEEDED); ac++;
    g_agenda_list = XmCreateScrolledList(g_agenda_form, "agendaList", al, ac);
    XtVaSetValues(XtParent(g_agenda_list),
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, hdr_rc,
        XmNtopOffset, 6,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        nullptr);
    XtAddCallback(g_agenda_list, XmNdefaultActionCallback, agenda_activate_cb, nullptr);
    XtManageChild(g_agenda_list);
}

static int
cmp_agenda_row(const void *a, const void *b)
{
    const AgendaRow *ra = (const AgendaRow *)a;
    const AgendaRow *rb = (const AgendaRow *)b;
    if (ra->start_time < rb->start_time) return -1;
    if (ra->start_time > rb->start_time) return 1;
    return strcmp(ra->summary, rb->summary);
}

void
paint_agendaview(Calendar *c)
{
    if (!c || c->cal_handle == 0)
        return;
    ensure_agenda_widgets(c);

    Tick start = lowerbound(c->view->date);
    Tick stop = next_ndays(start, g_agenda_days) - 1;
    CSA_attribute *range_attrs = nullptr;
    CSA_enum *ops = nullptr;
    int rcount = 0;
    CSA_uint32 n_entries = 0;
    CSA_entry_handle *list = nullptr;

    setup_range(&range_attrs, &ops, &rcount, start, stop, CSA_TYPE_EVENT,
                0, B_FALSE, c->general->version);
    CSA_return_code stat = csa_list_entries(c->cal_handle, rcount, range_attrs, ops,
                                            &n_entries, &list, nullptr);
    free_range(&range_attrs, &ops, rcount);

    g_row_count = 0;
    if (stat == CSA_SUCCESS && list && n_entries > 0) {
        Dtcm_appointment *appt = allocate_appt_struct(appt_read,
            c->general->version,
            CSA_ENTRY_ATTR_START_DATE_I,
            CSA_ENTRY_ATTR_END_DATE_I,
            CSA_ENTRY_ATTR_SUMMARY_I,
            CSA_X_DT_ENTRY_ATTR_SHOWTIME_I,
            nullptr);
        for (CSA_uint32 i = 0; i < n_entries; i++) {
            if (query_appt_struct(c->cal_handle, list[i], appt) != CSA_SUCCESS)
                continue;
            Tick st = 0, et = 0;
            if (appt->time && appt->time->value && appt->time->value->item.date_time_value)
                _csa_iso8601_to_tick(appt->time->value->item.date_time_value, &st);
            if (appt->end_time && appt->end_time->value && appt->end_time->value->item.date_time_value)
                _csa_iso8601_to_tick(appt->end_time->value->item.date_time_value, &et);
            const char *what = (appt->what && appt->what->value && appt->what->value->item.string_value)
                               ? appt->what->value->item.string_value : "";
            if (!dtcm_filter_matches(what))
                continue;
            if (g_row_count == g_row_cap) {
                int ncap = g_row_cap ? g_row_cap * 2 : 64;
                AgendaRow *nr = (AgendaRow *)realloc(g_rows, (size_t)ncap * sizeof(AgendaRow));
                if (!nr) break;
                g_rows = nr;
                g_row_cap = ncap;
            }
            AgendaRow *r = &g_rows[g_row_count++];
            r->start_time = st;
            r->end_time = et > st ? et : st;
            r->show_time = (appt->show_time && appt->show_time->value)
                           ? appt->show_time->value->item.sint32_value : 1;
            snprintf(r->summary, sizeof(r->summary), "%s", what);
        }
        free_appt_struct(&appt);
        csa_free(list);
    }

    if (g_row_count > 1)
        qsort(g_rows, (size_t)g_row_count, sizeof(AgendaRow), cmp_agenda_row);

    XmListDeleteAllItems(g_agenda_list);
    if (g_row_count == 0) {
        XmString empty_s = XmStringCreateLocalized(
            dtcm_filter_bar_is_active()
            ? "No matching appointments in this range."
            : "No upcoming appointments in this range.");
        XmListAddItemUnselected(g_agenda_list, empty_s, 0);
        XmStringFree(empty_s);
    } else {
        for (int i = 0; i < g_row_count; i++) {
            AgendaRow *r = &g_rows[i];
            struct tm tm_s, tm_e;
            localtime_r(&r->start_time, &tm_s);
            localtime_r(&r->end_time, &tm_e);
            char date_buf[32], time_buf[32];
            strftime(date_buf, sizeof(date_buf), "%a %b %d", &tm_s);
            if (r->show_time) {
                snprintf(time_buf, sizeof(time_buf), "%02d:%02d-%02d:%02d",
                         tm_s.tm_hour, tm_s.tm_min, tm_e.tm_hour, tm_e.tm_min);
            } else {
                snprintf(time_buf, sizeof(time_buf), "All Day    ");
            }
            char sum[256], loc[128], cat[64];
            dtcm_ics_decompose_what_ex(r->summary, sum, sizeof(sum),
                                       loc, sizeof(loc), cat, sizeof(cat),
                                       nullptr, 0);
            char line[640];
            snprintf(line, sizeof(line), "%s  %s  %s%s%s%s%s%s",
                     date_buf, time_buf,
                     sum[0] ? sum : r->summary,
                     loc[0] ? "  @ " : "", loc,
                     cat[0] ? "  [" : "", cat, cat[0] ? "]" : "");
            XmString xs = XmStringCreateLocalized(line);
            XmListAddItemUnselected(g_agenda_list, xs, 0);
            XmStringFree(xs);
        }
    }

    char status[128];
    snprintf(status, sizeof(status), "Agenda: %d appointment(s) over next %d days",
             g_row_count, g_agenda_days);
    set_message(c->message_text, status);
}

void
cleanup_after_agenda(Calendar *c)
{
    if (g_agenda_form && XtIsManaged(g_agenda_form))
        XtUnmanageChild(g_agenda_form);
    if (c && c->agenda_scope)
        XmToggleButtonGadgetSetState(c->agenda_scope, False, False);
    if (c && c->canvas)
        XtMapWidget(c->canvas);
}

void
agenda_button(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w; (void)client_data; (void)call_data;
    Calendar *c = calendar;
    if (!c)
        return;
    if (c->view->glance == agendaGlance) {
        paint_agendaview(c);
        return;
    }

    XtUnmapWidget(c->canvas);
    switch (c->view->glance) {
    case dayGlance:   cleanup_after_dayview(c);   break;
    case weekGlance:  cleanup_after_weekview(c);  break;
    case monthGlance: cleanup_after_monthview(c); break;
    case yearGlance:  cleanup_after_yearview(c);  break;
    default: break;
    }
    c->view->glance = agendaGlance;
    if (c->agenda_scope)
        XmToggleButtonGadgetSetState(c->agenda_scope, True, False);

    ensure_agenda_widgets(c);
    XtManageChild(g_agenda_form);
    paint_agendaview(c);
}
