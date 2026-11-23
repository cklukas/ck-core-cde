/*
 * CDE - Common Desktop Environment
 *
 * Inline single-line natural-language Quick-Add bar for dtcm.
 */

#include "quick_add.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef DTCM_QUICK_ADD_NO_GUI
#include <Xm/Form.h>
#include <Xm/Label.h>
#include <Xm/PushB.h>
#include <Xm/TextF.h>
#include "ics_csa.h"
#include "agenda_ui.h"
#include "dayglance.h"
#include "weekglance.h"
#include "monthglance.h"
#include "misc.h"
#endif

static void trim_str(char *s)
{
    if (!s) return;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t l = strlen(s);
    while (l > 0 && isspace((unsigned char)s[l - 1])) s[--l] = '\0';
}

static int parse_hhmm(const char *tok, int *out_h, int *out_m)
{
    if (!tok || !isdigit((unsigned char)tok[0])) return 0;
    const char *colon = strchr(tok, ':');
    if (!colon) return 0;
    int h = atoi(tok);
    int m = atoi(colon + 1);
    if (h < 0 || h > 23 || m < 0 || m > 59) return 0;
    *out_h = h;
    *out_m = m;
    return 1;
}

int dtcm_quick_add_parse(const char *input, time_t now, DtcmQuickAddEvent *out)
{
    if (!input || !out) return -1;
    memset(out, 0, sizeof(*out));

    char buf[512];
    snprintf(buf, sizeof(buf), "%s", input);
    trim_str(buf);
    if (!buf[0]) return -1;

    /* Extract "@ Location" suffix if present */
    char *at = strstr(buf, " @ ");
    if (!at && buf[0] != '@') {
        char *at2 = strstr(buf, "@ ");
        if (at2 && at2 > buf && isspace((unsigned char)at2[-1])) at = at2;
    }
    if (at) {
        char *loc = at;
        while (*loc == ' ' || *loc == '@') loc++;
        snprintf(out->location, sizeof(out->location), "%s", loc);
        trim_str(out->location);
        *at = '\0';
        trim_str(buf);
    }

    struct tm base_tm;
    localtime_r(&now, &base_tm);
    int target_year = base_tm.tm_year + 1900;
    int target_mon  = base_tm.tm_mon + 1;
    int target_day  = base_tm.tm_mday;
    int day_offset  = 0;

    int start_h = -1, start_m = 0;
    int end_h   = -1, end_m   = 0;

    /* Tokenize remaining words, consuming date/time tokens and keeping summary words */
    char summary[256] = "";
    char *savep = NULL;
    for (char *tok = strtok_r(buf, " \t", &savep); tok; tok = strtok_r(NULL, " \t", &savep)) {
        if (strcasecmp(tok, "today") == 0) {
            day_offset = 0;
            continue;
        }
        if (strcasecmp(tok, "tomorrow") == 0) {
            day_offset = 1;
            continue;
        }
        /* YYYY-MM-DD */
        int yy = 0, mm = 0, dd = 0;
        if (strlen(tok) == 10 && tok[4] == '-' && tok[7] == '-' &&
            sscanf(tok, "%d-%d-%d", &yy, &mm, &dd) == 3 &&
            yy >= 1970 && mm >= 1 && mm <= 12 && dd >= 1 && dd <= 31) {
            target_year = yy;
            target_mon  = mm;
            target_day  = dd;
            day_offset  = 0;
            continue;
        }
        /* HH:MM-HH:MM */
        char *dash = strchr(tok, '-');
        if (dash && dash > tok) {
            char left[16], right[16];
            size_t llen = (size_t)(dash - tok);
            if (llen < sizeof(left)) {
                memcpy(left, tok, llen);
                left[llen] = '\0';
                snprintf(right, sizeof(right), "%s", dash + 1);
                int sh = 0, sm = 0, eh = 0, em = 0;
                if (parse_hhmm(left, &sh, &sm) && parse_hhmm(right, &eh, &em)) {
                    start_h = sh; start_m = sm;
                    end_h   = eh; end_m   = em;
                    continue;
                }
            }
        }
        /* Single HH:MM */
        int sh = 0, sm = 0;
        if (parse_hhmm(tok, &sh, &sm)) {
            start_h = sh; start_m = sm;
            end_h   = (sh + 1) % 24; end_m = sm;
            continue;
        }

        if (summary[0]) strncat(summary, " ", sizeof(summary) - strlen(summary) - 1);
        strncat(summary, tok, sizeof(summary) - strlen(summary) - 1);
    }

    trim_str(summary);
    if (!summary[0]) return -1;
    snprintf(out->summary, sizeof(out->summary), "%s", summary);

    struct tm st;
    memset(&st, 0, sizeof(st));
    st.tm_year  = target_year - 1900;
    st.tm_mon   = target_mon - 1;
    st.tm_mday  = target_day + day_offset;
    st.tm_isdst = -1;

    if (start_h < 0) {
        st.tm_hour = 9;
        st.tm_min  = 0;
        out->start_time = mktime(&st);
        out->end_time   = out->start_time + 3600;
        out->all_day    = 0;
    } else {
        st.tm_hour = start_h;
        st.tm_min  = start_m;
        out->start_time = mktime(&st);
        struct tm et = st;
        et.tm_hour = end_h;
        et.tm_min  = end_m;
        out->end_time = mktime(&et);
        if (out->end_time <= out->start_time) {
            out->end_time = out->start_time + 3600;
        }
        out->all_day = 0;
    }

    return 0;
}

#ifndef DTCM_QUICK_ADD_NO_GUI
static Widget g_qa_form = NULL;
static Widget g_qa_field = NULL;
static Boolean g_qa_visible = False;
static Calendar *g_qa_cal = NULL;

static void qa_submit_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)cd; (void)call_data;
    if (!g_qa_cal || !g_qa_field) return;
    char *txt = XmTextFieldGetString(g_qa_field);
    if (!txt || !txt[0]) {
        if (txt) XtFree(txt);
        return;
    }
    DtcmQuickAddEvent qev;
    time_t base_t = (g_qa_cal->view && g_qa_cal->view->date > 0)
                    ? (time_t)g_qa_cal->view->date : time(NULL);
    if (dtcm_quick_add_parse(txt, base_t, &qev) == 0) {
        DtcmIcsEvent ev;
        memset(&ev, 0, sizeof(ev));
        snprintf(ev.uid, sizeof(ev.uid), "dtcm-qa-%ld@local", (long)time(NULL));
        snprintf(ev.summary, sizeof(ev.summary), "%s", qev.summary);
        snprintf(ev.location, sizeof(ev.location), "%s", qev.location);
        ev.start_utc = qev.start_time;
        ev.end_utc   = qev.end_time;
        ev.all_day   = qev.all_day;

        if (dtcm_ics_import_single_event(g_qa_cal, &ev) == 0) {
            invalidate_cache(g_qa_cal);
            if (g_qa_cal->view->glance == dayGlance)
                paint_dayview(g_qa_cal, True, NULL, False);
            else if (g_qa_cal->view->glance == weekGlance)
                paint_weekview(g_qa_cal, NULL);
            else if (g_qa_cal->view->glance == monthGlance)
                prepare_to_paint_monthview(g_qa_cal, NULL);
            else if (g_qa_cal->view->glance == agendaGlance)
                paint_agendaview(g_qa_cal);
            if (g_qa_cal->message_text) {
                char msg[320];
                snprintf(msg, sizeof(msg), "Added: %s", qev.summary);
                set_message(g_qa_cal->message_text, msg);
            }
            XmTextFieldSetString(g_qa_field, (char *)"");
            dtcm_quick_add_toggle(g_qa_cal, False);
        }
    } else if (g_qa_cal->message_text) {
        set_message(g_qa_cal->message_text,
            "Quick Add format: Title [today|tomorrow|YYYY-MM-DD] [HH:MM-HH:MM] [@ Location]");
    }
    XtFree(txt);
}

static void qa_close_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)cd; (void)call_data;
    dtcm_quick_add_toggle(g_qa_cal, False);
}

static void qa_create(Calendar *c)
{
    if (!c || !c->form || !c->toolbar) return;
    g_qa_cal = c;

    g_qa_form = XtVaCreateWidget("QuickAddBar", xmFormWidgetClass, c->form,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, c->toolbar,
        XmNtopOffset, 2,
        XmNleftAttachment, XmATTACH_FORM,
        XmNleftOffset, 2,
        XmNrightAttachment, XmATTACH_FORM,
        XmNrightOffset, 2,
        XmNshadowThickness, 1,
        XmNshadowType, XmSHADOW_ETCHED_IN,
        NULL);

    XmString lbl_s = XmStringCreateLocalized((char *)"Quick Add:");
    Widget lbl = XtVaCreateManagedWidget("qaLabel", xmLabelWidgetClass, g_qa_form,
        XmNlabelString, lbl_s,
        XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 5,
        XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 5,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 6,
        NULL);
    XmStringFree(lbl_s);

    XmString cls_s = XmStringCreateLocalized((char *)"X");
    Widget cls_btn = XtVaCreateManagedWidget("qaClose", xmPushButtonWidgetClass, g_qa_form,
        XmNlabelString, cls_s,
        XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 3,
        XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 3,
        XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 5,
        NULL);
    XmStringFree(cls_s);
    XtAddCallback(cls_btn, XmNactivateCallback, qa_close_cb, NULL);

    XmString add_s = XmStringCreateLocalized((char *)"Add");
    Widget add_btn = XtVaCreateManagedWidget("qaAdd", xmPushButtonWidgetClass, g_qa_form,
        XmNlabelString, add_s,
        XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 3,
        XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 3,
        XmNrightAttachment, XmATTACH_WIDGET, XmNrightWidget, cls_btn, XmNrightOffset, 4,
        NULL);
    XmStringFree(add_s);
    XtAddCallback(add_btn, XmNactivateCallback, qa_submit_cb, NULL);

    g_qa_field = XtVaCreateManagedWidget("qaField", xmTextFieldWidgetClass, g_qa_form,
        XmNcolumns, 44,
        XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 3,
        XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 3,
        XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, lbl, XmNleftOffset, 4,
        XmNrightAttachment, XmATTACH_WIDGET, XmNrightWidget, add_btn, XmNrightOffset, 4,
        NULL);
    XtAddCallback(g_qa_field, XmNactivateCallback, qa_submit_cb, NULL);
}

void dtcm_quick_add_toggle(Calendar *c, Boolean show_and_focus)
{
    if (!c) return;
    if (!g_qa_form) qa_create(c);
    if (!g_qa_form) return;

    if (show_and_focus && !g_qa_visible) {
        XtManageChild(g_qa_form);
        XtVaSetValues(c->canvas,
            XmNtopAttachment, XmATTACH_WIDGET,
            XmNtopWidget, g_qa_form,
            NULL);
        g_qa_visible = True;
    } else if (!show_and_focus && g_qa_visible) {
        XtVaSetValues(c->canvas,
            XmNtopAttachment, XmATTACH_WIDGET,
            XmNtopWidget, c->toolbar,
            NULL);
        XtUnmanageChild(g_qa_form);
        g_qa_visible = False;
    }
    if (g_qa_visible && g_qa_field) {
        XmProcessTraversal(g_qa_field, XmTRAVERSE_CURRENT);
    }
}
#endif
