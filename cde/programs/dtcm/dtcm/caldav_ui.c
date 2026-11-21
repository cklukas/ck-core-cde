/*
 * CDE - Common Desktop Environment
 *
 * Motif UI dialog for CalDAV (RFC 4791) encrypted calendar sync in dtcm.
 */

#include <EUSCompat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <Xm/Xm.h>
#include <Xm/Form.h>
#include <Xm/LabelG.h>
#include <Xm/PushB.h>
#include <Xm/PushBG.h>
#include <Xm/RowColumn.h>
#include <Xm/SeparatoG.h>
#include <Xm/Text.h>
#include <Xm/TextF.h>
#include <Xm/ToggleBG.h>

#include "caldav_client.h"
#include "caldav_ui.h"
#include "ics_csa.h"
#include "ics_io.h"

static Widget g_caldav_dialog = nullptr;
static Widget g_url_field = nullptr;
static Widget g_user_field = nullptr;
static Widget g_pass_field = nullptr;
static Widget g_tls_toggle = nullptr;
static Widget g_caldav_status = nullptr;

static char g_password_buf[256];

static void
set_status(const char *msg)
{
    if (g_caldav_status && msg)
        XmTextSetString(g_caldav_status, (char *)msg);
}

static void
read_form_config(DtcmCaldavConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    char *u = g_url_field ? XmTextFieldGetString(g_url_field) : nullptr;
    char *usr = g_user_field ? XmTextFieldGetString(g_user_field) : nullptr;

    if (u) {
        snprintf(cfg->url, sizeof(cfg->url), "%s", u);
        XtFree(u);
    }
    if (usr) {
        snprintf(cfg->username, sizeof(cfg->username), "%s", usr);
        XtFree(usr);
    }
    snprintf(cfg->password, sizeof(cfg->password), "%s", g_password_buf);
    cfg->verify_tls = (g_tls_toggle && XmToggleButtonGadgetGetState(g_tls_toggle)) ? 1 : 0;
}

static void
password_modify_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    XmTextVerifyCallbackStruct *cbs = (XmTextVerifyCallbackStruct *)call_data;
    size_t cur_len = strlen(g_password_buf);
    size_t start = (size_t)cbs->startPos;
    size_t end = (size_t)cbs->endPos;

    if (start > cur_len)
        start = cur_len;
    if (end > cur_len)
        end = cur_len;

    if (cbs->text == nullptr || cbs->text->length == 0) {
        /* Deletion */
        memmove(g_password_buf + start, g_password_buf + end, cur_len - end + 1);
        return;
    }

    size_t ins_len = (size_t)cbs->text->length;
    if (cur_len - (end - start) + ins_len >= sizeof(g_password_buf)) {
        cbs->doit = False;
        return;
    }

    memmove(g_password_buf + start + ins_len, g_password_buf + end, cur_len - end + 1);
    memcpy(g_password_buf + start, cbs->text->ptr, ins_len);
    for (int i = 0; i < cbs->text->length; i++)
        cbs->text->ptr[i] = '*';
}

static void
test_btn_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    DtcmCaldavConfig cfg;
    DtcmCaldavItem *items = nullptr;
    int count = 0;
    char err[512] = "";
    char msg[1024];

    read_form_config(&cfg);
    if (dtcm_caldav_propfind(&cfg, &items, &count, err, sizeof(err)) != 0) {
        snprintf(msg, sizeof(msg), "Connection test failed:\n%s", err);
        set_status(msg);
        return;
    }
    snprintf(msg, sizeof(msg),
             "Connection OK (%s).\nDiscovered %d iCalendar (.ics) item(s) in collection.",
             cfg.verify_tls ? "TLS verified" : "TLS verification disabled",
             count);
    dtcm_caldav_free_items(items);
    set_status(msg);
}

static void
pull_btn_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    Calendar *c = (Calendar *)client_data;
    DtcmCaldavConfig cfg;
    DtcmCaldavItem *items = nullptr;
    int item_count = 0;
    int total_imported = 0;
    int failed_items = 0;
    char err[512] = "";
    char msg[1024];

    read_form_config(&cfg);
    if (dtcm_caldav_propfind(&cfg, &items, &item_count, err, sizeof(err)) != 0) {
        snprintf(msg, sizeof(msg), "CalDAV Pull failed during PROPFIND:\n%s", err);
        set_status(msg);
        return;
    }

    const char *td = getenv("TMPDIR");
    if (!td || !td[0])
        td = "/tmp";

    for (int i = 0; i < item_count; i++) {
        DtcmIcsEvent *evs = nullptr;
        int ev_count = 0;
        if (dtcm_caldav_get_events(&cfg, items[i].href, &evs, &ev_count, err, sizeof(err)) != 0) {
            failed_items++;
            continue;
        }
        if (ev_count > 0) {
            char tmp_path[512];
            snprintf(tmp_path, sizeof(tmp_path), "%s/dtcm-pull-XXXXXX", td);
            int fd = mkstemp(tmp_path);
            if (fd >= 0) {
                (void)fchmod(fd, 0600);
                close(fd);
                if (dtcm_ics_write_file(tmp_path, evs, ev_count) == 0) {
                    int imp = 0;
                    if (dtcm_import_ics_file(c, tmp_path, &imp) == 0)
                        total_imported += imp;
                }
                (void)unlink(tmp_path);
            }
        }
        dtcm_ics_free_events(evs);
    }
    dtcm_caldav_free_items(items);

    snprintf(msg, sizeof(msg),
             "CalDAV Pull complete.\nScanned %d remote resource(s), imported %d event(s) (%d error(s)).",
             item_count, total_imported, failed_items);
    set_status(msg);
}

static void
push_btn_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    Calendar *c = (Calendar *)client_data;
    DtcmCaldavConfig cfg;
    char err[512] = "";
    char msg[1024];

    read_form_config(&cfg);
    if (dtcm_caldav_validate_url(cfg.url, err, sizeof(err)) != 0) {
        set_status(err);
        return;
    }

    const char *td = getenv("TMPDIR");
    if (!td || !td[0])
        td = "/tmp";
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s/dtcm-push-XXXXXX", td);
    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        set_status("Failed to create temporary export file.");
        return;
    }
    (void)fchmod(fd, 0600);
    close(fd);

    int exported = 0;
    if (dtcm_export_ics_file(c, tmp_path, &exported) != 0) {
        (void)unlink(tmp_path);
        set_status("Failed to export local calendar appointments.");
        return;
    }

    DtcmIcsEvent *evs = nullptr;
    int ev_count = 0;
    if (dtcm_ics_parse_file(tmp_path, &evs, &ev_count) != 0) {
        (void)unlink(tmp_path);
        set_status("Failed to read exported appointments for upload.");
        return;
    }
    (void)unlink(tmp_path);

    int uploaded = 0;
    int failed = 0;
    for (int i = 0; i < ev_count; i++) {
        if (dtcm_caldav_put_event(&cfg, &evs[i], err, sizeof(err)) == 0)
            uploaded++;
        else
            failed++;
    }
    dtcm_ics_free_events(evs);

    if (failed > 0) {
        snprintf(msg, sizeof(msg),
                 "CalDAV Push finished with errors: uploaded %d of %d event(s).\nLast error: %s",
                 uploaded, ev_count, err);
    } else {
        snprintf(msg, sizeof(msg),
                 "CalDAV Push complete: uploaded %d event(s) to %s.",
                 uploaded, cfg.url);
    }
    set_status(msg);
}

static void
save_btn_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    DtcmCaldavConfig cfg;

    read_form_config(&cfg);
    if (dtcm_caldav_save_profile(&cfg) == 0) {
        set_status("Saved CalDAV profile to ~/.dt/dtcm_caldav.conf (mode 0600; password not stored).");
    } else {
        set_status("Failed to save ~/.dt/dtcm_caldav.conf.");
    }
}

static void
close_btn_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    memset(g_password_buf, 0, sizeof(g_password_buf));
    if (g_pass_field)
        XmTextFieldSetString(g_pass_field, "");
    if (g_caldav_dialog)
        XtUnmanageChild(g_caldav_dialog);
}

static void
caldav_menu_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    dtcm_caldav_show_dialog((Calendar *)client_data);
}

void
dtcm_caldav_show_dialog(Calendar *c)
{
    DtcmCaldavConfig cfg;

    if (!c || !c->frame)
        return;

    if (!g_caldav_dialog) {
        Arg al[16];
        int ac = 0;
        XmString title = XmStringCreateLocalized("Calendar : CalDAV Sync (HTTPS)");
        XmString url_lbl = XmStringCreateLocalized("Collection URL:");
        XmString usr_lbl = XmStringCreateLocalized("Username:");
        XmString pwd_lbl = XmStringCreateLocalized("Password (session):");
        XmString tls_lbl = XmStringCreateLocalized("Verify TLS server certificates (recommended)");
        XmString test_lbl = XmStringCreateLocalized("Test");
        XmString pull_lbl = XmStringCreateLocalized("Pull");
        XmString push_lbl = XmStringCreateLocalized("Push");
        XmString save_lbl = XmStringCreateLocalized("Save Profile");
        XmString close_lbl = XmStringCreateLocalized("Close");

        XtSetArg(al[ac], XmNdialogTitle, title); ac++;
        XtSetArg(al[ac], XmNautoUnmanage, False); ac++;
        XtSetArg(al[ac], XmNmarginWidth, 12); ac++;
        XtSetArg(al[ac], XmNmarginHeight, 12); ac++;
        g_caldav_dialog = XmCreateFormDialog(c->frame, "caldavDialog", al, ac);

        /* URL row */
        ac = 0;
        XtSetArg(al[ac], XmNlabelString, url_lbl); ac++;
        XtSetArg(al[ac], XmNtopAttachment, XmATTACH_FORM); ac++;
        XtSetArg(al[ac], XmNleftAttachment, XmATTACH_FORM); ac++;
        Widget w_url_l = XmCreateLabelGadget(g_caldav_dialog, "urlLabel", al, ac);
        XtManageChild(w_url_l);

        ac = 0;
        XtSetArg(al[ac], XmNcolumns, 52); ac++;
        XtSetArg(al[ac], XmNtopAttachment, XmATTACH_WIDGET); ac++;
        XtSetArg(al[ac], XmNtopWidget, w_url_l); ac++;
        XtSetArg(al[ac], XmNtopOffset, 4); ac++;
        XtSetArg(al[ac], XmNleftAttachment, XmATTACH_FORM); ac++;
        XtSetArg(al[ac], XmNrightAttachment, XmATTACH_FORM); ac++;
        g_url_field = XmCreateTextField(g_caldav_dialog, "urlField", al, ac);
        XtManageChild(g_url_field);

        /* Username row */
        ac = 0;
        XtSetArg(al[ac], XmNlabelString, usr_lbl); ac++;
        XtSetArg(al[ac], XmNtopAttachment, XmATTACH_WIDGET); ac++;
        XtSetArg(al[ac], XmNtopWidget, g_url_field); ac++;
        XtSetArg(al[ac], XmNtopOffset, 8); ac++;
        XtSetArg(al[ac], XmNleftAttachment, XmATTACH_FORM); ac++;
        Widget w_usr_l = XmCreateLabelGadget(g_caldav_dialog, "userLabel", al, ac);
        XtManageChild(w_usr_l);

        ac = 0;
        XtSetArg(al[ac], XmNcolumns, 32); ac++;
        XtSetArg(al[ac], XmNtopAttachment, XmATTACH_WIDGET); ac++;
        XtSetArg(al[ac], XmNtopWidget, w_usr_l); ac++;
        XtSetArg(al[ac], XmNtopOffset, 4); ac++;
        XtSetArg(al[ac], XmNleftAttachment, XmATTACH_FORM); ac++;
        XtSetArg(al[ac], XmNrightAttachment, XmATTACH_FORM); ac++;
        g_user_field = XmCreateTextField(g_caldav_dialog, "userField", al, ac);
        XtManageChild(g_user_field);

        /* Password row */
        ac = 0;
        XtSetArg(al[ac], XmNlabelString, pwd_lbl); ac++;
        XtSetArg(al[ac], XmNtopAttachment, XmATTACH_WIDGET); ac++;
        XtSetArg(al[ac], XmNtopWidget, g_user_field); ac++;
        XtSetArg(al[ac], XmNtopOffset, 8); ac++;
        XtSetArg(al[ac], XmNleftAttachment, XmATTACH_FORM); ac++;
        Widget w_pwd_l = XmCreateLabelGadget(g_caldav_dialog, "passLabel", al, ac);
        XtManageChild(w_pwd_l);

        ac = 0;
        XtSetArg(al[ac], XmNcolumns, 32); ac++;
        XtSetArg(al[ac], XmNtopAttachment, XmATTACH_WIDGET); ac++;
        XtSetArg(al[ac], XmNtopWidget, w_pwd_l); ac++;
        XtSetArg(al[ac], XmNtopOffset, 4); ac++;
        XtSetArg(al[ac], XmNleftAttachment, XmATTACH_FORM); ac++;
        XtSetArg(al[ac], XmNrightAttachment, XmATTACH_FORM); ac++;
        g_pass_field = XmCreateTextField(g_caldav_dialog, "passField", al, ac);
        XtAddCallback(g_pass_field, XmNmodifyVerifyCallback, password_modify_cb, nullptr);
        XtManageChild(g_pass_field);

        /* TLS toggle */
        ac = 0;
        XtSetArg(al[ac], XmNlabelString, tls_lbl); ac++;
        XtSetArg(al[ac], XmNset, True); ac++;
        XtSetArg(al[ac], XmNtopAttachment, XmATTACH_WIDGET); ac++;
        XtSetArg(al[ac], XmNtopWidget, g_pass_field); ac++;
        XtSetArg(al[ac], XmNtopOffset, 8); ac++;
        XtSetArg(al[ac], XmNleftAttachment, XmATTACH_FORM); ac++;
        g_tls_toggle = XmCreateToggleButtonGadget(g_caldav_dialog, "tlsToggle", al, ac);
        XtManageChild(g_tls_toggle);

        /* Action button row at bottom */
        ac = 0;
        XtSetArg(al[ac], XmNorientation, XmHORIZONTAL); ac++;
        XtSetArg(al[ac], XmNspacing, 8); ac++;
        XtSetArg(al[ac], XmNbottomAttachment, XmATTACH_FORM); ac++;
        XtSetArg(al[ac], XmNleftAttachment, XmATTACH_FORM); ac++;
        XtSetArg(al[ac], XmNrightAttachment, XmATTACH_FORM); ac++;
        Widget btn_row = XmCreateRowColumn(g_caldav_dialog, "buttonRow", al, ac);

        Widget b_test = XmCreatePushButton(btn_row, "testBtn", (Arg[]){{XmNlabelString, (XtArgVal)test_lbl}}, 1);
        Widget b_pull = XmCreatePushButton(btn_row, "pullBtn", (Arg[]){{XmNlabelString, (XtArgVal)pull_lbl}}, 1);
        Widget b_push = XmCreatePushButton(btn_row, "pushBtn", (Arg[]){{XmNlabelString, (XtArgVal)push_lbl}}, 1);
        Widget b_save = XmCreatePushButton(btn_row, "saveBtn", (Arg[]){{XmNlabelString, (XtArgVal)save_lbl}}, 1);
        Widget b_close = XmCreatePushButton(btn_row, "closeBtn", (Arg[]){{XmNlabelString, (XtArgVal)close_lbl}}, 1);

        XtAddCallback(b_test, XmNactivateCallback, test_btn_cb, (XtPointer)c);
        XtAddCallback(b_pull, XmNactivateCallback, pull_btn_cb, (XtPointer)c);
        XtAddCallback(b_push, XmNactivateCallback, push_btn_cb, (XtPointer)c);
        XtAddCallback(b_save, XmNactivateCallback, save_btn_cb, (XtPointer)c);
        XtAddCallback(b_close, XmNactivateCallback, close_btn_cb, nullptr);

        XtManageChild(b_test);
        XtManageChild(b_pull);
        XtManageChild(b_push);
        XtManageChild(b_save);
        XtManageChild(b_close);
        XtManageChild(btn_row);

        ac = 0;
        XtSetArg(al[ac], XmNleftAttachment, XmATTACH_FORM); ac++;
        XtSetArg(al[ac], XmNrightAttachment, XmATTACH_FORM); ac++;
        XtSetArg(al[ac], XmNbottomAttachment, XmATTACH_WIDGET); ac++;
        XtSetArg(al[ac], XmNbottomWidget, btn_row); ac++;
        XtSetArg(al[ac], XmNbottomOffset, 8); ac++;
        Widget sep = XmCreateSeparatorGadget(g_caldav_dialog, "caldavSep", al, ac);
        XtManageChild(sep);

        /* Status output box */
        ac = 0;
        XtSetArg(al[ac], XmNeditMode, XmMULTI_LINE_EDIT); ac++;
        XtSetArg(al[ac], XmNeditable, False); ac++;
        XtSetArg(al[ac], XmNcursorPositionVisible, False); ac++;
        XtSetArg(al[ac], XmNrows, 5); ac++;
        XtSetArg(al[ac], XmNcolumns, 56); ac++;
        XtSetArg(al[ac], XmNwordWrap, True); ac++;
        XtSetArg(al[ac], XmNtopAttachment, XmATTACH_WIDGET); ac++;
        XtSetArg(al[ac], XmNtopWidget, g_tls_toggle); ac++;
        XtSetArg(al[ac], XmNtopOffset, 8); ac++;
        XtSetArg(al[ac], XmNleftAttachment, XmATTACH_FORM); ac++;
        XtSetArg(al[ac], XmNrightAttachment, XmATTACH_FORM); ac++;
        XtSetArg(al[ac], XmNbottomAttachment, XmATTACH_WIDGET); ac++;
        XtSetArg(al[ac], XmNbottomWidget, sep); ac++;
        XtSetArg(al[ac], XmNbottomOffset, 8); ac++;
        g_caldav_status = XmCreateScrolledText(g_caldav_dialog, "caldavStatus", al, ac);
        XtManageChild(g_caldav_status);

        XtVaSetValues(g_caldav_dialog, XmNcancelButton, b_close, nullptr);

        XmStringFree(title);
        XmStringFree(url_lbl);
        XmStringFree(usr_lbl);
        XmStringFree(pwd_lbl);
        XmStringFree(tls_lbl);
        XmStringFree(test_lbl);
        XmStringFree(pull_lbl);
        XmStringFree(push_lbl);
        XmStringFree(save_lbl);
        XmStringFree(close_lbl);
    }

    if (dtcm_caldav_load_profile(&cfg) == 0) {
        XmTextFieldSetString(g_url_field, cfg.url);
        XmTextFieldSetString(g_user_field, cfg.username);
        XmToggleButtonGadgetSetState(g_tls_toggle, cfg.verify_tls ? True : False, False);
    }
    if (!dtcm_caldav_is_supported()) {
        set_status("CalDAV support was not compiled in (libcurl4-openssl-dev not found at configure time).");
    } else {
        set_status("Enter an HTTPS CalDAV collection URL (e.g. Nextcloud, Radicale, Baikal) and click Test, Pull, or Push.");
    }
    XtManageChild(g_caldav_dialog);
}

void
dtcm_caldav_add_menu_item(Calendar *c, Widget file_menu)
{
    Arg al[4];
    int ac = 0;
    XmString label;
    Widget btn;

    if (!c || !file_menu)
        return;

    label = XmStringCreateLocalized("CalDAV Sync...");
    XtSetArg(al[ac], XmNlabelString, label); ac++;
    btn = XmCreatePushButtonGadget(file_menu, "caldavSync", al, ac);
    XtAddCallback(btn, XmNactivateCallback, caldav_menu_cb, (XtPointer)c);
    XmStringFree(label);
}
