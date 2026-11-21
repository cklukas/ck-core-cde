/*
 * CDE - Common Desktop Environment
 *
 * Accounts & Servers setup dialog (Gmail, iCloud, Fastmail, M365, Custom
 * IMAP/SMTP, Local) + non-blocking pthread + XtAppAddInput IMAP sync for dtmail.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "OAuth2Client.h"

#include <Xm/Xm.h>
#include <Xm/DialogS.h>
#include <Xm/Form.h>
#include <Xm/LabelG.h>
#include <Xm/List.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
#include <Xm/Separator.h>
#include <Xm/TextF.h>
#include <Xm/ToggleB.h>

#include "AccountSetupDialog.h"
#include "CurlMailEngine.h"
#include "MailAccountStore.h"

static Widget g_acct_shell = NULL;
static Widget g_acct_list = NULL;
static Widget g_tf_name = NULL;
static Widget g_tf_email = NULL;
static Widget g_tf_user = NULL;
static Widget g_tf_pass = NULL;
static Widget g_tf_ihost = NULL;
static Widget g_tf_iport = NULL;
static Widget g_tf_shost = NULL;
static Widget g_tf_sport = NULL;
static Widget g_lbl_status = NULL;
static Widget g_rb_auth_pass = NULL;
static Widget g_rb_auth_oauth = NULL;
static Widget g_rb_auth_local = NULL;
static DtMailSyncDoneCb g_sync_cb = NULL;
static void *g_sync_ud = NULL;
static int g_sync_in_flight = 0;

typedef struct {
    int pipe_fd[2];
    XtInputId input_id;
    DtMailSyncDoneCb cb;
    void *ud;
    int acct_count;
    DtMailAccount accts[DTMAIL_MAX_ACCOUNTS];
    char secrets[DTMAIL_MAX_ACCOUNTS][512];
    char mbox_paths[DTMAIL_MAX_ACCOUNTS][1024];
    int total_added;
    int errors;
    char last_err[256];
} AsyncSyncJob;

static void
set_dlg_status(const char *msg)
{
    if (!g_lbl_status) return;
    XmString s = XmStringCreateLocalized((char *)(msg ? msg : " "));
    XtVaSetValues(g_lbl_status, XmNlabelString, s, NULL);
    XmStringFree(s);
}

static void *
sync_worker_fn(void *arg)
{
    AsyncSyncJob *job = (AsyncSyncJob *)arg;
    for (int i = 0; i < job->acct_count; i++) {
        const DtMailAccount *a = &job->accts[i];
        if (!a->enabled || !a->imap_host[0])
            continue;
        int added = 0;
        char err[256] = "";
        if (dtmail_imap_sync_folder(a, job->secrets[i], "INBOX",
                                    job->mbox_paths[i], &added,
                                    err, sizeof(err)) == 0) {
            job->total_added += added;
        } else {
            job->errors++;
            snprintf(job->last_err, sizeof(job->last_err), "%s", err);
        }
    }
    char done = '1';
    (void)write(job->pipe_fd[1], &done, 1);
    return NULL;
}

static void
sync_pipe_ready_cb(XtPointer cd, int *fd, XtInputId *id)
{
    (void)fd;
    AsyncSyncJob *job = (AsyncSyncJob *)cd;
    if (!job) return;
    char buf[16];
    (void)read(job->pipe_fd[0], buf, sizeof(buf));
    if (id) XtRemoveInput(*id);
    close(job->pipe_fd[0]);
    close(job->pipe_fd[1]);

    char msg[320];
    if (job->errors > 0) {
        snprintf(msg, sizeof(msg), "Mail sync finished (%d new, %d error: %s)",
                 job->total_added, job->errors, job->last_err);
    } else {
        snprintf(msg, sizeof(msg), "Mail sync complete (%d new message%s).",
                 job->total_added, job->total_added == 1 ? "" : "s");
    }
    set_dlg_status(msg);
    DtMailSyncDoneCb cb = job->cb;
    void *ud = job->ud;
    int added = job->total_added;
    free(job);
    g_sync_in_flight = 0;

    if (cb) cb(added, msg, ud);
}

void
dtmail_start_async_sync(XtAppContext app, DtMailSyncDoneCb on_sync_done, void *user_data)
{
    if (!app || g_sync_in_flight)
        return;
    int n = dtmail_accounts_count();
    if (n <= 0)
        return;

    AsyncSyncJob *job = (AsyncSyncJob *)calloc(1, sizeof(AsyncSyncJob));
    if (!job) return;
    if (pipe(job->pipe_fd) != 0) {
        free(job);
        return;
    }
    job->cb = on_sync_done;
    job->ud = user_data;
    job->acct_count = n;
    for (int i = 0; i < n; i++) {
        const DtMailAccount *a = dtmail_accounts_get(i);
        job->accts[i] = *a;
        const char *kind = (a->auth_mode == DTMAIL_AUTH_OAUTH2) ? "oauth_access" : "password";
        (void)dtmail_keyring_lookup(a->id, kind, job->secrets[i], sizeof(job->secrets[i]));
        (void)dtmail_account_folder_path(a->id, "INBOX", job->mbox_paths[i], sizeof(job->mbox_paths[i]));
    }

    g_sync_in_flight = 1;
    set_dlg_status("Syncing mail in background...");
    job->input_id = XtAppAddInput(app, job->pipe_fd[0], (XtPointer)XtInputReadMask,
                                  sync_pipe_ready_cb, job);

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, sync_worker_fn, job) != 0) {
        XtRemoveInput(job->input_id);
        close(job->pipe_fd[0]);
        close(job->pipe_fd[1]);
        free(job);
        g_sync_in_flight = 0;
    }
    pthread_attr_destroy(&attr);
}

static void
rebuild_acct_list(void)
{
    if (!g_acct_list) return;
    XmListDeleteAllItems(g_acct_list);
    int n = dtmail_accounts_count();
    for (int i = 0; i < n; i++) {
        const DtMailAccount *a = dtmail_accounts_get(i);
        char line[768];
        if (a->auth_mode == DTMAIL_AUTH_SENDMAIL_LOCAL) {
            snprintf(line, sizeof(line), "[Local] %s <%s>", a->name, a->email);
        } else {
            snprintf(line, sizeof(line), "[%s] %s <%s>  (%s:%d / %s:%d)",
                     a->auth_mode == DTMAIL_AUTH_OAUTH2 ? "OAuth2" : "IMAP",
                     a->name, a->email,
                     a->imap_host, a->imap_port,
                     a->smtp_host, a->smtp_port);
        }
        XmString xs = XmStringCreateLocalized(line);
        XmListAddItemUnselected(g_acct_list, xs, 0);
        XmStringFree(xs);
    }
}

static void
populate_form_from_account(const DtMailAccount *a)
{
    if (!a) return;
    char pbuf[16];
    XmTextFieldSetString(g_tf_name, (char *)a->name);
    XmTextFieldSetString(g_tf_email, (char *)a->email);
    XmTextFieldSetString(g_tf_user, (char *)a->username);
    XmTextFieldSetString(g_tf_ihost, (char *)a->imap_host);
    snprintf(pbuf, sizeof(pbuf), "%d", a->imap_port);
    XmTextFieldSetString(g_tf_iport, pbuf);
    XmTextFieldSetString(g_tf_shost, (char *)a->smtp_host);
    snprintf(pbuf, sizeof(pbuf), "%d", a->smtp_port);
    XmTextFieldSetString(g_tf_sport, pbuf);
    XmToggleButtonSetState(g_rb_auth_pass, a->auth_mode == DTMAIL_AUTH_PASSWORD, False);
    XmToggleButtonSetState(g_rb_auth_oauth, a->auth_mode == DTMAIL_AUTH_OAUTH2, False);
    XmToggleButtonSetState(g_rb_auth_local, a->auth_mode == DTMAIL_AUTH_SENDMAIL_LOCAL, False);
}

static void
read_account_from_form(DtMailAccount *a, char *pass_out, size_t pass_len)
{
    memset(a, 0, sizeof(*a));
    char *s_name  = XmTextFieldGetString(g_tf_name);
    char *s_email = XmTextFieldGetString(g_tf_email);
    char *s_user  = XmTextFieldGetString(g_tf_user);
    char *s_pass  = XmTextFieldGetString(g_tf_pass);
    char *s_ihost = XmTextFieldGetString(g_tf_ihost);
    char *s_iport = XmTextFieldGetString(g_tf_iport);
    char *s_shost = XmTextFieldGetString(g_tf_shost);
    char *s_sport = XmTextFieldGetString(g_tf_sport);

    snprintf(a->name, sizeof(a->name), "%s", (s_name && s_name[0]) ? s_name : "Mail Account");
    snprintf(a->email, sizeof(a->email), "%s", s_email ? s_email : "");
    snprintf(a->username, sizeof(a->username), "%s", (s_user && s_user[0]) ? s_user : a->email);
    dtmail_sanitize_path_component(a->email[0] ? a->email : a->name, a->id, sizeof(a->id));
    snprintf(a->imap_host, sizeof(a->imap_host), "%s", s_ihost ? s_ihost : "");
    a->imap_port = (s_iport && atoi(s_iport) > 0) ? atoi(s_iport) : 993;
    a->imap_tls = (a->imap_port == 143) ? DTMAIL_TLS_STARTTLS : DTMAIL_TLS_IMPLICIT;
    snprintf(a->smtp_host, sizeof(a->smtp_host), "%s", s_shost ? s_shost : "");
    a->smtp_port = (s_sport && atoi(s_sport) > 0) ? atoi(s_sport) : 587;
    a->smtp_tls = (a->smtp_port == 465) ? DTMAIL_TLS_IMPLICIT : DTMAIL_TLS_STARTTLS;
    if (XmToggleButtonGetState(g_rb_auth_oauth))
        a->auth_mode = DTMAIL_AUTH_OAUTH2;
    else if (XmToggleButtonGetState(g_rb_auth_local))
        a->auth_mode = DTMAIL_AUTH_SENDMAIL_LOCAL;
    else
        a->auth_mode = DTMAIL_AUTH_PASSWORD;
    a->enabled = 1;
    a->sync_interval_sec = 300;

    if (pass_out && pass_len > 0)
        snprintf(pass_out, pass_len, "%s", s_pass ? s_pass : "");

    if (s_name) XtFree(s_name);
    if (s_email) XtFree(s_email);
    if (s_user) XtFree(s_user);
    if (s_pass) XtFree(s_pass);
    if (s_ihost) XtFree(s_ihost);
    if (s_iport) XtFree(s_iport);
    if (s_shost) XtFree(s_shost);
    if (s_sport) XtFree(s_sport);
}

static void
preset_btn_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)call_data;
    const char *preset = (const char *)cd;
    DtMailAccount a;
    char dummy[8];
    read_account_from_form(&a, dummy, sizeof(dummy));
    dtmail_account_apply_preset(&a, preset);
    populate_form_from_account(&a);
    char msg[128];
    snprintf(msg, sizeof(msg), "Applied %s preset. Enter your email & password (or OAuth2).", preset);
    set_dlg_status(msg);
}

static void
acct_list_select_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)cd;
    XmListCallbackStruct *cbs = (XmListCallbackStruct *)call_data;
    if (!cbs) return;
    const DtMailAccount *a = dtmail_accounts_get(cbs->item_position - 1);
    if (a) populate_form_from_account(a);
}

static void
save_acct_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)cd; (void)call_data;
    DtMailAccount a;
    char pass[256] = "";
    read_account_from_form(&a, pass, sizeof(pass));
    if (!a.email[0]) {
        set_dlg_status("Please enter your email address.");
        return;
    }
    if (dtmail_accounts_upsert(&a) < 0) {
        set_dlg_status("Account limit reached.");
        return;
    }
    char path[1024];
    if (dtmail_accounts_default_path(path, sizeof(path)) == 0)
        (void)dtmail_accounts_save_file(path);
    if (pass[0]) {
        const char *kind = (a.auth_mode == DTMAIL_AUTH_OAUTH2) ? "oauth_access" : "password";
        (void)dtmail_keyring_store(a.id, kind, pass);
        XmTextFieldSetString(g_tf_pass, (char *)"");
    }
    rebuild_acct_list();
    set_dlg_status("Account saved (credentials stored in desktop keyring).");
}

static void
test_conn_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)cd; (void)call_data;
    DtMailAccount a;
    char pass[256] = "";
    read_account_from_form(&a, pass, sizeof(pass));
    if (!pass[0]) {
        const char *kind = (a.auth_mode == DTMAIL_AUTH_OAUTH2) ? "oauth_access" : "password";
        (void)dtmail_keyring_lookup(a.id, kind, pass, sizeof(pass));
    }
    char err[256] = "";
    if (dtmail_imap_test_connection(&a, pass, err, sizeof(err)) == 0) {
        set_dlg_status("Connection succeeded! IMAP TLS login verified.");
    } else {
        set_dlg_status(err[0] ? err : "IMAP connection test failed.");
    }
}

typedef struct {
    DtMailAccount acct;
    DtMailPkcePair pkce;
    int listen_fd;
    int port;
} OAuth2ThreadCtx;

static void *
oauth2_worker_thread(void *arg)
{
    OAuth2ThreadCtx *ctx = (OAuth2ThreadCtx *)arg;
    char code[512] = "";
    if (dtmail_oauth2_loopback_wait_code(ctx->listen_fd, ctx->pkce.state, 180,
                                         code, sizeof(code)) == 0) {
        char redir[128];
        snprintf(redir, sizeof(redir), "http://127.0.0.1:%d/callback", ctx->port);
        DtMailOAuthTokens tok;
        char err[256] = "";
        if (dtmail_oauth2_exchange_code(&ctx->acct, code, ctx->pkce.code_verifier,
                                        redir, &tok, err, sizeof(err)) == 0) {
            (void)dtmail_keyring_store(ctx->acct.id, "oauth_access", tok.access_token);
            if (tok.refresh_token[0]) {
                (void)dtmail_keyring_store(ctx->acct.id, "oauth_refresh", tok.refresh_token);
            }
        }
    }
    close(ctx->listen_fd);
    free(ctx);
    return NULL;
}

static void
oauth2_signin_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)cd; (void)call_data;
    DtMailAccount a;
    char pass[256] = "";
    read_account_from_form(&a, pass, sizeof(pass));
    if (!a.email[0]) {
        set_dlg_status("Enter your email address before starting OAuth2 sign-in.");
        return;
    }
    a.auth_mode = DTMAIL_AUTH_OAUTH2;
    (void)dtmail_accounts_upsert(&a);

    DtMailPkcePair pkce;
    int port = 0;
    if (dtmail_oauth2_generate_pkce(&pkce) != 0) {
        set_dlg_status("Failed to initialize PKCE challenge.");
        return;
    }
    int lfd = dtmail_oauth2_loopback_listen(&port);
    if (lfd < 0) {
        set_dlg_status("Could not open local OAuth2 loopback listener.");
        return;
    }
    char auth_url[1024] = "";
    if (dtmail_oauth2_build_auth_url(&a, &pkce, port, auth_url, sizeof(auth_url)) != 0) {
        close(lfd);
        set_dlg_status("Failed to build OAuth2 authorization URL.");
        return;
    }
    OAuth2ThreadCtx *ctx = (OAuth2ThreadCtx *)calloc(1, sizeof(OAuth2ThreadCtx));
    if (!ctx) { close(lfd); return; }
    ctx->acct = a;
    ctx->pkce = pkce;
    ctx->listen_fd = lfd;
    ctx->port = port;
    pthread_t th;
    if (pthread_create(&th, NULL, oauth2_worker_thread, ctx) == 0) {
        pthread_detach(th);
        (void)dtmail_oauth2_open_browser(auth_url);
        set_dlg_status("Opened browser for OAuth2 sign-in (waiting on loopback callback)...");
    } else {
        close(lfd);
        free(ctx);
        set_dlg_status("Could not start OAuth2 sign-in thread.");
    }
}

static void
sync_now_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)cd; (void)call_data;
    dtmail_start_async_sync(XtWidgetToApplicationContext(w), g_sync_cb, g_sync_ud);
}

static void
remove_acct_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)cd; (void)call_data;
    int *pos = NULL;
    int cnt = 0;
    if (XmListGetSelectedPos(g_acct_list, &pos, &cnt) && cnt > 0 && pos) {
        int idx = pos[0] - 1;
        XtFree((char *)pos);
        const DtMailAccount *a = dtmail_accounts_get(idx);
        if (a) {
            (void)dtmail_keyring_clear(a->id, "password");
            (void)dtmail_keyring_clear(a->id, "oauth_access");
            (void)dtmail_keyring_clear(a->id, "oauth_refresh");
        }
        dtmail_accounts_remove(idx);
        char path[1024];
        if (dtmail_accounts_default_path(path, sizeof(path)) == 0)
            (void)dtmail_accounts_save_file(path);
        rebuild_acct_list();
        set_dlg_status("Account removed.");
    }
}

static void
close_acct_cb(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)cd; (void)call_data;
    if (g_acct_shell) XtPopdown(g_acct_shell);
}

static Widget
make_labeled_row(Widget parent, Widget top_w, const char *label, int cols, Widget *out_tf)
{
    XmString ls = XmStringCreateLocalized((char *)label);
    Widget lbl = XtVaCreateManagedWidget("acctLbl", xmLabelGadgetClass, parent,
        XmNlabelString, ls,
        XmNtopAttachment, top_w ? XmATTACH_WIDGET : XmATTACH_FORM,
        XmNtopWidget, top_w,
        XmNtopOffset, 6,
        XmNleftAttachment, XmATTACH_FORM,
        XmNwidth, 110,
        XmNalignment, XmALIGNMENT_END,
        NULL);
    XmStringFree(ls);

    Widget tf = XtVaCreateManagedWidget("acctTf", xmTextFieldWidgetClass, parent,
        XmNcolumns, cols,
        XmNtopAttachment, top_w ? XmATTACH_WIDGET : XmATTACH_FORM,
        XmNtopWidget, top_w,
        XmNtopOffset, 4,
        XmNleftAttachment, XmATTACH_WIDGET,
        XmNleftWidget, lbl,
        XmNleftOffset, 6,
        XmNrightAttachment, XmATTACH_FORM,
        NULL);
    if (out_tf) *out_tf = tf;
    return tf;
}

void
dtmail_show_accounts_dialog(Widget parent, DtMailSyncDoneCb on_sync_done, void *user_data)
{
    if (!parent) return;
    g_sync_cb = on_sync_done;
    g_sync_ud = user_data;

    if (!g_acct_shell) {
        g_acct_shell = XtVaCreatePopupShell("accountsSetupDialog",
            xmDialogShellWidgetClass, parent,
            XmNtitle, "Mailer : Accounts & Servers",
            XmNdeleteResponse, XmUNMAP,
            NULL);

        Widget form = XtVaCreateWidget("acctForm", xmFormWidgetClass, g_acct_shell,
            XmNmarginWidth, 12,
            XmNmarginHeight, 12,
            XmNwidth, 620,
            NULL);

        Arg al[6];
        int ac = 0;
        XtSetArg(al[ac], XmNvisibleItemCount, 4); ac++;
        XtSetArg(al[ac], XmNselectionPolicy, XmSINGLE_SELECT); ac++;
        g_acct_list = XmCreateScrolledList(form, "acctList", al, ac);
        XtVaSetValues(XtParent(g_acct_list),
            XmNtopAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_FORM,
            XmNrightAttachment, XmATTACH_FORM,
            NULL);
        XtAddCallback(g_acct_list, XmNsingleSelectionCallback, acct_list_select_cb, NULL);
        XtManageChild(g_acct_list);

        Widget preset_rc = XtVaCreateManagedWidget("presetRC", xmRowColumnWidgetClass, form,
            XmNorientation, XmHORIZONTAL,
            XmNspacing, 4,
            XmNtopAttachment, XmATTACH_WIDGET,
            XmNtopWidget, XtParent(g_acct_list),
            XmNtopOffset, 8,
            XmNleftAttachment, XmATTACH_FORM,
            XmNrightAttachment, XmATTACH_FORM,
            NULL);
        XmString pl = XmStringCreateLocalized((char *)"Preset:");
        XtVaCreateManagedWidget("presetLbl", xmLabelGadgetClass, preset_rc, XmNlabelString, pl, NULL);
        XmStringFree(pl);

        static const struct { const char *lbl; const char *key; } presets[] = {
            { "Gmail", "gmail" }, { "iCloud", "icloud" }, { "Fastmail", "fastmail" },
            { "M365", "m365" }, { "Custom", "custom" }, { "Local", "local" }
        };
        for (int i = 0; i < 6; i++) {
            XmString ps = XmStringCreateLocalized((char *)presets[i].lbl);
            Widget pb = XtVaCreateManagedWidget("presetBtn", xmPushButtonWidgetClass, preset_rc,
                XmNlabelString, ps, NULL);
            XtAddCallback(pb, XmNactivateCallback, preset_btn_cb, (XtPointer)presets[i].key);
            XmStringFree(ps);
        }

        Widget r1 = make_labeled_row(form, preset_rc, "Account Name:", 32, &g_tf_name);
        Widget r2 = make_labeled_row(form, r1, "Email:", 32, &g_tf_email);
        Widget r3 = make_labeled_row(form, r2, "Username:", 32, &g_tf_user);
        Widget r4 = make_labeled_row(form, r3, "Password:", 32, &g_tf_pass);
        Widget r5 = make_labeled_row(form, r4, "IMAP Host:", 26, &g_tf_ihost);
        Widget r6 = make_labeled_row(form, r5, "IMAP Port:", 8, &g_tf_iport);
        Widget r7 = make_labeled_row(form, r6, "SMTP Host:", 26, &g_tf_shost);
        Widget r8 = make_labeled_row(form, r7, "SMTP Port:", 8, &g_tf_sport);

        Widget auth_rc = XtVaCreateManagedWidget("authRC", xmRowColumnWidgetClass, form,
            XmNorientation, XmHORIZONTAL,
            XmNradioBehavior, True,
            XmNspacing, 10,
            XmNtopAttachment, XmATTACH_WIDGET,
            XmNtopWidget, r8,
            XmNtopOffset, 8,
            XmNleftAttachment, XmATTACH_FORM,
            XmNrightAttachment, XmATTACH_FORM,
            NULL);
        XmString a1 = XmStringCreateLocalized((char *)"Password / App Password");
        g_rb_auth_pass = XtVaCreateManagedWidget("authPass", xmToggleButtonWidgetClass, auth_rc,
            XmNlabelString, a1, XmNset, True, NULL);
        XmStringFree(a1);
        XmString a2 = XmStringCreateLocalized((char *)"OAuth2 (XOAUTH2)");
        g_rb_auth_oauth = XtVaCreateManagedWidget("authOAuth", xmToggleButtonWidgetClass, auth_rc,
            XmNlabelString, a2, XmNset, False, NULL);
        XmStringFree(a2);
        XmString a3 = XmStringCreateLocalized((char *)"Local sendmail");
        g_rb_auth_local = XtVaCreateManagedWidget("authLocal", xmToggleButtonWidgetClass, auth_rc,
            XmNlabelString, a3, XmNset, False, NULL);
        XmStringFree(a3);

        XmString st_s = XmStringCreateLocalized((char *)"Select a preset or enter IMAP/SMTP details.");
        g_lbl_status = XtVaCreateManagedWidget("acctStatus", xmLabelGadgetClass, form,
            XmNlabelString, st_s,
            XmNtopAttachment, XmATTACH_WIDGET,
            XmNtopWidget, auth_rc,
            XmNtopOffset, 8,
            XmNleftAttachment, XmATTACH_FORM,
            XmNrightAttachment, XmATTACH_FORM,
            XmNalignment, XmALIGNMENT_BEGINNING,
            NULL);
        XmStringFree(st_s);

        Widget sep = XtVaCreateManagedWidget("acctSep", xmSeparatorWidgetClass, form,
            XmNtopAttachment, XmATTACH_WIDGET,
            XmNtopWidget, g_lbl_status,
            XmNtopOffset, 6,
            XmNleftAttachment, XmATTACH_FORM,
            XmNrightAttachment, XmATTACH_FORM,
            NULL);

        Widget btn_rc = XtVaCreateManagedWidget("acctBtns", xmRowColumnWidgetClass, form,
            XmNorientation, XmHORIZONTAL,
            XmNspacing, 8,
            XmNtopAttachment, XmATTACH_WIDGET,
            XmNtopWidget, sep,
            XmNtopOffset, 8,
            XmNbottomAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_FORM,
            XmNrightAttachment, XmATTACH_FORM,
            NULL);

        static const struct { const char *lbl; XtCallbackProc cb; } btns[] = {
            { "Save Account", save_acct_cb },
            { "Sign In (OAuth2)", oauth2_signin_cb },
            { "Test Connection", test_conn_cb },
            { "Sync Now", sync_now_cb },
            { "Remove", remove_acct_cb },
            { "Close", close_acct_cb }
        };
        for (int i = 0; i < 6; i++) {
            XmString bs = XmStringCreateLocalized((char *)btns[i].lbl);
            Widget b = XtVaCreateManagedWidget("acctBtn", xmPushButtonWidgetClass, btn_rc,
                XmNlabelString, bs, NULL);
            XtAddCallback(b, XmNactivateCallback, btns[i].cb, NULL);
            XmStringFree(bs);
        }

        XtManageChild(form);
    }

    rebuild_acct_list();
    if (dtmail_accounts_count() > 0)
        populate_form_from_account(dtmail_accounts_get(0));
    XtPopup(g_acct_shell, XtGrabNone);
}
