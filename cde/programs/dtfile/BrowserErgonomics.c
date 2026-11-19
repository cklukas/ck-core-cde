/*
 * BrowserErgonomics.c - Multi-Tab Browsing (Ctrl+T / Ctrl+W / Ctrl+Tab / Middle-Click),
 *                       Per-Tab Back/Forward History (Alt+Left / Alt+Right / Button8/9),
 *                       Inline Rename (F2), Hidden Toggle (Ctrl+H), and Cancellable
 *                       ck-core-fs-lib Streaming Search Bar (Ctrl+F) for dtfile (WP-17).
 */

#define _GNU_SOURCE

#include "SyncFeature.h"
#include <Xm/Xm.h>
#include "Encaps.h"
#include "FileMgr.h"
#include "Desktop.h"
#include "Filter.h"
#include "Main.h"
#include "Help.h"
#include "SharedProcs.h"
#include "XdgTrashSync.h"
#include "BrowserErgonomics.h"
#include "ClipboardUndo.h"

#include <Dt/Action.h>
#include <Dt/Dts.h>
#include <Dt/FileM.h>
#include <Xm/Form.h>
#include <Xm/Frame.h>
#include <Xm/Gadget.h>
#include <Xm/LabelG.h>
#include <Xm/List.h>
#include <Xm/PushB.h>
#include <Xm/PushBG.h>
#include <Xm/RowColumn.h>
#include <Xm/ScrolledW.h>
#include <Xm/SeparatoG.h>
#include <Xm/Text.h>
#include <Xm/TextF.h>
#include <Xm/ToggleBG.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>

#if DTFILE_SYNC_FEATURE_ENABLED
#include <ck_fs_search.h>
#endif

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DTFILE_MAX_TABS 16
#define DTFILE_TAB_HIST_CAP 64
#define DTFILE_MAX_ERG_WINDOWS 32
#define DTFILE_SEARCH_POLL_MS 75

#ifndef Button8
#define Button8 8
#endif
#ifndef Button9
#define Button9 9
#endif

typedef struct {
    char *host;
    char *dir;
    char *hist_hosts[DTFILE_TAB_HIST_CAP];
    char *hist_dirs[DTFILE_TAB_HIST_CAP];
    int hist_len;
    int hist_idx;
} DtfileTabState;

typedef struct {
    char *rel_path;
    char *abs_path;
    Boolean is_dir;
} DtfileSearchHitItem;

typedef struct DtfileWindowErg {
    FileMgrRec *rec;
    FileMgrData *fmd;
    Boolean is_trash_view;
    Boolean suppress_history_push;

    /* Middle-click (Button2 without drag) tracking on file_window */
    Boolean b2_armed;
    int b2_press_x;
    int b2_press_y;

    /* Tabs */
    DtfileTabState tabs[DTFILE_MAX_TABS];
    int tab_count;
    int active_tab;
    Widget tab_sep;
    Widget back_btn;
    Widget fwd_btn;
    Widget new_tab_btn;
    Widget tab_strip_rc;
    Widget tab_select_btns[DTFILE_MAX_TABS];
    Widget tab_close_btns[DTFILE_MAX_TABS];

    /* Search bar */
    Boolean search_bar_open;
    Widget search_sep;
    Widget name_tf;
    Widget content_tf;
    Widget recursive_tb;
    Widget case_tb;
    Widget min_size_tf;
    Widget max_size_tf;
    Widget after_date_tf;
    Widget before_date_tf;
    Widget search_go_btn;
    Widget search_stop_btn;
    Widget search_close_btn;

    /* Search streaming results panel */
    Boolean search_results_visible;
    Widget search_status_lbl;
    Widget search_list;
    Widget search_open_btn;
    Widget search_newtab_btn;
    Widget search_reveal_btn;
    Widget search_back_btn;

    char *search_root_dir;
    char *pending_select_name;
    char search_state_str[24];
    uint64_t search_scanned;
    DtfileSearchHitItem *hits;
    int hit_count;
    int hit_cap;
    XtIntervalId search_timer;
#if DTFILE_SYNC_FEATURE_ENABLED
    ckfes_job *search_job;
#endif
} DtfileWindowErg;

static DtfileWindowErg g_windows[DTFILE_MAX_ERG_WINDOWS];
static int g_window_count = 0;
static DtfileWindowErg *g_last_active_erg = nullptr;

static Widget g_cmd_toplevel = nullptr;
static XtIntervalId g_cmd_timer = 0;

#if DTFILE_SYNC_FEATURE_ENABLED
#define DTFILE_RETIRED_CAP 32
static ckfes_job *g_retired_jobs[DTFILE_RETIRED_CAP];
static int g_retired_count = 0;
static XtIntervalId g_retired_timer = 0;

static void
retired_jobs_poll_cb(XtPointer client_data, XtIntervalId *id)
{
    (void)id;
    Widget ref_w = (Widget)client_data;
    g_retired_timer = 0;

    int w = 0;
    for (int r = 0; r < g_retired_count; ++r)
    {
        uint32_t drained = 0;
        if (ckfes_worker_drained_v1(g_retired_jobs[r], &drained) != CKFE_API_OK || drained != 0)
        {
            ckfes_release(g_retired_jobs[r]);
        }
        else
        {
            g_retired_jobs[w++] = g_retired_jobs[r];
        }
    }
    g_retired_count = w;

    if (g_retired_count > 0 && ref_w != nullptr)
    {
        g_retired_timer = XtAppAddTimeOut(
            XtWidgetToApplicationContext(ref_w), 50, retired_jobs_poll_cb, (XtPointer)ref_w);
    }
}

static void
retire_search_job(Widget ref_w, ckfes_job *job)
{
    if (!job)
        return;
    ckfes_request_cancel(job);
    uint32_t drained = 0;
    if (ckfes_worker_drained_v1(job, &drained) != CKFE_API_OK || drained != 0)
    {
        ckfes_release(job);
        return;
    }
    if (g_retired_count < DTFILE_RETIRED_CAP)
    {
        g_retired_jobs[g_retired_count++] = job;
        if (g_retired_timer == 0 && ref_w != nullptr)
        {
            g_retired_timer = XtAppAddTimeOut(
                XtWidgetToApplicationContext(ref_w), 50, retired_jobs_poll_cb, (XtPointer)ref_w);
        }
    }
}
#endif

static DtfileWindowErg *
find_erg_by_rec(const FileMgrRec *rec)
{
    if (!rec)
        return nullptr;
    for (int i = 0; i < g_window_count; ++i)
    {
        if (g_windows[i].rec == rec)
            return &g_windows[i];
    }
    return nullptr;
}

static DtfileWindowErg *
find_erg_by_fmd(const FileMgrData *fmd)
{
    if (!fmd)
        return g_last_active_erg;
    for (int i = 0; i < g_window_count; ++i)
    {
        if (g_windows[i].fmd == fmd)
            return &g_windows[i];
    }
    return find_erg_by_rec((const FileMgrRec *)fmd->file_mgr_rec);
}

static FileMgrData *
resolve_rec_fmd(FileMgrRec *rec)
{
    if (!rec)
        return nullptr;
    DialogData *dd = _DtGetInstanceData((XtPointer)rec);
    return dd ? (FileMgrData *)dd->data : nullptr;
}

static void
free_tab_state(DtfileTabState *t)
{
    if (!t)
        return;
    XtFree(t->host);
    XtFree(t->dir);
    for (int i = 0; i < t->hist_len; ++i)
    {
        XtFree(t->hist_hosts[i]);
        XtFree(t->hist_dirs[i]);
    }
    memset(t, 0, sizeof(*t));
}

static void
init_tab_state(DtfileTabState *t, const char *host, const char *dir)
{
    memset(t, 0, sizeof(*t));
    const char *h = (host && *host) ? host : home_host_name;
    const char *d = (dir && *dir) ? dir : "/";
    t->host = XtNewString((char *)h);
    t->dir = XtNewString((char *)d);
    t->hist_hosts[0] = XtNewString((char *)h);
    t->hist_dirs[0] = XtNewString((char *)d);
    t->hist_len = 1;
    t->hist_idx = 0;
}

static void
push_tab_history(DtfileTabState *t, const char *host, const char *dir)
{
    if (!t || !dir || !*dir)
        return;
    const char *h = (host && *host) ? host : home_host_name;

    XtFree(t->host);
    XtFree(t->dir);
    t->host = XtNewString((char *)h);
    t->dir = XtNewString((char *)dir);

    if (t->hist_len > 0 && t->hist_idx >= 0 && t->hist_idx < t->hist_len)
    {
        if (strcmp(t->hist_dirs[t->hist_idx], dir) == 0 &&
            strcmp(t->hist_hosts[t->hist_idx], h) == 0)
        {
            return;
        }
    }

    /* Truncate forward history when branching */
    for (int i = t->hist_idx + 1; i < t->hist_len; ++i)
    {
        XtFree(t->hist_hosts[i]);
        XtFree(t->hist_dirs[i]);
        t->hist_hosts[i] = nullptr;
        t->hist_dirs[i] = nullptr;
    }
    t->hist_len = t->hist_idx + 1;

    if (t->hist_len >= DTFILE_TAB_HIST_CAP)
    {
        XtFree(t->hist_hosts[0]);
        XtFree(t->hist_dirs[0]);
        memmove(&t->hist_hosts[0], &t->hist_hosts[1], (DTFILE_TAB_HIST_CAP - 1) * sizeof(char *));
        memmove(&t->hist_dirs[0], &t->hist_dirs[1], (DTFILE_TAB_HIST_CAP - 1) * sizeof(char *));
        t->hist_len = DTFILE_TAB_HIST_CAP - 1;
    }

    t->hist_hosts[t->hist_len] = XtNewString((char *)h);
    t->hist_dirs[t->hist_len] = XtNewString((char *)dir);
    t->hist_idx = t->hist_len;
    t->hist_len++;
}

static void
format_tab_title(const char *dir, Boolean is_active, char *out, size_t cap)
{
    if (!dir || !*dir || strcmp(dir, "/") == 0)
    {
        snprintf(out, cap, is_active ? "[ / ]" : "/");
        return;
    }
    const char *slash = strrchr(dir, '/');
    const char *base = (slash && slash[1] != '\0') ? (slash + 1) : dir;
    char short_base[28];
    if (strlen(base) > 22)
        snprintf(short_base, sizeof(short_base), "%.19s...", base);
    else
        snprintf(short_base, sizeof(short_base), "%s", base);

    if (is_active)
        snprintf(out, cap, "[ %s ]", short_base);
    else
        snprintf(out, cap, "  %s  ", short_base);
}

static void
refresh_tab_bar_widgets(DtfileWindowErg *erg)
{
    if (!erg || !erg->rec || erg->is_trash_view || !erg->rec->tab_bar_form)
        return;

    if (erg->tab_count <= 0)
        return;

    DtfileTabState *active = &erg->tabs[erg->active_tab];
    XtSetSensitive(erg->back_btn, active->hist_idx > 0);
    XtSetSensitive(erg->fwd_btn, active->hist_idx + 1 < active->hist_len);
    XtSetSensitive(erg->new_tab_btn, erg->tab_count < DTFILE_MAX_TABS);

    for (int i = 0; i < DTFILE_MAX_TABS; ++i)
    {
        if (i < erg->tab_count)
        {
            char lbl[40];
            format_tab_title(erg->tabs[i].dir, i == erg->active_tab, lbl, sizeof(lbl));
            XmString xs = XmStringCreateLocalized(lbl);
            XtVaSetValues(erg->tab_select_btns[i],
                          XmNlabelString, xs,
                          XmNshadowType, (i == erg->active_tab) ? XmSHADOW_IN : XmSHADOW_OUT,
                          XmNshadowThickness, (i == erg->active_tab) ? 2 : 1,
                          nullptr);
            XmStringFree(xs);
            if (!XtIsManaged(erg->tab_select_btns[i]))
                XtManageChild(erg->tab_select_btns[i]);
            if (!XtIsManaged(erg->tab_close_btns[i]))
                XtManageChild(erg->tab_close_btns[i]);
        }
        else
        {
            if (XtIsManaged(erg->tab_select_btns[i]))
                XtUnmanageChild(erg->tab_select_btns[i]);
            if (XtIsManaged(erg->tab_close_btns[i]))
                XtUnmanageChild(erg->tab_close_btns[i]);
        }
    }
}

void
DtfileErgonomicsSyncHeaderLayout(FileMgrRec *rec, FileMgrData *fmd)
{
    DtfileWindowErg *erg = find_erg_by_rec(rec);
    if (!erg || erg->is_trash_view || !rec || !rec->header_frame)
        return;

    Widget anchor = nullptr;
    if (fmd)
    {
        if (fmd->show_current_dir && rec->current_directory_frame &&
            XtIsManaged(rec->current_directory_frame))
        {
            anchor = rec->current_directory_frame;
        }
        else if (fmd->show_iconic_path && rec->iconic_path_da &&
                 XtIsManaged(rec->iconic_path_da))
        {
            anchor = rec->iconic_path_da;
        }
        else if (fmd->show_shelf && rec->shelf_frame &&
                 XtIsManaged(rec->shelf_frame))
        {
            anchor = (rec->shelf_separator && XtIsManaged(rec->shelf_separator))
                         ? rec->shelf_separator
                         : rec->shelf_frame;
        }
    }

    Boolean show_tabs = (erg->tab_count > 1);
    Boolean show_search = erg->search_bar_open;

    if (show_tabs && rec->tab_bar_form)
    {
        if (anchor)
        {
            XtVaSetValues(rec->tab_bar_form,
                          XmNtopAttachment, XmATTACH_WIDGET,
                          XmNtopWidget, anchor,
                          XmNtopOffset, 2,
                          nullptr);
        }
        else
        {
            XtVaSetValues(rec->tab_bar_form,
                          XmNtopAttachment, XmATTACH_FORM,
                          XmNtopOffset, 2,
                          nullptr);
        }
        if (!XtIsManaged(rec->tab_bar_form))
            XtManageChild(rec->tab_bar_form);
        anchor = rec->tab_bar_form;
    }
    else if (rec->tab_bar_form && XtIsManaged(rec->tab_bar_form))
    {
        XtUnmanageChild(rec->tab_bar_form);
    }

    if (show_search && rec->search_bar_form)
    {
        if (anchor)
        {
            XtVaSetValues(rec->search_bar_form,
                          XmNtopAttachment, XmATTACH_WIDGET,
                          XmNtopWidget, anchor,
                          XmNtopOffset, 2,
                          nullptr);
        }
        else
        {
            XtVaSetValues(rec->search_bar_form,
                          XmNtopAttachment, XmATTACH_FORM,
                          XmNtopOffset, 2,
                          nullptr);
        }
        if (!XtIsManaged(rec->search_bar_form))
            XtManageChild(rec->search_bar_form);
    }
    else if (rec->search_bar_form && XtIsManaged(rec->search_bar_form))
    {
        XtUnmanageChild(rec->search_bar_form);
    }

    Boolean any_header = show_tabs || show_search ||
                         (fmd && (fmd->show_shelf || fmd->show_iconic_path || fmd->show_current_dir));
    if (any_header && !XtIsManaged(rec->header_frame))
        XtManageChild(rec->header_frame);
    else if (!any_header && XtIsManaged(rec->header_frame))
        XtUnmanageChild(rec->header_frame);
}

static void
set_work_area_mode(DtfileWindowErg *erg, Boolean show_search_results)
{
    if (!erg || !erg->rec || !erg->rec->scroll_window || !erg->rec->search_results_form)
        return;

    erg->search_results_visible = show_search_results;
    if (show_search_results)
    {
        if (!XtIsManaged(erg->rec->search_results_form))
            XtManageChild(erg->rec->search_results_form);
        if (XtIsManaged(erg->rec->scroll_window))
            XtUnmanageChild(erg->rec->scroll_window);
    }
    else
    {
        if (!XtIsManaged(erg->rec->scroll_window))
            XtManageChild(erg->rec->scroll_window);
        if (XtIsManaged(erg->rec->search_results_form))
            XtUnmanageChild(erg->rec->search_results_form);
    }
}

void
DtfileErgonomicsPublishState(FileMgrData *fmd)
{
    DtfileWindowErg *erg = find_erg_by_fmd(fmd);
    if (!fmd && erg)
        fmd = erg->fmd;

    Widget ref_w = (erg && erg->rec && erg->rec->shell) ? erg->rec->shell : toplevel;
    if (!ref_w || !XtDisplay(ref_w))
        return;

    Display *dpy = XtDisplay(ref_w);
    Atom prop = XInternAtom(dpy, "_DT_FILEMGR_ERGONOMICS_STATE", False);
    Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);

    int tabs = (erg && erg->tab_count > 0) ? erg->tab_count : 1;
    int active_tab = erg ? erg->active_tab : 0;
    int can_back = 0;
    int can_fwd = 0;
    if (erg && active_tab >= 0 && active_tab < erg->tab_count)
    {
        can_back = (erg->tabs[active_tab].hist_idx > 0) ? 1 : 0;
        can_fwd = (erg->tabs[active_tab].hist_idx + 1 < erg->tabs[active_tab].hist_len) ? 1 : 0;
    }

    Boolean clip_cut = False;
    int clip_cnt = 0;
    Boolean clip_owned = DtfileClipboardHasLocalOwnership(&clip_cut, &clip_cnt);

    const char *sel0 = "";
    int sel_count = 0;
    if (fmd && fmd->selected_file_count > 0 && fmd->selection_list &&
        fmd->selection_list[0] && fmd->selection_list[0]->file_data &&
        fmd->selection_list[0]->file_data->file_name)
    {
        sel_count = fmd->selected_file_count;
        sel0 = fmd->selection_list[0]->file_data->file_name;
    }

    int screen_w = DisplayWidth(dpy, DefaultScreen(dpy));
    int screen_h = DisplayHeight(dpy, DefaultScreen(dpy));
    int win_rx = 0, win_ry = 0;
    int first_dir_rx = 0, first_dir_ry = 0;
    const char *first_dir_name = "";
    if (erg && erg->rec && erg->rec->file_window &&
        XtIsRealized(erg->rec->file_window) && XtWindow(erg->rec->file_window) != None)
    {
        Window child_ret = None;
        (void)XTranslateCoordinates(dpy, XtWindow(erg->rec->file_window),
                                    DefaultRootWindow(dpy), 24, 24,
                                    &win_rx, &win_ry, &child_ret);

        if (fmd && fmd->directory_count > 0 && fmd->directory_set && fmd->directory_set[0])
        {
            DirectorySet *ds = fmd->directory_set[0];
            for (int i = 0; i < ds->file_count; ++i)
            {
                FileViewData *fvd = ds->file_view_data[i];
                if (fvd && fvd->displayed && fvd->widget && XtIsManaged(fvd->widget) &&
                    fvd->file_data && fvd->file_data->file_name &&
                    (fvd->file_data->physical_type == DtDIRECTORY || fvd->file_data->is_subdir) &&
                    strcmp(fvd->file_data->file_name, ".") != 0 &&
                    strcmp(fvd->file_data->file_name, "..") != 0)
                {
                    int cx = (int)XtX(fvd->widget) + (int)(XtWidth(fvd->widget) / 2);
                    int cy = (int)XtY(fvd->widget) + (int)(XtHeight(fvd->widget) / 2);
                    (void)XTranslateCoordinates(dpy, XtWindow(erg->rec->file_window),
                                                DefaultRootWindow(dpy), cx, cy,
                                                &first_dir_rx, &first_dir_ry, &child_ret);
                    first_dir_name = fvd->file_data->file_name;
                    break;
                }
            }
        }
    }

    char buf[2560];
    int len = snprintf(
        buf, sizeof(buf),
        "tabs=%d\n"
        "active_tab=%d\n"
        "can_back=%d\n"
        "can_fwd=%d\n"
        "hidden=%d\n"
        "renaming=%d\n"
        "search_open=%d\n"
        "search_results_visible=%d\n"
        "search_state=%s\n"
        "search_hits=%d\n"
        "search_scanned=%llu\n"
        "clip_owned=%d\n"
        "clip_cut=%d\n"
        "clip_count=%d\n"
        "undo_depth=%d\n"
        "undo_top=%s\n"
        "selected=%d\n"
        "sel0=%s\n"
        "screen_w=%d\n"
        "screen_h=%d\n"
        "win_rx=%d\n"
        "win_ry=%d\n"
        "first_dir_rx=%d\n"
        "first_dir_ry=%d\n"
        "first_dir_name=%s\n"
        "dir=%s\n",
        tabs,
        active_tab,
        can_back,
        can_fwd,
        (fmd && fmd->show_hid_enabled) ? 1 : 0,
        (fmd && fmd->renaming != nullptr) ? 1 : 0,
        (erg && erg->search_bar_open) ? 1 : 0,
        (erg && erg->search_results_visible) ? 1 : 0,
        (erg && erg->search_state_str[0]) ? erg->search_state_str : "idle",
        erg ? erg->hit_count : 0,
        (unsigned long long)(erg ? erg->search_scanned : 0),
        clip_owned ? 1 : 0,
        clip_cut ? 1 : 0,
        clip_cnt,
        DtfileUndoDepth(),
        DtfileUndoTopSummary(),
        sel_count,
        sel0,
        screen_w,
        screen_h,
        win_rx,
        win_ry,
        first_dir_rx,
        first_dir_ry,
        first_dir_name,
        (fmd && fmd->current_directory) ? fmd->current_directory : "");

    if (len > 0)
    {
        if (len >= (int)sizeof(buf))
            len = (int)sizeof(buf) - 1;
        XChangeProperty(dpy, DefaultRootWindow(dpy), prop, utf8, 8,
                        PropModeReplace, (const unsigned char *)buf, len);
        XFlush(dpy);
    }
}

static void
navigate_fmd_to(DtfileWindowErg *erg, const char *host, const char *dir, Boolean suppress_hist)
{
    if (!erg || !erg->fmd || !dir || !*dir)
        return;

    if (erg->search_results_visible)
        set_work_area_mode(erg, False);

    char *h_copy = XtNewString((char *)((host && *host) ? host : home_host_name));
    char *d_copy = XtNewString((char *)dir);

    Boolean prev = erg->suppress_history_push;
    erg->suppress_history_push = suppress_hist;
    ShowNewDirectory(erg->fmd, h_copy, d_copy);
    erg->suppress_history_push = prev;

    XtFree(h_copy);
    XtFree(d_copy);

    refresh_tab_bar_widgets(erg);
    DtfileErgonomicsSyncHeaderLayout(erg->rec, erg->fmd);
    DtfileErgonomicsPublishState(erg->fmd);
}

void
DtfileErgonomicsOnDirectoryShown(FileMgrData *fmd)
{
    DtfileWindowErg *erg = find_erg_by_fmd(fmd);
    if (!erg || erg->is_trash_view || !fmd || !fmd->current_directory)
        return;

    g_last_active_erg = erg;
    erg->fmd = fmd;

    if (erg->tab_count <= 0)
    {
        init_tab_state(&erg->tabs[0], fmd->host, fmd->current_directory);
        erg->tab_count = 1;
        erg->active_tab = 0;
    }
    else if (!erg->suppress_history_push)
    {
        push_tab_history(&erg->tabs[erg->active_tab], fmd->host, fmd->current_directory);
    }
    else
    {
        DtfileTabState *t = &erg->tabs[erg->active_tab];
        XtFree(t->host);
        XtFree(t->dir);
        t->host = XtNewString(fmd->host ? fmd->host : home_host_name);
        t->dir = XtNewString(fmd->current_directory);
    }

    refresh_tab_bar_widgets(erg);
    DtfileErgonomicsSyncHeaderLayout(erg->rec, fmd);
    DtfileErgonomicsPublishState(fmd);
}

void
DtfileErgonomicsOnIconsLaidOut(FileMgrData *fmd)
{
    DtfileWindowErg *erg = find_erg_by_fmd(fmd);
    if (!erg || erg->is_trash_view || !fmd)
        return;

    if (erg->pending_select_name && fmd->directory_count > 0 && fmd->directory_set)
    {
        DirectorySet *ds = fmd->directory_set[0];
        if (ds && ds->file_count > 0)
        {
            for (int i = 0; i < ds->file_count; ++i)
            {
                FileViewData *fvd = ds->file_view_data[i];
                if (fvd && fvd->file_data && fvd->file_data->file_name &&
                    strcmp(fvd->file_data->file_name, erg->pending_select_name) == 0 &&
                    !fvd->filtered)
                {
                    DeselectAllFiles(fmd);
                    SelectFile(fmd, fvd);
                    if (fvd->widget && erg->rec && erg->rec->scroll_window)
                    {
                        XmScrollVisible(erg->rec->scroll_window, fvd->widget, 12, 12);
                    }
                    XtFree(erg->pending_select_name);
                    erg->pending_select_name = nullptr;
                    break;
                }
            }
        }
    }

    DtfileErgonomicsPublishState(fmd);
}

static void
switch_to_tab_index(DtfileWindowErg *erg, int idx)
{
    if (!erg || !erg->fmd || idx < 0 || idx >= erg->tab_count)
        return;

    if (idx == erg->active_tab && erg->fmd->current_directory &&
        strcmp(erg->fmd->current_directory, erg->tabs[idx].dir) == 0)
    {
        refresh_tab_bar_widgets(erg);
        DtfileErgonomicsPublishState(erg->fmd);
        return;
    }

    erg->active_tab = idx;
    navigate_fmd_to(erg, erg->tabs[idx].host, erg->tabs[idx].dir, True);
}

Boolean
DtfileTabOpen(FileMgrData *fmd, const char *host, const char *dir, Boolean switch_to)
{
    DtfileWindowErg *erg = find_erg_by_fmd(fmd);
    if (!erg || erg->is_trash_view || !fmd || fmd->toolbox)
        return False;
    if (erg->tab_count >= DTFILE_MAX_TABS)
        return False;

    const char *h = (host && *host) ? host : (fmd->host ? fmd->host : home_host_name);
    const char *d = (dir && *dir) ? dir : (fmd->current_directory ? fmd->current_directory : "/");

    /* Resolve Clean canonical path without trailing slash (unless "/") */
    char clean_dir[PATH_MAX];
    snprintf(clean_dir, sizeof(clean_dir), "%s", d);
    size_t clen = strlen(clean_dir);
    while (clen > 1 && clean_dir[clen - 1] == '/')
        clean_dir[--clen] = '\0';

    int new_idx = erg->tab_count++;
    init_tab_state(&erg->tabs[new_idx], h, clean_dir);

    refresh_tab_bar_widgets(erg);
    DtfileErgonomicsSyncHeaderLayout(erg->rec, fmd);

    if (switch_to)
        switch_to_tab_index(erg, new_idx);
    else
        DtfileErgonomicsPublishState(fmd);
    return True;
}

static Boolean
close_tab_at_index(DtfileWindowErg *erg, int idx)
{
    if (!erg || !erg->fmd || idx < 0 || idx >= erg->tab_count)
        return False;

    if (erg->tab_count <= 1)
    {
        DialogData *dd = _DtGetInstanceData((XtPointer)erg->rec);
        if (dd)
        {
            CloseView(dd);
            return True;
        }
        return False;
    }

    free_tab_state(&erg->tabs[idx]);
    for (int i = idx; i + 1 < erg->tab_count; ++i)
        erg->tabs[i] = erg->tabs[i + 1];
    memset(&erg->tabs[erg->tab_count - 1], 0, sizeof(DtfileTabState));
    erg->tab_count--;

    if (erg->active_tab >= erg->tab_count)
        erg->active_tab = erg->tab_count - 1;
    else if (idx < erg->active_tab)
        erg->active_tab--;

    refresh_tab_bar_widgets(erg);
    DtfileErgonomicsSyncHeaderLayout(erg->rec, erg->fmd);
    navigate_fmd_to(erg, erg->tabs[erg->active_tab].host,
                    erg->tabs[erg->active_tab].dir, True);
    return True;
}

Boolean
DtfileTabCloseActive(FileMgrData *fmd)
{
    DtfileWindowErg *erg = find_erg_by_fmd(fmd);
    if (!erg || erg->is_trash_view)
        return False;
    return close_tab_at_index(erg, erg->active_tab);
}

Boolean
DtfileTabCycle(FileMgrData *fmd, int delta)
{
    DtfileWindowErg *erg = find_erg_by_fmd(fmd);
    if (!erg || erg->is_trash_view || erg->tab_count <= 1)
        return False;
    int next = (erg->active_tab + delta) % erg->tab_count;
    if (next < 0)
        next += erg->tab_count;
    switch_to_tab_index(erg, next);
    return True;
}

Boolean
DtfileHistoryStep(FileMgrData *fmd, int delta)
{
    DtfileWindowErg *erg = find_erg_by_fmd(fmd);
    if (!erg || erg->is_trash_view || !fmd || erg->tab_count <= 0)
        return False;

    DtfileTabState *t = &erg->tabs[erg->active_tab];
    int target = t->hist_idx + delta;
    if (target < 0 || target >= t->hist_len)
        return False;

    t->hist_idx = target;
    navigate_fmd_to(erg, t->hist_hosts[target], t->hist_dirs[target], True);
    return True;
}

Boolean
DtfileGoParent(FileMgrData *fmd)
{
    DtfileWindowErg *erg = find_erg_by_fmd(fmd);
    if (!erg || erg->is_trash_view || !fmd || !fmd->current_directory)
        return False;
    if (strcmp(fmd->current_directory, "/") == 0)
        return False;
    if (fmd->restricted_directory &&
        strcmp(fmd->current_directory, fmd->restricted_directory) == 0)
    {
        return False;
    }

    char parent[PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", fmd->current_directory);
    size_t len = strlen(parent);
    while (len > 1 && parent[len - 1] == '/')
        parent[--len] = '\0';
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent)
        snprintf(parent, sizeof(parent), "/");
    else
        *slash = '\0';

    navigate_fmd_to(erg, fmd->host, parent, False);
    return True;
}

Boolean
DtfileStartInlineRename(FileMgrData *fmd)
{
    if (!fmd || fmd == trashFileMgrData || fmd->toolbox || !fmd->file_mgr_rec)
        return False;

    FileMgrRec *rec = (FileMgrRec *)fmd->file_mgr_rec;
    FileViewData *target = nullptr;
    if (fmd->selected_file_count == 1 && fmd->selection_list)
    {
        target = fmd->selection_list[0];
    }
    else if (fmd->selected_file_count == 0 && rec->file_window)
    {
        Widget fw = XmGetFocusWidget(rec->shell);
        if (fw && fmd->directory_count > 0 && fmd->directory_set)
        {
            for (int d = 0; d < fmd->directory_count && !target; ++d)
            {
                DirectorySet *ds = fmd->directory_set[d];
                for (int i = 0; ds && i < ds->file_count; ++i)
                {
                    if (ds->file_view_data[i]->widget == fw)
                    {
                        target = ds->file_view_data[i];
                        break;
                    }
                }
            }
        }
    }

    if (!target || !target->file_data || !target->widget ||
        strcmp(target->file_data->file_name, ".") == 0 ||
        strcmp(target->file_data->file_name, "..") == 0)
    {
        return False;
    }

    UnpostTextField(fmd);
    CreateNameChangeDialog(target->widget, target, (XtPointer)fmd, 0);
    Widget tf = XtNameToWidget(XtParent(target->widget), "nameChangeT");
    if (tf && XmIsTextField(tf))
    {
        XmTextPosition end_pos = XmTextFieldGetLastPosition(tf);
        if (end_pos > 0)
            XmTextFieldSetSelection(tf, 0, end_pos, CurrentTime);
    }
    DtfileErgonomicsPublishState(fmd);
    return True;
}

Boolean
DtfileToggleHiddenFiles(FileMgrData *fmd)
{
    if (!fmd || fmd == trashFileMgrData || !fmd->filter_active || !fmd->file_mgr_rec)
        return False;

    FileMgrRec *rec = (FileMgrRec *)fmd->file_mgr_rec;
    FilterData *filter_data = (FilterData *)fmd->filter_active->data;
    if (!filter_data)
        return False;

    Boolean next_state = !fmd->show_hid_enabled;
    filter_data->show_hidden = next_state;
    fmd->show_hid_enabled = next_state;

    FileMgrRedisplayFiles(rec, fmd, False);
    DtfileErgonomicsPublishState(fmd);
    return True;
}

/* --- Asynchronous In-Window Search (ck-core-fs-lib) --- */

static void
clear_search_hits(DtfileWindowErg *erg)
{
    if (!erg)
        return;
    for (int i = 0; i < erg->hit_count; ++i)
    {
        XtFree(erg->hits[i].rel_path);
        XtFree(erg->hits[i].abs_path);
    }
    XtFree((char *)erg->hits);
    erg->hits = nullptr;
    erg->hit_count = 0;
    erg->hit_cap = 0;
    erg->search_scanned = 0;
    if (erg->search_list)
        XmListDeleteAllItems(erg->search_list);
}

static void
set_search_status_text(DtfileWindowErg *erg, const char *text)
{
    if (!erg || !erg->search_status_lbl)
        return;
    XmString xs = XmStringCreateLocalized((char *)(text ? text : ""));
    XtVaSetValues(erg->search_status_lbl, XmNlabelString, xs, nullptr);
    XmStringFree(xs);
}

static Boolean
parse_size_bound(const char *s, uint64_t *out_bytes)
{
    if (!s)
        return False;
    while (*s && isspace((unsigned char)*s))
        ++s;
    if (!*s)
        return False;

    errno = 0;
    char *end = nullptr;
    unsigned long long base = strtoull(s, &end, 10);
    if (errno != 0 || end == s)
        return False;
    while (*end && isspace((unsigned char)*end))
        ++end;

    uint64_t mult = 1;
    if (*end)
    {
        char c = (char)toupper((unsigned char)end[0]);
        if (c == 'K') mult = 1024ULL;
        else if (c == 'M') mult = 1024ULL * 1024ULL;
        else if (c == 'G') mult = 1024ULL * 1024ULL * 1024ULL;
        else if (c == 'B' && end[1] == '\0') mult = 1ULL;
        else return False;
    }
    *out_bytes = (uint64_t)base * mult;
    return True;
}

static Boolean
parse_date_bound(const char *s, int64_t *out_sec)
{
    if (!s)
        return False;
    while (*s && isspace((unsigned char)*s))
        ++s;
    if (!*s)
        return False;

    if (*s == '@')
    {
        char *end = nullptr;
        long long epoch = strtoll(s + 1, &end, 10);
        if (end == s + 1)
            return False;
        *out_sec = (int64_t)epoch;
        return True;
    }

    int y = 0, m = 0, d = 0;
    if (sscanf(s, "%d-%d-%d", &y, &m, &d) != 3)
        return False;
    if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31)
        return False;

    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = y - 1900;
    tmv.tm_mon = m - 1;
    tmv.tm_mday = d;
    tmv.tm_isdst = -1;
    time_t t = mktime(&tmv);
    if (t == (time_t)-1)
        return False;
    *out_sec = (int64_t)t;
    return True;
}

static void
format_human_bytes(uint64_t b, char *out, size_t cap)
{
    if (b < 1024ULL)
        snprintf(out, cap, "%llu B", (unsigned long long)b);
    else if (b < 1024ULL * 1024ULL)
        snprintf(out, cap, "%.1f KB", (double)b / 1024.0);
    else if (b < 1024ULL * 1024ULL * 1024ULL)
        snprintf(out, cap, "%.1f MB", (double)b / (1024.0 * 1024.0));
    else
        snprintf(out, cap, "%.2f GB", (double)b / (1024.0 * 1024.0 * 1024.0));
}

#if DTFILE_SYNC_FEATURE_ENABLED
static void
drain_new_search_hits(DtfileWindowErg *erg, uint64_t available)
{
    if (!erg || !erg->search_job || available <= (uint64_t)erg->hit_count)
        return;

    ckfes_hit_v1 page[64];
    uint64_t offset = (uint64_t)erg->hit_count;

    while (offset < available)
    {
        uint32_t got = 0;
        if (ckfes_page_v1_get(erg->search_job, offset, page, 64, &got) != CKFE_API_OK || got == 0)
            break;

        XmString *batch_items = (XmString *)XtCalloc(got, sizeof(XmString));
        int batch_len = 0;

        for (uint32_t i = 0; i < got; ++i)
        {
            const ckfes_hit_v1 *h = &page[i];
            uint64_t req_units = 0;
            uint32_t enc = 0;
            char *rel = (char *)XtMalloc((Cardinal)(h->relative_units + 1));
            if (ckfes_path_copy_v1(erg->search_job, h->hit_index, rel,
                                   h->relative_units, &req_units, &enc) != CKFE_API_OK)
            {
                XtFree(rel);
                continue;
            }
            rel[h->relative_units] = '\0';

            const char *root = erg->search_root_dir ? erg->search_root_dir : "/";
            size_t alen = strlen(root) + 1 + strlen(rel) + 1;
            char *abs_p = (char *)XtMalloc((Cardinal)alen);
            snprintf(abs_p, alen, "%s/%s", strcmp(root, "/") == 0 ? "" : root, rel);

            if (erg->hit_count >= erg->hit_cap)
            {
                erg->hit_cap = (erg->hit_cap == 0) ? 64 : erg->hit_cap * 2;
                erg->hits = (DtfileSearchHitItem *)XtRealloc(
                    (char *)erg->hits, (Cardinal)(erg->hit_cap * sizeof(DtfileSearchHitItem)));
            }
            Boolean is_dir = (h->kind == CKFE_OBJECT_DIRECTORY);
            erg->hits[erg->hit_count].rel_path = rel;
            erg->hits[erg->hit_count].abs_path = abs_p;
            erg->hits[erg->hit_count].is_dir = is_dir;
            erg->hit_count++;

            char size_buf[32] = "";
            if (!is_dir)
                format_human_bytes(h->logical_size, size_buf, sizeof(size_buf));

            char date_buf[32] = "";
            if (h->has_modified)
            {
                time_t ts = (time_t)h->modified.seconds;
                struct tm tmv;
                if (localtime_r(&ts, &tmv))
                    strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %H:%M", &tmv);
            }

            char line[PATH_MAX + 96];
            if (is_dir)
                snprintf(line, sizeof(line), "[Folder]  %s   (%s)", rel, date_buf);
            else
                snprintf(line, sizeof(line), "[File]    %s   (%s, %s)", rel, size_buf, date_buf);

            batch_items[batch_len++] = XmStringCreateLocalized(line);
        }

        if (batch_len > 0 && erg->search_list)
        {
            XmListAddItemsUnselected(erg->search_list, batch_items, batch_len, 0);
            if (erg->hit_count == batch_len)
                XmListSelectPos(erg->search_list, 1, False);
            for (int k = 0; k < batch_len; ++k)
                XmStringFree(batch_items[k]);
        }
        XtFree((char *)batch_items);
        offset += got;
    }
}

static void
search_poll_timer_cb(XtPointer client_data, XtIntervalId *id)
{
    (void)id;
    DtfileWindowErg *erg = (DtfileWindowErg *)client_data;
    erg->search_timer = 0;

    if (!erg->search_job)
        return;

    ckfes_snapshot_v1 snap;
    memset(&snap, 0, sizeof(snap));
    snap.version = CKFES_VERSION;
    snap.struct_size = sizeof(snap);

    if (ckfes_poll_v1(erg->search_job, &snap) != CKFE_API_OK)
        return;

    erg->search_scanned = snap.scanned_entries;
    drain_new_search_hits(erg, snap.matched_entries);

    char status_msg[512];
    const char *root = erg->search_root_dir ? erg->search_root_dir : "";

    if (snap.state == CKFES_RUNNING)
    {
        snprintf(erg->search_state_str, sizeof(erg->search_state_str), "running");
        snprintf(status_msg, sizeof(status_msg),
                 "Searching %s ... %d match%s (%llu scanned)",
                 root, erg->hit_count, erg->hit_count == 1 ? "" : "es",
                 (unsigned long long)snap.scanned_entries);
        set_search_status_text(erg, status_msg);
        DtfileErgonomicsPublishState(erg->fmd);

        erg->search_timer = XtAppAddTimeOut(
            XtWidgetToApplicationContext(erg->rec->shell),
            DTFILE_SEARCH_POLL_MS, search_poll_timer_cb, (XtPointer)erg);
        return;
    }

    XtSetSensitive(erg->search_stop_btn, False);
    if (snap.state == CKFES_COMPLETE)
    {
        snprintf(erg->search_state_str, sizeof(erg->search_state_str), "complete");
        snprintf(status_msg, sizeof(status_msg),
                 "Search complete in %s: %d match%s (%llu scanned%s)",
                 root, erg->hit_count, erg->hit_count == 1 ? "" : "es",
                 (unsigned long long)snap.scanned_entries,
                 snap.truncated ? ", limit reached" : "");
    }
    else if (snap.state == CKFES_CANCELLED)
    {
        snprintf(erg->search_state_str, sizeof(erg->search_state_str), "cancelled");
        snprintf(status_msg, sizeof(status_msg),
                 "Search stopped in %s: %d match%s (%llu scanned)",
                 root, erg->hit_count, erg->hit_count == 1 ? "" : "es",
                 (unsigned long long)snap.scanned_entries);
    }
    else
    {
        snprintf(erg->search_state_str, sizeof(erg->search_state_str), "failed");
        snprintf(status_msg, sizeof(status_msg),
                 "Search failed in %s (code %lld)",
                 root, (long long)snap.failure_code);
    }

    set_search_status_text(erg, status_msg);
    retire_search_job(erg->rec->shell, erg->search_job);
    erg->search_job = nullptr;
    DtfileErgonomicsPublishState(erg->fmd);
}
#endif

void
DtfileSearchCancelActive(FileMgrData *fmd)
{
    DtfileWindowErg *erg = find_erg_by_fmd(fmd);
    if (!erg)
        return;

    if (erg->search_timer != 0)
    {
        XtRemoveTimeOut(erg->search_timer);
        erg->search_timer = 0;
    }

#if DTFILE_SYNC_FEATURE_ENABLED
    if (erg->search_job != nullptr)
    {
        retire_search_job(erg->rec ? erg->rec->shell : toplevel, erg->search_job);
        erg->search_job = nullptr;
        snprintf(erg->search_state_str, sizeof(erg->search_state_str), "cancelled");
        if (erg->search_stop_btn)
            XtSetSensitive(erg->search_stop_btn, False);
        char msg[384];
        snprintf(msg, sizeof(msg), "Search stopped: %d match%s",
                 erg->hit_count, erg->hit_count == 1 ? "" : "es");
        set_search_status_text(erg, msg);
        DtfileErgonomicsPublishState(erg->fmd);
    }
#endif
}

Boolean
DtfileSearchStartCustom(
    FileMgrData *fmd,
    const char *name_query,
    const char *content_query,
    const char *min_size_str,
    const char *max_size_str,
    const char *after_date_str,
    const char *before_date_str,
    Boolean recursive,
    Boolean case_sensitive)
{
    DtfileWindowErg *erg = find_erg_by_fmd(fmd);
    if (!erg || erg->is_trash_view || !fmd || !fmd->current_directory)
        return False;

#if !DTFILE_SYNC_FEATURE_ENABLED
    (void)name_query; (void)content_query; (void)min_size_str; (void)max_size_str;
    (void)after_date_str; (void)before_date_str; (void)recursive; (void)case_sensitive;
    return False;
#else
    DtfileSearchCancelActive(fmd);
    clear_search_hits(erg);

    if (!erg->search_bar_open)
    {
        erg->search_bar_open = True;
        DtfileErgonomicsSyncHeaderLayout(erg->rec, fmd);
    }

    if (name_query) XmTextFieldSetString(erg->name_tf, (char *)name_query);
    if (content_query) XmTextFieldSetString(erg->content_tf, (char *)content_query);
    if (min_size_str) XmTextFieldSetString(erg->min_size_tf, (char *)min_size_str);
    if (max_size_str) XmTextFieldSetString(erg->max_size_tf, (char *)max_size_str);
    if (after_date_str) XmTextFieldSetString(erg->after_date_tf, (char *)after_date_str);
    if (before_date_str) XmTextFieldSetString(erg->before_date_tf, (char *)before_date_str);
    XmToggleButtonGadgetSetState(erg->recursive_tb, recursive, False);
    XmToggleButtonGadgetSetState(erg->case_tb, case_sensitive, False);

    XtFree(erg->search_root_dir);
    erg->search_root_dir = XtNewString(fmd->current_directory);

    ckfes_options_v1 opts;
    ckfes_options_init_v1(&opts);
    opts.root_path.data = erg->search_root_dir;
    opts.root_path.units = strlen(erg->search_root_dir);
    opts.root_path.encoding = CKFES_POSIX_BYTES;

    const char *nq = name_query ? name_query : "";
    const char *cq = content_query ? content_query : "";
    if (*nq)
    {
        opts.name_query.data = nq;
        opts.name_query.units = strlen(nq);
        opts.name_query.encoding = CKFES_POSIX_BYTES;
        if (strchr(nq, '*') || strchr(nq, '?') || strchr(nq, '['))
            opts.name_mode = CKFES_NAME_GLOB;
        else
            opts.name_mode = CKFES_NAME_SUBSTRING;
    }
    else
    {
        opts.name_mode = CKFES_NAME_NONE;
    }

    if (*cq)
    {
        opts.content_query.data = cq;
        opts.content_query.units = strlen(cq);
        opts.content_query.encoding = CKFES_POSIX_BYTES;
    }

    opts.case_sensitive = case_sensitive ? 1 : 0;
    opts.include_hidden = fmd->show_hid_enabled ? 1 : 0;
    opts.max_depth = recursive ? 1024 : 1;
    opts.max_results = 10000;

    uint64_t min_b = 0, max_b = 0;
    if (parse_size_bound(min_size_str, &min_b))
    {
        opts.has_minimum_size = 1;
        opts.minimum_size = min_b;
    }
    if (parse_size_bound(max_size_str, &max_b))
    {
        opts.has_maximum_size = 1;
        opts.maximum_size = max_b;
    }

    int64_t after_s = 0, before_s = 0;
    if (parse_date_bound(after_date_str, &after_s))
    {
        opts.has_modified_not_before = 1;
        opts.modified_not_before.seconds = after_s;
        opts.modified_not_before.nanoseconds = 0;
    }
    if (parse_date_bound(before_date_str, &before_s))
    {
        opts.has_modified_before = 1;
        opts.modified_before.seconds = before_s;
        opts.modified_before.nanoseconds = 0;
    }

    set_work_area_mode(erg, True);

    ckfe_error err;
    memset(&err, 0, sizeof(err));
    if (ckfes_start_v1(&opts, &erg->search_job, &err) != CKFE_API_OK || !erg->search_job)
    {
        snprintf(erg->search_state_str, sizeof(erg->search_state_str), "failed");
        set_search_status_text(erg, "Invalid search filter or busy search engine.");
        XtSetSensitive(erg->search_stop_btn, False);
        DtfileErgonomicsPublishState(fmd);
        return False;
    }

    snprintf(erg->search_state_str, sizeof(erg->search_state_str), "running");
    XtSetSensitive(erg->search_stop_btn, True);
    set_search_status_text(erg, "Searching...");
    DtfileErgonomicsPublishState(fmd);

    erg->search_timer = XtAppAddTimeOut(
        XtWidgetToApplicationContext(erg->rec->shell),
        25, search_poll_timer_cb, (XtPointer)erg);
    return True;
#endif
}

static void
start_search_from_ui(DtfileWindowErg *erg)
{
    if (!erg || !erg->fmd)
        return;

    char *nq = XmTextFieldGetString(erg->name_tf);
    char *cq = XmTextFieldGetString(erg->content_tf);
    char *mins = XmTextFieldGetString(erg->min_size_tf);
    char *maxs = XmTextFieldGetString(erg->max_size_tf);
    char *afts = XmTextFieldGetString(erg->after_date_tf);
    char *befs = XmTextFieldGetString(erg->before_date_tf);
    Boolean rec = XmToggleButtonGadgetGetState(erg->recursive_tb);
    Boolean cs = XmToggleButtonGadgetGetState(erg->case_tb);

    (void)DtfileSearchStartCustom(erg->fmd, nq, cq, mins, maxs, afts, befs, rec, cs);

    XtFree(nq);
    XtFree(cq);
    XtFree(mins);
    XtFree(maxs);
    XtFree(afts);
    XtFree(befs);
}

void
DtfileSearchOpenBar(FileMgrData *fmd)
{
    DtfileWindowErg *erg = find_erg_by_fmd(fmd);
    if (!erg || erg->is_trash_view || !fmd)
        return;

    erg->search_bar_open = True;
    DtfileErgonomicsSyncHeaderLayout(erg->rec, fmd);
    if (erg->name_tf)
        XmProcessTraversal(erg->name_tf, XmTRAVERSE_CURRENT);
    DtfileErgonomicsPublishState(fmd);
}

void
DtfileSearchCloseBar(FileMgrData *fmd)
{
    DtfileWindowErg *erg = find_erg_by_fmd(fmd);
    if (!erg || !fmd)
        return;

    DtfileSearchCancelActive(fmd);
    erg->search_bar_open = False;
    set_work_area_mode(erg, False);
    DtfileErgonomicsSyncHeaderLayout(erg->rec, fmd);
    if (erg->rec && erg->rec->current_directory && XtIsManaged(erg->rec->current_directory))
        XmProcessTraversal(erg->rec->current_directory, XmTRAVERSE_CURRENT);
    DtfileErgonomicsPublishState(fmd);
}

static int
get_selected_hit_index(DtfileWindowErg *erg)
{
    if (!erg || !erg->search_list || erg->hit_count <= 0)
        return -1;
    int *pos_list = nullptr;
    int pos_cnt = 0;
    int idx = -1;
    if (XmListGetSelectedPos(erg->search_list, &pos_list, &pos_cnt) && pos_cnt > 0)
    {
        if (pos_list[0] >= 1 && pos_list[0] <= erg->hit_count)
            idx = pos_list[0] - 1;
        XtFree((char *)pos_list);
    }
    return idx;
}

Boolean
DtfileSearchRevealHitIndex(FileMgrData *fmd, int hit_idx)
{
    DtfileWindowErg *erg = find_erg_by_fmd(fmd);
    if (!erg || !erg->fmd || hit_idx < 0 || hit_idx >= erg->hit_count)
        return False;

    char *abs_copy = XtNewString(erg->hits[hit_idx].abs_path);
    DtfileSearchCancelActive(erg->fmd);
    set_work_area_mode(erg, False);

    char parent[PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", abs_copy);
    char *slash = strrchr(parent, '/');
    const char *leaf = abs_copy;
    if (slash)
    {
        leaf = slash + 1;
        if (slash == parent)
            snprintf(parent, sizeof(parent), "/");
        else
            *slash = '\0';
    }
    else
    {
        snprintf(parent, sizeof(parent), "/");
    }

    /* Ensure hidden item can be revealed if matched */
    if (leaf[0] == '.' && !erg->fmd->show_hid_enabled && erg->fmd->filter_active)
    {
        FilterData *fd = (FilterData *)erg->fmd->filter_active->data;
        if (fd)
        {
            fd->show_hidden = True;
            erg->fmd->show_hid_enabled = True;
        }
    }

    if (!erg->fmd->current_directory || strcmp(erg->fmd->current_directory, parent) != 0)
        navigate_fmd_to(erg, erg->fmd->host, parent, False);
    else
        FileMgrRedisplayFiles(erg->rec, erg->fmd, False);

    Boolean selected = False;
    if (erg->fmd->directory_count > 0 && erg->fmd->directory_set)
    {
        DeselectAllFiles(erg->fmd);
        DirectorySet *ds = erg->fmd->directory_set[0];
        for (int i = 0; ds && i < ds->file_count; ++i)
        {
            FileViewData *fvd = ds->file_view_data[i];
            if (fvd && fvd->file_data && fvd->file_data->file_name &&
                strcmp(fvd->file_data->file_name, leaf) == 0 && !fvd->filtered)
            {
                SelectFile(erg->fmd, fvd);
                if (fvd->widget && erg->rec->scroll_window)
                {
                    XmScrollVisible(erg->rec->scroll_window, fvd->widget, 12, 12);
                    XmProcessTraversal(fvd->widget, XmTRAVERSE_CURRENT);
                }
                selected = True;
                break;
            }
        }
    }

    if (!selected)
    {
        XtFree(erg->pending_select_name);
        erg->pending_select_name = XtNewString(leaf);
    }
    else
    {
        XtFree(erg->pending_select_name);
        erg->pending_select_name = nullptr;
    }

    XtFree(abs_copy);
    DtfileErgonomicsPublishState(erg->fmd);
    return True;
}

static void
open_hit_index(DtfileWindowErg *erg, int hit_idx, Boolean in_new_tab)
{
    if (!erg || !erg->fmd || hit_idx < 0 || hit_idx >= erg->hit_count)
        return;

    const DtfileSearchHitItem *item = &erg->hits[hit_idx];
    if (item->is_dir)
    {
        char *dcopy = XtNewString(item->abs_path);
        set_work_area_mode(erg, False);
        if (in_new_tab)
            (void)DtfileTabOpen(erg->fmd, erg->fmd->host, dcopy, True);
        else
            navigate_fmd_to(erg, erg->fmd->host, dcopy, False);
        XtFree(dcopy);
        return;
    }

    if (in_new_tab)
    {
        char parent[PATH_MAX];
        snprintf(parent, sizeof(parent), "%s", item->abs_path);
        char *slash = strrchr(parent, '/');
        if (!slash || slash == parent) snprintf(parent, sizeof(parent), "/");
        else *slash = '\0';
        (void)DtfileTabOpen(erg->fmd, erg->fmd->host, parent, True);
        return;
    }

    DtActionArg arg;
    memset(&arg, 0, sizeof(arg));
    arg.argClass = DtACTION_FILE;
    arg.u.file.name = item->abs_path;
    DtActionInvoke(erg->rec->shell, "Open", &arg, 1,
                   nullptr, nullptr, erg->fmd->current_directory,
                   True, nullptr, nullptr);
}

/* --- Widget Callbacks & Event Handlers --- */

static void
TabBackBtnCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w; (void)call_data;
    DtfileWindowErg *erg = (DtfileWindowErg *)client_data;
    if (erg && erg->fmd)
        (void)DtfileHistoryStep(erg->fmd, -1);
}

static void
TabFwdBtnCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w; (void)call_data;
    DtfileWindowErg *erg = (DtfileWindowErg *)client_data;
    if (erg && erg->fmd)
        (void)DtfileHistoryStep(erg->fmd, +1);
}

static void
TabNewBtnCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w; (void)call_data;
    DtfileWindowErg *erg = (DtfileWindowErg *)client_data;
    if (erg && erg->fmd)
        (void)DtfileTabOpen(erg->fmd, erg->fmd->host, erg->fmd->current_directory, True);
}

static void
TabSelectBtnCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)call_data;
    DtfileWindowErg *erg = (DtfileWindowErg *)client_data;
    if (!erg)
        return;
    for (int i = 0; i < erg->tab_count; ++i)
    {
        if (erg->tab_select_btns[i] == w)
        {
            switch_to_tab_index(erg, i);
            return;
        }
    }
}

static void
TabCloseBtnCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)call_data;
    DtfileWindowErg *erg = (DtfileWindowErg *)client_data;
    if (!erg)
        return;
    for (int i = 0; i < erg->tab_count; ++i)
    {
        if (erg->tab_close_btns[i] == w)
        {
            (void)close_tab_at_index(erg, i);
            return;
        }
    }
}

static void
SearchStartBtnCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w; (void)call_data;
    start_search_from_ui((DtfileWindowErg *)client_data);
}

static void
SearchStopBtnCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w; (void)call_data;
    DtfileWindowErg *erg = (DtfileWindowErg *)client_data;
    if (erg && erg->fmd)
        DtfileSearchCancelActive(erg->fmd);
}

static void
SearchCloseBtnCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w; (void)call_data;
    DtfileWindowErg *erg = (DtfileWindowErg *)client_data;
    if (erg && erg->fmd)
        DtfileSearchCloseBar(erg->fmd);
}

static void
SearchOpenHitCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w; (void)call_data;
    DtfileWindowErg *erg = (DtfileWindowErg *)client_data;
    int idx = get_selected_hit_index(erg);
    if (idx >= 0)
        open_hit_index(erg, idx, False);
}

static void
SearchOpenNewTabHitCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w; (void)call_data;
    DtfileWindowErg *erg = (DtfileWindowErg *)client_data;
    int idx = get_selected_hit_index(erg);
    if (idx >= 0)
        open_hit_index(erg, idx, True);
}

static void
SearchRevealHitCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w; (void)call_data;
    DtfileWindowErg *erg = (DtfileWindowErg *)client_data;
    int idx = get_selected_hit_index(erg);
    if (idx >= 0 && erg->fmd)
        (void)DtfileSearchRevealHitIndex(erg->fmd, idx);
}

static void
SearchBackToFolderCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w; (void)call_data;
    DtfileWindowErg *erg = (DtfileWindowErg *)client_data;
    if (erg)
    {
        set_work_area_mode(erg, False);
        DtfileErgonomicsPublishState(erg->fmd);
    }
}

static Boolean
is_descendant_of(Widget child, Widget ancestor)
{
    if (!child || !ancestor)
        return False;
    for (Widget p = child; p != nullptr; p = XtParent(p))
    {
        if (p == ancestor)
            return True;
    }
    return False;
}

static void
WindowKeyEventHandler(Widget w, XtPointer client_data, XEvent *event, Boolean *cont)
{
    (void)w;
    if (event->type != KeyPress)
        return;

    DtfileWindowErg *erg = (DtfileWindowErg *)client_data;
    if (!erg || erg->is_trash_view)
        return;

    XKeyEvent *kev = &event->xkey;

    FileMgrData *fmd = resolve_rec_fmd(erg->rec);
    if (!fmd)
        return;
    erg->fmd = fmd;
    g_last_active_erg = erg;

    KeySym ks = XLookupKeysym(kev, 0);
    KeySym ks_shift = XLookupKeysym(kev, 1);
    Boolean ctrl = (kev->state & ControlMask) != 0;
    Boolean alt = (kev->state & Mod1Mask) != 0;
    Boolean shift = (kev->state & ShiftMask) != 0;

    Widget focus_w = XmGetFocusWidget(erg->rec->shell);
    Boolean focus_is_active_text =
        (focus_w != nullptr && XtIsManaged(focus_w) &&
         (XmIsTextField(focus_w) || XmIsText(focus_w)) &&
         (!is_descendant_of(focus_w, erg->rec->search_bar_form) || erg->search_bar_open));
    Boolean in_text = (fmd->renaming != nullptr) || focus_is_active_text;
    Boolean in_search = (focus_w != nullptr &&
                         (is_descendant_of(focus_w, erg->rec->search_bar_form) ||
                          is_descendant_of(focus_w, erg->rec->search_results_form)));

    /* Escape closes search bar / results when not renaming a file */
    if (ks == XK_Escape && fmd->renaming == nullptr &&
        (erg->search_bar_open || erg->search_results_visible))
    {
        DtfileSearchCloseBar(fmd);
        *cont = False;
        return;
    }

    /* Tab & search-bar shortcuts work everywhere except during active file rename */
    if (fmd->renaming == nullptr)
    {
        if (ctrl && !alt && (ks == XK_t || ks == XK_T))
        {
            (void)DtfileTabOpen(fmd, fmd->host, fmd->current_directory, True);
            *cont = False;
            return;
        }
        if (ctrl && !alt && (ks == XK_w || ks == XK_W))
        {
            (void)DtfileTabCloseActive(fmd);
            *cont = False;
            return;
        }
        if (ctrl && !alt && (ks == XK_f || ks == XK_F))
        {
            DtfileSearchOpenBar(fmd);
            *cont = False;
            return;
        }
        if (ctrl && !alt && (ks == XK_Page_Down || ks == XK_KP_Page_Down ||
                             (!shift && ks == XK_Tab)))
        {
            (void)DtfileTabCycle(fmd, +1);
            *cont = False;
            return;
        }
        if (ctrl && !alt && (ks == XK_Page_Up || ks == XK_KP_Page_Up ||
                             ks == XK_ISO_Left_Tab || ks_shift == XK_ISO_Left_Tab ||
                             (shift && ks == XK_Tab)))
        {
            (void)DtfileTabCycle(fmd, -1);
            *cont = False;
            return;
        }
    }

    /* Do not hijack text editing keys inside text fields */
    if (in_text)
    {
        (void)in_search;
        return;
    }

    if (alt && !ctrl && (ks == XK_Left || ks == XK_KP_Left))
    {
        (void)DtfileHistoryStep(fmd, -1);
        *cont = False;
        return;
    }
    if (alt && !ctrl && (ks == XK_Right || ks == XK_KP_Right))
    {
        (void)DtfileHistoryStep(fmd, +1);
        *cont = False;
        return;
    }
    if (alt && !ctrl && (ks == XK_Up || ks == XK_KP_Up))
    {
        (void)DtfileGoParent(fmd);
        *cont = False;
        return;
    }
    if (!ctrl && !alt && ks == XK_F2)
    {
        if (DtfileStartInlineRename(fmd))
            *cont = False;
        return;
    }
    if (ctrl && !alt && (ks == XK_h || ks == XK_H))
    {
        if (DtfileToggleHiddenFiles(fmd))
            *cont = False;
        return;
    }
    if (ctrl && !alt && (ks == XK_c || ks == XK_C))
    {
        (void)DtfileClipboardCopyOrCutSelection(fmd, False, kev->time);
        *cont = False;
        return;
    }
    if (ctrl && !alt && (ks == XK_x || ks == XK_X))
    {
        (void)DtfileClipboardCopyOrCutSelection(fmd, True, kev->time);
        *cont = False;
        return;
    }
    if (ctrl && !alt && (ks == XK_v || ks == XK_V))
    {
        DtfileClipboardPasteIntoView(fmd, kev->time);
        *cont = False;
        return;
    }
    if (ctrl && !alt && (ks == XK_z || ks == XK_Z))
    {
        (void)DtfileUndoExecuteLast(fmd);
        *cont = False;
        return;
    }
}

Boolean
DtfileErgonomicsFilterKeyEvent(XEvent *event)
{
    if (!event || event->type != KeyPress || !event->xany.display)
        return False;

    Widget ev_w = XtWindowToWidget(event->xany.display, event->xany.window);
    if (!ev_w)
        return False;

    Widget top_shell = ev_w;
    while (XtParent(top_shell) != nullptr)
        top_shell = XtParent(top_shell);

    for (int i = 0; i < g_window_count; ++i)
    {
        if (g_windows[i].rec && g_windows[i].rec->shell == top_shell)
        {
            Boolean cont = True;
            WindowKeyEventHandler(ev_w, (XtPointer)&g_windows[i], event, &cont);
            return !cont;
        }
    }
    return False;
}

static FileViewData *
hit_test_file_view_icon(FileMgrData *fmd, int x, int y)
{
    if (!fmd || fmd->directory_count <= 0 || !fmd->directory_set)
        return nullptr;

    for (int d = 0; d < fmd->directory_count; ++d)
    {
        DirectorySet *ds = fmd->directory_set[d];
        for (int i = 0; ds && i < ds->file_count; ++i)
        {
            FileViewData *fvd = ds->file_view_data[i];
            if (!fvd || !fvd->displayed || !fvd->widget || !XtIsManaged(fvd->widget))
                continue;
            int gx = (int)XtX(fvd->widget);
            int gy = (int)XtY(fvd->widget);
            int gw = (int)XtWidth(fvd->widget);
            int gh = (int)XtHeight(fvd->widget);
            if (x >= gx && x < gx + gw && y >= gy && y < gy + gh)
                return fvd;
        }
    }
    return nullptr;
}

static Boolean
resolve_dir_fvd_path(FileMgrData *fmd, FileViewData *fvd, char *out_path, size_t cap)
{
    if (!fmd || !fvd || !fvd->file_data || !fvd->file_data->file_name)
        return False;
    if (fvd->file_data->physical_type != DtDIRECTORY && !fvd->file_data->is_subdir)
        return False;

    const char *fname = fvd->file_data->file_name;
    if (strcmp(fname, ".") == 0)
        return False;

    DirectorySet *ds = (DirectorySet *)fvd->directory_set;
    const char *base_dir = (ds && ds->name) ? ds->name : fmd->current_directory;
    if (!base_dir || !*base_dir)
        return False;

    if (strcmp(fname, "..") == 0)
    {
        if (strcmp(base_dir, "/") == 0)
            return False;
        snprintf(out_path, cap, "%s", base_dir);
        char *slash = strrchr(out_path, '/');
        if (!slash || slash == out_path)
            snprintf(out_path, cap, "/");
        else
            *slash = '\0';
        return True;
    }

    return snprintf(out_path, cap, "%s/%s",
                    strcmp(base_dir, "/") == 0 ? "" : base_dir, fname) < (int)cap;
}

static void
FileWindowButtonHandler(Widget w, XtPointer client_data, XEvent *event, Boolean *cont)
{
    (void)w;
    DtfileWindowErg *erg = (DtfileWindowErg *)client_data;
    if (!erg || erg->is_trash_view)
        return;

    FileMgrData *fmd = resolve_rec_fmd(erg->rec);
    if (!fmd)
        return;
    erg->fmd = fmd;
    g_last_active_erg = erg;

    if (event->type == ButtonPress)
    {
        XButtonEvent *bev = &event->xbutton;
        if (bev->button == Button8)
        {
            (void)DtfileHistoryStep(fmd, -1);
            *cont = False;
            return;
        }
        if (bev->button == Button9)
        {
            (void)DtfileHistoryStep(fmd, +1);
            *cont = False;
            return;
        }
        if (bev->button == Button2)
        {
            erg->b2_armed = True;
            erg->b2_press_x = bev->x;
            erg->b2_press_y = bev->y;
        }
    }
    else if (event->type == ButtonRelease)
    {
        XButtonEvent *bev = &event->xbutton;
        if (bev->button == Button2 && erg->b2_armed)
        {
            erg->b2_armed = False;
            int dx = abs(bev->x - erg->b2_press_x);
            int dy = abs(bev->y - erg->b2_press_y);
            if (dx < 6 && dy < 6)
            {
                FileViewData *fvd = hit_test_file_view_icon(fmd, bev->x, bev->y);
                char target_dir[PATH_MAX];
                if (fvd && resolve_dir_fvd_path(fmd, fvd, target_dir, sizeof(target_dir)))
                {
                    (void)DtfileTabOpen(fmd, fmd->host, target_dir, True);
                }
            }
        }
    }
}

static void
ShellStructureHandler(Widget w, XtPointer client_data, XEvent *event, Boolean *cont)
{
    (void)w;
    (void)cont;
    if (event->type != MapNotify && event->type != ConfigureNotify)
        return;
    DtfileWindowErg *erg = (DtfileWindowErg *)client_data;
    if (erg && erg->fmd)
        DtfileErgonomicsPublishState(erg->fmd);
}

/* --- UI Construction --- */

static void
dtfile_try_apply_icon(Widget btn, const char *icon_name, const char *tooltip)
{
    if (!btn || !icon_name || !icon_name[0])
        return;
    Widget host = XmIsGadget(btn) ? XtParent(btn) : btn;
    if (!host)
        return;

    const char *env_dir = getenv("CK_TOOLBAR_ICON_DIR");
    const char *roots[4];
    int nroots = 0;
    if (env_dir && env_dir[0])
        roots[nroots++] = env_dir;
    roots[nroots++] = "/usr/dt/appconfig/icons/C/toolbar";
    roots[nroots++] = "/usr/local/share/ck-core/icons/toolbar";
    roots[nroots++] = "/usr/share/ck-core/icons/toolbar";

    const int sizes[] = { 16, 24, 32, 48 };
    char found[PATH_MAX] = "";
    for (int r = 0; r < nroots && !found[0]; ++r)
    {
        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s)
        {
            char cand[PATH_MAX];
            snprintf(cand, sizeof(cand), "%s/%dx%d/%s.png",
                     roots[r], sizes[s], sizes[s], icon_name);
            if (access(cand, R_OK) == 0)
            {
                snprintf(found, sizeof(found), "%s", cand);
                break;
            }
        }
    }
    if (!found[0])
        return;

    Pixel fg = 0, bg = 0, arm = 0;
    XtVaGetValues(host, XmNforeground, &fg, XmNbackground, &bg, nullptr);
    arm = bg;
    if (!XmIsGadget(btn))
        XtVaGetValues(btn, XmNarmColor, &arm, nullptr);

    int depth = DefaultDepthOfScreen(XtScreen(host));
    Pixmap p_norm = XmGetScaledPixmap(host, (String)found, fg, bg, depth, 0.0);
    if (p_norm != XmUNSPECIFIED_PIXMAP)
    {
        XtVaSetValues(btn, XmNlabelType, XmPIXMAP, XmNlabelPixmap, p_norm, nullptr);
        Pixmap p_arm = XmGetScaledPixmap(host, (String)found, fg, arm, depth, 0.0);
        if (p_arm != XmUNSPECIFIED_PIXMAP)
            XtVaSetValues(btn, XmNarmPixmap, p_arm, nullptr);
        if (tooltip && tooltip[0] && !XmIsGadget(btn))
        {
            XmString tip = XmStringCreateLocalized((String)tooltip);
            XtVaSetValues(btn, XmNtoolTipString, tip, nullptr);
            XmStringFree(tip);
        }
    }
}

void
DtfileErgonomicsAttachWindow(FileMgrRec *rec, Boolean is_trash_view)
{
    if (!rec || find_erg_by_rec(rec) != nullptr)
        return;
    if (g_window_count >= DTFILE_MAX_ERG_WINDOWS)
        return;

    DtfileWindowErg *erg = &g_windows[g_window_count++];
    memset(erg, 0, sizeof(*erg));
    erg->rec = rec;
    erg->is_trash_view = is_trash_view;
    snprintf(erg->search_state_str, sizeof(erg->search_state_str), "idle");

    if (is_trash_view)
        return;

    /* 1. Tab Bar Form inside header_frame (unmanaged until tab_count > 1) */
    rec->tab_bar_form = XmCreateForm(rec->header_frame, "tab_bar_form", nullptr, 0);
    XtVaSetValues(rec->tab_bar_form,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNmarginWidth, 2,
                  XmNmarginHeight, 2,
                  nullptr);

    erg->tab_sep = XmCreateSeparatorGadget(rec->tab_bar_form, "tab_sep", nullptr, 0);
    XtVaSetValues(erg->tab_sep,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  nullptr);
    XtManageChild(erg->tab_sep);

    XmString xs_back = XmStringCreateLocalized("<");
    erg->back_btn = XmCreatePushButtonGadget(rec->tab_bar_form, "tab_back", nullptr, 0);
    XtVaSetValues(erg->back_btn,
                  XmNlabelString, xs_back,
                  XmNtopAttachment, XmATTACH_WIDGET,
                  XmNtopWidget, erg->tab_sep,
                  XmNtopOffset, 2,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNleftOffset, 2,
                  nullptr);
    XmStringFree(xs_back);
    dtfile_try_apply_icon(erg->back_btn, "go-previous", "Back (Alt+Left)");
    XtAddCallback(erg->back_btn, XmNactivateCallback, TabBackBtnCB, (XtPointer)erg);
    XtManageChild(erg->back_btn);

    XmString xs_fwd = XmStringCreateLocalized(">");
    erg->fwd_btn = XmCreatePushButtonGadget(rec->tab_bar_form, "tab_fwd", nullptr, 0);
    XtVaSetValues(erg->fwd_btn,
                  XmNlabelString, xs_fwd,
                  XmNtopAttachment, XmATTACH_WIDGET,
                  XmNtopWidget, erg->tab_sep,
                  XmNtopOffset, 2,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_WIDGET,
                  XmNleftWidget, erg->back_btn,
                  XmNleftOffset, 2,
                  nullptr);
    XmStringFree(xs_fwd);
    dtfile_try_apply_icon(erg->fwd_btn, "go-next", "Forward (Alt+Right)");
    XtAddCallback(erg->fwd_btn, XmNactivateCallback, TabFwdBtnCB, (XtPointer)erg);
    XtManageChild(erg->fwd_btn);

    XmString xs_new = XmStringCreateLocalized(" + ");
    erg->new_tab_btn = XmCreatePushButtonGadget(rec->tab_bar_form, "tab_new", nullptr, 0);
    XtVaSetValues(erg->new_tab_btn,
                  XmNlabelString, xs_new,
                  XmNtopAttachment, XmATTACH_WIDGET,
                  XmNtopWidget, erg->tab_sep,
                  XmNtopOffset, 2,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNrightOffset, 2,
                  nullptr);
    XmStringFree(xs_new);
    dtfile_try_apply_icon(erg->new_tab_btn, "tab-new", "New Tab (Ctrl+T)");
    XtAddCallback(erg->new_tab_btn, XmNactivateCallback, TabNewBtnCB, (XtPointer)erg);
    XtManageChild(erg->new_tab_btn);

    erg->tab_strip_rc = XmCreateRowColumn(rec->tab_bar_form, "tab_strip_rc", nullptr, 0);
    XtVaSetValues(erg->tab_strip_rc,
                  XmNorientation, XmHORIZONTAL,
                  XmNpacking, XmPACK_TIGHT,
                  XmNspacing, 2,
                  XmNmarginWidth, 2,
                  XmNmarginHeight, 0,
                  XmNtopAttachment, XmATTACH_WIDGET,
                  XmNtopWidget, erg->tab_sep,
                  XmNtopOffset, 2,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_WIDGET,
                  XmNleftWidget, erg->fwd_btn,
                  XmNleftOffset, 6,
                  XmNrightAttachment, XmATTACH_WIDGET,
                  XmNrightWidget, erg->new_tab_btn,
                  XmNrightOffset, 4,
                  nullptr);

    XmString xs_x = XmStringCreateLocalized("x");
    for (int i = 0; i < DTFILE_MAX_TABS; ++i)
    {
        char bname[24], cname[24];
        snprintf(bname, sizeof(bname), "tab_btn_%d", i);
        snprintf(cname, sizeof(cname), "tab_close_%d", i);
        erg->tab_select_btns[i] = XmCreatePushButton(erg->tab_strip_rc, bname, nullptr, 0);
        XtAddCallback(erg->tab_select_btns[i], XmNactivateCallback, TabSelectBtnCB, (XtPointer)erg);

        erg->tab_close_btns[i] = XmCreatePushButtonGadget(erg->tab_strip_rc, cname, nullptr, 0);
        XtVaSetValues(erg->tab_close_btns[i], XmNlabelString, xs_x, nullptr);
        dtfile_try_apply_icon(erg->tab_close_btns[i], "tab-close", "Close Tab (Ctrl+W)");
        XtAddCallback(erg->tab_close_btns[i], XmNactivateCallback, TabCloseBtnCB, (XtPointer)erg);
    }
    XmStringFree(xs_x);
    XtManageChild(erg->tab_strip_rc);

    /* 2. Collapsible Search Bar Form inside header_frame */
    rec->search_bar_form = XmCreateForm(rec->header_frame, "search_bar_form", nullptr, 0);
    XtVaSetValues(rec->search_bar_form,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNmarginWidth, 4,
                  XmNmarginHeight, 3,
                  nullptr);

    erg->search_sep = XmCreateSeparatorGadget(rec->search_bar_form, "search_sep", nullptr, 0);
    XtVaSetValues(erg->search_sep,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  nullptr);
    XtManageChild(erg->search_sep);

    Widget row1 = XmCreateRowColumn(rec->search_bar_form, "search_row1", nullptr, 0);
    XtVaSetValues(row1,
                  XmNorientation, XmHORIZONTAL,
                  XmNpacking, XmPACK_TIGHT,
                  XmNspacing, 4,
                  XmNmarginHeight, 1,
                  XmNtopAttachment, XmATTACH_WIDGET,
                  XmNtopWidget, erg->search_sep,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  nullptr);

    XmString xs;
    xs = XmStringCreateLocalized("Name:");
    Widget l_name = XmCreateLabelGadget(row1, "l_name", nullptr, 0);
    XtVaSetValues(l_name, XmNlabelString, xs, nullptr);
    XmStringFree(xs);
    XtManageChild(l_name);

    erg->name_tf = XmCreateTextField(row1, "search_name_tf", nullptr, 0);
    XtVaSetValues(erg->name_tf, XmNcolumns, 16, nullptr);
    XtAddCallback(erg->name_tf, XmNactivateCallback, SearchStartBtnCB, (XtPointer)erg);
    XtManageChild(erg->name_tf);

    xs = XmStringCreateLocalized("Text:");
    Widget l_text = XmCreateLabelGadget(row1, "l_text", nullptr, 0);
    XtVaSetValues(l_text, XmNlabelString, xs, nullptr);
    XmStringFree(xs);
    XtManageChild(l_text);

    erg->content_tf = XmCreateTextField(row1, "search_content_tf", nullptr, 0);
    XtVaSetValues(erg->content_tf, XmNcolumns, 14, nullptr);
    XtAddCallback(erg->content_tf, XmNactivateCallback, SearchStartBtnCB, (XtPointer)erg);
    XtManageChild(erg->content_tf);

    xs = XmStringCreateLocalized("Subfolders");
    erg->recursive_tb = XmCreateToggleButtonGadget(row1, "search_rec_tb", nullptr, 0);
    XtVaSetValues(erg->recursive_tb, XmNlabelString, xs, XmNset, True, nullptr);
    XmStringFree(xs);
    XtManageChild(erg->recursive_tb);

    xs = XmStringCreateLocalized("Case");
    erg->case_tb = XmCreateToggleButtonGadget(row1, "search_case_tb", nullptr, 0);
    XtVaSetValues(erg->case_tb, XmNlabelString, xs, XmNset, False, nullptr);
    XmStringFree(xs);
    XtManageChild(erg->case_tb);

    xs = XmStringCreateLocalized("Search");
    erg->search_go_btn = XmCreatePushButtonGadget(row1, "search_go", nullptr, 0);
    XtVaSetValues(erg->search_go_btn, XmNlabelString, xs, nullptr);
    XmStringFree(xs);
    dtfile_try_apply_icon(erg->search_go_btn, "edit-find", "Start Search");
    XtAddCallback(erg->search_go_btn, XmNactivateCallback, SearchStartBtnCB, (XtPointer)erg);
    XtManageChild(erg->search_go_btn);

    xs = XmStringCreateLocalized("Stop");
    erg->search_stop_btn = XmCreatePushButtonGadget(row1, "search_stop", nullptr, 0);
    XtVaSetValues(erg->search_stop_btn, XmNlabelString, xs, XmNsensitive, False, nullptr);
    XmStringFree(xs);
    dtfile_try_apply_icon(erg->search_stop_btn, "process-stop", "Stop Search");
    XtAddCallback(erg->search_stop_btn, XmNactivateCallback, SearchStopBtnCB, (XtPointer)erg);
    XtManageChild(erg->search_stop_btn);

    xs = XmStringCreateLocalized("Close");
    erg->search_close_btn = XmCreatePushButtonGadget(row1, "search_close", nullptr, 0);
    XtVaSetValues(erg->search_close_btn, XmNlabelString, xs, nullptr);
    XmStringFree(xs);
    dtfile_try_apply_icon(erg->search_close_btn, "window-close", "Close Search Bar");
    XtAddCallback(erg->search_close_btn, XmNactivateCallback, SearchCloseBtnCB, (XtPointer)erg);
    XtManageChild(erg->search_close_btn);

    XtManageChild(row1);

    Widget row2 = XmCreateRowColumn(rec->search_bar_form, "search_row2", nullptr, 0);
    XtVaSetValues(row2,
                  XmNorientation, XmHORIZONTAL,
                  XmNpacking, XmPACK_TIGHT,
                  XmNspacing, 4,
                  XmNmarginHeight, 1,
                  XmNtopAttachment, XmATTACH_WIDGET,
                  XmNtopWidget, row1,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  nullptr);

    xs = XmStringCreateLocalized("Min size:");
    Widget l_min = XmCreateLabelGadget(row2, "l_min", nullptr, 0);
    XtVaSetValues(l_min, XmNlabelString, xs, nullptr);
    XmStringFree(xs);
    XtManageChild(l_min);

    erg->min_size_tf = XmCreateTextField(row2, "search_min_tf", nullptr, 0);
    XtVaSetValues(erg->min_size_tf, XmNcolumns, 7, nullptr);
    XtAddCallback(erg->min_size_tf, XmNactivateCallback, SearchStartBtnCB, (XtPointer)erg);
    XtManageChild(erg->min_size_tf);

    xs = XmStringCreateLocalized("Max:");
    Widget l_max = XmCreateLabelGadget(row2, "l_max", nullptr, 0);
    XtVaSetValues(l_max, XmNlabelString, xs, nullptr);
    XmStringFree(xs);
    XtManageChild(l_max);

    erg->max_size_tf = XmCreateTextField(row2, "search_max_tf", nullptr, 0);
    XtVaSetValues(erg->max_size_tf, XmNcolumns, 7, nullptr);
    XtAddCallback(erg->max_size_tf, XmNactivateCallback, SearchStartBtnCB, (XtPointer)erg);
    XtManageChild(erg->max_size_tf);

    xs = XmStringCreateLocalized("Modified >= YYYY-MM-DD:");
    Widget l_aft = XmCreateLabelGadget(row2, "l_aft", nullptr, 0);
    XtVaSetValues(l_aft, XmNlabelString, xs, nullptr);
    XmStringFree(xs);
    XtManageChild(l_aft);

    erg->after_date_tf = XmCreateTextField(row2, "search_after_tf", nullptr, 0);
    XtVaSetValues(erg->after_date_tf, XmNcolumns, 10, nullptr);
    XtAddCallback(erg->after_date_tf, XmNactivateCallback, SearchStartBtnCB, (XtPointer)erg);
    XtManageChild(erg->after_date_tf);

    xs = XmStringCreateLocalized("<:");
    Widget l_bef = XmCreateLabelGadget(row2, "l_bef", nullptr, 0);
    XtVaSetValues(l_bef, XmNlabelString, xs, nullptr);
    XmStringFree(xs);
    XtManageChild(l_bef);

    erg->before_date_tf = XmCreateTextField(row2, "search_before_tf", nullptr, 0);
    XtVaSetValues(erg->before_date_tf, XmNcolumns, 10, nullptr);
    XtAddCallback(erg->before_date_tf, XmNactivateCallback, SearchStartBtnCB, (XtPointer)erg);
    XtManageChild(erg->before_date_tf);

    XtManageChild(row2);

    /* 3. Streaming Search Results Panel inside work_area_form */
    rec->search_results_form = XmCreateForm(rec->work_area_form, "search_results_form", nullptr, 0);
    XtVaSetValues(rec->search_results_form,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNmarginWidth, 4,
                  XmNmarginHeight, 4,
                  nullptr);

    xs = XmStringCreateLocalized("Ready to search.");
    erg->search_status_lbl = XmCreateLabelGadget(rec->search_results_form, "search_status", nullptr, 0);
    XtVaSetValues(erg->search_status_lbl,
                  XmNlabelString, xs,
                  XmNalignment, XmALIGNMENT_BEGINNING,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  nullptr);
    XmStringFree(xs);
    XtManageChild(erg->search_status_lbl);

    Widget res_actions = XmCreateRowColumn(rec->search_results_form, "res_actions", nullptr, 0);
    XtVaSetValues(res_actions,
                  XmNorientation, XmHORIZONTAL,
                  XmNspacing, 6,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  nullptr);

    xs = XmStringCreateLocalized("Open");
    erg->search_open_btn = XmCreatePushButtonGadget(res_actions, "res_open", nullptr, 0);
    XtVaSetValues(erg->search_open_btn, XmNlabelString, xs, nullptr);
    XmStringFree(xs);
    XtAddCallback(erg->search_open_btn, XmNactivateCallback, SearchOpenHitCB, (XtPointer)erg);
    XtManageChild(erg->search_open_btn);

    xs = XmStringCreateLocalized("Open in New Tab");
    erg->search_newtab_btn = XmCreatePushButtonGadget(res_actions, "res_newtab", nullptr, 0);
    XtVaSetValues(erg->search_newtab_btn, XmNlabelString, xs, nullptr);
    XmStringFree(xs);
    XtAddCallback(erg->search_newtab_btn, XmNactivateCallback, SearchOpenNewTabHitCB, (XtPointer)erg);
    XtManageChild(erg->search_newtab_btn);

    xs = XmStringCreateLocalized("Reveal in Folder");
    erg->search_reveal_btn = XmCreatePushButtonGadget(res_actions, "res_reveal", nullptr, 0);
    XtVaSetValues(erg->search_reveal_btn, XmNlabelString, xs, nullptr);
    XmStringFree(xs);
    XtAddCallback(erg->search_reveal_btn, XmNactivateCallback, SearchRevealHitCB, (XtPointer)erg);
    XtManageChild(erg->search_reveal_btn);

    xs = XmStringCreateLocalized("Back to Folder View");
    erg->search_back_btn = XmCreatePushButtonGadget(res_actions, "res_back", nullptr, 0);
    XtVaSetValues(erg->search_back_btn, XmNlabelString, xs, nullptr);
    XmStringFree(xs);
    XtAddCallback(erg->search_back_btn, XmNactivateCallback, SearchBackToFolderCB, (XtPointer)erg);
    XtManageChild(erg->search_back_btn);

    XtManageChild(res_actions);

    Arg largs[8];
    int ln = 0;
    XtSetArg(largs[ln], XmNselectionPolicy, XmBROWSE_SELECT); ln++;
    XtSetArg(largs[ln], XmNlistSizePolicy, XmCONSTANT); ln++;
    erg->search_list = XmCreateScrolledList(rec->search_results_form, "search_hits_list", largs, ln);
    XtVaSetValues(XtParent(erg->search_list),
                  XmNtopAttachment, XmATTACH_WIDGET,
                  XmNtopWidget, erg->search_status_lbl,
                  XmNtopOffset, 4,
                  XmNbottomAttachment, XmATTACH_WIDGET,
                  XmNbottomWidget, res_actions,
                  XmNbottomOffset, 4,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  nullptr);
    XtAddCallback(erg->search_list, XmNdefaultActionCallback, SearchOpenHitCB, (XtPointer)erg);
    XtManageChild(erg->search_list);

    /* 4. Install Structure & Pointer Event Handlers (KeyPress is filtered in Main.c) */
    XtAddEventHandler(rec->shell, StructureNotifyMask, False,
                      ShellStructureHandler, (XtPointer)erg);
    XtAddEventHandler(rec->file_window, ButtonPressMask | ButtonReleaseMask, False,
                      FileWindowButtonHandler, (XtPointer)erg);
}

void
DtfileErgonomicsBindView(FileMgrRec *rec, FileMgrData *fmd)
{
    DtfileWindowErg *erg = find_erg_by_rec(rec);
    if (!erg || erg->is_trash_view || !fmd)
        return;

    DtfileSearchCancelActive(fmd);
    clear_search_hits(erg);
    for (int i = 0; i < erg->tab_count; ++i)
        free_tab_state(&erg->tabs[i]);

    erg->fmd = fmd;
    erg->tab_count = 1;
    erg->active_tab = 0;
    init_tab_state(&erg->tabs[0], fmd->host, fmd->current_directory);
    erg->search_bar_open = False;
    snprintf(erg->search_state_str, sizeof(erg->search_state_str), "idle");
    set_work_area_mode(erg, False);

    g_last_active_erg = erg;
    refresh_tab_bar_widgets(erg);
    DtfileErgonomicsSyncHeaderLayout(rec, fmd);
    DtfileErgonomicsPublishState(fmd);
}

void
DtfileErgonomicsUnbindView(FileMgrRec *rec, FileMgrData *fmd)
{
    DtfileWindowErg *erg = find_erg_by_rec(rec);
    if (!erg)
        return;

    DtfileSearchCancelActive(fmd);
    clear_search_hits(erg);
    XtFree(erg->search_root_dir);
    erg->search_root_dir = nullptr;
    XtFree(erg->pending_select_name);
    erg->pending_select_name = nullptr;

    for (int i = 0; i < erg->tab_count; ++i)
        free_tab_state(&erg->tabs[i]);
    erg->tab_count = 0;
    erg->active_tab = 0;
    erg->search_bar_open = False;
    set_work_area_mode(erg, False);

    if (rec && rec->tab_bar_form && XtIsManaged(rec->tab_bar_form))
        XtUnmanageChild(rec->tab_bar_form);
    if (rec && rec->search_bar_form && XtIsManaged(rec->search_bar_form))
        XtUnmanageChild(rec->search_bar_form);

    if (erg->fmd == fmd)
        erg->fmd = nullptr;
    if (g_last_active_erg == erg)
        g_last_active_erg = nullptr;
}

/* --- Menu & Popup Callbacks --- */

static FileMgrData *
menu_widget_to_fmd_erg(Widget w)
{
    Widget mbar = XmGetPostedFromWidget(XtParent(w));
    if (!mbar)
        return nullptr;
    FileMgrRec *rec = nullptr;
    XtVaGetValues(mbar, XmNuserData, &rec, nullptr);
    return resolve_rec_fmd(rec);
}

void
DtfileNewTabMenuCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)client_data; (void)call_data;
    FileMgrData *fmd = menu_widget_to_fmd_erg(w);
    if (fmd)
        (void)DtfileTabOpen(fmd, fmd->host, fmd->current_directory, True);
}

void
DtfileCloseTabMenuCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)client_data; (void)call_data;
    FileMgrData *fmd = menu_widget_to_fmd_erg(w);
    if (fmd)
        (void)DtfileTabCloseActive(fmd);
}

void
DtfileSearchWindowMenuCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)client_data; (void)call_data;
    FileMgrData *fmd = menu_widget_to_fmd_erg(w);
    if (fmd)
        DtfileSearchOpenBar(fmd);
}

void
DtfileOpenNewTabPopupCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)call_data;
    FileMgrRec *rec = nullptr;
    XtVaGetValues(XtParent(w), XmNuserData, &rec, nullptr);
    FileMgrData *fmd = resolve_rec_fmd(rec);
    FileViewData *fvd = (FileViewData *)client_data;
    if (!fmd || !fvd)
        return;

    char target_dir[PATH_MAX];
    if (resolve_dir_fvd_path(fmd, fvd, target_dir, sizeof(target_dir)))
        (void)DtfileTabOpen(fmd, fmd->host, target_dir, True);
}

/* --- Test Command Bridge (_DT_FILEMGR_CMD when DTFILE_ERGONOMICS_TEST=1) --- */

static void
extract_kv_line(const char *cmd, const char *key, char *out, size_t cap)
{
    out[0] = '\0';
    size_t klen = strlen(key);
    const char *p = cmd;
    while (*p)
    {
        const char *eol = strchr(p, '\n');
        size_t llen = eol ? (size_t)(eol - p) : strlen(p);
        if (llen > klen + 1 && strncmp(p, key, klen) == 0 && p[klen] == '=')
        {
            size_t vlen = llen - (klen + 1);
            if (vlen >= cap) vlen = cap - 1;
            memcpy(out, p + klen + 1, vlen);
            out[vlen] = '\0';
            return;
        }
        if (!eol)
            break;
        p = eol + 1;
    }
}

static void
root_cmd_poll_cb(XtPointer client_data, XtIntervalId *id)
{
    (void)client_data; (void)id;
    g_cmd_timer = 0;
    if (!g_cmd_toplevel || !XtDisplay(g_cmd_toplevel))
        return;

    Display *dpy = XtDisplay(g_cmd_toplevel);
    Atom prop = XInternAtom(dpy, "_DT_FILEMGR_CMD", False);
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = nullptr;

    if (XGetWindowProperty(dpy, DefaultRootWindow(dpy), prop, 0, 1024, True,
                           AnyPropertyType, &actual_type, &actual_format,
                           &nitems, &bytes_after, &data) == Success && data != nullptr)
    {
        if (actual_format == 8 && nitems > 0)
        {
            char *cmd = (char *)XtMalloc((Cardinal)(nitems + 1));
            memcpy(cmd, data, nitems);
            cmd[nitems] = '\0';

            DtfileWindowErg *erg = g_last_active_erg;
            if (!erg && g_window_count > 0)
                erg = &g_windows[0];
            FileMgrData *fmd = erg ? resolve_rec_fmd(erg->rec) : nullptr;
            if (erg && fmd)
            {
                erg->fmd = fmd;
                char op[64];
                extract_kv_line(cmd, "op", op, sizeof(op));
                if (strcmp(op, "search") == 0)
                {
                    char nq[256], cq[256], mins[64], maxs[64], afts[64], befs[64], recs[16], css[16];
                    extract_kv_line(cmd, "name", nq, sizeof(nq));
                    extract_kv_line(cmd, "content", cq, sizeof(cq));
                    extract_kv_line(cmd, "min_size", mins, sizeof(mins));
                    extract_kv_line(cmd, "max_size", maxs, sizeof(maxs));
                    extract_kv_line(cmd, "after", afts, sizeof(afts));
                    extract_kv_line(cmd, "before", befs, sizeof(befs));
                    extract_kv_line(cmd, "recursive", recs, sizeof(recs));
                    extract_kv_line(cmd, "case", css, sizeof(css));
                    (void)DtfileSearchStartCustom(
                        fmd, nq, cq, mins, maxs, afts, befs,
                        recs[0] != '0', css[0] == '1');
                }
                else if (strcmp(op, "search_cancel") == 0)
                {
                    DtfileSearchCancelActive(fmd);
                }
                else if (strcmp(op, "search_reveal") == 0)
                {
                    char idxs[32];
                    extract_kv_line(cmd, "index", idxs, sizeof(idxs));
                    (void)DtfileSearchRevealHitIndex(fmd, atoi(idxs));
                }
                else if (strcmp(op, "select") == 0)
                {
                    char fname[256];
                    extract_kv_line(cmd, "file", fname, sizeof(fname));
                    if (fname[0])
                    {
                        Boolean found = False;
                        if (fmd->directory_count > 0 && fmd->directory_set)
                        {
                            DeselectAllFiles(fmd);
                            DirectorySet *ds = fmd->directory_set[0];
                            for (int i = 0; ds && i < ds->file_count; ++i)
                            {
                                FileViewData *fvd = ds->file_view_data[i];
                                if (fvd && fvd->file_data && fvd->file_data->file_name &&
                                    strcmp(fvd->file_data->file_name, fname) == 0 && !fvd->filtered)
                                {
                                    SelectFile(fmd, fvd);
                                    found = True;
                                    break;
                                }
                            }
                        }
                        XtFree(erg->pending_select_name);
                        erg->pending_select_name = found ? nullptr : XtNewString(fname);
                        DtfileErgonomicsPublishState(fmd);
                    }
                }
                else if (strcmp(op, "trash_selected") == 0)
                {
                    if (fmd->selected_file_count > 0)
                    {
                        char **origs = (char **)XtCalloc((Cardinal)fmd->selected_file_count, sizeof(char *));
                        int v = 0;
                        for (int i = 0; i < fmd->selected_file_count; ++i)
                        {
                            FileViewData *fvd = fmd->selection_list[i];
                            if (fvd && fvd->file_data && fvd->file_data->file_name)
                            {
                                char full[PATH_MAX];
                                snprintf(full, sizeof(full), "%s/%s",
                                         strcmp(fmd->current_directory, "/") == 0 ? "" : fmd->current_directory,
                                         fvd->file_data->file_name);
                                origs[v++] = XtNewString(full);
                            }
                        }
                        if (v > 0)
                        {
                            DtfileUndoRecordTrashBatch((const char *const *)origs, v);
                            if (TrashIsInitialized())
                            {
                                DtfileTrashBackendMovePaths((const char *const *)origs, v);
                            }
                            else
                            {
                                for (int i = 0; i < v; ++i)
                                {
                                    const char *slash = strrchr(origs[i], '/');
                                    const char *bname = (slash && slash[1]) ? (slash + 1) : origs[i];
                                    char *payload_out = nullptr;
                                    if (DtfileXdgTrashMoveItem(origs[i], bname, trash_dir, &payload_out))
                                        XtFree(payload_out);
                                }
                                UpdateDirectory(nullptr, fmd->host, fmd->current_directory);
                            }
                        }
                        DtfileFreeStringArray(origs, v);
                        DtfileErgonomicsPublishState(fmd);
                    }
                }
                else if (strcmp(op, "navigate") == 0)
                {
                    char ndir[PATH_MAX];
                    extract_kv_line(cmd, "dir", ndir, sizeof(ndir));
                    if (ndir[0] == '/')
                        navigate_fmd_to(erg, fmd->host, ndir, False);
                }
                else if (strcmp(op, "publish") == 0)
                {
                    if (erg->rec && erg->rec->shell)
                        XmUpdateDisplay(erg->rec->shell);
                    DtfileErgonomicsPublishState(fmd);
                }
            }
            XtFree(cmd);
        }
        XFree(data);
    }

    g_cmd_timer = XtAppAddTimeOut(
        XtWidgetToApplicationContext(g_cmd_toplevel), 50, root_cmd_poll_cb, nullptr);
}

void
DtfileErgonomicsInitRootCmd(Widget toplevel_widget)
{
    g_cmd_toplevel = toplevel_widget;
    if (getenv("DTFILE_ERGONOMICS_TEST") != nullptr && g_cmd_timer == 0 && toplevel_widget)
    {
        g_cmd_timer = XtAppAddTimeOut(
            XtWidgetToApplicationContext(toplevel_widget), 50, root_cmd_poll_cb, nullptr);
    }
}
