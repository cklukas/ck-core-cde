/*
 * CDE - Common Desktop Environment
 *
 * portal_dialogs.c: Native Motif FileChooser (XmFileSelectionDialog + dtfile
 * favorites + ck-core-fs-lib validation), AppChooser (DtAction + .desktop),
 * Wallpaper (PNG/PPM/XPM -> CDE .pm backdrop + f.change_backdrop), and
 * GlobalShortcuts (persistent ~/.config/ck-core/global-shortcuts.conf +
 * live dtwm-wayland shortcut registration & Activated/Deactivated signals).
 */

#define _GNU_SOURCE
#include "dtportal.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <Dt/Action.h>
#include <Dt/Dt.h>
#include <Xm/DialogS.h>
#include <Xm/FileSB.h>
#include <Xm/Form.h>
#include <Xm/Label.h>
#include <Xm/List.h>
#include <Xm/MessageB.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
#include <Xm/SeparatoG.h>
#include <Xm/TextF.h>
#include <Xm/ToggleB.h>
#include <cairo/cairo.h>

#ifdef HAVE_CK_CORE_FS
#include <ck_file_engine.h>
#endif

static void mkdir_p_local(const char *dir) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static bool validate_directory_usable(const char *path) {
    if (!path || !path[0]) return false;
#ifdef HAVE_CK_CORE_FS
    ckfe_engine *eng = NULL;
    if (ckfe_engine_create(&eng) == CKFE_API_OK && eng) {
        ckfe_error err;
        ckfe_error_init(&err);
        ckfe_directory *dir = NULL;
        ckfe_api_status st = ckfe_directory_open_utf8(eng, path, 1, &dir, &err);
        if (dir) ckfe_directory_destroy(dir);
        ckfe_error_free(&err);
        ckfe_engine_destroy(eng);
        return st == CKFE_API_OK;
    }
#endif
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static char *extract_bytestring_path(GVariant *dict, const char *key) {
    GVariant *v = g_variant_lookup_value(dict, key, G_VARIANT_TYPE_BYTESTRING);
    if (!v) return NULL;
    gsize len = 0;
    const gchar *raw = g_variant_get_fixed_array(v, &len, sizeof(gchar));
    char *out = NULL;
    if (raw && len > 0) {
        size_t slen = strnlen(raw, len);
        out = g_strndup(raw, slen);
    }
    g_variant_unref(v);
    return out;
}

static char *path_to_file_uri(const char *path) {
    if (!path || !path[0]) return g_strdup("file:///");
    GError *err = NULL;
    char *uri = g_filename_to_uri(path, NULL, &err);
    if (err) g_error_free(err);
    if (uri) return uri;
    return g_strdup_printf("file://%s", path);
}

static char *uri_to_local_path(const char *uri) {
    if (!uri || !uri[0]) return NULL;
    if (uri[0] == '/') return g_strdup(uri);
    GError *err = NULL;
    char *path = g_filename_from_uri(uri, NULL, &err);
    if (err) g_error_free(err);
    return path;
}

/* -------------------------------------------------------------------------
 * 1. FileChooser (OpenFile, SaveFile, SaveFiles)
 * ------------------------------------------------------------------------- */

typedef struct {
    DtPortalRequest *req;
    char dir_path[PATH_MAX];
} FavBtnCbData;

static void on_fav_dir_clicked(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    FavBtnCbData *fb = (FavBtnCbData *)client_data;
    if (!fb || !fb->req || !fb->req->dialog) return;
    if (!validate_directory_usable(fb->dir_path)) return;
    char dir_slash[PATH_MAX];
    snprintf(dir_slash, sizeof(dir_slash), "%s/", fb->dir_path);
    XmString xdir = XmStringCreateLocalized(dir_slash);
    XmFileSelectionDoSearch(fb->req->dialog, xdir);
    XmStringFree(xdir);
}

static void on_fav_destroy(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    g_free(client_data);
}

typedef struct {
    DtPortalRequest *req;
    int choice_idx;
    int opt_idx;
} ChoiceOptCbData;

static void on_choice_opt_selected(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    ChoiceOptCbData *cd = (ChoiceOptCbData *)client_data;
    if (!cd || !cd->req) return;
    DtPortalChoiceItem *ci = &cd->req->choices[cd->choice_idx];
    if (cd->opt_idx >= 0 && cd->opt_idx < ci->num_opts) {
        snprintf(ci->selected_id, sizeof(ci->selected_id), "%s", ci->opt_ids[cd->opt_idx]);
    }
}

typedef struct {
    DtPortalRequest *req;
    int filter_idx;
} FilterOptCbData;

static void on_filter_opt_selected(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    FilterOptCbData *fd = (FilterOptCbData *)client_data;
    if (!fd || !fd->req || !fd->req->dialog) return;
    if (fd->filter_idx >= 0 && fd->filter_idx < fd->req->filter_count) {
        fd->req->active_filter_idx = fd->filter_idx;
        const char *glob = fd->req->filters[fd->filter_idx].first_glob;
        XmString xpat = XmStringCreateLocalized((char *)(glob[0] ? glob : "*"));
        XtVaSetValues(fd->req->dialog, XmNpattern, xpat, NULL);
        XmFileSelectionDoSearch(fd->req->dialog, NULL);
        XmStringFree(xpat);
    }
}

static void finish_file_chooser_request(
    DtPortalRequest *req, uint32_t response, const char *override_path
) {
    if (!req || !req->in_use) return;
    GVariantBuilder res;
    g_variant_builder_init(&res, G_VARIANT_TYPE("a{sv}"));

    if (response == 0) {
        GVariantBuilder uris;
        g_variant_builder_init(&uris, G_VARIANT_TYPE("as"));

        if (req->kind == DTPORTAL_REQ_FILE_SAVE_MULTI) {
            const char *folder = (override_path && override_path[0])
                                     ? override_path
                                     : req->save_folder;
            for (int i = 0; i < req->save_files_count; ++i) {
                char full[PATH_MAX];
                snprintf(full, sizeof(full), "%s/%s", folder, req->save_files_names[i]);
                char *u = path_to_file_uri(full);
                g_variant_builder_add(&uris, "s", u);
                g_free(u);
            }
        } else if (override_path && override_path[0]) {
            char *u = path_to_file_uri(override_path);
            g_variant_builder_add(&uris, "s", u);
            g_free(u);
        } else if (req->dialog) {
            bool added = false;
            if (req->multiple_mode && req->list_widget) {
                XmStringTable sel = NULL;
                int sel_cnt = 0;
                XtVaGetValues(req->list_widget,
                              XmNselectedItems, &sel,
                              XmNselectedItemCount, &sel_cnt,
                              NULL);
                for (int i = 0; i < sel_cnt && sel; ++i) {
                    char *txt = NULL;
                    if (XmStringGetLtoR(sel[i], XmFONTLIST_DEFAULT_TAG, &txt) && txt) {
                        if (txt[0]) {
                            char *u = path_to_file_uri(txt);
                            g_variant_builder_add(&uris, "s", u);
                            g_free(u);
                            added = true;
                        }
                        XtFree(txt);
                    }
                }
            }
            if (!added) {
                XmString xspec = NULL;
                XtVaGetValues(req->dialog, XmNdirSpec, &xspec, NULL);
                if (xspec) {
                    char *txt = NULL;
                    if (XmStringGetLtoR(xspec, XmFONTLIST_DEFAULT_TAG, &txt) && txt) {
                        size_t l = strlen(txt);
                        while (l > 1 && txt[l - 1] == '/') txt[--l] = '\0';
                        char *u = path_to_file_uri(txt);
                        g_variant_builder_add(&uris, "s", u);
                        g_free(u);
                        XtFree(txt);
                    }
                    XmStringFree(xspec);
                }
            }
        }

        g_variant_builder_add(&res, "{sv}", "uris", g_variant_builder_end(&uris));
        g_variant_builder_add(&res, "{sv}", "writable", g_variant_new_boolean(TRUE));

        GVariantBuilder ch_b;
        g_variant_builder_init(&ch_b, G_VARIANT_TYPE("a(ss)"));
        for (int i = 0; i < req->choice_count; ++i) {
            DtPortalChoiceItem *ci = &req->choices[i];
            if (ci->is_boolean && ci->widget) {
                Boolean set = XmToggleButtonGetState(ci->widget);
                snprintf(ci->selected_id, sizeof(ci->selected_id), "%s", set ? "true" : "false");
            }
            g_variant_builder_add(&ch_b, "(ss)", ci->id, ci->selected_id);
        }
        g_variant_builder_add(&res, "{sv}", "choices", g_variant_builder_end(&ch_b));

        if (req->active_filter_idx >= 0 && req->active_filter_idx < req->filter_count &&
            req->filters[req->active_filter_idx].raw_tuple) {
            g_variant_builder_add(
                &res, "{sv}", "current_filter",
                g_variant_ref(req->filters[req->active_filter_idx].raw_tuple)
            );
        }
    }

    if (req->invocation) {
        g_dbus_method_invocation_return_value(
            req->invocation,
            g_variant_new("(ua{sv})", response, &res)
        );
        req->invocation = NULL;
    } else {
        g_variant_builder_clear(&res);
    }
    DtPortalCtx *ctx = req->ctx;
    dtportal_free_request(req);
    dtportal_write_test_state(ctx);
}

static void on_fsb_ok(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    DtPortalRequest *req = (DtPortalRequest *)client_data;
    XmFileSelectionBoxCallbackStruct *cbs = (XmFileSelectionBoxCallbackStruct *)call_data;
    char *chosen = NULL;
    if (cbs && cbs->value) {
        XmStringGetLtoR(cbs->value, XmFONTLIST_DEFAULT_TAG, &chosen);
    }
    if (chosen && req->kind == DTPORTAL_REQ_FILE_SAVE_MULTI) {
        snprintf(req->save_folder, sizeof(req->save_folder), "%s", chosen);
        size_t l = strlen(req->save_folder);
        while (l > 1 && req->save_folder[l - 1] == '/') req->save_folder[--l] = '\0';
        XtFree(chosen);
        finish_file_chooser_request(req, 0, req->save_folder);
        return;
    }
    finish_file_chooser_request(req, 0, chosen);
    if (chosen) XtFree(chosen);
}

static void on_fsb_cancel(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    DtPortalRequest *req = (DtPortalRequest *)client_data;
    finish_file_chooser_request(req, 1, NULL);
}

static void populate_cde_favorites_row(DtPortalRequest *req, Widget parent_rc) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) return;

    struct {
        const char *label;
        char path[PATH_MAX];
    } favs[8];
    int nfav = 0;

    favs[nfav].label = "Home";
    snprintf(favs[nfav++].path, PATH_MAX, "%s", home);

    const char *std_dirs[] = {"Desktop", "Documents", "Downloads"};
    for (size_t i = 0; i < 3 && nfav < 8; ++i) {
        char cand[PATH_MAX];
        snprintf(cand, sizeof(cand), "%s/%s", home, std_dirs[i]);
        if (validate_directory_usable(cand)) {
            favs[nfav].label = std_dirs[i];
            snprintf(favs[nfav++].path, PATH_MAX, "%s", cand);
        }
    }

    char dt_fav_path[PATH_MAX];
    snprintf(dt_fav_path, sizeof(dt_fav_path), "%s/.dt/dtfile.favorites", home);
    FILE *fp = fopen(dt_fav_path, "r");
    if (fp) {
        char line[PATH_MAX];
        while (nfav < 8 && fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0] == '/' && validate_directory_usable(line)) {
                const char *base = strrchr(line, '/');
                favs[nfav].label = (base && base[1]) ? base + 1 : line;
                snprintf(favs[nfav++].path, PATH_MAX, "%s", line);
            }
        }
        fclose(fp);
    }

    Widget row = XtVaCreateManagedWidget(
        "favRow", xmRowColumnWidgetClass, parent_rc,
        XmNorientation, XmHORIZONTAL,
        XmNpacking, XmPACK_TIGHT,
        XmNspacing, 6,
        NULL
    );
    XmString xl = XmStringCreateLocalized("Places:");
    XtVaCreateManagedWidget("placesLbl", xmLabelWidgetClass, row, XmNlabelString, xl, NULL);
    XmStringFree(xl);

    for (int i = 0; i < nfav; ++i) {
        XmString xb = XmStringCreateLocalized((char *)favs[i].label);
        Widget btn = XtVaCreateManagedWidget(
            "favBtn", xmPushButtonWidgetClass, row,
            XmNlabelString, xb,
            NULL
        );
        XmStringFree(xb);
        FavBtnCbData *cb = g_new0(FavBtnCbData, 1);
        cb->req = req;
        snprintf(cb->dir_path, sizeof(cb->dir_path), "%s", favs[i].path);
        XtAddCallback(btn, XmNactivateCallback, on_fav_dir_clicked, cb);
        XtAddCallback(btn, XmNdestroyCallback, on_fav_destroy, cb);
    }
}

static void parse_portal_choices(DtPortalRequest *req, GVariant *options, Widget work_rc) {
    GVariant *ch_var = g_variant_lookup_value(options, "choices", G_VARIANT_TYPE("a(ssa(ss)s)"));
    if (!ch_var) return;

    GVariantIter iter;
    g_variant_iter_init(&iter, ch_var);
    const char *cid = NULL, *clabel = NULL, *cinit = NULL;
    GVariant *opts_arr = NULL;

    while (req->choice_count < DTPORTAL_MAX_CHOICES &&
           g_variant_iter_next(&iter, "(&s&s@a(ss)&s)", &cid, &clabel, &opts_arr, &cinit)) {
        DtPortalChoiceItem *ci = &req->choices[req->choice_count];
        snprintf(ci->id, sizeof(ci->id), "%s", cid ? cid : "");
        snprintf(ci->label, sizeof(ci->label), "%s", clabel ? clabel : "");
        gsize nopts = g_variant_n_children(opts_arr);

        if (nopts == 0) {
            ci->is_boolean = true;
            bool init_on = (cinit && strcmp(cinit, "true") == 0);
            snprintf(ci->selected_id, sizeof(ci->selected_id), "%s", init_on ? "true" : "false");
            XmString xlbl = XmStringCreateLocalized(ci->label);
            ci->widget = XtVaCreateManagedWidget(
                "choiceToggle", xmToggleButtonWidgetClass, work_rc,
                XmNlabelString, xlbl,
                XmNset, init_on ? True : False,
                NULL
            );
            XmStringFree(xlbl);
        } else {
            ci->is_boolean = false;
            Widget pull = XmCreatePulldownMenu(work_rc, "choicePull", NULL, 0);
            Widget init_btn = NULL;
            GVariantIter oit;
            g_variant_iter_init(&oit, opts_arr);
            const char *oid = NULL, *olbl = NULL;
            while (ci->num_opts < 16 && g_variant_iter_next(&oit, "(&s&s)", &oid, &olbl)) {
                int oidx = ci->num_opts++;
                snprintf(ci->opt_ids[oidx], sizeof(ci->opt_ids[oidx]), "%s", oid ? oid : "");
                snprintf(ci->opt_labels[oidx], sizeof(ci->opt_labels[oidx]), "%s", olbl ? olbl : "");
                if (oidx == 0 || (cinit && strcmp(cinit, ci->opt_ids[oidx]) == 0)) {
                    snprintf(ci->selected_id, sizeof(ci->selected_id), "%s", ci->opt_ids[oidx]);
                }
                XmString xo = XmStringCreateLocalized(ci->opt_labels[oidx]);
                Widget pb = XtVaCreateManagedWidget("optBtn", xmPushButtonWidgetClass, pull,
                                                    XmNlabelString, xo, NULL);
                XmStringFree(xo);
                if (cinit && strcmp(cinit, ci->opt_ids[oidx]) == 0) init_btn = pb;
                ChoiceOptCbData *cd = g_new0(ChoiceOptCbData, 1);
                cd->req = req;
                cd->choice_idx = req->choice_count;
                cd->opt_idx = oidx;
                XtAddCallback(pb, XmNactivateCallback, on_choice_opt_selected, cd);
                XtAddCallback(pb, XmNdestroyCallback, on_fav_destroy, cd);
            }
            XmString xcl = XmStringCreateLocalized(ci->label);
            Arg args[4];
            Cardinal n = 0;
            XtSetArg(args[n], XmNlabelString, xcl); n++;
            XtSetArg(args[n], XmNsubMenuId, pull); n++;
            if (init_btn) { XtSetArg(args[n], XmNmenuHistory, init_btn); n++; }
            ci->widget = XmCreateOptionMenu(work_rc, "choiceOptMenu", args, n);
            XtManageChild(ci->widget);
            XmStringFree(xcl);
        }

        g_variant_unref(opts_arr);
        req->choice_count++;
    }
    g_variant_unref(ch_var);
}

static void parse_portal_filters(DtPortalRequest *req, GVariant *options, Widget work_rc) {
    GVariant *f_var = g_variant_lookup_value(options, "filters", G_VARIANT_TYPE("a(sa(us))"));
    if (!f_var) return;
    gsize nf = g_variant_n_children(f_var);
    if (nf == 0) {
        g_variant_unref(f_var);
        return;
    }

    Widget pull = XmCreatePulldownMenu(work_rc, "filterPull", NULL, 0);
    for (gsize i = 0; i < nf && req->filter_count < DTPORTAL_MAX_FILTERS; ++i) {
        GVariant *child = g_variant_get_child_value(f_var, i);
        const char *fname = NULL;
        GVariant *rules = NULL;
        g_variant_get(child, "(&s@a(us))", &fname, &rules);

        int fidx = req->filter_count++;
        DtPortalFilterItem *fi = &req->filters[fidx];
        snprintf(fi->name, sizeof(fi->name), "%s", fname ? fname : "Files");
        snprintf(fi->first_glob, sizeof(fi->first_glob), "*");
        fi->raw_tuple = child; /* takes ref */

        GVariantIter rit;
        g_variant_iter_init(&rit, rules);
        guint32 rtype = 0;
        const char *rpat = NULL;
        while (g_variant_iter_next(&rit, "(u&s)", &rtype, &rpat)) {
            if (rtype == 0 && rpat && rpat[0]) {
                snprintf(fi->first_glob, sizeof(fi->first_glob), "%s", rpat);
                break;
            }
        }
        g_variant_unref(rules);

        XmString xf = XmStringCreateLocalized(fi->name);
        Widget pb = XtVaCreateManagedWidget("filterItem", xmPushButtonWidgetClass, pull,
                                            XmNlabelString, xf, NULL);
        XmStringFree(xf);
        FilterOptCbData *fd = g_new0(FilterOptCbData, 1);
        fd->req = req;
        fd->filter_idx = fidx;
        XtAddCallback(pb, XmNactivateCallback, on_filter_opt_selected, fd);
        XtAddCallback(pb, XmNdestroyCallback, on_fav_destroy, fd);
    }
    g_variant_unref(f_var);

    if (req->filter_count > 0) {
        req->active_filter_idx = 0;
        XmString xfl = XmStringCreateLocalized("Filter:");
        Arg args[4];
        Cardinal n = 0;
        XtSetArg(args[n], XmNlabelString, xfl); n++;
        XtSetArg(args[n], XmNsubMenuId, pull); n++;
        req->filter_menu = XmCreateOptionMenu(work_rc, "filterMenu", args, n);
        XtManageChild(req->filter_menu);
        XmStringFree(xfl);
    }
}

void dtportal_handle_file_chooser(
    DtPortalCtx *ctx,
    const char *method_name,
    GVariant *parameters,
    GDBusMethodInvocation *invocation
) {
    const char *handle = NULL, *app_id = NULL, *parent_win = NULL, *title = NULL;
    GVariant *options = NULL;
    g_variant_get(parameters, "(&o&s&s&s@a{sv})", &handle, &app_id, &parent_win, &title, &options);
    (void)parent_win;

    DtPortalReqKind kind = DTPORTAL_REQ_FILE_OPEN;
    if (strcmp(method_name, "SaveFile") == 0) kind = DTPORTAL_REQ_FILE_SAVE;
    else if (strcmp(method_name, "SaveFiles") == 0) kind = DTPORTAL_REQ_FILE_SAVE_MULTI;

    DtPortalRequest *req = dtportal_alloc_request(ctx, kind, handle, app_id, invocation);
    if (!req) {
        g_variant_unref(options);
        GVariantBuilder empty;
        g_variant_builder_init(&empty, G_VARIANT_TYPE("a{sv}"));
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(ua{sv})", 2u, &empty));
        return;
    }
    req->active_filter_idx = -1;

    gboolean dir_mode = FALSE, multi_mode = FALSE;
    g_variant_lookup(options, "directory", "b", &dir_mode);
    g_variant_lookup(options, "multiple", "b", &multi_mode);
    req->directory_mode = (kind == DTPORTAL_REQ_FILE_SAVE_MULTI) || dir_mode;
    req->multiple_mode = (kind == DTPORTAL_REQ_FILE_OPEN) && multi_mode;

    char *cur_folder = extract_bytestring_path(options, "current_folder");
    char *cur_file = extract_bytestring_path(options, "current_file");
    const char *cur_name = NULL;
    g_variant_lookup(options, "current_name", "&s", &cur_name);
    const char *accept_label = NULL;
    g_variant_lookup(options, "accept_label", "&s", &accept_label);

    if (kind == DTPORTAL_REQ_FILE_SAVE_MULTI) {
        GVariant *files_v = g_variant_lookup_value(options, "files", G_VARIANT_TYPE("aay"));
        if (files_v) {
            gsize n = g_variant_n_children(files_v);
            for (gsize i = 0; i < n && req->save_files_count < 16; ++i) {
                GVariant *el = g_variant_get_child_value(files_v, i);
                gsize bl = 0;
                const gchar *raw = g_variant_get_fixed_array(el, &bl, sizeof(gchar));
                if (raw && bl > 0) {
                    const char *base = strrchr(raw, '/');
                    snprintf(req->save_files_names[req->save_files_count++], 256, "%s",
                             base ? base + 1 : raw);
                }
                g_variant_unref(el);
            }
            g_variant_unref(files_v);
        }
    }

    const char *home = getenv("HOME");
    const char *init_dir = (cur_folder && validate_directory_usable(cur_folder))
                               ? cur_folder
                               : ((home && home[0]) ? home : "/var/tmp");
    snprintf(req->save_folder, sizeof(req->save_folder), "%s", init_dir);

    char dir_slash[PATH_MAX];
    snprintf(dir_slash, sizeof(dir_slash), "%s/", init_dir);
    XmString xtitle = XmStringCreateLocalized((char *)(title && title[0] ? title : "Select File"));
    XmString xdir = XmStringCreateLocalized(dir_slash);

    Arg args[12];
    Cardinal n = 0;
    XtSetArg(args[n], XmNdialogTitle, xtitle); n++;
    XtSetArg(args[n], XmNdirectory, xdir); n++;
    XtSetArg(args[n], XmNfileTypeMask, req->directory_mode ? XmFILE_DIRECTORY : XmFILE_REGULAR); n++;
    if (accept_label && accept_label[0]) {
        XmString xok = XmStringCreateLocalized((char *)accept_label);
        XtSetArg(args[n], XmNokLabelString, xok); n++;
        req->dialog = XmCreateFileSelectionDialog(ctx->top_shell, "cdePortalFileDialog", args, n);
        XmStringFree(xok);
    } else {
        req->dialog = XmCreateFileSelectionDialog(ctx->top_shell, "cdePortalFileDialog", args, n);
    }
    XmStringFree(xtitle);
    XmStringFree(xdir);

    req->shell = XtParent(req->dialog);
    Widget help_btn = XmFileSelectionBoxGetChild(req->dialog, XmDIALOG_HELP_BUTTON);
    if (help_btn) XtUnmanageChild(help_btn);
    req->list_widget = XmFileSelectionBoxGetChild(req->dialog, XmDIALOG_LIST);
    if (req->multiple_mode && req->list_widget) {
        XtVaSetValues(req->list_widget, XmNselectionPolicy, XmEXTENDED_SELECT, NULL);
    }

    Widget work_rc = XtVaCreateManagedWidget(
        "portalWorkArea", xmRowColumnWidgetClass, req->dialog,
        XmNorientation, XmVERTICAL,
        XmNspacing, 4,
        NULL
    );
    populate_cde_favorites_row(req, work_rc);
    parse_portal_filters(req, options, work_rc);
    parse_portal_choices(req, options, work_rc);

    if (req->filter_count > 0 && req->filters[0].first_glob[0]) {
        XmString xpat = XmStringCreateLocalized(req->filters[0].first_glob);
        XtVaSetValues(req->dialog, XmNpattern, xpat, NULL);
        XmFileSelectionDoSearch(req->dialog, NULL);
        XmStringFree(xpat);
    }

    if (cur_file && cur_file[0]) {
        XmString xspec = XmStringCreateLocalized(cur_file);
        XtVaSetValues(req->dialog, XmNdirSpec, xspec, NULL);
        XmStringFree(xspec);
    } else if (cur_name && cur_name[0]) {
        char prefill[PATH_MAX];
        snprintf(prefill, sizeof(prefill), "%s/%s", init_dir, cur_name);
        XmString xspec = XmStringCreateLocalized(prefill);
        XtVaSetValues(req->dialog, XmNdirSpec, xspec, NULL);
        XmStringFree(xspec);
    }

    XtAddCallback(req->dialog, XmNokCallback, on_fsb_ok, req);
    XtAddCallback(req->dialog, XmNcancelCallback, on_fsb_cancel, req);

    XtManageChild(req->dialog);
    XFlush(ctx->dpy);

    g_free(cur_folder);
    g_free(cur_file);
    g_variant_unref(options);
    dtportal_write_test_state(ctx);
}

/* -------------------------------------------------------------------------
 * 2. AppChooser (ChooseApplication & UpdateChoices via DtAction + .desktop)
 * ------------------------------------------------------------------------- */

static bool lookup_desktop_display_name(const char *app_id, char *out_label, size_t out_sz) {
    const char *dirs[] = {
        "/usr/share/applications",
        "/usr/local/share/applications",
        CDE_INSTALLATION_TOP "/share/applications",
    };
    const char *home = getenv("HOME");
    char user_dir[PATH_MAX] = {0};
    if (home && home[0]) {
        snprintf(user_dir, sizeof(user_dir), "%s/.local/share/applications", home);
    }

    for (size_t di = 0; di < 4; ++di) {
        const char *base = (di == 0 && user_dir[0]) ? user_dir : dirs[di > 0 ? di - 1 : 0];
        if (!base || !base[0]) continue;
        char cand[PATH_MAX];
        if (g_str_has_suffix(app_id, ".desktop")) {
            snprintf(cand, sizeof(cand), "%s/%s", base, app_id);
        } else {
            snprintf(cand, sizeof(cand), "%s/%s.desktop", base, app_id);
        }
        GKeyFile *kf = g_key_file_new();
        if (g_key_file_load_from_file(kf, cand, G_KEY_FILE_NONE, NULL)) {
            char *nm = g_key_file_get_locale_string(kf, "Desktop Entry", "Name", NULL, NULL);
            if (nm && nm[0]) {
                snprintf(out_label, out_sz, "%s (%s)", nm, app_id);
                g_free(nm);
                g_key_file_free(kf);
                return true;
            }
            g_free(nm);
        }
        g_key_file_free(kf);
    }
    return false;
}

static void resolve_app_entry_label(const char *choice_id, char *out_label, size_t out_sz) {
    if (!choice_id || !choice_id[0]) {
        snprintf(out_label, out_sz, "Unknown Application");
        return;
    }
    /* First consult CDE DtAction database (libDtSvc) */
    if (DtActionExists((char *)choice_id)) {
        char *lbl = DtActionLabel((char *)choice_id);
        if (lbl && lbl[0]) {
            snprintf(out_label, out_sz, "%s [CDE Action: %s]", lbl, choice_id);
            XtFree(lbl);
            return;
        }
        if (lbl) XtFree(lbl);
    }
    if (lookup_desktop_display_name(choice_id, out_label, out_sz)) {
        return;
    }
    snprintf(out_label, out_sz, "%s", choice_id);
}

static void populate_app_chooser_list(
    DtPortalRequest *req, const char * const *choices, const char *last_chosen
) {
    if (!req || !req->list_widget) return;
    XmListDeleteAllItems(req->list_widget);
    req->app_count = 0;
    int select_pos = 1;

    for (int i = 0; choices && choices[i] && req->app_count < 64; ++i) {
        int idx = req->app_count++;
        snprintf(req->app_ids[idx], sizeof(req->app_ids[idx]), "%s", choices[i]);
        resolve_app_entry_label(choices[i], req->app_labels[idx], sizeof(req->app_labels[idx]));
        XmString xit = XmStringCreateLocalized(req->app_labels[idx]);
        XmListAddItemUnselected(req->list_widget, xit, 0);
        XmStringFree(xit);
        if (last_chosen && strcmp(last_chosen, choices[i]) == 0) {
            select_pos = idx + 1;
        }
    }

    /* Append standard CDE Desktop Actions when choices list is empty or as fallback */
    static const char * const cde_actions[] = {"Dtpad", "Dtfile", "Dtterm", NULL};
    if (req->app_count == 0) {
        for (int i = 0; cde_actions[i] && req->app_count < 64; ++i) {
            int idx = req->app_count++;
            snprintf(req->app_ids[idx], sizeof(req->app_ids[idx]), "%s", cde_actions[i]);
            resolve_app_entry_label(cde_actions[i], req->app_labels[idx], sizeof(req->app_labels[idx]));
            XmString xit = XmStringCreateLocalized(req->app_labels[idx]);
            XmListAddItemUnselected(req->list_widget, xit, 0);
            XmStringFree(xit);
        }
    }

    if (req->app_count > 0) {
        XmListSelectPos(req->list_widget, select_pos, False);
    }
}

static void finish_app_chooser_request(
    DtPortalRequest *req, uint32_t response, const char *override_choice
) {
    if (!req || !req->in_use) return;
    GVariantBuilder res;
    g_variant_builder_init(&res, G_VARIANT_TYPE("a{sv}"));

    if (response == 0) {
        const char *chosen_id = override_choice;
        if ((!chosen_id || !chosen_id[0]) && req->list_widget && req->app_count > 0) {
            int *pos_list = NULL, pos_cnt = 0;
            if (XmListGetSelectedPos(req->list_widget, &pos_list, &pos_cnt) && pos_cnt > 0) {
                int idx = pos_list[0] - 1;
                if (idx >= 0 && idx < req->app_count) {
                    chosen_id = req->app_ids[idx];
                }
                XtFree((char *)pos_list);
            }
            if (!chosen_id) chosen_id = req->app_ids[0];
        }
        if (chosen_id && chosen_id[0]) {
            g_variant_builder_add(&res, "{sv}", "choice", g_variant_new_string(chosen_id));
            char *tok = dtportal_mint_activation_token(req->ctx, chosen_id);
            if (tok && tok[0]) {
                g_variant_builder_add(&res, "{sv}", "activation_token", g_variant_new_string(tok));
            }
            g_free(tok);
        }
    }

    if (req->invocation) {
        g_dbus_method_invocation_return_value(
            req->invocation, g_variant_new("(ua{sv})", response, &res)
        );
        req->invocation = NULL;
    } else {
        g_variant_builder_clear(&res);
    }
    DtPortalCtx *ctx = req->ctx;
    dtportal_free_request(req);
    dtportal_write_test_state(ctx);
}

static void on_app_ok(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    finish_app_chooser_request((DtPortalRequest *)client_data, 0, NULL);
}

static void on_app_cancel(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    finish_app_chooser_request((DtPortalRequest *)client_data, 1, NULL);
}

void dtportal_handle_app_chooser(
    DtPortalCtx *ctx,
    const char *method_name,
    GVariant *parameters,
    GDBusMethodInvocation *invocation
) {
    if (strcmp(method_name, "UpdateChoices") == 0) {
        const char *handle = NULL;
        const char **choices = NULL;
        g_variant_get(parameters, "(&o^a&s)", &handle, &choices);
        DtPortalRequest *req = dtportal_find_request(ctx, handle);
        if (req && req->kind == DTPORTAL_REQ_APP_CHOOSER) {
            populate_app_chooser_list(req, choices, NULL);
            XFlush(ctx->dpy);
            dtportal_write_test_state(ctx);
        }
        g_free(choices);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (strcmp(method_name, "ChooseApplication") == 0) {
        const char *handle = NULL, *app_id = NULL, *parent_win = NULL;
        const char **choices = NULL;
        GVariant *options = NULL;
        g_variant_get(parameters, "(&o&s&s^a&s@a{sv})", &handle, &app_id, &parent_win, &choices, &options);
        (void)parent_win;

        DtPortalRequest *req = dtportal_alloc_request(ctx, DTPORTAL_REQ_APP_CHOOSER, handle, app_id, invocation);
        if (!req) {
            g_free(choices);
            g_variant_unref(options);
            GVariantBuilder empty;
            g_variant_builder_init(&empty, G_VARIANT_TYPE("a{sv}"));
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(ua{sv})", 2u, &empty));
            return;
        }

        const char *heading = NULL, *last_chosen = NULL;
        g_variant_lookup(options, "heading", "&s", &heading);
        g_variant_lookup(options, "last_chosen", "&s", &last_chosen);

        XmString xtitle = XmStringCreateLocalized("Open With - CDE Application Chooser");
        XmString xok = XmStringCreateLocalized("Open");
        Arg args[6];
        Cardinal n = 0;
        XtSetArg(args[n], XmNdialogTitle, xtitle); n++;
        XtSetArg(args[n], XmNokLabelString, xok); n++;
        req->dialog = XmCreateTemplateDialog(ctx->top_shell, "cdeAppChooserDialog", args, n);
        XmStringFree(xtitle);
        XmStringFree(xok);
        req->shell = XtParent(req->dialog);

        Widget rc = XtVaCreateManagedWidget(
            "appChooserRC", xmRowColumnWidgetClass, req->dialog,
            XmNorientation, XmVERTICAL,
            XmNspacing, 6,
            NULL
        );
        XmString xh = XmStringCreateLocalized(
            (char *)(heading && heading[0] ? heading : "Choose an application or CDE action:")
        );
        XtVaCreateManagedWidget("appHeading", xmLabelWidgetClass, rc, XmNlabelString, xh, NULL);
        XmStringFree(xh);

        Arg largs[4];
        Cardinal ln = 0;
        XtSetArg(largs[ln], XmNvisibleItemCount, 8); ln++;
        XtSetArg(largs[ln], XmNselectionPolicy, XmBROWSE_SELECT); ln++;
        XtSetArg(largs[ln], XmNwidth, 420); ln++;
        req->list_widget = XmCreateScrolledList(rc, "appList", largs, ln);
        XtManageChild(req->list_widget);

        populate_app_chooser_list(req, choices, last_chosen);

        XtAddCallback(req->dialog, XmNokCallback, on_app_ok, req);
        XtAddCallback(req->dialog, XmNcancelCallback, on_app_cancel, req);
        XtAddCallback(req->list_widget, XmNdefaultActionCallback, on_app_ok, req);

        XtManageChild(req->dialog);
        XFlush(ctx->dpy);

        g_free(choices);
        g_variant_unref(options);
        dtportal_write_test_state(ctx);
        return;
    }

    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown AppChooser method"
    );
}

/* -------------------------------------------------------------------------
 * 3. Wallpaper (SetWallpaperURI -> ~/.dt/backdrops/PortalWallpaper.pm + live apply)
 * ------------------------------------------------------------------------- */

static bool write_xpm3_from_argb_surface(cairo_surface_t *surf, const char *out_pm_path) {
    int src_w = cairo_image_surface_get_width(surf);
    int src_h = cairo_image_surface_get_height(surf);
    if (src_w <= 0 || src_h <= 0) return false;

    /* Scale down huge 4K photos cleanly to <= 640x480 for fast XPM3 backdrop parsing,
     * and quantize RGB to 6x6x6 (216-color web-safe cube) so XPM3 palette stays compact! */
    int dst_w = src_w, dst_h = src_h;
    if (dst_w > 640 || dst_h > 480) {
        double sx = 640.0 / (double)dst_w;
        double sy = 480.0 / (double)dst_h;
        double s = sx < sy ? sx : sy;
        dst_w = (int)((double)dst_w * s);
        dst_h = (int)((double)dst_h * s);
        if (dst_w < 1) dst_w = 1;
        if (dst_h < 1) dst_h = 1;
    }

    cairo_surface_t *scaled = cairo_image_surface_create(CAIRO_FORMAT_RGB24, dst_w, dst_h);
    cairo_t *cr = cairo_create(scaled);
    cairo_scale(cr, (double)dst_w / (double)src_w, (double)dst_h / (double)src_h);
    cairo_set_source_surface(cr, surf, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(scaled);

    const uint8_t *data = cairo_image_surface_get_data(scaled);
    int stride = cairo_image_surface_get_stride(scaled);

    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    bool used[216] = {false};
    for (int y = 0; y < dst_h; ++y) {
        const uint32_t *row = (const uint32_t *)(data + y * stride);
        for (int x = 0; x < dst_w; ++x) {
            uint32_t px = row[x];
            int r6 = (((px >> 16) & 0xff) * 5 + 127) / 255;
            int g6 = (((px >> 8) & 0xff) * 5 + 127) / 255;
            int b6 = ((px & 0xff) * 5 + 127) / 255;
            used[r6 * 36 + g6 * 6 + b6] = true;
        }
    }
    int ncolors = 0;
    int map_idx[216];
    for (int i = 0; i < 216; ++i) {
        map_idx[i] = used[i] ? ncolors++ : 0;
    }
    if (ncolors == 0) {
        used[0] = true;
        map_idx[0] = 0;
        ncolors = 1;
    }

    char tmp_path[PATH_MAX];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", out_pm_path, (int)getpid());
    FILE *fp = fopen(tmp_path, "w");
    if (!fp) {
        cairo_surface_destroy(scaled);
        return false;
    }

    fprintf(fp, "/* XPM */\nstatic char * PortalWallpaper_pm[] = {\n");
    fprintf(fp, "\"%d %d %d 2\",\n", dst_w, dst_h, ncolors);
    for (int i = 0; i < 216; ++i) {
        if (!used[i]) continue;
        int idx = map_idx[i];
        int r8 = (i / 36) * 51;
        int g8 = ((i / 6) % 6) * 51;
        int b8 = (i % 6) * 51;
        fprintf(fp, "\"%c%c c #%02X%02X%02X\",\n",
                b64[(idx >> 6) & 63], b64[idx & 63], r8, g8, b8);
    }

    for (int y = 0; y < dst_h; ++y) {
        fputc('"', fp);
        const uint32_t *row = (const uint32_t *)(data + y * stride);
        for (int x = 0; x < dst_w; ++x) {
            uint32_t px = row[x];
            int r6 = (((px >> 16) & 0xff) * 5 + 127) / 255;
            int g6 = (((px >> 8) & 0xff) * 5 + 127) / 255;
            int b6 = ((px & 0xff) * 5 + 127) / 255;
            int idx = map_idx[r6 * 36 + g6 * 6 + b6];
            fputc(b64[(idx >> 6) & 63], fp);
            fputc(b64[idx & 63], fp);
        }
        fprintf(fp, "\"%s\n", (y + 1 < dst_h) ? "," : "};");
    }
    fclose(fp);
    cairo_surface_destroy(scaled);
    return rename(tmp_path, out_pm_path) == 0;
}

static bool apply_cde_wallpaper_uri(DtPortalCtx *ctx, const char *uri) {
    char *local_path = uri_to_local_path(uri);
    if (!local_path) return false;
    if (access(local_path, R_OK) != 0) {
        g_free(local_path);
        return false;
    }

    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        g_free(local_path);
        return false;
    }
    char bd_dir[PATH_MAX], out_pm[PATH_MAX];
    snprintf(bd_dir, sizeof(bd_dir), "%s/.dt/backdrops", home);
    mkdir_p_local(bd_dir);
    snprintf(out_pm, sizeof(out_pm), "%s/PortalWallpaper.pm", bd_dir);

    bool ok = false;
    if (g_str_has_suffix(local_path, ".pm") || g_str_has_suffix(local_path, ".xpm")) {
        gchar *contents = NULL;
        gsize len = 0;
        if (g_file_get_contents(local_path, &contents, &len, NULL)) {
            ok = g_file_set_contents(out_pm, contents, (gssize)len, NULL);
            g_free(contents);
        }
    } else {
        cairo_surface_t *surf = cairo_image_surface_create_from_png(local_path);
        if (surf && cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS) {
            ok = write_xpm3_from_argb_surface(surf, out_pm);
        }
        if (surf) cairo_surface_destroy(surf);
    }
    g_free(local_path);
    if (!ok) return false;

    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "f.change_backdrop %s 0 2", out_pm);
    dtportal_send_wm_request(ctx, cmd);
    return true;
}

static void on_wallpaper_confirm(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    DtPortalRequest *req = (DtPortalRequest *)client_data;
    if (!req || !req->in_use) return;
    bool ok = apply_cde_wallpaper_uri(req->ctx, req->wallpaper_uri);
    if (req->invocation) {
        g_dbus_method_invocation_return_value(req->invocation, g_variant_new("(u)", ok ? 0u : 2u));
        req->invocation = NULL;
    }
    DtPortalCtx *ctx = req->ctx;
    dtportal_free_request(req);
    dtportal_write_test_state(ctx);
}

static void on_wallpaper_cancel(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    DtPortalRequest *req = (DtPortalRequest *)client_data;
    if (!req || !req->in_use) return;
    if (req->invocation) {
        g_dbus_method_invocation_return_value(req->invocation, g_variant_new("(u)", 1u));
        req->invocation = NULL;
    }
    DtPortalCtx *ctx = req->ctx;
    dtportal_free_request(req);
    dtportal_write_test_state(ctx);
}

void dtportal_handle_wallpaper(
    DtPortalCtx *ctx,
    const char *method_name,
    GVariant *parameters,
    GDBusMethodInvocation *invocation
) {
    if (strcmp(method_name, "SetWallpaperURI") != 0) {
        g_dbus_method_invocation_return_dbus_error(
            invocation, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown Wallpaper method"
        );
        return;
    }
    const char *handle = NULL, *app_id = NULL, *parent_win = NULL, *uri = NULL;
    GVariant *options = NULL;
    g_variant_get(parameters, "(&o&s&s&s@a{sv})", &handle, &app_id, &parent_win, &uri, &options);
    (void)parent_win;

    gboolean show_preview = FALSE;
    g_variant_lookup(options, "show-preview", "b", &show_preview);
    const char *set_on = "both";
    g_variant_lookup(options, "set-on", "&s", &set_on);

    if (!show_preview) {
        bool ok = apply_cde_wallpaper_uri(ctx, uri);
        g_variant_unref(options);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", ok ? 0u : 2u));
        return;
    }

    DtPortalRequest *req = dtportal_alloc_request(ctx, DTPORTAL_REQ_WALLPAPER, handle, app_id, invocation);
    if (!req) {
        g_variant_unref(options);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", 2u));
        return;
    }
    snprintf(req->wallpaper_uri, sizeof(req->wallpaper_uri), "%s", uri ? uri : "");
    snprintf(req->wallpaper_set_on, sizeof(req->wallpaper_set_on), "%s", set_on ? set_on : "both");

    char msg_txt[512];
    snprintf(msg_txt, sizeof(msg_txt),
             "Allow %s to set the CDE workspace backdrop?\n\nSource: %s",
             (app_id && app_id[0]) ? app_id : "Application", req->wallpaper_uri);
    XmString xmsg = XmStringCreateLocalized(msg_txt);
    XmString xtitle = XmStringCreateLocalized("Set CDE Workspace Backdrop");
    XmString xok = XmStringCreateLocalized("Set Backdrop");
    Arg args[6];
    Cardinal n = 0;
    XtSetArg(args[n], XmNmessageString, xmsg); n++;
    XtSetArg(args[n], XmNdialogTitle, xtitle); n++;
    XtSetArg(args[n], XmNokLabelString, xok); n++;
    req->dialog = XmCreateQuestionDialog(ctx->top_shell, "cdeWallpaperConfirm", args, n);
    XmStringFree(xmsg);
    XmStringFree(xtitle);
    XmStringFree(xok);
    req->shell = XtParent(req->dialog);
    Widget hbtn = XmMessageBoxGetChild(req->dialog, XmDIALOG_HELP_BUTTON);
    if (hbtn) XtUnmanageChild(hbtn);

    XtAddCallback(req->dialog, XmNokCallback, on_wallpaper_confirm, req);
    XtAddCallback(req->dialog, XmNcancelCallback, on_wallpaper_cancel, req);
    XtManageChild(req->dialog);
    XFlush(ctx->dpy);

    g_variant_unref(options);
    dtportal_write_test_state(ctx);
}

/* -------------------------------------------------------------------------
 * 4. GlobalShortcuts (CreateSession, BindShortcuts, ListShortcuts, ConfigureShortcuts)
 * ------------------------------------------------------------------------- */

static void save_global_shortcuts_conf(DtPortalSession *sess) {
    const char *xdg_cfg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char dir[PATH_MAX], path[PATH_MAX];
    if (xdg_cfg && xdg_cfg[0]) {
        snprintf(dir, sizeof(dir), "%s/ck-core", xdg_cfg);
    } else if (home && home[0]) {
        snprintf(dir, sizeof(dir), "%s/.config/ck-core", home);
    } else {
        return;
    }
    mkdir_p_local(dir);
    snprintf(path, sizeof(path), "%s/global-shortcuts.conf", dir);

    GKeyFile *kf = g_key_file_new();
    (void)g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
    const char *grp = sess->app_id[0] ? sess->app_id : "default";
    for (int i = 0; i < sess->shortcut_count; ++i) {
        g_key_file_set_string(kf, grp, sess->shortcuts[i].id, sess->shortcuts[i].trigger);
    }
    (void)g_key_file_save_to_file(kf, path, NULL);
    g_key_file_free(kf);
}

static GVariant *build_shortcuts_array_variant(DtPortalSession *sess) {
    GVariantBuilder arr;
    g_variant_builder_init(&arr, G_VARIANT_TYPE("a(sa{sv})"));
    for (int i = 0; i < sess->shortcut_count; ++i) {
        GVariantBuilder props;
        g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&props, "{sv}", "description",
                              g_variant_new_string(sess->shortcuts[i].description));
        g_variant_builder_add(&props, "{sv}", "trigger_description",
                              g_variant_new_string(sess->shortcuts[i].trigger));
        g_variant_builder_add(&arr, "(sa{sv})", sess->shortcuts[i].id, &props);
    }
    return g_variant_builder_end(&arr);
}

static void sync_session_shortcuts_to_compositor(DtPortalSession *sess) {
    if (!sess || !sess->ctx) return;
    char unreg[160];
    snprintf(unreg, sizeof(unreg), "f.unregister_portal_session %s", sess->session_key);
    dtportal_send_wm_request(sess->ctx, unreg);

    for (int i = 0; i < sess->shortcut_count; ++i) {
        if (!sess->shortcuts[i].trigger[0]) continue;
        char reg_cmd[512];
        snprintf(reg_cmd, sizeof(reg_cmd), "f.register_portal_shortcut %s %s %s",
                 sess->session_key, sess->shortcuts[i].id, sess->shortcuts[i].trigger);
        dtportal_send_wm_request(sess->ctx, reg_cmd);
    }
}

void dtportal_handle_global_shortcuts(
    DtPortalCtx *ctx,
    const char *method_name,
    GVariant *parameters,
    GDBusMethodInvocation *invocation
) {
    const char *sender = g_dbus_method_invocation_get_sender(invocation);

    if (strcmp(method_name, "CreateSession") == 0) {
        const char *handle = NULL, *session_handle = NULL, *app_id = NULL;
        GVariant *options = NULL;
        g_variant_get(parameters, "(&o&o&s@a{sv})", &handle, &session_handle, &app_id, &options);
        (void)handle;
        g_variant_unref(options);

        DtPortalSession *sess = dtportal_alloc_session(
            ctx, DTPORTAL_SESS_GLOBAL_SHORTCUTS, session_handle, app_id, sender
        );
        GVariantBuilder res;
        g_variant_builder_init(&res, G_VARIANT_TYPE("a{sv}"));
        if (!sess) {
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(ua{sv})", 2u, &res));
            return;
        }
        g_variant_builder_add(&res, "{sv}", "session_id", g_variant_new_string(sess->session_key));
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(ua{sv})", 0u, &res));
        return;
    }

    if (strcmp(method_name, "BindShortcuts") == 0) {
        const char *handle = NULL, *session_handle = NULL, *parent_win = NULL;
        GVariant *shortcuts = NULL, *options = NULL;
        g_variant_get(parameters, "(&o&o@a(sa{sv})&s@a{sv})",
                      &handle, &session_handle, &shortcuts, &parent_win, &options);
        (void)handle;
        (void)parent_win;

        DtPortalSession *sess = dtportal_find_session(ctx, session_handle);
        if (!sess || sess->kind != DTPORTAL_SESS_GLOBAL_SHORTCUTS) {
            g_variant_unref(shortcuts);
            g_variant_unref(options);
            GVariantBuilder empty;
            g_variant_builder_init(&empty, G_VARIANT_TYPE("a{sv}"));
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(ua{sv})", 2u, &empty));
            return;
        }

        sess->shortcut_count = 0;
        GVariantIter sit;
        g_variant_iter_init(&sit, shortcuts);
        const char *sid = NULL;
        GVariant *sprops = NULL;
        while (sess->shortcut_count < DTPORTAL_MAX_SHORTCUTS_PER_SESSION &&
               g_variant_iter_next(&sit, "(&s@a{sv})", &sid, &sprops)) {
            DtPortalShortcutEntry *se = &sess->shortcuts[sess->shortcut_count++];
            snprintf(se->id, sizeof(se->id), "%s", sid ? sid : "");
            const char *desc = NULL, *pref_trig = NULL;
            g_variant_lookup(sprops, "description", "&s", &desc);
            g_variant_lookup(sprops, "preferred_trigger", "&s", &pref_trig);
            snprintf(se->description, sizeof(se->description), "%s", desc ? desc : se->id);
            snprintf(se->trigger, sizeof(se->trigger), "%s",
                     (pref_trig && pref_trig[0]) ? pref_trig : "<Control><Alt>g");
            g_variant_unref(sprops);
        }

        save_global_shortcuts_conf(sess);
        sync_session_shortcuts_to_compositor(sess);

        GVariantBuilder res;
        g_variant_builder_init(&res, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&res, "{sv}", "shortcuts", build_shortcuts_array_variant(sess));

        g_variant_unref(shortcuts);
        g_variant_unref(options);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(ua{sv})", 0u, &res));
        return;
    }

    if (strcmp(method_name, "ListShortcuts") == 0) {
        const char *handle = NULL, *session_handle = NULL;
        g_variant_get(parameters, "(&o&o)", &handle, &session_handle);
        (void)handle;
        DtPortalSession *sess = dtportal_find_session(ctx, session_handle);
        GVariantBuilder res;
        g_variant_builder_init(&res, G_VARIANT_TYPE("a{sv}"));
        if (!sess || sess->kind != DTPORTAL_SESS_GLOBAL_SHORTCUTS) {
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(ua{sv})", 2u, &res));
            return;
        }
        g_variant_builder_add(&res, "{sv}", "shortcuts", build_shortcuts_array_variant(sess));
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(ua{sv})", 0u, &res));
        return;
    }

    if (strcmp(method_name, "ConfigureShortcuts") == 0) {
        const char *session_handle = NULL, *parent_win = NULL;
        GVariant *options = NULL;
        g_variant_get(parameters, "(&o&s@a{sv})", &session_handle, &parent_win, &options);
        (void)parent_win;
        DtPortalSession *sess = dtportal_find_session(ctx, session_handle);
        if (sess && sess->kind == DTPORTAL_SESS_GLOBAL_SHORTCUTS && ctx->bus) {
            g_dbus_connection_emit_signal(
                ctx->bus,
                sess->sender[0] ? sess->sender : NULL,
                "/org/freedesktop/portal/desktop",
                "org.freedesktop.impl.portal.GlobalShortcuts",
                "ShortcutsChanged",
                g_variant_new("(o@a(sa{sv}))", sess->session_path, build_shortcuts_array_variant(sess)),
                NULL
            );
        }
        g_variant_unref(options);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown GlobalShortcuts method"
    );
}

void dtportal_on_shortcut_ipc_event(DtPortalCtx *ctx, const char *payload) {
    if (!ctx || !ctx->bus || !payload) return;
    char kind[32] = {0}, skey[96] = {0}, sid[96] = {0};
    unsigned int seq = 0, t_ms = 0;
    if (sscanf(payload, "%31s %95s %95s %u %u", kind, skey, sid, &seq, &t_ms) != 5) {
        return;
    }
    if (seq == ctx->last_shortcut_seq) return;
    ctx->last_shortcut_seq = seq;

    DtPortalSession *sess = dtportal_find_session_by_key(ctx, skey);
    if (!sess || sess->kind != DTPORTAL_SESS_GLOBAL_SHORTCUTS) return;

    const char *sig_name = NULL;
    if (strcmp(kind, "ACTIVATED") == 0) sig_name = "Activated";
    else if (strcmp(kind, "DEACTIVATED") == 0) sig_name = "Deactivated";
    if (!sig_name) return;

    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
    g_dbus_connection_emit_signal(
        ctx->bus,
        sess->sender[0] ? sess->sender : NULL,
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.impl.portal.GlobalShortcuts",
        sig_name,
        g_variant_new("(osta{sv})", sess->session_path, sid, (guint64)t_ms, &opts),
        NULL
    );
}

/* -------------------------------------------------------------------------
 * 5. Test/Automation Dialog Accept & Cancel Hooks
 * ------------------------------------------------------------------------- */

void dtportal_test_accept_active_dialog(DtPortalCtx *ctx, const char *arg) {
    if (!ctx) return;
    for (int i = 0; i < DTPORTAL_MAX_REQUESTS; ++i) {
        DtPortalRequest *req = &ctx->requests[i];
        if (!req->in_use || !req->dialog) continue;
        switch (req->kind) {
        case DTPORTAL_REQ_FILE_OPEN:
        case DTPORTAL_REQ_FILE_SAVE:
        case DTPORTAL_REQ_FILE_SAVE_MULTI:
            finish_file_chooser_request(req, 0, (arg && arg[0]) ? arg : NULL);
            return;
        case DTPORTAL_REQ_APP_CHOOSER:
            finish_app_chooser_request(req, 0, (arg && arg[0]) ? arg : NULL);
            return;
        case DTPORTAL_REQ_WALLPAPER:
            on_wallpaper_confirm(req->dialog, req, NULL);
            return;
        case DTPORTAL_REQ_REMOTEDESKTOP_START:
            XtCallCallbacks(req->dialog, XmNokCallback, NULL);
            return;
        default:
            break;
        }
    }
}

void dtportal_test_cancel_active_dialog(DtPortalCtx *ctx) {
    if (!ctx) return;
    for (int i = 0; i < DTPORTAL_MAX_REQUESTS; ++i) {
        DtPortalRequest *req = &ctx->requests[i];
        if (!req->in_use || !req->dialog) continue;
        XtCallCallbacks(req->dialog, XmNcancelCallback, NULL);
        return;
    }
}
