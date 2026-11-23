/*
 * CDE - Common Desktop Environment
 *
 * iTIP (RFC 5546) meeting invitation parser, Motif RSVP banner, and
 * _DTCM_IPC / --import-ics handoff to dtcm.
 */

#include "ItipBanner.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef DTMAIL_ITIP_NO_GUI
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <Xm/Form.h>
#include <Xm/Label.h>
#include <Xm/PushB.h>
#endif

static DtMailItipInvite g_cur_invite;
static char *g_cur_ics = NULL;
static size_t g_cur_ics_len = 0;

static DtMailItipChangeCB g_change_cb = NULL;
static void *g_change_ud = NULL;

#ifndef DTMAIL_ITIP_NO_GUI
static Display *g_itip_dpy = NULL;

struct DtMailItipBanner {
    Widget form;
    Widget above_widget;
    Widget below_widget;
    Widget label_w;
    Widget open_btn;
    int    visible;
};
#endif

static void trim_crlf(char *s)
{
    size_t l = strlen(s);
    while (l > 0 && (s[l - 1] == '\r' || s[l - 1] == '\n' || s[l - 1] == ' ')) {
        s[--l] = '\0';
    }
}

static int parse_prop(const char *line, const char *prop_name, char *out, size_t out_len)
{
    size_t plen = strlen(prop_name);
    if (strncasecmp(line, prop_name, plen) != 0) return 0;
    if (line[plen] != ':' && line[plen] != ';') return 0;
    const char *colon = strchr(line + plen, ':');
    if (!colon) return 0;
    snprintf(out, out_len, "%s", colon + 1);
    trim_crlf(out);
    return 1;
}

int dtmail_itip_parse(const char *raw, size_t len, DtMailItipInvite *out)
{
    if (!raw || len == 0 || !out) return -1;
    memset(out, 0, sizeof(*out));

    const char *start = strstr(raw, "BEGIN:VCALENDAR");
    if (!start) return -1;
    const char *end = strstr(start, "END:VCALENDAR");
    if (!end) return -1;
    end += strlen("END:VCALENDAR");
    if ((size_t)(end - raw) > len) return -1;

    int in_vevent = 0;
    const char *p = start;
    while (p < end) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        size_t llen = nl ? (size_t)(nl - p) : (size_t)(end - p);
        char line[512];
        if (llen >= sizeof(line)) llen = sizeof(line) - 1;
        memcpy(line, p, llen);
        line[llen] = '\0';
        trim_crlf(line);

        if (strcasecmp(line, "BEGIN:VEVENT") == 0) {
            in_vevent = 1;
        } else if (strcasecmp(line, "END:VEVENT") == 0) {
            in_vevent = 0;
        } else if (!in_vevent) {
            (void)parse_prop(line, "METHOD", out->method, sizeof(out->method));
        } else {
            (void)parse_prop(line, "SUMMARY", out->summary, sizeof(out->summary));
            (void)parse_prop(line, "DTSTART", out->dtstart, sizeof(out->dtstart));
            (void)parse_prop(line, "DTEND", out->dtend, sizeof(out->dtend));
            (void)parse_prop(line, "LOCATION", out->location, sizeof(out->location));
            (void)parse_prop(line, "ORGANIZER", out->organizer, sizeof(out->organizer));
            (void)parse_prop(line, "UID", out->uid, sizeof(out->uid));
        }
        p = nl ? nl + 1 : end;
    }

    if (out->summary[0] || out->dtstart[0]) {
        out->valid = 1;
        return 0;
    }
    return -1;
}

#ifndef DTMAIL_ITIP_NO_GUI
static Window find_dtcm_window_rec(Display *dpy, Window win, Atom state_atom, int depth)
{
    if (!dpy || !win || depth > 3) return None;

    Atom actual_type = None;
    int actual_fmt = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *prop = NULL;
    if (XGetWindowProperty(dpy, win, state_atom, 0, 16, False, AnyPropertyType,
                           &actual_type, &actual_fmt, &nitems, &bytes_after, &prop) == Success) {
        if (prop) XFree(prop);
        if (actual_type != None && nitems > 0) {
            return win;
        }
    }

    Window root = None, parent = None, *children = NULL;
    unsigned int nchildren = 0;
    if (XQueryTree(dpy, win, &root, &parent, &children, &nchildren)) {
        Window found = None;
        for (unsigned int i = 0; i < nchildren && found == None; i++) {
            found = find_dtcm_window_rec(dpy, children[i], state_atom, depth + 1);
        }
        if (children) XFree(children);
        return found;
    }
    return None;
}

static int send_import_ics_to_running_dtcm(Display *dpy, const char *ics_path)
{
    if (!dpy || !ics_path || !ics_path[0]) return -1;
    Atom state_atom = XInternAtom(dpy, "_DTCM_STATE", False);
    Atom ipc_atom   = XInternAtom(dpy, "_DTCM_IPC", False);
    Atom utf8_atom  = XInternAtom(dpy, "UTF8_STRING", False);
    if (state_atom == None || ipc_atom == None) return -1;

    Window target = find_dtcm_window_rec(dpy, DefaultRootWindow(dpy), state_atom, 0);
    if (target == None) return -1;

    char cmd[1200];
    int n = snprintf(cmd, sizeof(cmd), "IMPORT_ICS %s", ics_path);
    if (n <= 0 || (size_t)n >= sizeof(cmd)) return -1;

    Atom type = (utf8_atom != None) ? utf8_atom : XA_STRING;
    XChangeProperty(dpy, target, ipc_atom, type, 8, PropModeReplace,
                    (const unsigned char *)cmd, n);
    XRaiseWindow(dpy, target);
    XFlush(dpy);
    return 0;
}
#endif

int dtmail_itip_open_in_dtcm(const char *ics_raw, size_t ics_len,
                             char *out_saved_path, size_t out_path_len)
{
    if (!ics_raw || ics_len == 0) return -1;
    const char *home = getenv("HOME");
    if (!home || !home[0]) return -1;

    char dir1[1024], dir2[1024], path[1024];
    snprintf(dir1, sizeof(dir1), "%s/.dt", home);
    snprintf(dir2, sizeof(dir2), "%s/.dt/calendar", home);
    (void)mkdir(dir1, 0700);
    (void)mkdir(dir2, 0700);
    snprintf(path, sizeof(path), "%s/incoming_invite.ics", dir2);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    (void)fchmod(fd, 0600);
    size_t written = 0;
    while (written < ics_len) {
        ssize_t n = write(fd, ics_raw + written, ics_len - written);
        if (n <= 0) { close(fd); return -1; }
        written += (size_t)n;
    }
    close(fd);

    if (out_saved_path && out_path_len > 0) {
        snprintf(out_saved_path, out_path_len, "%s", path);
    }

    if (getenv("DTMAIL_NO_LAUNCH_DTCM")) {
        return 0;
    }

#ifndef DTMAIL_ITIP_NO_GUI
    /* First try notifying an already-running dtcm via _DTCM_IPC IMPORT_ICS */
    if (g_itip_dpy && send_import_ics_to_running_dtcm(g_itip_dpy, path) == 0) {
        return 0;
    }
#endif

    /* Otherwise launch dtcm with --import-ics <path> so the RSVP dialog opens automatically */
    pid_t pid = fork();
    if (pid == 0) {
        execlp("/usr/dt/bin/dtcm", "dtcm", "--import-ics", path, (char *)NULL);
        execlp("dtcm", "dtcm", "--import-ics", path, (char *)NULL);
        _exit(127);
    }
    return (pid > 0) ? 0 : -1;
}

void dtmail_itip_register_change_cb(DtMailItipChangeCB cb, void *userdata)
{
    g_change_cb = cb;
    g_change_ud = userdata;
}

void dtmail_itip_set_current(const char *raw, size_t len)
{
    free(g_cur_ics);
    g_cur_ics = NULL;
    g_cur_ics_len = 0;
    memset(&g_cur_invite, 0, sizeof(g_cur_invite));

    if (raw && len > 0 && dtmail_itip_parse(raw, len, &g_cur_invite) == 0 && g_cur_invite.valid) {
        const char *start = strstr(raw, "BEGIN:VCALENDAR");
        const char *end = start ? strstr(start, "END:VCALENDAR") : NULL;
        if (start && end) {
            size_t ilen = (size_t)(end + strlen("END:VCALENDAR") - start);
            g_cur_ics = (char *)malloc(ilen + 1);
            if (g_cur_ics) {
                memcpy(g_cur_ics, start, ilen);
                g_cur_ics[ilen] = '\0';
                g_cur_ics_len = ilen;
            }
        }
    }

    if (g_change_cb) {
        g_change_cb(dtmail_itip_has_current(), dtmail_itip_get_current(), g_change_ud);
    }
}

int dtmail_itip_has_current(void)
{
    return (g_cur_invite.valid && g_cur_ics && g_cur_ics_len > 0) ? 1 : 0;
}

const DtMailItipInvite *dtmail_itip_get_current(void)
{
    return dtmail_itip_has_current() ? &g_cur_invite : NULL;
}

int dtmail_itip_open_current_in_dtcm(void)
{
    if (!dtmail_itip_has_current()) return -1;
    return dtmail_itip_open_in_dtcm(g_cur_ics, g_cur_ics_len, NULL, 0);
}

#ifndef DTMAIL_ITIP_NO_GUI
static void on_itip_open_btn(Widget w, XtPointer cd, XtPointer call_data)
{
    (void)w; (void)cd; (void)call_data;
    (void)dtmail_itip_open_current_in_dtcm();
}

static void itip_banner_set_visible(DtMailItipBanner *b, int visible)
{
    if (!b || !b->form) return;
    b->visible = visible ? 1 : 0;
    if (b->visible) {
        XtManageChild(b->form);
        if (b->below_widget) {
            XtVaSetValues(b->below_widget,
                XmNtopAttachment, XmATTACH_WIDGET,
                XmNtopWidget, b->form,
                XmNtopOffset, 2,
                NULL);
        }
    } else {
        if (b->below_widget && b->above_widget) {
            XtVaSetValues(b->below_widget,
                XmNtopAttachment, XmATTACH_WIDGET,
                XmNtopWidget, b->above_widget,
                XmNtopOffset, 2,
                NULL);
        }
        XtUnmanageChild(b->form);
    }
}

void dtmail_itip_banner_refresh(DtMailItipBanner *b)
{
    if (!b) return;
    const DtMailItipInvite *inv = dtmail_itip_get_current();
    if (!inv) {
        itip_banner_set_visible(b, 0);
        return;
    }
    char txt[640];
    snprintf(txt, sizeof(txt), "Meeting Invitation: %s  (%s%s%s)",
             inv->summary[0] ? inv->summary : "(Untitled)",
             inv->dtstart[0] ? inv->dtstart : "scheduled",
             inv->location[0] ? " @ " : "",
             inv->location);
    XmString s = XmStringCreateLocalized(txt);
    XtVaSetValues(b->label_w, XmNlabelString, s, NULL);
    XmStringFree(s);
    itip_banner_set_visible(b, 1);
}

DtMailItipBanner *dtmail_itip_banner_create(Widget parent_form,
                                            Widget above_widget,
                                            Widget below_widget)
{
    if (!parent_form) return NULL;
    DtMailItipBanner *b = (DtMailItipBanner *)calloc(1, sizeof(DtMailItipBanner));
    if (!b) return NULL;
    b->above_widget = above_widget;
    b->below_widget = below_widget;
    b->visible = 0;
    g_itip_dpy = XtDisplay(parent_form);

    b->form = XtVaCreateWidget("ItipInviteBanner", xmFormWidgetClass, parent_form,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, above_widget,
        XmNtopOffset, 3,
        XmNleftAttachment, XmATTACH_FORM,
        XmNleftOffset, 4,
        XmNrightAttachment, XmATTACH_FORM,
        XmNrightOffset, 4,
        XmNshadowThickness, 1,
        XmNshadowType, XmSHADOW_ETCHED_IN,
        NULL);

    XmString btn_s = XmStringCreateLocalized((char *)"RSVP in Calendar (dtcm)");
    b->open_btn = XtVaCreateManagedWidget("ItipOpenDtcmBtn", xmPushButtonWidgetClass, b->form,
        XmNlabelString, btn_s,
        XmNtopAttachment, XmATTACH_FORM,
        XmNtopOffset, 3,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNbottomOffset, 3,
        XmNrightAttachment, XmATTACH_FORM,
        XmNrightOffset, 5,
        NULL);
    XmStringFree(btn_s);
    XtAddCallback(b->open_btn, XmNactivateCallback, on_itip_open_btn, b);

    XmString lbl_s = XmStringCreateLocalized((char *)"Meeting Invitation");
    b->label_w = XtVaCreateManagedWidget("ItipBannerLabel", xmLabelWidgetClass, b->form,
        XmNlabelString, lbl_s,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNtopAttachment, XmATTACH_FORM,
        XmNtopOffset, 5,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNbottomOffset, 5,
        XmNleftAttachment, XmATTACH_FORM,
        XmNleftOffset, 8,
        XmNrightAttachment, XmATTACH_WIDGET,
        XmNrightWidget, b->open_btn,
        XmNrightOffset, 8,
        NULL);
    XmStringFree(lbl_s);

    return b;
}

void dtmail_itip_banner_destroy(DtMailItipBanner *b)
{
    if (!b) return;
    free(b);
}
#endif
