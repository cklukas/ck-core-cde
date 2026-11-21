#include <EUSCompat.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xatom.h>
#include <Xm/FileSB.h>
#include <Xm/PushBG.h>
#include <Xm/SeparatoG.h>
#include <Xm/Xm.h>

#include "calendar.h"
#include "cm_tty.h"
#include "ics_csa.h"
#include "ics_io.h"
#include "itip_ui.h"
#include "misc.h"

extern void scrub_attr_list(Dtcm_appointment *appt);

static DtcmIcsEvent *g_offline_events = NULL;
static int g_offline_count = 0;
static int g_offline_cap = 0;

static int g_last_imported = 0;
static int g_last_exported = 0;
static char g_last_ics_path[PATH_MAX];
static char g_status_msg[512];

static Atom g_state_atom = None;
static Atom g_ipc_atom = None;
static Atom g_utf8_atom = None;

static Widget g_import_fsb = NULL;
static Widget g_export_fsb = NULL;

static void json_escape(const char *src, char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0)
        return;
    size_t j = 0;
    if (src) {
        for (size_t i = 0; src[i] != '\0' && j + 2 < dst_len; ++i) {
            unsigned char ch = (unsigned char)src[i];
            if (ch == '"' || ch == '\\') {
                if (j + 3 >= dst_len)
                    break;
                dst[j++] = '\\';
                dst[j++] = (char)ch;
            } else if (ch >= 0x20) {
                dst[j++] = (char)ch;
            }
        }
    }
    dst[j] = '\0';
}

void dtcm_ics_publish_state(Calendar *c)
{
    if (!c || !c->frame || !XtIsRealized(c->frame))
        return;

    Display *dpy = XtDisplay(c->frame);
    Window win = XtWindow(c->frame);
    if (!dpy || !win)
        return;

    if (g_state_atom == None) {
        g_state_atom = XInternAtom(dpy, "_DTCM_STATE", False);
        g_ipc_atom = XInternAtom(dpy, "_DTCM_IPC", False);
        g_utf8_atom = XInternAtom(dpy, "UTF8_STRING", False);
    }

    char esc_path[PATH_MAX + 64];
    char esc_msg[600];
    json_escape(g_last_ics_path, esc_path, sizeof(esc_path));
    json_escape(g_status_msg, esc_msg, sizeof(esc_msg));

    char json[PATH_MAX + 1536];
    int n = snprintf(json, sizeof(json),
                     "{\"loaded\":true,\"cal_connected\":%s,"
                     "\"offline_count\":%d,"
                     "\"last_imported\":%d,\"last_exported\":%d,"
                     "\"last_ics_path\":\"%s\",\"message\":\"%s\"}",
                     c->cal_handle ? "true" : "false",
                     g_offline_count,
                     g_last_imported,
                     g_last_exported,
                     esc_path,
                     esc_msg);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof(json))
        n = (int)sizeof(json) - 1;

    Atom type = (g_utf8_atom != None) ? g_utf8_atom : XA_STRING;
    XChangeProperty(dpy, win, g_state_atom, type, 8, PropModeReplace,
                    (const unsigned char *)json, n);
    XFlush(dpy);
}

static void offline_store_append(const DtcmIcsEvent *ev)
{
    if (!ev)
        return;
    /* Replace by UID if already present */
    if (ev->uid[0] != '\0') {
        for (int i = 0; i < g_offline_count; ++i) {
            if (strcmp(g_offline_events[i].uid, ev->uid) == 0) {
                g_offline_events[i] = *ev;
                return;
            }
        }
    }
    if (g_offline_count >= g_offline_cap) {
        int ncap = (g_offline_cap == 0) ? 16 : g_offline_cap * 2;
        DtcmIcsEvent *next =
            (DtcmIcsEvent *)realloc(g_offline_events, (size_t)ncap * sizeof(DtcmIcsEvent));
        if (!next)
            return;
        g_offline_events = next;
        g_offline_cap = ncap;
    }
    g_offline_events[g_offline_count++] = *ev;
}

static void apply_ics_recurrence_to_appt(const DtcmIcsEvent *ev, Dtcm_appointment *appt, int version)
{
    CSA_sint32 rtype = CSA_X_DT_REPEAT_ONETIME;
    CSA_uint32 rtimes = 0;
    CSA_uint32 rinterval = 0;
    char rule[64];
    int interval = (ev->interval > 0) ? ev->interval : 1;

    rule[0] = '\0';
    switch (ev->freq) {
    case DTCM_ICS_DAILY:
        rtype = (interval == 1) ? CSA_X_DT_REPEAT_DAILY : CSA_X_DT_REPEAT_EVERY_NDAY;
        rinterval = (interval > 1) ? (CSA_uint32)interval : 0;
        rtimes = (ev->count > 0) ? (CSA_uint32)ev->count : CSA_X_DT_DT_REPEAT_FOREVER;
        snprintf(rule, sizeof(rule), "D%d #%d", interval, (ev->count > 0) ? ev->count : 0);
        break;
    case DTCM_ICS_WEEKLY:
        if (interval == 1)
            rtype = CSA_X_DT_REPEAT_WEEKLY;
        else if (interval == 2)
            rtype = CSA_X_DT_REPEAT_BIWEEKLY;
        else {
            rtype = CSA_X_DT_REPEAT_EVERY_NWEEK;
            rinterval = (CSA_uint32)interval;
        }
        rtimes = (ev->count > 0) ? (CSA_uint32)ev->count : CSA_X_DT_DT_REPEAT_FOREVER;
        snprintf(rule, sizeof(rule), "W%d #%d", interval, (ev->count > 0) ? ev->count : 0);
        break;
    case DTCM_ICS_MONTHLY:
        rtype = (interval == 1) ? CSA_X_DT_REPEAT_MONTHLY_BY_DATE : CSA_X_DT_REPEAT_EVERY_NMONTH;
        rinterval = (interval > 1) ? (CSA_uint32)interval : 0;
        rtimes = (ev->count > 0) ? (CSA_uint32)ev->count : CSA_X_DT_DT_REPEAT_FOREVER;
        snprintf(rule, sizeof(rule), "MD%d #%d", interval, (ev->count > 0) ? ev->count : 0);
        break;
    case DTCM_ICS_YEARLY:
        rtype = CSA_X_DT_REPEAT_YEARLY;
        rtimes = (ev->count > 0) ? (CSA_uint32)ev->count : CSA_X_DT_DT_REPEAT_FOREVER;
        snprintf(rule, sizeof(rule), "YM%d #%d", interval, (ev->count > 0) ? ev->count : 0);
        break;
    case DTCM_ICS_ONCE:
    default:
        rtype = CSA_X_DT_REPEAT_ONETIME;
        rtimes = 0;
        rinterval = 0;
        snprintf(rule, sizeof(rule), "D1 #1");
        break;
    }

    if (appt->repeat_type && appt->repeat_type->value)
        appt->repeat_type->value->item.sint32_value = rtype;
    if (appt->repeat_times && appt->repeat_times->value)
        appt->repeat_times->value->item.uint32_value = rtimes;
    if (appt->repeat_interval && appt->repeat_interval->value)
        appt->repeat_interval->value->item.uint32_value = rinterval;
    if ((version >= DATAVER4 || version == DATAVER_ARCHIVE) &&
        appt->recurrence_rule && appt->recurrence_rule->value) {
        free(appt->recurrence_rule->value->item.string_value);
        appt->recurrence_rule->value->item.string_value = ckstrdup(rule);
    }
}

static void extract_appt_recurrence(const Dtcm_appointment *appt, DtcmIcsEvent *ev)
{
    ev->freq = DTCM_ICS_ONCE;
    ev->interval = 1;
    ev->count = 0;

    if (appt->repeat_type && appt->repeat_type->value) {
        CSA_sint32 rt = appt->repeat_type->value->item.sint32_value;
        CSA_uint32 rtimes = (appt->repeat_times && appt->repeat_times->value)
                                ? appt->repeat_times->value->item.uint32_value
                                : 0;
        CSA_uint32 rint = (appt->repeat_interval && appt->repeat_interval->value)
                              ? appt->repeat_interval->value->item.uint32_value
                              : 1;
        if (rint == 0)
            rint = 1;

        switch (rt) {
        case CSA_X_DT_REPEAT_DAILY:
            ev->freq = DTCM_ICS_DAILY;
            ev->interval = 1;
            break;
        case CSA_X_DT_REPEAT_EVERY_NDAY:
            ev->freq = DTCM_ICS_DAILY;
            ev->interval = (int)rint;
            break;
        case CSA_X_DT_REPEAT_WEEKLY:
            ev->freq = DTCM_ICS_WEEKLY;
            ev->interval = 1;
            break;
        case CSA_X_DT_REPEAT_BIWEEKLY:
            ev->freq = DTCM_ICS_WEEKLY;
            ev->interval = 2;
            break;
        case CSA_X_DT_REPEAT_EVERY_NWEEK:
            ev->freq = DTCM_ICS_WEEKLY;
            ev->interval = (int)rint;
            break;
        case CSA_X_DT_REPEAT_MONTHLY_BY_DATE:
        case CSA_X_DT_REPEAT_MONTHLY_BY_WEEKDAY:
            ev->freq = DTCM_ICS_MONTHLY;
            ev->interval = 1;
            break;
        case CSA_X_DT_REPEAT_EVERY_NMONTH:
            ev->freq = DTCM_ICS_MONTHLY;
            ev->interval = (int)rint;
            break;
        case CSA_X_DT_REPEAT_YEARLY:
            ev->freq = DTCM_ICS_YEARLY;
            ev->interval = 1;
            break;
        default:
            ev->freq = DTCM_ICS_ONCE;
            break;
        }
        if (ev->freq != DTCM_ICS_ONCE) {
            ev->count = (rtimes == CSA_X_DT_DT_REPEAT_FOREVER) ? 0 : (int)rtimes;
        }
        return;
    }

    if (appt->recurrence_rule && appt->recurrence_rule->value &&
        appt->recurrence_rule->value->item.string_value) {
        const char *r = appt->recurrence_rule->value->item.string_value;
        int n = 1;
        if (strncmp(r, "MD", 2) == 0 || strncmp(r, "MP", 2) == 0) {
            ev->freq = DTCM_ICS_MONTHLY;
            n = atoi(r + 2);
        } else if (strncmp(r, "YM", 2) == 0 || strncmp(r, "YD", 2) == 0) {
            ev->freq = DTCM_ICS_YEARLY;
            n = atoi(r + 2);
        } else if (r[0] == 'W') {
            ev->freq = DTCM_ICS_WEEKLY;
            n = atoi(r + 1);
        } else if (r[0] == 'D') {
            ev->freq = DTCM_ICS_DAILY;
            n = atoi(r + 1);
        }
        ev->interval = (n > 0) ? n : 1;
        const char *hash = strchr(r, '#');
        int cnt = hash ? atoi(hash + 1) : 0;
        if (ev->freq == DTCM_ICS_DAILY && ev->interval == 1 && cnt == 1) {
            ev->freq = DTCM_ICS_ONCE;
            ev->count = 0;
        } else {
            ev->count = (cnt > 0) ? cnt : 0;
        }
    }
}

int dtcm_ics_import_single_event(Calendar *c, const DtcmIcsEvent *ev)
{
    if (!c || !ev)
        return 0;
    offline_store_append(ev);
    if (!c->cal_handle)
        return 1;

    int version = (c->general && c->general->version) ? c->general->version : DATAVER4;
    Dtcm_appointment *appt = allocate_appt_struct(appt_write, version, nullptr);
    load_appt_defaults(appt, (Props *)c->properties);

    time_t st = ev->start_utc;
    time_t et = (ev->end_utc > ev->start_utc)
                    ? ev->end_utc
                    : (ev->all_day ? ev->start_utc + 86400 : ev->start_utc + 3600);

    if (appt->time && appt->time->value) {
        free(appt->time->value->item.date_time_value);
        appt->time->value->item.date_time_value = ckalloc(64);
        _csa_tick_to_iso8601(st, appt->time->value->item.date_time_value);
    }
    if (appt->end_time && appt->end_time->value) {
        free(appt->end_time->value->item.date_time_value);
        appt->end_time->value->item.date_time_value = ckalloc(64);
        _csa_tick_to_iso8601(et, appt->end_time->value->item.date_time_value);
    }
    if (appt->show_time && appt->show_time->value) {
        appt->show_time->value->item.sint32_value = ev->all_day ? B_FALSE : B_TRUE;
    }

    char what_buf[3072];
    dtcm_ics_compose_what(ev, what_buf, sizeof(what_buf));
    if (appt->what && appt->what->value) {
        free(appt->what->value->item.string_value);
        appt->what->value->item.string_value = cm_strdup(what_buf);
    }

    apply_ics_recurrence_to_appt(ev, appt, version);
    scrub_attr_list(appt);

    int ok = 0;
    CSA_entry_handle new_entry = 0;
    CSA_return_code rc =
        csa_add_entry(c->cal_handle, appt->count, appt->attrs, &new_entry, nullptr);
    if (rc == CSA_SUCCESS) {
        csa_free((CSA_buffer)new_entry);
        ok = 1;
    }
    free_appt_struct(&appt);
    return ok;
}

int dtcm_import_ics_file(Calendar *c, const char *ics_path, int *out_count)
{
    if (out_count)
        *out_count = 0;
    if (!c || !ics_path || !*ics_path)
        return -1;

    DtcmIcsEvent *events = NULL;
    int count = 0;
    if (dtcm_ics_parse_file(ics_path, &events, &count) != 0) {
        snprintf(g_status_msg, sizeof(g_status_msg),
                 "Failed to parse iCalendar file: %s", ics_path);
        dtcm_ics_publish_state(c);
        return -1;
    }

    int imported = 0;
    int invitations = 0;
    for (int i = 0; i < count; ++i) {
        const DtcmIcsEvent *ev = &events[i];
        if (strcasecmp(ev->method, "REQUEST") == 0 && c->frame && XtIsRealized(c->frame)) {
            dtcm_itip_show_invitation_dialog(c, ev);
            invitations++;
            continue;
        }
        imported += dtcm_ics_import_single_event(c, ev);
    }

    dtcm_ics_free_events(events);

    if (c->cal_handle && imported > 0) {
        invalidate_cache(c);
        paint_canvas(c, nullptr, RENDER_UNMAP);
    }

    snprintf(g_last_ics_path, sizeof(g_last_ics_path), "%s", ics_path);
    g_last_imported = imported;
    if (invitations > 0 && imported == 0) {
        snprintf(g_status_msg, sizeof(g_status_msg),
                 "Opened %d meeting invitation%s from %s",
                 invitations, (invitations == 1) ? "" : "s", ics_path);
    } else {
        snprintf(g_status_msg, sizeof(g_status_msg),
                 "Imported %d event%s from %s",
                 imported, (imported == 1) ? "" : "s", ics_path);
    }
    if (c->message_text)
        set_message(c->message_text, g_status_msg);
    dtcm_ics_publish_state(c);

    if (out_count)
        *out_count = imported;
    return 0;
}

int dtcm_export_ics_file(Calendar *c, const char *ics_path, int *out_count)
{
    if (out_count)
        *out_count = 0;
    if (!c || !ics_path || !*ics_path)
        return -1;

    if (!c->cal_handle) {
        if (dtcm_ics_write_file(ics_path, g_offline_events, g_offline_count) != 0) {
            snprintf(g_status_msg, sizeof(g_status_msg),
                     "Failed to write iCalendar file: %s", ics_path);
            dtcm_ics_publish_state(c);
            return -1;
        }
        snprintf(g_last_ics_path, sizeof(g_last_ics_path), "%s", ics_path);
        g_last_exported = g_offline_count;
        snprintf(g_status_msg, sizeof(g_status_msg),
                 "Exported %d event%s to %s",
                 g_offline_count, (g_offline_count == 1) ? "" : "s", ics_path);
        if (c->message_text)
            set_message(c->message_text, g_status_msg);
        dtcm_ics_publish_state(c);
        if (out_count)
            *out_count = g_offline_count;
        return 0;
    }

    int version = (c->general && c->general->version) ? c->general->version : DATAVER4;
    CSA_attribute *range_attrs = nullptr;
    CSA_enum *ops = nullptr;
    int range_count = 0;
    CSA_uint32 num_entries = 0;
    CSA_entry_handle *entries = nullptr;

    setup_range(&range_attrs, &ops, &range_count, get_bot(), get_eot(),
                CSA_TYPE_EVENT, 0, B_FALSE, version);
    CSA_return_code stat = csa_list_entries(c->cal_handle, range_count, range_attrs, ops,
                                            &num_entries, &entries, nullptr);
    free_range(&range_attrs, &ops, range_count);

    if (stat != CSA_SUCCESS) {
        snprintf(g_status_msg, sizeof(g_status_msg),
                 "Failed to query calendar entries for export");
        dtcm_ics_publish_state(c);
        return -1;
    }

    Dtcm_appointment *appt = allocate_appt_struct(
        appt_read, version,
        CSA_ENTRY_ATTR_REFERENCE_IDENTIFIER_I,
        CSA_ENTRY_ATTR_START_DATE_I,
        CSA_ENTRY_ATTR_END_DATE_I,
        CSA_ENTRY_ATTR_SUMMARY_I,
        CSA_X_DT_ENTRY_ATTR_SHOWTIME_I,
        CSA_X_DT_ENTRY_ATTR_REPEAT_TYPE_I,
        CSA_X_DT_ENTRY_ATTR_REPEAT_TIMES_I,
        CSA_X_DT_ENTRY_ATTR_REPEAT_INTERVAL_I,
        CSA_ENTRY_ATTR_RECURRENCE_RULE_I,
        nullptr);

    DtcmIcsEvent *out = NULL;
    int count = 0;
    int cap = 0;

    for (CSA_uint32 i = 0; i < num_entries; ++i) {
        if (query_appt_struct(c->cal_handle, entries[i], appt) != CSA_SUCCESS)
            continue;

        char uid_str[256];
        uid_str[0] = '\0';
        if (appt->identifier && appt->identifier->value &&
            appt->identifier->value->item.opaque_data_value &&
            appt->identifier->value->item.opaque_data_value->data &&
            appt->identifier->value->item.opaque_data_value->size > 0) {
            CSA_opaque_data *od = appt->identifier->value->item.opaque_data_value;
            size_t copy_n = od->size;
            if (copy_n >= sizeof(uid_str))
                copy_n = sizeof(uid_str) - 1;
            memcpy(uid_str, od->data, copy_n);
            uid_str[copy_n] = '\0';
        }

        Tick start_tick = 0;
        Tick end_tick = 0;
        if (appt->time && appt->time->value && appt->time->value->item.date_time_value)
            _csa_iso8601_to_tick(appt->time->value->item.date_time_value, &start_tick);
        if (appt->end_time && appt->end_time->value && appt->end_time->value->item.date_time_value)
            _csa_iso8601_to_tick(appt->end_time->value->item.date_time_value, &end_tick);
        if (end_tick < start_tick)
            end_tick = start_tick;

        /* Deduplicate recurring appointment occurrences by CSA reference identifier */
        int dup_idx = -1;
        if (uid_str[0] != '\0') {
            for (int k = 0; k < count; ++k) {
                if (strcmp(out[k].uid, uid_str) == 0) {
                    dup_idx = k;
                    break;
                }
            }
        }
        if (dup_idx >= 0) {
            if ((time_t)start_tick < out[dup_idx].start_utc) {
                time_t dur = out[dup_idx].end_utc - out[dup_idx].start_utc;
                out[dup_idx].start_utc = (time_t)start_tick;
                out[dup_idx].end_utc = (time_t)start_tick + dur;
            }
            continue;
        }

        if (count >= cap) {
            int ncap = (cap == 0) ? 32 : cap * 2;
            DtcmIcsEvent *next =
                (DtcmIcsEvent *)realloc(out, (size_t)ncap * sizeof(DtcmIcsEvent));
            if (!next)
                break;
            out = next;
            cap = ncap;
        }

        DtcmIcsEvent *ev = &out[count];
        memset(ev, 0, sizeof(*ev));
        snprintf(ev->uid, sizeof(ev->uid), "%s", uid_str);
        ev->start_utc = (time_t)start_tick;
        ev->end_utc = (time_t)end_tick;
        ev->all_day = (appt->show_time && appt->show_time->value &&
                       !appt->show_time->value->item.sint32_value)
                          ? 1
                          : 0;

        const char *what = (appt->what && appt->what->value)
                               ? appt->what->value->item.string_value
                               : "";
        dtcm_ics_decompose_what_ex(what,
                                   ev->summary, sizeof(ev->summary),
                                   ev->location, sizeof(ev->location),
                                   ev->category, sizeof(ev->category),
                                   ev->description, sizeof(ev->description));
        extract_appt_recurrence(appt, ev);
        count++;
    }

    free_appt_struct(&appt);
    if (entries)
        csa_free(entries);

    int rc = dtcm_ics_write_file(ics_path, out, count);
    free(out);
    if (rc != 0) {
        snprintf(g_status_msg, sizeof(g_status_msg),
                 "Failed to write iCalendar file: %s", ics_path);
        dtcm_ics_publish_state(c);
        return -1;
    }

    snprintf(g_last_ics_path, sizeof(g_last_ics_path), "%s", ics_path);
    g_last_exported = count;
    snprintf(g_status_msg, sizeof(g_status_msg),
             "Exported %d event%s to %s",
             count, (count == 1) ? "" : "s", ics_path);
    if (c->message_text)
        set_message(c->message_text, g_status_msg);
    dtcm_ics_publish_state(c);

    if (out_count)
        *out_count = count;
    return 0;
}

static void fsb_cancel_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)client_data;
    (void)call_data;
    XtUnmanageChild(w);
}

static void import_fsb_ok_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    Calendar *c = (Calendar *)client_data;
    XmFileSelectionBoxCallbackStruct *cbs = (XmFileSelectionBoxCallbackStruct *)call_data;
    char *path = NULL;
    if (cbs && XmStringGetLtoR(cbs->value, XmFONTLIST_DEFAULT_TAG, &path) && path) {
        if (*path)
            dtcm_import_ics_file(c, path, NULL);
        XtFree(path);
    }
    XtUnmanageChild(w);
}

static void export_fsb_ok_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    Calendar *c = (Calendar *)client_data;
    XmFileSelectionBoxCallbackStruct *cbs = (XmFileSelectionBoxCallbackStruct *)call_data;
    char *path = NULL;
    if (cbs && XmStringGetLtoR(cbs->value, XmFONTLIST_DEFAULT_TAG, &path) && path) {
        if (*path)
            dtcm_export_ics_file(c, path, NULL);
        XtFree(path);
    }
    XtUnmanageChild(w);
}

static void import_menu_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    Calendar *c = (Calendar *)client_data;
    if (!c || !c->frame)
        return;

    if (!g_import_fsb) {
        Arg al[8];
        int ac = 0;
        XmString pattern = XmStringCreateLocalized("*.ics");
        XmString title = XmStringCreateLocalized("Calendar - Import iCalendar (.ics)");
        XtSetArg(al[ac], XmNpattern, pattern); ac++;
        XtSetArg(al[ac], XmNdialogTitle, title); ac++;
        g_import_fsb = XmCreateFileSelectionDialog(c->frame, "importIcsDialog", al, ac);
        XmStringFree(pattern);
        XmStringFree(title);
        Widget help_btn = XmFileSelectionBoxGetChild(g_import_fsb, XmDIALOG_HELP_BUTTON);
        if (help_btn)
            XtUnmanageChild(help_btn);
        XtAddCallback(g_import_fsb, XmNokCallback, import_fsb_ok_cb, (XtPointer)c);
        XtAddCallback(g_import_fsb, XmNcancelCallback, fsb_cancel_cb, NULL);
    }
    XtManageChild(g_import_fsb);
}

static void export_menu_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    Calendar *c = (Calendar *)client_data;
    if (!c || !c->frame)
        return;

    if (!g_export_fsb) {
        Arg al[8];
        int ac = 0;
        XmString pattern = XmStringCreateLocalized("*.ics");
        XmString title = XmStringCreateLocalized("Calendar - Export iCalendar (.ics)");
        XtSetArg(al[ac], XmNpattern, pattern); ac++;
        XtSetArg(al[ac], XmNdialogTitle, title); ac++;
        g_export_fsb = XmCreateFileSelectionDialog(c->frame, "exportIcsDialog", al, ac);
        XmStringFree(pattern);
        XmStringFree(title);
        Widget help_btn = XmFileSelectionBoxGetChild(g_export_fsb, XmDIALOG_HELP_BUTTON);
        if (help_btn)
            XtUnmanageChild(help_btn);
        XtAddCallback(g_export_fsb, XmNokCallback, export_fsb_ok_cb, (XtPointer)c);
        XtAddCallback(g_export_fsb, XmNcancelCallback, fsb_cancel_cb, NULL);
    }
    XtManageChild(g_export_fsb);
}

void dtcm_ics_add_menu_items(Calendar *c, Widget file_menu)
{
    if (!c || !file_menu)
        return;

    Arg al[4];
    int ac = 0;
    XmCreateSeparatorGadget(file_menu, "icsSeparator", al, ac);

    XmString imp_str = XmStringCreateLocalized("Import iCalendar (.ics)...");
    ac = 0;
    XtSetArg(al[ac], XmNlabelString, imp_str); ac++;
    Widget imp_btn = XmCreatePushButtonGadget(file_menu, "importIcs", al, ac);
    XtAddCallback(imp_btn, XmNactivateCallback, import_menu_cb, (XtPointer)c);
    XmStringFree(imp_str);

    XmString exp_str = XmStringCreateLocalized("Export iCalendar (.ics)...");
    ac = 0;
    XtSetArg(al[ac], XmNlabelString, exp_str); ac++;
    Widget exp_btn = XmCreatePushButtonGadget(file_menu, "exportIcs", al, ac);
    XtAddCallback(exp_btn, XmNactivateCallback, export_menu_cb, (XtPointer)c);
    XmStringFree(exp_str);
}

static void dtcm_ipc_prop_cb(Widget w, XtPointer client_data, XEvent *event, Boolean *cont)
{
    (void)w;
    (void)cont;
    Calendar *c = (Calendar *)client_data;
    if (!c || !event || event->type != PropertyNotify)
        return;
    XPropertyEvent *pe = &event->xproperty;
    if (pe->state != PropertyNewValue || pe->atom != g_ipc_atom || g_ipc_atom == None)
        return;

    Display *dpy = XtDisplay(c->frame);
    Window win = XtWindow(c->frame);
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long nitems = 0;
    unsigned long bytes_after = 0;
    unsigned char *prop = NULL;

    if (XGetWindowProperty(dpy, win, g_ipc_atom, 0, 4096, True, AnyPropertyType,
                           &actual_type, &actual_format, &nitems, &bytes_after,
                           &prop) != Success || !prop) {
        return;
    }

    char cmd[4096];
    size_t len = (nitems < sizeof(cmd) - 1) ? (size_t)nitems : sizeof(cmd) - 1;
    memcpy(cmd, prop, len);
    cmd[len] = '\0';
    XFree(prop);

    while (len > 0 && (cmd[len - 1] == '\n' || cmd[len - 1] == '\r' || cmd[len - 1] == ' '))
        cmd[--len] = '\0';

    if (strncmp(cmd, "IMPORT_ICS ", 11) == 0) {
        const char *p = cmd + 11;
        while (*p == ' ')
            p++;
        dtcm_import_ics_file(c, p, NULL);
    } else if (strncmp(cmd, "EXPORT_ICS ", 11) == 0) {
        const char *p = cmd + 11;
        while (*p == ' ')
            p++;
        dtcm_export_ics_file(c, p, NULL);
    } else if (strcmp(cmd, "SYNC_STATE") == 0) {
        dtcm_ics_publish_state(c);
    }
}

void dtcm_ics_init_gui(Calendar *c)
{
    if (!c || !c->frame || !XtIsRealized(c->frame))
        return;

    Display *dpy = XtDisplay(c->frame);
    g_state_atom = XInternAtom(dpy, "_DTCM_STATE", False);
    g_ipc_atom = XInternAtom(dpy, "_DTCM_IPC", False);
    g_utf8_atom = XInternAtom(dpy, "UTF8_STRING", False);

    XtAddEventHandler(c->frame, PropertyChangeMask, False, dtcm_ipc_prop_cb, (XtPointer)c);
    dtcm_ics_publish_state(c);
}
