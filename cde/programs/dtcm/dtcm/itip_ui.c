/*
 * CDE - Common Desktop Environment
 *
 * iTIP / iMIP Meeting Invitation response dialog (Accept / Tentative / Decline)
 * with both "Send Reply via Mail" (submit_mail) and "Save Reply .ics..."
 * delivery options for Calendar Manager (dtcm).
 */

#include <EUSCompat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <Xm/Xm.h>
#include <Xm/DialogS.h>
#include <Xm/FileSB.h>
#include <Xm/Form.h>
#include <Xm/LabelG.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
#include <Xm/Separator.h>
#include <Xm/Text.h>
#include <Xm/TextF.h>
#include <Xm/ToggleB.h>

#include "agenda_ui.h"
#include "calendar.h"
#include "dayglance.h"
#include "ics_csa.h"
#include "ics_io.h"
#include "itip_ui.h"
#include "misc.h"
#include "monthglance.h"
#include "weekglance.h"

extern int submit_mail(const char *to, const char *subject, const char *body);

typedef struct {
    Calendar *c;
    DtcmIcsEvent ev;
    Widget shell;
    Widget email_field;
    Widget send_mail_toggle;
    Widget save_ics_toggle;
    char pending_partstat[32];
    char pending_email[256];
} ItipDialogCtx;

static void
refresh_after_itip(Calendar *c)
{
    if (!c || !c->view)
        return;
    invalidate_cache(c);
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
save_reply_fsb_ok_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    ItipDialogCtx *ctx = (ItipDialogCtx *)cd;
    XmFileSelectionBoxCallbackStruct *cbs = (XmFileSelectionBoxCallbackStruct *)call_data;
    if (ctx && cbs) {
        char *path = nullptr;
        if (XmStringGetLtoR(cbs->value, XmFONTLIST_DEFAULT_TAG, &path) && path && path[0]) {
            if (dtcm_ics_write_itip_reply(path, &ctx->ev, ctx->pending_email, ctx->pending_partstat) == 0) {
                char msg[512];
                snprintf(msg, sizeof(msg), "Saved iTIP %s reply to %s", ctx->pending_partstat, path);
                set_message(ctx->c->message_text, msg);
            } else {
                set_message(ctx->c->message_text, "Failed to write iTIP reply .ics file.");
            }
        }
        if (path) XtFree(path);
        XtDestroyWidget(ctx->shell);
        free(ctx);
    }
    XtDestroyWidget(XtParent(w));
}

static void
save_reply_fsb_cancel_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)call_data;
    ItipDialogCtx *ctx = (ItipDialogCtx *)cd;
    if (ctx) {
        XtDestroyWidget(ctx->shell);
        free(ctx);
    }
    XtDestroyWidget(XtParent(w));
}

static void
send_reply_via_mail(ItipDialogCtx *ctx)
{
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !tmpdir[0]) tmpdir = "/tmp";
    char tmppath[1024];
    snprintf(tmppath, sizeof(tmppath), "%s/dtcm-itip-reply-%d.ics", tmpdir, (int)getpid());
    if (dtcm_ics_write_itip_reply(tmppath, &ctx->ev, ctx->pending_email, ctx->pending_partstat) != 0) {
        set_message(ctx->c->message_text, "Failed to build iTIP reply payload.");
        return;
    }

    FILE *fp = fopen(tmppath, "rb");
    char *body = nullptr;
    if (fp) {
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (sz > 0 && sz < 65536) {
            body = (char *)malloc((size_t)sz + 1);
            if (body) {
                size_t nr = fread(body, 1, (size_t)sz, fp);
                body[nr] = '\0';
            }
        }
        fclose(fp);
    }
    unlink(tmppath);

    char subj[600];
    snprintf(subj, sizeof(subj), "%s: %s",
             ctx->pending_partstat,
             ctx->ev.summary[0] ? ctx->ev.summary : "Meeting Invitation");

    const char *to = ctx->ev.organizer_mailto[0] ? ctx->ev.organizer_mailto : ctx->ev.organizer;
    if (to && to[0] && body && submit_mail(to, subj, body) == 0) {
        char msg[320];
        snprintf(msg, sizeof(msg), "Sent %s reply to %s", ctx->pending_partstat, to);
        set_message(ctx->c->message_text, msg);
    } else {
        set_message(ctx->c->message_text, "Mail delivery unavailable; use 'Save Reply .ics...' instead.");
    }
    free(body);
}

static void
handle_itip_decision(ItipDialogCtx *ctx, const char *partstat, Boolean add_to_calendar)
{
    if (!ctx) return;

    char *email = XmTextFieldGetString(ctx->email_field);
    snprintf(ctx->pending_email, sizeof(ctx->pending_email), "%s",
             (email && email[0]) ? email : (ctx->c->calname ? ctx->c->calname : "user@localhost"));
    if (email) XtFree(email);
    snprintf(ctx->pending_partstat, sizeof(ctx->pending_partstat), "%s", partstat);

    if (add_to_calendar) {
        dtcm_ics_import_single_event(ctx->c, &ctx->ev);
        refresh_after_itip(ctx->c);
    }

    Boolean save_ics = XmToggleButtonGetState(ctx->save_ics_toggle);
    if (save_ics) {
        XtPopdown(ctx->shell);
        Arg al[6];
        int ac = 0;
        XmString title = XmStringCreateLocalized("Save iTIP Reply (.ics)");
        XmString pat = XmStringCreateLocalized("*.ics");
        XtSetArg(al[ac], XmNdialogTitle, title); ac++;
        XtSetArg(al[ac], XmNpattern, pat); ac++;
        Widget fsb = XmCreateFileSelectionDialog(ctx->c->frame, "itipSaveReplyFsb", al, ac);
        XmStringFree(title);
        XmStringFree(pat);
        Widget help_b = XmFileSelectionBoxGetChild(fsb, XmDIALOG_HELP_BUTTON);
        if (help_b) XtUnmanageChild(help_b);
        XtAddCallback(fsb, XmNokCallback, save_reply_fsb_ok_cb, ctx);
        XtAddCallback(fsb, XmNcancelCallback, save_reply_fsb_cancel_cb, ctx);
        XtManageChild(fsb);
        return;
    }

    send_reply_via_mail(ctx);
    XtDestroyWidget(ctx->shell);
    free(ctx);
}

static void itip_accept_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)call_data;
    handle_itip_decision((ItipDialogCtx *)cd, "ACCEPTED", True);
}

static void itip_tentative_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)call_data;
    handle_itip_decision((ItipDialogCtx *)cd, "TENTATIVE", True);
}

static void itip_decline_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)call_data;
    handle_itip_decision((ItipDialogCtx *)cd, "DECLINED", False);
}

static void itip_cancel_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)call_data;
    ItipDialogCtx *ctx = (ItipDialogCtx *)cd;
    if (ctx) {
        XtDestroyWidget(ctx->shell);
        free(ctx);
    }
}

void
dtcm_itip_show_invitation_dialog(Calendar *c, const DtcmIcsEvent *ev)
{
    if (!c || !c->frame || !ev)
        return;

    ItipDialogCtx *ctx = (ItipDialogCtx *)calloc(1, sizeof(ItipDialogCtx));
    if (!ctx)
        return;
    ctx->c = c;
    ctx->ev = *ev;

    ctx->shell = XtVaCreatePopupShell("meetingInvitationDialog",
        xmDialogShellWidgetClass, c->frame,
        XmNtitle, "Calendar : Meeting Invitation",
        XmNdeleteResponse, XmDESTROY,
        nullptr);

    Widget form = XtVaCreateWidget("itipForm",
        xmFormWidgetClass, ctx->shell,
        XmNmarginWidth, 12,
        XmNmarginHeight, 12,
        XmNwidth, 520,
        nullptr);

    struct tm tm_s, tm_e;
    localtime_r(&ev->start_utc, &tm_s);
    time_t end_t = ev->end_utc > ev->start_utc ? ev->end_utc : (ev->start_utc + 3600);
    localtime_r(&end_t, &tm_e);
    char when_buf[128];
    if (ev->all_day) {
        strftime(when_buf, sizeof(when_buf), "%A, %B %d, %Y (All Day)", &tm_s);
    } else {
        char d1[64], t1[16], t2[16];
        strftime(d1, sizeof(d1), "%a %b %d, %Y", &tm_s);
        snprintf(t1, sizeof(t1), "%02d:%02d", tm_s.tm_hour, tm_s.tm_min);
        snprintf(t2, sizeof(t2), "%02d:%02d", tm_e.tm_hour, tm_e.tm_min);
        snprintf(when_buf, sizeof(when_buf), "%s  %s - %s", d1, t1, t2);
    }

    char info_buf[2048];
    snprintf(info_buf, sizeof(info_buf),
             "Meeting:   %s\n"
             "When:      %s\n"
             "Location:  %s\n"
             "Organizer: %s\n"
             "Attendees:\n%s",
             ev->summary[0] ? ev->summary : "(Untitled Meeting)",
             when_buf,
             ev->location[0] ? ev->location : "(None)",
             ev->organizer[0] ? ev->organizer : "(Unknown)",
             ev->attendees[0] ? ev->attendees : "(None listed)");

    Arg al[8];
    int ac = 0;
    XtSetArg(al[ac], XmNeditMode, XmMULTI_LINE_EDIT); ac++;
    XtSetArg(al[ac], XmNeditable, False); ac++;
    XtSetArg(al[ac], XmNcursorPositionVisible, False); ac++;
    XtSetArg(al[ac], XmNrows, 8); ac++;
    XtSetArg(al[ac], XmNcolumns, 58); ac++;
    XtSetArg(al[ac], XmNvalue, info_buf); ac++;
    Widget info_txt = XmCreateScrolledText(form, "itipInfo", al, ac);
    XtVaSetValues(XtParent(info_txt),
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        nullptr);
    XtManageChild(info_txt);

    XmString em_lbl_s = XmStringCreateLocalized("Your Email:");
    Widget em_lbl = XtVaCreateManagedWidget("itipEmailLbl", xmLabelGadgetClass, form,
        XmNlabelString, em_lbl_s,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, XtParent(info_txt),
        XmNtopOffset, 10,
        XmNleftAttachment, XmATTACH_FORM,
        nullptr);
    XmStringFree(em_lbl_s);

    ctx->email_field = XtVaCreateManagedWidget("itipEmailTxt", xmTextFieldWidgetClass, form,
        XmNvalue, c->calname ? c->calname : "user@localhost",
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, XtParent(info_txt),
        XmNtopOffset, 8,
        XmNleftAttachment, XmATTACH_WIDGET,
        XmNleftWidget, em_lbl,
        XmNleftOffset, 6,
        XmNrightAttachment, XmATTACH_FORM,
        nullptr);

    Widget mode_rc = XtVaCreateManagedWidget("itipModeRC", xmRowColumnWidgetClass, form,
        XmNorientation, XmHORIZONTAL,
        XmNradioBehavior, True,
        XmNspacing, 12,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, ctx->email_field,
        XmNtopOffset, 8,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        nullptr);

    XmString m1 = XmStringCreateLocalized("Send Reply via Mail");
    ctx->send_mail_toggle = XtVaCreateManagedWidget("itipSendMail",
        xmToggleButtonWidgetClass, mode_rc,
        XmNlabelString, m1,
        XmNset, True,
        nullptr);
    XmStringFree(m1);

    XmString m2 = XmStringCreateLocalized("Save Reply .ics...");
    ctx->save_ics_toggle = XtVaCreateManagedWidget("itipSaveIcs",
        xmToggleButtonWidgetClass, mode_rc,
        XmNlabelString, m2,
        XmNset, False,
        nullptr);
    XmStringFree(m2);

    Widget sep = XtVaCreateManagedWidget("itipSep", xmSeparatorWidgetClass, form,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, mode_rc,
        XmNtopOffset, 8,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        nullptr);

    Widget btn_rc = XtVaCreateManagedWidget("itipBtnRC", xmRowColumnWidgetClass, form,
        XmNorientation, XmHORIZONTAL,
        XmNspacing, 10,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, sep,
        XmNtopOffset, 8,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        nullptr);

    static const struct { const char *lbl; XtCallbackProc cb; } actions[4] = {
        { "Accept",    itip_accept_cb },
        { "Tentative", itip_tentative_cb },
        { "Decline",   itip_decline_cb },
        { "Cancel",    itip_cancel_cb },
    };
    for (int i = 0; i < 4; i++) {
        XmString bs = XmStringCreateLocalized((char *)actions[i].lbl);
        Widget b = XtVaCreateManagedWidget("itipActionBtn",
            xmPushButtonWidgetClass, btn_rc,
            XmNlabelString, bs,
            nullptr);
        XtAddCallback(b, XmNactivateCallback, actions[i].cb, ctx);
        XmStringFree(bs);
    }

    XtManageChild(form);
    XtPopup(ctx->shell, XtGrabNone);
}
