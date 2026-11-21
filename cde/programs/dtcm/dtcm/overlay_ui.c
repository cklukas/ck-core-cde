/*
 * CDE - Common Desktop Environment
 *
 * Overlaid Calendars management dialog + toggleable left Calendars sidebar
 * strip with non-blocking pthread + XtAppAddInput refresh for dtcm.
 */

#include <EUSCompat.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <Xm/Xm.h>
#include <Xm/Form.h>
#include <Xm/Frame.h>
#include <Xm/LabelG.h>
#include <Xm/List.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
#include <Xm/Separator.h>
#include <Xm/TextF.h>
#include <Xm/ToggleB.h>
#include <Xm/DialogS.h>

#include "agenda_ui.h"
#include "calendar.h"
#include "dayglance.h"
#include "misc.h"
#include "monthglance.h"
#include "overlay_store.h"
#include "overlay_ui.h"
#include "ux_helpers.h"
#include "weekglance.h"

static Calendar *g_ovl_cal = nullptr;

/* Left sidebar widgets */
static Widget g_sidebar_form = nullptr;
static Widget g_sidebar_list_rc = nullptr;
static Boolean g_sidebar_visible = False;

/* Management dialog widgets */
static Widget g_dlg_shell = nullptr;
static Widget g_dlg_list = nullptr;
static Widget g_dlg_name = nullptr;
static Widget g_dlg_url = nullptr;
static Widget g_dlg_color_btns[4] = { nullptr };
static DtcmCategory g_dlg_sel_color = DTCM_CAT_WORK;
static Widget g_dlg_status = nullptr;

/* Async refresh state */
typedef struct {
    int idx;
    char url[1024];
    DtcmIcsEvent *events;
    int count;
    int ok;
    char err[256];
} OverlayFetchJob;

typedef struct {
    int pipe_fd[2];
    XtInputId input_id;
    int count;
    OverlayFetchJob jobs[DTCM_MAX_OVERLAYS];
} OverlayRefreshBatch;

static Boolean g_refresh_in_flight = False;

static void rebuild_sidebar_items(void);
static void rebuild_dialog_list(void);

static void
repaint_active_view(Calendar *c)
{
    if (!c || !c->view)
        return;
    if (c->view->glance == agendaGlance)
        paint_agendaview(c);
    else if (c->view->glance == monthGlance)
        prepare_to_paint_monthview(c, nullptr);
    else if (c->view->glance == weekGlance)
        paint_weekview(c, nullptr);
    else if (c->view->glance == dayGlance)
        paint_dayview(c, True, nullptr, False);
}

static void
persist_overlays(void)
{
    char path[1024];
    if (dtcm_overlay_default_path(path, sizeof(path)) == 0)
        (void)dtcm_overlay_save_file(path);
}

static void *
refresh_worker_thread(void *arg)
{
    OverlayRefreshBatch *batch = (OverlayRefreshBatch *)arg;
    for (int i = 0; i < batch->count; i++) {
        OverlayFetchJob *j = &batch->jobs[i];
        int rc = dtcm_overlay_fetch_url(j->url, &j->events, &j->count, j->err, sizeof(j->err));
        j->ok = (rc == 0) ? 1 : 0;
    }
    char done = '1';
    (void)write(batch->pipe_fd[1], &done, 1);
    return nullptr;
}

static void
refresh_pipe_ready_cb(XtPointer cd, int *fd, XtInputId *id)
{
    (void)fd;
    OverlayRefreshBatch *batch = (OverlayRefreshBatch *)cd;
    if (!batch)
        return;
    char buf[16];
    (void)read(batch->pipe_fd[0], buf, sizeof(buf));
    if (id)
        XtRemoveInput(*id);
    close(batch->pipe_fd[0]);
    close(batch->pipe_fd[1]);

    int total_events = 0;
    int errors = 0;
    for (int i = 0; i < batch->count; i++) {
        OverlayFetchJob *j = &batch->jobs[i];
        if (j->ok) {
            dtcm_overlay_replace_events(j->idx, j->events, j->count, "");
            total_events += j->count;
        } else {
            dtcm_ics_free_events(j->events);
            dtcm_overlay_replace_events(j->idx, nullptr, 0, j->err);
            errors++;
        }
    }
    free(batch);
    g_refresh_in_flight = False;

    rebuild_sidebar_items();
    rebuild_dialog_list();
    if (g_ovl_cal) {
        char msg[160];
        if (errors > 0)
            snprintf(msg, sizeof(msg), "Overlaid calendars refreshed (%d events, %d error%s).",
                     total_events, errors, errors == 1 ? "" : "s");
        else
            snprintf(msg, sizeof(msg), "Overlaid calendars refreshed (%d events).", total_events);
        set_message(g_ovl_cal->message_text, msg);
        if (g_dlg_status) {
            XmString s = XmStringCreateLocalized(msg);
            XtVaSetValues(g_dlg_status, XmNlabelString, s, nullptr);
            XmStringFree(s);
        }
        repaint_active_view(g_ovl_cal);
    }
}

void
dtcm_overlay_refresh_async(Calendar *c)
{
    if (!c || !c->xcontext || !c->xcontext->app)
        return;
    g_ovl_cal = c;
    int n = dtcm_overlay_count();
    if (n <= 0 || g_refresh_in_flight)
        return;

    OverlayRefreshBatch *batch = (OverlayRefreshBatch *)calloc(1, sizeof(OverlayRefreshBatch));
    if (!batch)
        return;
    if (pipe(batch->pipe_fd) != 0) {
        free(batch);
        return;
    }
    batch->count = n;
    for (int i = 0; i < n; i++) {
        const DtcmOverlaySource *src = dtcm_overlay_get(i);
        batch->jobs[i].idx = i;
        snprintf(batch->jobs[i].url, sizeof(batch->jobs[i].url), "%s", src ? src->url : "");
    }
    g_refresh_in_flight = True;
    batch->input_id = XtAppAddInput(c->xcontext->app, batch->pipe_fd[0],
                                    (XtPointer)XtInputReadMask,
                                    refresh_pipe_ready_cb, batch);

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, refresh_worker_thread, batch) != 0) {
        XtRemoveInput(batch->input_id);
        close(batch->pipe_fd[0]);
        close(batch->pipe_fd[1]);
        free(batch);
        g_refresh_in_flight = False;
    }
    pthread_attr_destroy(&attr);
}

static void
sidebar_toggle_item_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w;
    XmToggleButtonCallbackStruct *cbs = (XmToggleButtonCallbackStruct *)call_data;
    int idx = (int)(intptr_t)cd;
    if (!cbs)
        return;
    dtcm_overlay_set_enabled(idx, cbs->set ? 1 : 0);
    persist_overlays();
    rebuild_dialog_list();
    repaint_active_view(g_ovl_cal);
}

static void
sidebar_manage_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)cd; (void)call_data;
    dtcm_overlay_show_dialog(g_ovl_cal);
}

static void
sidebar_refresh_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)cd; (void)call_data;
    dtcm_overlay_refresh_async(g_ovl_cal);
}

static Pixel
alloc_category_pixel(Widget w, DtcmCategory cat)
{
    unsigned short r, g, b;
    dtcm_category_rgb(cat, &r, &g, &b);
    XColor xc;
    memset(&xc, 0, sizeof(xc));
    xc.red = r;
    xc.green = g;
    xc.blue = b;
    if (XAllocColor(XtDisplay(w), DefaultColormapOfScreen(XtScreen(w)), &xc))
        return xc.pixel;
    return BlackPixelOfScreen(XtScreen(w));
}

static void
rebuild_sidebar_items(void)
{
    if (!g_sidebar_list_rc)
        return;
    WidgetList children = nullptr;
    Cardinal nchildren = 0;
    XtVaGetValues(g_sidebar_list_rc, XmNchildren, &children, XmNnumChildren, &nchildren, nullptr);
    for (Cardinal i = 0; i < nchildren; i++)
        XtDestroyWidget(children[i]);

    XmString prim_s = XmStringCreateLocalized("Primary Calendar");
    XtVaCreateManagedWidget("ovlPrimary",
        xmToggleButtonWidgetClass, g_sidebar_list_rc,
        XmNlabelString, prim_s,
        XmNset, True,
        XmNsensitive, False,
        XmNalignment, XmALIGNMENT_BEGINNING,
        nullptr);
    XmStringFree(prim_s);

    int n = dtcm_overlay_count();
    for (int i = 0; i < n; i++) {
        const DtcmOverlaySource *src = dtcm_overlay_get(i);
        if (!src) continue;
        char lbl[192];
        snprintf(lbl, sizeof(lbl), "%s (%d)", src->name, src->event_count);
        XmString xs = XmStringCreateLocalized(lbl);
        Pixel px = alloc_category_pixel(g_sidebar_list_rc, src->color);
        Widget tb = XtVaCreateManagedWidget("ovlItem",
            xmToggleButtonWidgetClass, g_sidebar_list_rc,
            XmNlabelString, xs,
            XmNset, src->enabled ? True : False,
            XmNselectColor, px,
            XmNalignment, XmALIGNMENT_BEGINNING,
            nullptr);
        XtAddCallback(tb, XmNvalueChangedCallback, sidebar_toggle_item_cb, (XtPointer)(intptr_t)i);
        XmStringFree(xs);
    }
    if (n == 0) {
        XmString hint = XmStringCreateLocalized("(No overlaid calendars)");
        XtVaCreateManagedWidget("ovlEmpty",
            xmLabelGadgetClass, g_sidebar_list_rc,
            XmNlabelString, hint,
            XmNalignment, XmALIGNMENT_BEGINNING,
            nullptr);
        XmStringFree(hint);
    }
}

static void
ensure_sidebar_widget(Calendar *c)
{
    if (g_sidebar_form || !c || !c->form)
        return;
    g_ovl_cal = c;

    g_sidebar_form = XtVaCreateWidget("calendarsSidebar",
        xmFormWidgetClass, c->form,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, c->toolbar,
        XmNbottomAttachment, XmATTACH_WIDGET,
        XmNbottomWidget, c->message_text,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_NONE,
        XmNwidth, 190,
        XmNresizable, False,
        XmNmarginWidth, 6,
        XmNmarginHeight, 6,
        nullptr);

    XmString hdr_s = XmStringCreateLocalized("Calendars");
    Widget hdr = XtVaCreateManagedWidget("sidebarHdr",
        xmLabelGadgetClass, g_sidebar_form,
        XmNlabelString, hdr_s,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNalignment, XmALIGNMENT_BEGINNING,
        nullptr);
    XmStringFree(hdr_s);

    Widget btn_rc = XtVaCreateManagedWidget("sidebarBtns",
        xmRowColumnWidgetClass, g_sidebar_form,
        XmNorientation, XmHORIZONTAL,
        XmNspacing, 4,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        nullptr);

    XmString m_s = XmStringCreateLocalized("Manage...");
    Widget m_btn = XtVaCreateManagedWidget("sidebarManage",
        xmPushButtonWidgetClass, btn_rc,
        XmNlabelString, m_s,
        nullptr);
    XtAddCallback(m_btn, XmNactivateCallback, sidebar_manage_cb, nullptr);
    XmStringFree(m_s);

    XmString r_s = XmStringCreateLocalized("Refresh");
    Widget r_btn = XtVaCreateManagedWidget("sidebarRefresh",
        xmPushButtonWidgetClass, btn_rc,
        XmNlabelString, r_s,
        nullptr);
    XtAddCallback(r_btn, XmNactivateCallback, sidebar_refresh_cb, nullptr);
    XmStringFree(r_s);

    g_sidebar_list_rc = XtVaCreateManagedWidget("sidebarList",
        xmRowColumnWidgetClass, g_sidebar_form,
        XmNorientation, XmVERTICAL,
        XmNspacing, 4,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, hdr,
        XmNtopOffset, 6,
        XmNbottomAttachment, XmATTACH_WIDGET,
        XmNbottomWidget, btn_rc,
        XmNbottomOffset, 6,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        nullptr);

    rebuild_sidebar_items();
}

Widget
dtcm_overlay_sidebar_widget(void)
{
    return g_sidebar_visible ? g_sidebar_form : nullptr;
}

Boolean
dtcm_overlay_sidebar_is_visible(void)
{
    return g_sidebar_visible;
}

void
dtcm_overlay_sidebar_toggle(Calendar *c, Boolean show)
{
    if (!c)
        return;
    ensure_sidebar_widget(c);
    if (!g_sidebar_form)
        return;

    if (show && !g_sidebar_visible) {
        rebuild_sidebar_items();
        XtManageChild(g_sidebar_form);
        XtVaSetValues(c->canvas,
            XmNleftAttachment, XmATTACH_WIDGET,
            XmNleftWidget, g_sidebar_form,
            XmNleftOffset, 2,
            nullptr);
        dtcm_agenda_set_left_widget(g_sidebar_form);
        g_sidebar_visible = True;
        repaint_active_view(c);
    } else if (!show && g_sidebar_visible) {
        XtVaSetValues(c->canvas,
            XmNleftAttachment, XmATTACH_FORM,
            XmNleftOffset, 1,
            nullptr);
        dtcm_agenda_set_left_widget(nullptr);
        XtUnmanageChild(g_sidebar_form);
        g_sidebar_visible = False;
        repaint_active_view(c);
    }
}

/* --- Management Dialog --- */

static void
rebuild_dialog_list(void)
{
    if (!g_dlg_list)
        return;
    XmListDeleteAllItems(g_dlg_list);
    int n = dtcm_overlay_count();
    for (int i = 0; i < n; i++) {
        const DtcmOverlaySource *s = dtcm_overlay_get(i);
        if (!s) continue;
        char line[1280];
        snprintf(line, sizeof(line), "[%s] %s (%s, %d events) - %s%s%s",
                 s->enabled ? "On" : "Off",
                 s->name,
                 dtcm_category_name(s->color),
                 s->event_count,
                 s->url,
                 s->last_error[0] ? " [ERR: " : "",
                 s->last_error[0] ? s->last_error : "");
        XmString xs = XmStringCreateLocalized(line);
        XmListAddItemUnselected(g_dlg_list, xs, 0);
        XmStringFree(xs);
    }
}

static void
dlg_color_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)call_data;
    g_dlg_sel_color = (DtcmCategory)(intptr_t)cd;
}

static void
dlg_add_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)cd; (void)call_data;
    char *name = XmTextFieldGetString(g_dlg_name);
    char *url = XmTextFieldGetString(g_dlg_url);
    int idx = dtcm_overlay_add(name, url, g_dlg_sel_color, 1);
    if (name) XtFree(name);
    if (url) XtFree(url);

    if (idx < 0) {
        if (g_dlg_status) {
            XmString s = XmStringCreateLocalized("Enter a valid https://, webcal://, or local .ics path.");
            XtVaSetValues(g_dlg_status, XmNlabelString, s, nullptr);
            XmStringFree(s);
        }
        return;
    }
    XmTextFieldSetString(g_dlg_name, "");
    XmTextFieldSetString(g_dlg_url, "");
    persist_overlays();
    rebuild_dialog_list();
    rebuild_sidebar_items();
    dtcm_overlay_refresh_async(g_ovl_cal);
}

static int
dlg_selected_index(void)
{
    if (!g_dlg_list) return -1;
    int *pos = nullptr;
    int cnt = 0;
    if (XmListGetSelectedPos(g_dlg_list, &pos, &cnt) && cnt > 0 && pos) {
        int idx = pos[0] - 1;
        XtFree((char *)pos);
        return idx;
    }
    return -1;
}

static void
dlg_toggle_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)cd; (void)call_data;
    int idx = dlg_selected_index();
    const DtcmOverlaySource *s = dtcm_overlay_get(idx);
    if (!s) return;
    dtcm_overlay_set_enabled(idx, !s->enabled);
    persist_overlays();
    rebuild_dialog_list();
    rebuild_sidebar_items();
    repaint_active_view(g_ovl_cal);
}

static void
dlg_remove_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)cd; (void)call_data;
    int idx = dlg_selected_index();
    if (idx < 0) return;
    dtcm_overlay_remove(idx);
    persist_overlays();
    rebuild_dialog_list();
    rebuild_sidebar_items();
    repaint_active_view(g_ovl_cal);
}

static void
dlg_refresh_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)cd; (void)call_data;
    dtcm_overlay_refresh_async(g_ovl_cal);
}

static void
dlg_close_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)cd; (void)call_data;
    if (g_dlg_shell)
        XtPopdown(g_dlg_shell);
}

void
dtcm_overlay_show_dialog(Calendar *c)
{
    if (!c || !c->frame)
        return;
    g_ovl_cal = c;

    if (!g_dlg_shell) {
        g_dlg_shell = XtVaCreatePopupShell("overlaidCalendarsDialog",
            xmDialogShellWidgetClass, c->frame,
            XmNtitle, "Calendar : Overlaid Calendars",
            XmNdeleteResponse, XmUNMAP,
            nullptr);

        Widget form = XtVaCreateWidget("ovlDlgForm",
            xmFormWidgetClass, g_dlg_shell,
            XmNmarginWidth, 10,
            XmNmarginHeight, 10,
            XmNwidth, 580,
            XmNheight, 380,
            nullptr);

        XmString intro_s = XmStringCreateLocalized(
            "Subscribe to read-only .ics / webcal:// feeds or remote CalDAV calendars:");
        Widget intro = XtVaCreateManagedWidget("ovlIntro",
            xmLabelGadgetClass, form,
            XmNlabelString, intro_s,
            XmNtopAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_FORM,
            XmNrightAttachment, XmATTACH_FORM,
            XmNalignment, XmALIGNMENT_BEGINNING,
            nullptr);
        XmStringFree(intro_s);

        /* Bottom buttons */
        Widget bot_rc = XtVaCreateManagedWidget("ovlBotRC",
            xmRowColumnWidgetClass, form,
            XmNorientation, XmHORIZONTAL,
            XmNspacing, 8,
            XmNbottomAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_FORM,
            XmNrightAttachment, XmATTACH_FORM,
            nullptr);

        static const struct { const char *lbl; XtCallbackProc cb; } btns[] = {
            { "Toggle On/Off", dlg_toggle_cb },
            { "Remove", dlg_remove_cb },
            { "Refresh Now", dlg_refresh_cb },
            { "Close", dlg_close_cb },
        };
        for (int i = 0; i < 4; i++) {
            XmString bs = XmStringCreateLocalized((char *)btns[i].lbl);
            Widget b = XtVaCreateManagedWidget("ovlBtn",
                xmPushButtonWidgetClass, bot_rc,
                XmNlabelString, bs,
                nullptr);
            XtAddCallback(b, XmNactivateCallback, btns[i].cb, nullptr);
            XmStringFree(bs);
        }

        XmString st_s = XmStringCreateLocalized(" ");
        g_dlg_status = XtVaCreateManagedWidget("ovlStatus",
            xmLabelGadgetClass, form,
            XmNlabelString, st_s,
            XmNbottomAttachment, XmATTACH_WIDGET,
            XmNbottomWidget, bot_rc,
            XmNbottomOffset, 6,
            XmNleftAttachment, XmATTACH_FORM,
            XmNrightAttachment, XmATTACH_FORM,
            XmNalignment, XmALIGNMENT_BEGINNING,
            nullptr);
        XmStringFree(st_s);

        /* Add-subscription panel */
        Widget add_form = XtVaCreateManagedWidget("ovlAddForm",
            xmFormWidgetClass, form,
            XmNbottomAttachment, XmATTACH_WIDGET,
            XmNbottomWidget, g_dlg_status,
            XmNbottomOffset, 6,
            XmNleftAttachment, XmATTACH_FORM,
            XmNrightAttachment, XmATTACH_FORM,
            nullptr);

        XmString nl_s = XmStringCreateLocalized("Name:");
        Widget nl = XtVaCreateManagedWidget("ovlNameLbl", xmLabelGadgetClass, add_form,
            XmNlabelString, nl_s,
            XmNtopAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_FORM,
            nullptr);
        XmStringFree(nl_s);

        g_dlg_name = XtVaCreateManagedWidget("ovlNameTxt", xmTextFieldWidgetClass, add_form,
            XmNcolumns, 18,
            XmNtopAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_WIDGET,
            XmNleftWidget, nl,
            XmNleftOffset, 4,
            nullptr);

        Widget col_rc = XtVaCreateManagedWidget("ovlColRC", xmRowColumnWidgetClass, add_form,
            XmNorientation, XmHORIZONTAL,
            XmNradioBehavior, True,
            XmNspacing, 2,
            XmNtopAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_WIDGET,
            XmNleftWidget, g_dlg_name,
            XmNleftOffset, 8,
            nullptr);
        static const struct { const char *lbl; DtcmCategory cat; } cols[4] = {
            { "Blue", DTCM_CAT_WORK },
            { "Green", DTCM_CAT_PERSONAL },
            { "Coral", DTCM_CAT_IMPORTANT },
            { "Amber", DTCM_CAT_TRAVEL },
        };
        for (int i = 0; i < 4; i++) {
            XmString cs = XmStringCreateLocalized((char *)cols[i].lbl);
            g_dlg_color_btns[i] = XtVaCreateManagedWidget("ovlColBtn",
                xmToggleButtonWidgetClass, col_rc,
                XmNlabelString, cs,
                XmNindicatorOn, False,
                XmNshadowThickness, 1,
                XmNset, (i == 0) ? True : False,
                nullptr);
            XtAddCallback(g_dlg_color_btns[i], XmNvalueChangedCallback,
                          dlg_color_cb, (XtPointer)(intptr_t)cols[i].cat);
            XmStringFree(cs);
        }

        XmString ul_s = XmStringCreateLocalized("URL / Path:");
        Widget ul = XtVaCreateManagedWidget("ovlUrlLbl", xmLabelGadgetClass, add_form,
            XmNlabelString, ul_s,
            XmNtopAttachment, XmATTACH_WIDGET,
            XmNtopWidget, g_dlg_name,
            XmNtopOffset, 6,
            XmNleftAttachment, XmATTACH_FORM,
            nullptr);
        XmStringFree(ul_s);

        XmString add_s = XmStringCreateLocalized("Add");
        Widget add_btn = XtVaCreateManagedWidget("ovlAddBtn", xmPushButtonWidgetClass, add_form,
            XmNlabelString, add_s,
            XmNtopAttachment, XmATTACH_WIDGET,
            XmNtopWidget, g_dlg_name,
            XmNtopOffset, 6,
            XmNrightAttachment, XmATTACH_FORM,
            nullptr);
        XtAddCallback(add_btn, XmNactivateCallback, dlg_add_cb, nullptr);
        XmStringFree(add_s);

        g_dlg_url = XtVaCreateManagedWidget("ovlUrlTxt", xmTextFieldWidgetClass, add_form,
            XmNtopAttachment, XmATTACH_WIDGET,
            XmNtopWidget, g_dlg_name,
            XmNtopOffset, 6,
            XmNleftAttachment, XmATTACH_WIDGET,
            XmNleftWidget, ul,
            XmNleftOffset, 4,
            XmNrightAttachment, XmATTACH_WIDGET,
            XmNrightWidget, add_btn,
            XmNrightOffset, 6,
            nullptr);

        Arg al[8];
        int ac = 0;
        XtSetArg(al[ac], XmNvisibleItemCount, 8); ac++;
        XtSetArg(al[ac], XmNselectionPolicy, XmSINGLE_SELECT); ac++;
        g_dlg_list = XmCreateScrolledList(form, "ovlList", al, ac);
        XtVaSetValues(XtParent(g_dlg_list),
            XmNtopAttachment, XmATTACH_WIDGET,
            XmNtopWidget, intro,
            XmNtopOffset, 6,
            XmNbottomAttachment, XmATTACH_WIDGET,
            XmNbottomWidget, add_form,
            XmNbottomOffset, 8,
            XmNleftAttachment, XmATTACH_FORM,
            XmNrightAttachment, XmATTACH_FORM,
            nullptr);
        XtManageChild(g_dlg_list);
        XtManageChild(form);
    }

    rebuild_dialog_list();
    XtPopup(g_dlg_shell, XtGrabNone);
}

void
dtcm_overlay_ui_init(Calendar *c)
{
    g_ovl_cal = c;
    dtcm_overlay_init();
    if (dtcm_overlay_count() > 0)
        dtcm_overlay_refresh_async(c);
}
