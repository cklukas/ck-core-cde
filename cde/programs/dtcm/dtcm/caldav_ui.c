/*
 * CDE - Common Desktop Environment
 *
 * Motif UI dialog for CalDAV (RFC 4791) encrypted calendar sync in dtcm.
 * Supports non-blocking background HTTPS sync (pthread + XtAppAddInput),
 * persistent (UID, ETag) two-way deduplication, optional desktop keyring
 * (secret-tool / gnome-keyring), and periodic auto-sync via XtAppAddTimeOut.
 */

#include <EUSCompat.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

typedef enum {
    CALDAV_OP_TEST = 0,
    CALDAV_OP_PULL,
    CALDAV_OP_PUSH,
    CALDAV_OP_SYNC
} CaldavOp;

typedef struct {
    Calendar *cal;
    CaldavOp op;
    DtcmCaldavConfig cfg;
    char push_ics_path[512];   /* prepared on Xt thread for PUSH/SYNC */
    char pull_ics_path[512];   /* written by worker for PULL/SYNC */
    char result_msg[1024];
    int pipe_write_fd;
} CaldavJob;

static Widget g_caldav_dialog = nullptr;
static Widget g_url_field = nullptr;
static Widget g_user_field = nullptr;
static Widget g_pass_field = nullptr;
static Widget g_autosync_field = nullptr;
static Widget g_tls_toggle = nullptr;
static Widget g_caldav_status = nullptr;
static Widget g_btn_test = nullptr;
static Widget g_btn_sync = nullptr;
static Widget g_btn_pull = nullptr;
static Widget g_btn_push = nullptr;
static Widget g_btn_save = nullptr;

static char g_password_buf[256];
static int g_pass_updating = 0;
static int g_job_busy = 0;
static int g_wakeup_pipe[2] = { -1, -1 };
static XtInputId g_wakeup_input = 0;
static XtIntervalId g_autosync_timer = 0;
static CaldavJob *g_active_job = nullptr;
static Calendar *g_autosync_cal = nullptr;

static void start_async_op(Calendar *c, CaldavOp op, const DtcmCaldavConfig *cfg_override);

static void
set_status(const char *msg)
{
    if (g_caldav_status && msg)
        XmTextSetString(g_caldav_status, (char *)msg);
}

static void
set_buttons_sensitive(Boolean sens)
{
    if (g_btn_test) XtSetSensitive(g_btn_test, sens);
    if (g_btn_sync) XtSetSensitive(g_btn_sync, sens);
    if (g_btn_pull) XtSetSensitive(g_btn_pull, sens);
    if (g_btn_push) XtSetSensitive(g_btn_push, sens);
    if (g_btn_save) XtSetSensitive(g_btn_save, sens);
}

static void
set_password_masked(const char *plain)
{
    char stars[256];
    size_t len = plain ? strlen(plain) : 0;
    if (len >= sizeof(g_password_buf))
        len = sizeof(g_password_buf) - 1;
    memcpy(g_password_buf, plain ? plain : "", len);
    g_password_buf[len] = '\0';
    memset(stars, '*', len);
    stars[len] = '\0';
    if (g_pass_field) {
        g_pass_updating = 1;
        XmTextFieldSetString(g_pass_field, stars);
        g_pass_updating = 0;
    }
}

static void
read_form_config(DtcmCaldavConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    char *u = g_url_field ? XmTextFieldGetString(g_url_field) : nullptr;
    char *usr = g_user_field ? XmTextFieldGetString(g_user_field) : nullptr;
    char *mins = g_autosync_field ? XmTextFieldGetString(g_autosync_field) : nullptr;

    if (u) {
        snprintf(cfg->url, sizeof(cfg->url), "%s", u);
        XtFree(u);
    }
    if (usr) {
        snprintf(cfg->username, sizeof(cfg->username), "%s", usr);
        XtFree(usr);
    }
    if (mins) {
        int m = atoi(mins);
        cfg->auto_sync_mins = (m > 0 && m <= 1440) ? m : 0;
        XtFree(mins);
    }
    snprintf(cfg->password, sizeof(cfg->password), "%s", g_password_buf);
    cfg->verify_tls = (g_tls_toggle && XmToggleButtonGadgetGetState(g_tls_toggle)) ? 1 : 0;
}

static void
password_modify_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    if (g_pass_updating)
        return;

    XmTextVerifyCallbackStruct *cbs = (XmTextVerifyCallbackStruct *)call_data;
    size_t cur_len = strlen(g_password_buf);
    size_t start = (size_t)cbs->startPos;
    size_t end = (size_t)cbs->endPos;

    if (start > cur_len) start = cur_len;
    if (end > cur_len) end = cur_len;

    if (cbs->text == nullptr || cbs->text->length == 0) {
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
autosync_timer_cb(XtPointer client_data, XtIntervalId *id)
{
    (void)id;
    Calendar *c = (Calendar *)client_data;
    g_autosync_timer = 0;

    DtcmCaldavConfig cfg;
    if (dtcm_caldav_load_profile(&cfg) != 0 || cfg.auto_sync_mins <= 0 || !cfg.url[0])
        return;

    if (cfg.username[0]) {
        if (g_password_buf[0])
            snprintf(cfg.password, sizeof(cfg.password), "%s", g_password_buf);
        else
            (void)dtcm_caldav_keyring_lookup(cfg.url, cfg.username,
                                             cfg.password, sizeof(cfg.password));
    }

    if (!g_job_busy)
        start_async_op(c, CALDAV_OP_SYNC, &cfg);

    if (c && c->xcontext && c->xcontext->app) {
        unsigned long ms = (unsigned long)cfg.auto_sync_mins * 60UL * 1000UL;
        g_autosync_timer = XtAppAddTimeOut(c->xcontext->app, ms, autosync_timer_cb, (XtPointer)c);
    }
}

static void
reschedule_autosync(Calendar *c, int mins)
{
    if (g_autosync_timer) {
        XtRemoveTimeOut(g_autosync_timer);
        g_autosync_timer = 0;
    }
    g_autosync_cal = c;
    if (c && c->xcontext && c->xcontext->app && mins > 0 && mins <= 1440) {
        unsigned long ms = (unsigned long)mins * 60UL * 1000UL;
        g_autosync_timer = XtAppAddTimeOut(c->xcontext->app, ms, autosync_timer_cb, (XtPointer)c);
    }
}

static void *
caldav_worker_thread(void *arg)
{
    CaldavJob *job = (CaldavJob *)arg;
    char err[512] = "";
    DtcmCaldavSyncState st;
    dtcm_caldav_state_load(&st);

    if (job->op == CALDAV_OP_TEST) {
        DtcmCaldavItem *items = nullptr;
        int count = 0;
        if (dtcm_caldav_propfind(&job->cfg, &items, &count, err, sizeof(err)) != 0) {
            snprintf(job->result_msg, sizeof(job->result_msg),
                     "Connection test failed:\n%s", err);
        } else {
            snprintf(job->result_msg, sizeof(job->result_msg),
                     "Connection OK (%s).\nDiscovered %d iCalendar (.ics) item(s) in collection.",
                     job->cfg.verify_tls ? "TLS verified" : "TLS verification disabled",
                     count);
            dtcm_caldav_free_items(items);
        }
    } else {
        int pulled_new = 0;
        int skipped_unchanged = 0;
        int pull_errors = 0;
        int pushed_new = 0;
        int skipped_synced = 0;
        int push_errors = 0;

        /* Pull phase (for PULL and SYNC) */
        if (job->op == CALDAV_OP_PULL || job->op == CALDAV_OP_SYNC) {
            DtcmCaldavItem *items = nullptr;
            int item_count = 0;
            if (dtcm_caldav_propfind(&job->cfg, &items, &item_count, err, sizeof(err)) != 0) {
                snprintf(job->result_msg, sizeof(job->result_msg),
                         "CalDAV PROPFIND failed:\n%s", err);
                goto done;
            }

            DtcmIcsEvent *accum = nullptr;
            int accum_count = 0;
            int accum_cap = 0;

            for (int i = 0; i < item_count; i++) {
                const DtcmCaldavStateEntry *prev =
                    dtcm_caldav_state_find_href(&st, job->cfg.url, items[i].href);
                if (prev && items[i].etag[0] && strcmp(prev->etag, items[i].etag) == 0) {
                    skipped_unchanged++;
                    continue;
                }

                DtcmIcsEvent *evs = nullptr;
                int ev_count = 0;
                if (dtcm_caldav_get_events(&job->cfg, items[i].href,
                                           &evs, &ev_count, err, sizeof(err)) != 0) {
                    pull_errors++;
                    continue;
                }
                for (int j = 0; j < ev_count; j++) {
                    if (evs[j].uid[0] &&
                        dtcm_caldav_state_find_uid(&st, job->cfg.url, evs[j].uid) != nullptr &&
                        prev == nullptr) {
                        skipped_unchanged++;
                        continue;
                    }
                    if (accum_count == accum_cap) {
                        int ncap = accum_cap ? accum_cap * 2 : 16;
                        DtcmIcsEvent *na = (DtcmIcsEvent *)realloc(
                            accum, (size_t)ncap * sizeof(DtcmIcsEvent));
                        if (!na) break;
                        accum = na;
                        accum_cap = ncap;
                    }
                    accum[accum_count++] = evs[j];
                    pulled_new++;
                    (void)dtcm_caldav_state_upsert(&st, job->cfg.url,
                                                   items[i].href, evs[j].uid, items[i].etag);
                }
                dtcm_ics_free_events(evs);
            }
            dtcm_caldav_free_items(items);

            if (accum_count > 0) {
                const char *td = getenv("TMPDIR");
                if (!td || !td[0]) td = "/tmp";
                snprintf(job->pull_ics_path, sizeof(job->pull_ics_path),
                         "%s/dtcm-caldav-pull-XXXXXX", td);
                int fd = mkstemp(job->pull_ics_path);
                if (fd >= 0) {
                    (void)fchmod(fd, 0600);
                    close(fd);
                    if (dtcm_ics_write_file(job->pull_ics_path, accum, accum_count) != 0) {
                        (void)unlink(job->pull_ics_path);
                        job->pull_ics_path[0] = '\0';
                    }
                } else {
                    job->pull_ics_path[0] = '\0';
                }
            }
            free(accum);
        }

        /* Push phase (for PUSH and SYNC) */
        if ((job->op == CALDAV_OP_PUSH || job->op == CALDAV_OP_SYNC) &&
            job->push_ics_path[0]) {
            DtcmIcsEvent *evs = nullptr;
            int ev_count = 0;
            if (dtcm_ics_parse_file(job->push_ics_path, &evs, &ev_count) == 0) {
                for (int i = 0; i < ev_count; i++) {
                    if (job->op == CALDAV_OP_SYNC && evs[i].uid[0] &&
                        dtcm_caldav_state_find_uid(&st, job->cfg.url, evs[i].uid) != nullptr) {
                        skipped_synced++;
                        continue;
                    }
                    char href[1024] = "";
                    char etag[256] = "";
                    if (dtcm_caldav_put_event_ex(&job->cfg, &evs[i],
                                                 href, sizeof(href),
                                                 etag, sizeof(etag),
                                                 err, sizeof(err)) == 0) {
                        pushed_new++;
                        (void)dtcm_caldav_state_upsert(&st, job->cfg.url,
                                                       href, evs[i].uid, etag);
                    } else {
                        push_errors++;
                    }
                }
                dtcm_ics_free_events(evs);
            }
        }

        (void)dtcm_caldav_state_save(&st);

        if (job->op == CALDAV_OP_PULL) {
            snprintf(job->result_msg, sizeof(job->result_msg),
                     "CalDAV Pull complete: %d new/updated, %d unchanged skipped (%d error(s)).",
                     pulled_new, skipped_unchanged, pull_errors);
        } else if (job->op == CALDAV_OP_PUSH) {
            snprintf(job->result_msg, sizeof(job->result_msg),
                     "CalDAV Push complete: %d uploaded (%d error(s)).%s%s",
                     pushed_new, push_errors,
                     push_errors ? "\nLast error: " : "",
                     push_errors ? err : "");
        } else {
            snprintf(job->result_msg, sizeof(job->result_msg),
                     "Two-Way Sync complete: pulled %d new (%d unchanged), pushed %d new (%d already synced), %d error(s).",
                     pulled_new, skipped_unchanged, pushed_new, skipped_synced,
                     pull_errors + push_errors);
        }
    }

done:
    dtcm_caldav_state_free(&st);
    if (job->push_ics_path[0]) {
        (void)unlink(job->push_ics_path);
        job->push_ics_path[0] = '\0';
    }
    char b = 1;
    (void)write(job->pipe_write_fd, &b, 1);
    return nullptr;
}

static void
wakeup_input_cb(XtPointer client_data, int *source, XtInputId *id)
{
    (void)client_data;
    (void)id;
    char drain[16];
    while (read(*source, drain, sizeof(drain)) > 0) {
        /* drain */
    }

    CaldavJob *job = g_active_job;
    g_active_job = nullptr;
    g_job_busy = 0;

    if (job) {
        if (job->pull_ics_path[0]) {
            int imported = 0;
            (void)dtcm_import_ics_file(job->cal, job->pull_ics_path, &imported);
            (void)unlink(job->pull_ics_path);
        }
        set_status(job->result_msg);
        free(job);
    }
    set_buttons_sensitive(True);
}

static int
ensure_wakeup_pipe(Calendar *c)
{
    if (g_wakeup_pipe[0] >= 0)
        return 0;
    if (!c || !c->xcontext || !c->xcontext->app)
        return -1;
    if (pipe(g_wakeup_pipe) != 0)
        return -1;
    (void)fcntl(g_wakeup_pipe[0], F_SETFL, O_NONBLOCK);
    (void)fcntl(g_wakeup_pipe[1], F_SETFL, O_NONBLOCK);
    g_wakeup_input = XtAppAddInput(c->xcontext->app, g_wakeup_pipe[0],
                                   (XtPointer)XtInputReadMask,
                                   wakeup_input_cb, nullptr);
    return 0;
}

static void
start_async_op(Calendar *c, CaldavOp op, const DtcmCaldavConfig *cfg_override)
{
    if (g_job_busy) {
        set_status("A CalDAV operation is already running...");
        return;
    }

    CaldavJob *job = (CaldavJob *)calloc(1, sizeof(CaldavJob));
    if (!job)
        return;

    job->cal = c;
    job->op = op;
    if (cfg_override)
        job->cfg = *cfg_override;
    else
        read_form_config(&job->cfg);

    char err[256] = "";
    if (dtcm_caldav_validate_url(job->cfg.url, err, sizeof(err)) != 0) {
        set_status(err);
        free(job);
        return;
    }

    if (ensure_wakeup_pipe(c) != 0) {
        set_status("Failed to initialize background CalDAV worker pipe.");
        free(job);
        return;
    }

    /* Export local calendar on the Xt thread before spawning worker */
    if (op == CALDAV_OP_PUSH || op == CALDAV_OP_SYNC) {
        const char *td = getenv("TMPDIR");
        if (!td || !td[0]) td = "/tmp";
        snprintf(job->push_ics_path, sizeof(job->push_ics_path),
                 "%s/dtcm-caldav-push-XXXXXX", td);
        int fd = mkstemp(job->push_ics_path);
        if (fd >= 0) {
            (void)fchmod(fd, 0600);
            close(fd);
            int exported = 0;
            if (dtcm_export_ics_file(c, job->push_ics_path, &exported) != 0) {
                (void)unlink(job->push_ics_path);
                job->push_ics_path[0] = '\0';
            }
        } else {
            job->push_ics_path[0] = '\0';
        }
    }

    job->pipe_write_fd = g_wakeup_pipe[1];
    g_active_job = job;
    g_job_busy = 1;
    set_buttons_sensitive(False);
    set_status("Contacting CalDAV server in background...");

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, caldav_worker_thread, job) != 0) {
        pthread_attr_destroy(&attr);
        g_active_job = nullptr;
        g_job_busy = 0;
        set_buttons_sensitive(True);
        if (job->push_ics_path[0]) (void)unlink(job->push_ics_path);
        free(job);
        set_status("Failed to start background CalDAV thread.");
        return;
    }
    pthread_attr_destroy(&attr);
}

static void test_btn_cb(Widget w, XtPointer cd, XtPointer call_data)
{ (void)w; (void)call_data; start_async_op((Calendar *)cd, CALDAV_OP_TEST, nullptr); }

static void sync_btn_cb(Widget w, XtPointer cd, XtPointer call_data)
{ (void)w; (void)call_data; start_async_op((Calendar *)cd, CALDAV_OP_SYNC, nullptr); }

static void pull_btn_cb(Widget w, XtPointer cd, XtPointer call_data)
{ (void)w; (void)call_data; start_async_op((Calendar *)cd, CALDAV_OP_PULL, nullptr); }

static void push_btn_cb(Widget w, XtPointer cd, XtPointer call_data)
{ (void)w; (void)call_data; start_async_op((Calendar *)cd, CALDAV_OP_PUSH, nullptr); }

static void
save_btn_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    Calendar *c = (Calendar *)client_data;
    DtcmCaldavConfig cfg;
    char msg[512];

    read_form_config(&cfg);
    if (dtcm_caldav_save_profile(&cfg) != 0) {
        set_status("Failed to save ~/.dt/dtcm_caldav.conf.");
        return;
    }

    reschedule_autosync(c, cfg.auto_sync_mins);

    int kr_ok = 0;
    if (cfg.username[0] && cfg.password[0])
        kr_ok = (dtcm_caldav_keyring_store(cfg.url, cfg.username, cfg.password) == 0);

    snprintf(msg, sizeof(msg),
             "Saved profile to ~/.dt/dtcm_caldav.conf (mode 0600; auto-sync: %d min).\n%s",
             cfg.auto_sync_mins,
             kr_ok ? "Password stored securely in desktop keyring (secret-tool)."
                   : "Password kept in memory for this session only (keyring unavailable or empty).");
    set_status(msg);
}

static void
close_btn_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
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
        XmString pwd_lbl = XmStringCreateLocalized("Password (keyring / session):");
        XmString auto_lbl = XmStringCreateLocalized("Auto-sync interval (minutes, 0 = off):");
        XmString tls_lbl = XmStringCreateLocalized("Verify TLS server certificates (recommended)");
        XmString test_lbl = XmStringCreateLocalized("Test");
        XmString sync_lbl = XmStringCreateLocalized("Sync");
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
        XtSetArg(al[ac], XmNcolumns, 54); ac++;
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

        /* Auto-sync row */
        ac = 0;
        XtSetArg(al[ac], XmNlabelString, auto_lbl); ac++;
        XtSetArg(al[ac], XmNtopAttachment, XmATTACH_WIDGET); ac++;
        XtSetArg(al[ac], XmNtopWidget, g_pass_field); ac++;
        XtSetArg(al[ac], XmNtopOffset, 8); ac++;
        XtSetArg(al[ac], XmNleftAttachment, XmATTACH_FORM); ac++;
        Widget w_auto_l = XmCreateLabelGadget(g_caldav_dialog, "autoLabel", al, ac);
        XtManageChild(w_auto_l);

        ac = 0;
        XtSetArg(al[ac], XmNcolumns, 6); ac++;
        XtSetArg(al[ac], XmNvalue, "0"); ac++;
        XtSetArg(al[ac], XmNtopAttachment, XmATTACH_WIDGET); ac++;
        XtSetArg(al[ac], XmNtopWidget, g_pass_field); ac++;
        XtSetArg(al[ac], XmNtopOffset, 4); ac++;
        XtSetArg(al[ac], XmNleftAttachment, XmATTACH_WIDGET); ac++;
        XtSetArg(al[ac], XmNleftWidget, w_auto_l); ac++;
        XtSetArg(al[ac], XmNleftOffset, 8); ac++;
        g_autosync_field = XmCreateTextField(g_caldav_dialog, "autoField", al, ac);
        XtManageChild(g_autosync_field);

        /* TLS toggle */
        ac = 0;
        XtSetArg(al[ac], XmNlabelString, tls_lbl); ac++;
        XtSetArg(al[ac], XmNset, True); ac++;
        XtSetArg(al[ac], XmNtopAttachment, XmATTACH_WIDGET); ac++;
        XtSetArg(al[ac], XmNtopWidget, g_autosync_field); ac++;
        XtSetArg(al[ac], XmNtopOffset, 8); ac++;
        XtSetArg(al[ac], XmNleftAttachment, XmATTACH_FORM); ac++;
        g_tls_toggle = XmCreateToggleButtonGadget(g_caldav_dialog, "tlsToggle", al, ac);
        XtManageChild(g_tls_toggle);

        /* Action button row */
        ac = 0;
        XtSetArg(al[ac], XmNorientation, XmHORIZONTAL); ac++;
        XtSetArg(al[ac], XmNspacing, 6); ac++;
        XtSetArg(al[ac], XmNbottomAttachment, XmATTACH_FORM); ac++;
        XtSetArg(al[ac], XmNleftAttachment, XmATTACH_FORM); ac++;
        XtSetArg(al[ac], XmNrightAttachment, XmATTACH_FORM); ac++;
        Widget btn_row = XmCreateRowColumn(g_caldav_dialog, "buttonRow", al, ac);

        g_btn_test = XmCreatePushButton(btn_row, "testBtn", (Arg[]){{XmNlabelString, (XtArgVal)test_lbl}}, 1);
        g_btn_sync = XmCreatePushButton(btn_row, "syncBtn", (Arg[]){{XmNlabelString, (XtArgVal)sync_lbl}}, 1);
        g_btn_pull = XmCreatePushButton(btn_row, "pullBtn", (Arg[]){{XmNlabelString, (XtArgVal)pull_lbl}}, 1);
        g_btn_push = XmCreatePushButton(btn_row, "pushBtn", (Arg[]){{XmNlabelString, (XtArgVal)push_lbl}}, 1);
        g_btn_save = XmCreatePushButton(btn_row, "saveBtn", (Arg[]){{XmNlabelString, (XtArgVal)save_lbl}}, 1);
        Widget b_close = XmCreatePushButton(btn_row, "closeBtn", (Arg[]){{XmNlabelString, (XtArgVal)close_lbl}}, 1);

        XtAddCallback(g_btn_test, XmNactivateCallback, test_btn_cb, (XtPointer)c);
        XtAddCallback(g_btn_sync, XmNactivateCallback, sync_btn_cb, (XtPointer)c);
        XtAddCallback(g_btn_pull, XmNactivateCallback, pull_btn_cb, (XtPointer)c);
        XtAddCallback(g_btn_push, XmNactivateCallback, push_btn_cb, (XtPointer)c);
        XtAddCallback(g_btn_save, XmNactivateCallback, save_btn_cb, (XtPointer)c);
        XtAddCallback(b_close, XmNactivateCallback, close_btn_cb, nullptr);

        XtManageChild(g_btn_test);
        XtManageChild(g_btn_sync);
        XtManageChild(g_btn_pull);
        XtManageChild(g_btn_push);
        XtManageChild(g_btn_save);
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
        XtSetArg(al[ac], XmNcolumns, 58); ac++;
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
        XmStringFree(auto_lbl);
        XmStringFree(tls_lbl);
        XmStringFree(test_lbl);
        XmStringFree(sync_lbl);
        XmStringFree(pull_lbl);
        XmStringFree(push_lbl);
        XmStringFree(save_lbl);
        XmStringFree(close_lbl);
    }

    if (dtcm_caldav_load_profile(&cfg) == 0) {
        char mbuf[16];
        snprintf(mbuf, sizeof(mbuf), "%d", cfg.auto_sync_mins);
        XmTextFieldSetString(g_url_field, cfg.url);
        XmTextFieldSetString(g_user_field, cfg.username);
        XmTextFieldSetString(g_autosync_field, mbuf);
        XmToggleButtonGadgetSetState(g_tls_toggle, cfg.verify_tls ? True : False, False);
        if (!g_password_buf[0] && cfg.url[0] && cfg.username[0]) {
            char kr_pass[256];
            if (dtcm_caldav_keyring_lookup(cfg.url, cfg.username, kr_pass, sizeof(kr_pass)) == 0)
                set_password_masked(kr_pass);
        }
        reschedule_autosync(c, cfg.auto_sync_mins);
    }
    if (!dtcm_caldav_is_supported()) {
        set_status("CalDAV support was not compiled in (libcurl4-openssl-dev not found at configure time).");
    } else {
        set_status("Enter an HTTPS CalDAV collection URL and click Test, Sync (two-way), Pull, or Push.");
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

    /* Arm auto-sync timer at startup if a saved profile has auto_sync_mins > 0 */
    DtcmCaldavConfig cfg;
    if (dtcm_caldav_load_profile(&cfg) == 0 && cfg.auto_sync_mins > 0)
        reschedule_autosync(c, cfg.auto_sync_mins);
}
