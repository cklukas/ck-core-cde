/*
 * CDE - Common Desktop Environment
 *
 * wayland/WmFpWayland.c: Hosts the genuine CDE Front Panel (WmFP.c, UI.c,
 * DataBaseLoad.c, Callback.c, Button.c, Clock.c, PanelS.c, PopupMenu.c,
 * Session.c) on rootless XWayland under dtwm-wayland.
 */

#include "WmGlobal.h"
#include "WmPanelP.h"
#include "PanelSP.h"
#include "DataBaseLoad.h"
#include "UI.h"
#include "WmFunction.h"
#include "WmHelp.h"
#include "WmManage.h"
#include "WmResParse.h"
#include "WmWinInfo.h"
#define DTWM_NEED_FNTPL
#include "WmIBitmap.h"

#include <Dt/Action.h>
#include <Dt/Control.h>
#include <Dt/DbReader.h>
#include <Dt/Dt.h>
#include <Dt/DtP.h>
#include <Dt/EnvControlP.h>
#include <Dt/MacrosP.h>
#include <Dt/Message.h>
#include <Dt/MsgLog.h>
#include <Dt/Wsm.h>
#include <Dt/WsmM.h>
#include <Dt/WsmP.h>
#include <Tt/tttk.h>

#include <Xm/AtomMgr.h>
#include <Xm/ColorObjP.h>
#include <Xm/MenuShell.h>
#include <Xm/MessageB.h>
#include <Xm/PushBG.h>
#include <Xm/RowColumn.h>
#include <Xm/SeparatoG.h>
#include <Xm/Xm.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>

#include <ctype.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Global Workspace Manager state referenced by the genuine Front Panel code */
WmGlobalData wmGD;
static WmScreenData g_fpScreen;
static ClientData g_panelClientData;
static Window g_colorSrvWin = None;
static char g_colorSrvPixelSet[2048];
static char g_colorSrvTypeMon[64];
static Window g_mwmWin = None;
static Atom g_atomWsCurrent = None;
static Atom g_atomWsList = None;
static Atom g_atomWmRequest = None;
static Widget g_fpPopupMenu = nullptr;
static Widget g_topShell = nullptr;
static bool g_ttWsmRegistered = false;
static int g_ttRetryCount = 0;

extern void WorkspaceModifyCB(Widget, Atom, int, XtPointer);

/* --- Append _DT_WM_REQUEST command to dtwm-wayland's WSM window --- */
static Window find_mwm_window(Display *dpy) {
    Window root = DefaultRootWindow(dpy);
    Atom mwm_info = XInternAtom(dpy, "_MOTIF_WM_INFO", True);
    if (mwm_info == None) return None;
    Atom actual = None;
    int fmt = 0;
    unsigned long nitems = 0, after = 0;
    unsigned char *data = nullptr;
    Window win = None;
    if (XGetWindowProperty(
            dpy, root, mwm_info, 0, 2, False, mwm_info,
            &actual, &fmt, &nitems, &after, &data
        ) == Success && data) {
        if (fmt == 32 && nitems >= 2) {
            const long *vals = (const long *)data;
            win = (Window)vals[1];
        }
        XFree(data);
    }
    return win;
}

static void send_dt_wm_request(Display *dpy, const char *req) {
    if (!dpy || !req || !req[0]) return;
    if (g_mwmWin == None) {
        g_mwmWin = find_mwm_window(dpy);
    }
    if (g_mwmWin == None) return;
    if (g_atomWmRequest == None) {
        g_atomWmRequest = XInternAtom(dpy, "_DT_WM_REQUEST", False);
    }
    XChangeProperty(
        dpy, g_mwmWin, g_atomWmRequest, XA_STRING, 8, PropModeAppend,
        (const unsigned char *)req, (int)strlen(req) + 1
    );
    XFlush(dpy);
}

static Pixel alloc_rgb16(Display *dpy, Colormap cmap, unsigned short r, unsigned short g, unsigned short b) {
    XColor xc = {.red = r, .green = g, .blue = b, .flags = DoRed | DoGreen | DoBlue};
    if (XAllocColor(dpy, cmap, &xc)) {
        return xc.pixel;
    }
    return BlackPixel(dpy, DefaultScreen(dpy));
}

static unsigned short clamp_u16(int v) {
    if (v < 0) return 0;
    if (v > 65535) return 65535;
    return (unsigned short)v;
}

/* --- Standalone CDE 8-Color-Set Server Bootstrap (when dtsession is absent) ---
 * Must use pure Xlib only (no Xm* calls!) so XmDisplay / XmColorObj is NOT
 * instantiated until AFTER CUSTOMIZE_DATA:0 and PIXEL_SET_PROP are published!
 */
static void bootstrap_color_server_if_needed(Display *dpy, unsigned char profile) {
    char cust_name[64];
    int scr_num = DefaultScreen(dpy);
    snprintf(cust_name, sizeof(cust_name), "%s%d", XmSCUSTOMIZE_DATA, scr_num);
    Atom cust_atom = XInternAtom(dpy, cust_name, False);
    if (XGetSelectionOwner(dpy, cust_atom) != None) {
        /* dtsession Color Server is already active on this display */
        return;
    }

    static const char *const classic_bgs[8] = {
        "#ed00a8007000", "#9900991b99fe", "#89559808aa00", "#68006f008200",
        "#c600b2d2a87e", "#49009200a700", "#b70087008d00", "#938eab73bf00",
    };
    static const char *const modern_light_bgs[8] = {
        "#5b008400b800", "#9a00a300b000", "#7e008e00a400", "#3d0046005400",
        "#c800cc00d400", "#52007a009400", "#8c009600a600", "#7c008c00a200",
    };
    static const char *const modern_dark_bgs[8] = {
        "#280039005200", "#250027002b00", "#1b0022002c00", "#170018001b00",
        "#200021002400", "#28002a002d00", "#1f0029003700", "#250027002b00",
    };
    static const char *const high_contrast_bgs[8] = {
        "#ff00ff000000", "#000000000000", "#000000000000", "#000000000000",
        "#000000000000", "#000000000000", "#000000000000", "#000000000000",
    };

    const char *const *bgs = classic_bgs;
    if (profile == XmAPPEARANCE_MODERN_LIGHT) bgs = modern_light_bgs;
    else if (profile == XmAPPEARANCE_MODERN_DARK) bgs = modern_dark_bgs;
    else if (profile == XmAPPEARANCE_HIGH_CONTRAST) bgs = high_contrast_bgs;

    Screen *screen = ScreenOfDisplay(dpy, scr_num);
    Colormap cmap = DefaultColormapOfScreen(screen);
    XmPixelSet psets[XmCO_MAX_NUM_COLORS];

    for (int i = 0; i < XmCO_MAX_NUM_COLORS; ++i) {
        XColor exact = {0}, def = {0};
        if (!XParseColor(dpy, cmap, bgs[i], &exact) || !XAllocColor(dpy, cmap, &exact)) {
            def.pixel = WhitePixelOfScreen(screen);
            exact.red = exact.green = exact.blue = 45000;
        } else {
            def = exact;
        }
        psets[i].bg = def.pixel;

        /* Compute Motif-standard luminance fg, top_shadow, bottom_shadow, select_color */
        int lum = (30 * (int)exact.red + 59 * (int)exact.green + 11 * (int)exact.blue) / 100;
        psets[i].fg = (lum > 34000) ? BlackPixelOfScreen(screen) : WhitePixelOfScreen(screen);
        psets[i].ts = alloc_rgb16(
            dpy, cmap,
            clamp_u16((int)exact.red + (65535 - (int)exact.red) * 45 / 100),
            clamp_u16((int)exact.green + (65535 - (int)exact.green) * 45 / 100),
            clamp_u16((int)exact.blue + (65535 - (int)exact.blue) * 45 / 100)
        );
        psets[i].bs = alloc_rgb16(
            dpy, cmap,
            clamp_u16((int)exact.red * 48 / 100),
            clamp_u16((int)exact.green * 48 / 100),
            clamp_u16((int)exact.blue * 48 / 100)
        );
        psets[i].sc = alloc_rgb16(
            dpy, cmap,
            clamp_u16((int)exact.red * 85 / 100),
            clamp_u16((int)exact.green * 85 / 100),
            clamp_u16((int)exact.blue * 85 / 100)
        );
        if (profile == XmAPPEARANCE_HIGH_CONTRAST) {
            psets[i].fg = (i == 0) ? BlackPixelOfScreen(screen) : WhitePixelOfScreen(screen);
            psets[i].ts = WhitePixelOfScreen(screen);
        }
    }

    char buf[2048];
    int pos = snprintf(buf, sizeof(buf), "%x_", XmCO_HIGH_COLOR);
    for (int i = 0; i < XmCO_MAX_NUM_COLORS; ++i) {
        pos += snprintf(
            buf + pos, sizeof(buf) - (size_t)pos, "%lx_%lx_%lx_%lx_%lx_",
            (unsigned long)psets[i].bg,
            (unsigned long)psets[i].fg,
            (unsigned long)psets[i].ts,
            (unsigned long)psets[i].bs,
            (unsigned long)psets[i].sc
        );
    }
    buf[pos] = '\0';
    memcpy(g_colorSrvPixelSet, buf, (size_t)pos);
    g_colorSrvPixelSet[pos] = '\0';
    snprintf(g_colorSrvTypeMon, sizeof(g_colorSrvTypeMon), "%x_%x_%x_%x",
             XmCO_HIGH_COLOR, 0, 0, 0);

    buf[pos++] = XmPIXEL_SET_PROP_VERSION;
    buf[pos] = '\0';

    g_colorSrvWin = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), -200, -200, 1, 1, 0, 0, 0);
    Atom pixel_set_prop = XInternAtom(dpy, XmSPIXEL_SET_PROP, False);
    XChangeProperty(
        dpy, g_colorSrvWin, pixel_set_prop, XA_STRING, 8, PropModeReplace,
        (const unsigned char *)buf, pos
    );
    XSetSelectionOwner(dpy, cust_atom, g_colorSrvWin, CurrentTime);
    XFlush(dpy);
}

/* --- Discover appconfig search paths for both installed (/usr/dt) and build-tree runs --- */
static void ensure_cde_search_paths(const char *argv0) {
    struct stat st;
    if (getenv("DTDATABASESEARCHPATH") && getenv("XMICONSEARCHPATH")) {
        return;
    }

    char stage_appconfig[PATH_MAX] = {0};
    const char *appcfg = CDE_INSTALLATION_TOP "/appconfig";

    if (stat(CDE_INSTALLATION_TOP "/appconfig/types/C/dtwm.fp", &st) != 0 && argv0) {
        char resolved[PATH_MAX];
        if (realpath(argv0, resolved)) {
            /* Check <root>/cde-build/programs/dtwm/dtwm-panel -> <root>/cde-stage/usr/dt/appconfig */
            char *slash = strrchr(resolved, '/');
            if (slash) {
                *slash = '\0';
                static const char *const rels[2] = {"../../..", "../../../.."};
                for (int r = 0; r < 2; ++r) {
                    snprintf(
                        stage_appconfig, sizeof(stage_appconfig),
                        "%s/%s/cde-stage" CDE_INSTALLATION_TOP "/appconfig", resolved, rels[r]
                    );
                    if (stat(stage_appconfig, &st) == 0 && S_ISDIR(st.st_mode)) {
                        appcfg = stage_appconfig;
                        break;
                    }
                }
            }
        }
    }

    const char *home = getenv("HOME");
    if (home && home[0]) {
        char dt_dir[PATH_MAX], sess_dir[PATH_MAX], types_dir[PATH_MAX], dyn_dir[PATH_MAX];
        snprintf(dt_dir, sizeof(dt_dir), "%s/.dt", home);
        snprintf(sess_dir, sizeof(sess_dir), "%s/.dt/sessions", home);
        snprintf(types_dir, sizeof(types_dir), "%s/.dt/types", home);
        snprintf(dyn_dir, sizeof(dyn_dir), "%s/.dt/types/fp_dynamic", home);
        mkdir(dt_dir, 0755);
        mkdir(sess_dir, 0755);
        mkdir(types_dir, 0755);
        mkdir(dyn_dir, 0755);
    }
    if (!getenv("DTDATABASESEARCHPATH")) {
        char dbpath[PATH_MAX * 2];
        if (home && home[0]) {
            snprintf(
                dbpath, sizeof(dbpath),
                "%s/.dt/types,%s/types/%%L,%s/types/C",
                home, appcfg, appcfg
            );
        } else {
            snprintf(dbpath, sizeof(dbpath), "%s/types/%%L,%s/types/C", appcfg, appcfg);
        }
        setenv("DTDATABASESEARCHPATH", dbpath, 1);
    }

    if (!getenv("XMICONSEARCHPATH")) {
        char icpath[PATH_MAX * 3];
        if (home && home[0]) {
            snprintf(
                icpath, sizeof(icpath),
                "%s/.dt/icons/%%B%%M.pm:%s/.dt/icons/%%B%%M.bm:"
                "%s/icons/%%L/%%B%%M.pm:%s/icons/%%L/%%B%%M.bm:"
                "%s/icons/C/%%B%%M.pm:%s/icons/C/%%B%%M.bm",
                home, home, appcfg, appcfg, appcfg, appcfg
            );
        } else {
            snprintf(
                icpath, sizeof(icpath),
                "%s/icons/%%L/%%B%%M.pm:%s/icons/%%L/%%B%%M.bm:"
                "%s/icons/C/%%B%%M.pm:%s/icons/C/%%B%%M.bm",
                appcfg, appcfg, appcfg, appcfg
            );
        }
        setenv("XMICONSEARCHPATH", icpath, 1);
    }

    if (!getenv("XMICONBMSEARCHPATH")) {
        char bmpath[PATH_MAX * 3];
        snprintf(
            bmpath, sizeof(bmpath),
            "%s/icons/%%L/%%B%%M.bm:%s/icons/C/%%B%%M.bm:"
            "%s/icons/%%L/%%B%%M.pm:%s/icons/C/%%B%%M.pm",
            appcfg, appcfg, appcfg, appcfg
        );
        setenv("XMICONBMSEARCHPATH", bmpath, 1);
    }
}

/* --- Register shell windows in wmGD.windowContextType for CallWmFunction --- */
static ClientData *ensure_shell_context(Widget shell, long behaviors) {
    if (!shell || !XtIsRealized(shell)) return nullptr;
    Display *dpy = XtDisplay(shell);
    Window win = XtWindow(shell);
    ClientData *existing = nullptr;
    if (XFindContext(dpy, win, wmGD.windowContextType, (XPointer *)&existing) == 0 && existing) {
        existing->dtwmBehaviors = behaviors;
        return existing;
    }
    ClientData *pcd = (ClientData *)XtCalloc(1, sizeof(ClientData));
    pcd->client = win;
    pcd->pSD = &g_fpScreen;
    pcd->dtwmBehaviors = behaviors;
    pcd->clientFunctions = MWM_FUNC_ALL;
    XSaveContext(dpy, win, wmGD.windowContextType, (XPointer)pcd);
    return pcd;
}

static void register_all_subpanel_contexts(void) {
    if (!panel.shell) return;
    CompositeWidget cw;
    Widget *widget_list = M_PopupList(panel.shell);
    for (int i = 0; i < M_NumPopups(panel.shell); ++i) {
        cw = (CompositeWidget)widget_list[i];
        if (XtIsRealized((Widget)cw)) {
            ensure_shell_context((Widget)cw, DtWM_BEHAVIOR_PANEL | DtWM_BEHAVIOR_SUBPANEL);
        }
    }
    for (int i = 0; i < panel.box_data_count; ++i) {
        BoxData *bd = panel.box_data[i];
        if (!bd) continue;
        for (int j = 0; j < bd->control_data_count; ++j) {
            ControlData *cd = bd->control_data[j];
            if (cd && cd->subpanel_data && cd->subpanel_data->shell &&
                XtIsRealized(cd->subpanel_data->shell)) {
                ensure_shell_context(
                    cd->subpanel_data->shell, DtWM_BEHAVIOR_PANEL | DtWM_BEHAVIOR_SUBPANEL
                );
            }
        }
    }
}

/* --- TYPE client X11 Window Embedding (e.g. ck-clock via .dt/types/.fp) --- */
static void publish_embedded_client_names(Display *dpy) {
    if (!panel.shell || !XtIsRealized(panel.shell)) return;
    Atom prop = XInternAtom(dpy, "_DT_PANEL_EMBEDDED_CLIENTS", False);
    char buf[1024];
    int pos = 0;
    WmFpEmbeddedClientList list = (WmFpEmbeddedClientList)panel.embedded_client_list;
    for (int i = 0; i < panel.embedded_client_count; ++i) {
        if (list[i].pchResName && list[i].pchResName[0]) {
            int n = snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%s", list[i].pchResName);
            if (pos + n + 1 < (int)sizeof(buf)) {
                pos += n + 1;
            }
        }
    }
    if (pos > 0) {
        XChangeProperty(
            dpy, XtWindow(panel.shell), prop, XA_STRING, 8, PropModeReplace,
            (const unsigned char *)buf, pos
        );
    }
}

static bool try_embed_x11_window(Display *dpy, Window win) {
    if (win == None || panel.embedded_client_count <= 0) return false;
    if (panel.shell && XtIsRealized(panel.shell) && win == XtWindow(panel.shell)) return false;

    XClassHint ch = {nullptr, nullptr};
    if (!XGetClassHint(dpy, win, &ch)) {
        return false;
    }

    bool matched = false;
    WmFpEmbeddedClientList list = (WmFpEmbeddedClientList)panel.embedded_client_list;
    for (int i = 0; i < panel.embedded_client_count; ++i) {
        WmFpEmbeddedClientData *ecd = &list[i];
        if (!ecd->pchResName || ecd->pCD != nullptr || ecd->winParent == None) {
            continue;
        }
        if ((ch.res_name && strcmp(ch.res_name, ecd->pchResName) == 0) ||
            (ch.res_class && strcmp(ch.res_class, ecd->pchResName) == 0)) {
            ClientData *pcd = (ClientData *)XtCalloc(1, sizeof(ClientData));
            pcd->client = win;
            pcd->pSD = &g_fpScreen;
            pcd->pECD = (struct _WmFpEmbeddedClientData *)ecd;
            ecd->pCD = pcd;

            if (ecd->wControl != nullptr) {
                ecd->winParent = XtWindow(XtParent(ecd->wControl));
                ecd->x = XtX(ecd->wControl) + 3;
                ecd->y = XtY(ecd->wControl) + 3;
                if (XtWidth(ecd->wControl) > 6) ecd->width = XtWidth(ecd->wControl) - 6;
                if (XtHeight(ecd->wControl) > 6) ecd->height = XtHeight(ecd->wControl) - 6;
            }
            unsigned int w = ecd->width > 8 ? (unsigned int)ecd->width : 48u;
            unsigned int h = ecd->height > 8 ? (unsigned int)ecd->height : 48u;
            XUnmapWindow(dpy, win);
            XSetWindowBorderWidth(dpy, win, 0);
            XResizeWindow(dpy, win, w, h);
            XReparentWindow(dpy, win, ecd->winParent, ecd->x, ecd->y);
            XMapWindow(dpy, win);
            XFlush(dpy);
            matched = true;
            break;
        }
    }

    if (ch.res_name) XFree(ch.res_name);
    if (ch.res_class) XFree(ch.res_class);
    return matched;
}

static void scan_existing_embedded_clients(Display *dpy) {
    if (panel.embedded_client_count <= 0) return;
    Window root_ret = None, parent_ret = None, *children = nullptr;
    unsigned int nchildren = 0;
    if (XQueryTree(dpy, DefaultRootWindow(dpy), &root_ret, &parent_ret, &children, &nchildren) && children) {
        for (unsigned int i = 0; i < nchildren; ++i) {
            try_embed_x11_window(dpy, children[i]);
        }
        XFree(children);
    }
}

/* --- WM Bridge Functions Called by Genuine Front Panel Sources --- */

Boolean F_Action(String actionName, ClientData *pCD, XEvent *event) {
    (void)pCD;
    (void)event;
    if (!actionName || !panel.shell) return False;
    DtActionInvoke(panel.shell, actionName, nullptr, 0, nullptr, nullptr, nullptr, 1, nullptr, nullptr);
    return True;
}

Boolean F_Kill(String args, ClientData *pCD, XEvent *event) {
    (void)args;
    (void)event;
    if (!pCD) return False;
    if (pCD->dtwmBehaviors & DtWM_BEHAVIOR_SUBPANEL) {
        Widget sub_shell = XtWindowToWidget(wmGD.display, pCD->client);
        if (sub_shell) {
            XtUnmanageChild(sub_shell);
            return True;
        }
    }
    return False;
}

Boolean F_Minimize(String args, ClientData *pCD, XEvent *event) {
    (void)args;
    (void)pCD;
    (void)event;
    if (panel.shell && XtIsRealized(panel.shell)) {
        XIconifyWindow(wmGD.display, XtWindow(panel.shell), DefaultScreen(wmGD.display));
    }
    return True;
}

Boolean F_Move(String args, ClientData *pCD, XEvent *event) {
    (void)args;
    (void)pCD;
    (void)event;
    send_dt_wm_request(wmGD.display, "f.move_panel");
    return True;
}

Boolean F_Raise(String args, ClientData *pCD, XEvent *event) {
    (void)args;
    (void)event;
    if (pCD && pCD->client != None) {
        XRaiseWindow(wmGD.display, pCD->client);
    }
    return True;
}

static void fp_menu_minimize_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)client_data;
    (void)call_data;
    F_Minimize(nullptr, &g_panelClientData, nullptr);
}

static void fp_menu_exit_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)client_data;
    (void)call_data;
    DtActionInvoke(panel.shell, "ExitSession", nullptr, 0, nullptr, nullptr, nullptr, 1, nullptr, nullptr);
}

Boolean F_Post_FpMenu(String args, ClientData *pCD, XEvent *event) {
    (void)args;
    (void)pCD;
    if (!panel.shell) return False;
    if (!g_fpPopupMenu) {
        g_fpPopupMenu = XmCreatePopupMenu(panel.shell, "fpSystemMenu", nullptr, 0);
        Widget btn_min = XtVaCreateManagedWidget("Minimize", xmPushButtonGadgetClass, g_fpPopupMenu, nullptr);
        XtAddCallback(btn_min, XmNactivateCallback, fp_menu_minimize_cb, nullptr);
        XtVaCreateManagedWidget("sep", xmSeparatorGadgetClass, g_fpPopupMenu, nullptr);
        Widget btn_exit = XtVaCreateManagedWidget("Log Out...", xmPushButtonGadgetClass, g_fpPopupMenu, nullptr);
        XtAddCallback(btn_exit, XmNactivateCallback, fp_menu_exit_cb, nullptr);
    }
    if (event && (event->type == ButtonPress || event->type == ButtonRelease)) {
        XmMenuPosition(g_fpPopupMenu, (XButtonPressedEvent *)event);
    }
    XtManageChild(g_fpPopupMenu);
    return True;
}

Boolean F_Push_Recall(String args, ClientData *pCD, XEvent *event) {
    (void)pCD;
    (void)event;
    if (!args || !panel.shell) return False;
    WmPushRecallArg *prp = (WmPushRecallArg *)args;
    WmFpPushRecallClientList pr_list = (WmFpPushRecallClientList)panel.push_recall_list;
    if (prp->ixReg >= 0 && prp->ixReg < panel.push_recall_count && pr_list) {
        const char *res_name = pr_list[prp->ixReg].pchResName;
        if (res_name && res_name[0]) {
            /* Ask dtwm-wayland to focus/restore an existing instance if running */
            Window root_ret = None, parent_ret = None, *children = nullptr;
            unsigned int nchildren = 0;
            bool found = false;
            if (XQueryTree(wmGD.display, DefaultRootWindow(wmGD.display), &root_ret, &parent_ret, &children, &nchildren) && children) {
                for (unsigned int i = 0; i < nchildren; ++i) {
                    XClassHint ch = {nullptr, nullptr};
                    if (XGetClassHint(wmGD.display, children[i], &ch)) {
                        XWindowAttributes wattr;
                        bool match = (ch.res_name && strcasecmp(ch.res_name, res_name) == 0) ||
                                     (ch.res_class && strcasecmp(ch.res_class, res_name) == 0);
                        if (ch.res_name) XFree(ch.res_name);
                        if (ch.res_class) XFree(ch.res_class);
                        if (match && XGetWindowAttributes(wmGD.display, children[i], &wattr)) {
                            XMapRaised(wmGD.display, children[i]);
                            XSetInputFocus(wmGD.display, children[i], RevertToPointerRoot, CurrentTime);
                            XFlush(wmGD.display);
                            found = true;
                            break;
                        }
                    }
                }
                XFree(children);
            }
            if (found) return True;
        }
    }

    if (prp->wmFunc == F_Action && prp->pArgs) {
        WmActionArg *ap = (WmActionArg *)prp->pArgs;
        if (ap->actionName && ap->actionName[0]) {
            DtActionInvoke(
                panel.shell, ap->actionName, ap->aap, ap->numArgs,
                nullptr, nullptr, nullptr, 1, nullptr, nullptr
            );
            return True;
        }
    }
    return False;
}

int ParseWmFunction(unsigned char **linePP, unsigned int res_spec, WmFunction *pWmFunction) {
    (void)linePP;
    (void)res_spec;
    if (pWmFunction) {
        *pWmFunction = F_Action;
    }
    return 0;
}

Boolean ParseWmFunctionArg(
    unsigned char **linePP,
    int ix,
    WmFunction wmFunc,
    String *pArg,
    String sClientName,
    String sTitle
) {
    (void)ix;
    (void)sClientName;
    (void)sTitle;
    if (!pArg) return False;
    *pArg = nullptr;
    if (wmFunc != F_Action || !linePP || !*linePP) return False;

    const char *src = (const char *)*linePP;
    while (*src && isspace((unsigned char)*src)) src++;
    if (!*src) return False;

    /* Split optional file argument if present */
    const char *sp = src;
    while (*sp && !isspace((unsigned char)*sp)) sp++;
    size_t act_len = (size_t)(sp - src);
    while (*sp && isspace((unsigned char)*sp)) sp++;

    WmActionArg *ap = (WmActionArg *)XtCalloc(1, sizeof(WmActionArg));
    ap->actionName = XtMalloc((Cardinal)(act_len + 1));
    memcpy(ap->actionName, src, act_len);
    ap->actionName[act_len] = '\0';

    if (*sp) {
        ap->numArgs = 1;
        ap->aap = (DtActionArg *)XtCalloc(1, sizeof(DtActionArg));
        ap->aap[0].argClass = DtACTION_FILE;
        ap->aap[0].u.file.name = XtNewString(sp);
    }

    *pArg = (String)ap;
    return True;
}

void RegisterLauncherWindow(WmScreenData *pSD, Window win) {
    (void)pSD;
    (void)win;
    ensure_shell_context(panel.shell, DtWM_BEHAVIOR_PANEL);
    register_all_subpanel_contexts();
}

Boolean ReparentEmbeddedClient(
    WmFpEmbeddedClientData *pECD,
    Widget newControl,
    Window newWin,
    int x,
    int y,
    unsigned int width,
    unsigned int height
) {
    if (!pECD) return False;
    pECD->wControl = newControl;
    pECD->winParent = newWin;
    pECD->x = x;
    pECD->y = y;
    pECD->width = width;
    pECD->height = height;
    if (pECD->pCD && pECD->pCD->client != None && newWin != None) {
        XReparentWindow(wmGD.display, pECD->pCD->client, newWin, x, y);
        XResizeWindow(wmGD.display, pECD->pCD->client, width > 8 ? width : 48, height > 8 ? height : 48);
        XMapWindow(wmGD.display, pECD->pCD->client);
        return True;
    }
    return False;
}

void UnManageWindow(ClientData *pCD) {
    if (!pCD || pCD->client == None) return;
    XReparentWindow(wmGD.display, pCD->client, DefaultRootWindow(wmGD.display), 60, 60);
    if (pCD->pECD) {
        ((WmFpEmbeddedClientData *)pCD->pECD)->pCD = nullptr;
    }
    XtFree((char *)pCD);
}

Boolean WmDtHelpMode(void) {
    if (panel.shell) {
        DtActionInvoke(panel.shell, "Dthelpview", nullptr, 0, nullptr, nullptr, nullptr, 1, nullptr, nullptr);
    }
    return True;
}

void WmDtDisplayTopicHelp(Widget widget, char *helpVolume, char *locationID) {
    (void)widget;
    (void)locationID;
    if (panel.shell && helpVolume) {
        DtActionArg arg = {0};
        arg.argClass = DtACTION_FILE;
        arg.u.file.name = helpVolume;
        DtActionInvoke(panel.shell, "Dthelpview", &arg, 1, nullptr, nullptr, nullptr, 1, nullptr, nullptr);
    }
}

void WmDtDisplayStringHelp(Widget widget, char *helpString) {
    Widget parent = widget ? widget : panel.shell;
    if (!parent || !helpString) return;
    XmString msg = XmStringCreateLocalized(helpString);
    Arg al[2];
    XtSetArg(al[0], XmNmessageString, msg);
    Widget dlg = XmCreateInformationDialog(parent, "fpHelpDialog", al, 1);
    XmStringFree(msg);
    XtManageChild(dlg);
}

/* --- ToolTalk WSM Request Handler & PropertyNotify Bridge --- */
static void emit_tt_workspace_modified(int screen_num, Atom ws_atom, int reason) {
    char *proc_id = tt_default_procid();
    if (tt_ptr_error(proc_id) != TT_OK || !proc_id) {
        return;
    }
    tt_free(proc_id);

    Tt_message msg = tt_pnotice_create(TT_SESSION, "DtWorkspace_Modified");
    if (tt_ptr_error(msg) != TT_OK) {
        return;
    }
    char s_num[32], s_ws[40], s_type[32];
    snprintf(s_num, sizeof(s_num), "%d", screen_num);
    snprintf(s_ws, sizeof(s_ws), "%lu", (unsigned long)ws_atom);
    snprintf(s_type, sizeof(s_type), "%d", reason);
    if (tt_message_arg_add(msg, TT_IN, Tttk_string, s_num) == TT_OK &&
        tt_message_arg_add(msg, TT_IN, Tttk_string, s_ws) == TT_OK &&
        tt_message_arg_add(msg, TT_IN, Tttk_string, s_type) == TT_OK) {
        tt_message_send(msg);
    }
    tt_message_destroy(msg);
}

static Tt_callback_action wayland_wsm_tt_request_cb(Tt_message m, Tt_pattern p) {
    (void)p;
    if (tt_message_state(m) != TT_SENT) {
        return TT_CALLBACK_CONTINUE;
    }
    char *op = tt_message_op(m);
    if (tt_ptr_error(op) != TT_OK || !op) {
        return TT_CALLBACK_CONTINUE;
    }

    Display *dpy = wmGD.display;
    bool handled = false;

    if (strcmp(op, "DtWorkspace_SetCurrent") == 0) {
        char *pch = tt_message_arg_val(m, 1);
        if (tt_ptr_error(pch) == TT_OK && pch) {
            char req[128];
            snprintf(req, sizeof(req), "f.goto_workspace %s", pch);
            send_dt_wm_request(dpy, req);
            tt_free(pch);
        }
        handled = true;
    } else if (strcmp(op, "DtWorkspace_Title_Set") == 0) {
        char *pch_ws = tt_message_arg_val(m, 1);
        char *pch_title = tt_message_arg_val(m, 2);
        if (tt_ptr_error(pch_ws) == TT_OK && pch_ws &&
            tt_ptr_error(pch_title) == TT_OK && pch_title) {
            char req[256];
            snprintf(req, sizeof(req), "f.set_workspace_title %s %s", pch_ws, pch_title);
            send_dt_wm_request(dpy, req);
        }
        if (tt_ptr_error(pch_ws) == TT_OK && pch_ws) tt_free(pch_ws);
        if (tt_ptr_error(pch_title) == TT_OK && pch_title) tt_free(pch_title);
        handled = true;
    } else if (strcmp(op, "DtWorkspace_Add") == 0) {
        char *pch_title = tt_message_arg_val(m, 0);
        char req[256];
        snprintf(
            req, sizeof(req), "f.add_workspace %s",
            (tt_ptr_error(pch_title) == TT_OK && pch_title && pch_title[0]) ? pch_title : "New"
        );
        send_dt_wm_request(dpy, req);
        if (tt_ptr_error(pch_title) == TT_OK && pch_title) tt_free(pch_title);
        handled = true;
    } else if (strcmp(op, "DtWorkspace_Delete") == 0) {
        char *pch_ws = tt_message_arg_val(m, 0);
        if (tt_ptr_error(pch_ws) == TT_OK && pch_ws) {
            char req[128];
            snprintf(req, sizeof(req), "f.delete_workspace %s", pch_ws);
            send_dt_wm_request(dpy, req);
            tt_free(pch_ws);
        }
        handled = true;
    } else if (strcmp(op, "DtPanel_Restore") == 0) {
        send_dt_wm_request(dpy, "f.restart");
        handled = true;
    } else if (strcmp(op, "GetWsmClients") == 0) {
        handled = true;
    }

    tt_free(op);
    if (!handled) {
        return TT_CALLBACK_CONTINUE;
    }
    tt_message_reply(m);
    tt_message_destroy(m);
    return TT_CALLBACK_PROCESSED;
}

static SwitchData *find_panel_switch_data(void) {
    for (int i = 0; i < panel.box_data_count; ++i) {
        if (panel.box_data[i] && panel.box_data[i]->switch_data) {
            return panel.box_data[i]->switch_data;
        }
    }
    return nullptr;
}

static void register_wayland_wsm_tooltalk_handlers(Widget topShell) {
    if (g_ttWsmRegistered || !topShell) {
        return;
    }
    char *proc_id = tt_default_procid();
    if (tt_ptr_error(proc_id) != TT_OK || !proc_id) {
        proc_id = tt_open();
        if (tt_ptr_error(proc_id) != TT_OK || !proc_id) {
            return;
        }
        int ttfd = tt_fd();
        if (tt_int_error(ttfd) == TT_OK && ttfd >= 0) {
            XtAppAddInput(
                XtWidgetToApplicationContext(topShell), ttfd,
                (XtPointer)XtInputReadMask, tttk_Xt_input_handler, proc_id
            );
        }
    } else {
        tt_free(proc_id);
    }

    char *sess = tt_default_session();
    if (tt_ptr_error(sess) != TT_OK || !sess) {
        return;
    }
    Tt_pattern pat = tt_pattern_create();
    if (tt_ptr_error(pat) != TT_OK) {
        tt_free(sess);
        return;
    }
    tt_pattern_category_set(pat, TT_HANDLE);
    tt_pattern_scope_add(pat, TT_SESSION);
    tt_pattern_session_add(pat, sess);
    tt_free(sess);
    tt_pattern_class_add(pat, TT_REQUEST);
    tt_pattern_state_add(pat, TT_SENT);
    tt_pattern_op_add(pat, "DtWorkspace_SetCurrent");
    tt_pattern_op_add(pat, "DtWorkspace_Title_Set");
    tt_pattern_op_add(pat, "DtWorkspace_Add");
    tt_pattern_op_add(pat, "DtWorkspace_Delete");
    tt_pattern_op_add(pat, "DtPanel_Restore");
    tt_pattern_op_add(pat, "GetWsmClients");
    tt_pattern_callback_add(pat, wayland_wsm_tt_request_cb);
    if (tt_pattern_register(pat) == TT_OK) {
        g_ttWsmRegistered = true;
        /* Also re-register WorkspaceModifyCB on ToolTalk if WmPanelistShow ran before ttsession */
        SwitchData *sw = find_panel_switch_data();
        if (sw && panel.shell) {
            DtWsmAddWorkspaceModifiedCallback(panel.shell, WorkspaceModifyCB, (XtPointer)sw);
        }
    }
}

static void retry_tt_wsm_timer_cb(XtPointer client_data, XtIntervalId *id) {
    (void)client_data;
    (void)id;
    if (g_ttWsmRegistered) {
        return;
    }
    register_wayland_wsm_tooltalk_handlers(g_topShell);
    if (!g_ttWsmRegistered && g_ttRetryCount < 30 && g_topShell) {
        g_ttRetryCount++;
        XtAppAddTimeOut(
            XtWidgetToApplicationContext(g_topShell), 500, retry_tt_wsm_timer_cb, nullptr
        );
    }
}

static void sync_panel_workspaces_from_x11(Display *dpy, Atom changed_atom) {
    SwitchData *sw = find_panel_switch_data();
    if (!sw || !panel.shell) return;
    Window root = DefaultRootWindow(dpy);
    int scr_num = DefaultScreen(dpy);

    if (changed_atom == g_atomWsList) {
        Atom *new_atoms = nullptr;
        int new_count = 0;
        if (DtWsmGetWorkspaceList(dpy, root, &new_atoms, &new_count) == Success && new_atoms) {
            if (new_count > sw->switch_count) {
                /* Added workspace is the one not in sw->atom_names */
                for (int i = 0; i < new_count; ++i) {
                    bool seen = false;
                    for (int j = 0; j < sw->switch_count; ++j) {
                        if (sw->atom_names[j] == new_atoms[i]) {
                            seen = true;
                            break;
                        }
                    }
                    if (!seen) {
                        Atom added_ws = new_atoms[i];
                        WorkspaceModifyCB(panel.shell, added_ws, DtWSM_REASON_ADD, (XtPointer)sw);
                        emit_tt_workspace_modified(scr_num, added_ws, DtWSM_REASON_ADD);
                    }
                }
            } else if (new_count < sw->switch_count) {
                for (int j = 0; j < sw->switch_count; ++j) {
                    bool still_there = false;
                    for (int i = 0; i < new_count; ++i) {
                        if (sw->atom_names[j] == new_atoms[i]) {
                            still_there = true;
                            break;
                        }
                    }
                    if (!still_there) {
                        Atom deleted_ws = sw->atom_names[j];
                        WorkspaceModifyCB(panel.shell, deleted_ws, DtWSM_REASON_DELETE, (XtPointer)sw);
                        emit_tt_workspace_modified(scr_num, deleted_ws, DtWSM_REASON_DELETE);
                        break;
                    }
                }
            }
            XFree(new_atoms);
        }
        return;
    }

    if (changed_atom == g_atomWsCurrent) {
        Atom cur = None;
        if (DtWsmGetCurrentWorkspace(dpy, root, &cur) == Success && cur != None) {
            WorkspaceModifyCB(panel.shell, cur, DtWSM_REASON_CURRENT, (XtPointer)sw);
            emit_tt_workspace_modified(scr_num, cur, DtWSM_REASON_CURRENT);
        }
        return;
    }

    /* Check _DT_WORKSPACE_INFO_<ws> title updates */
    char *aname = XGetAtomName(dpy, changed_atom);
    if (aname) {
        if (strncmp(aname, "_DT_WORKSPACE_INFO_", 19) == 0) {
            Atom ws_atom = XInternAtom(dpy, aname + 19, True);
            if (ws_atom != None) {
                WorkspaceModifyCB(panel.shell, ws_atom, DtWSM_REASON_TITLE, (XtPointer)sw);
                emit_tt_workspace_modified(scr_num, ws_atom, DtWSM_REASON_TITLE);
            }
        }
        XFree(aname);
    }
}

static unsigned char parse_profile_arg(const char *s) {
    if (!s) return XmAPPEARANCE_CLASSIC;
    if (strcasecmp(s, "modern_light") == 0 || strcasecmp(s, "modern-light") == 0) {
        return XmAPPEARANCE_MODERN_LIGHT;
    }
    if (strcasecmp(s, "modern_dark") == 0 || strcasecmp(s, "modern-dark") == 0) {
        return XmAPPEARANCE_MODERN_DARK;
    }
    if (strcasecmp(s, "high_contrast") == 0 || strcasecmp(s, "high-contrast") == 0) {
        return XmAPPEARANCE_HIGH_CONTRAST;
    }
    return XmAPPEARANCE_CLASSIC;
}

static XtSignalId g_sigId = 0;

static void handle_term_sig(int sig) {
    (void)sig;
    if (g_sigId) {
        XtNoticeSignal(g_sigId);
    }
}

static void on_xt_term_signal(XtPointer client_data, XtSignalId *id) {
    (void)client_data;
    (void)id;
    WmFrontPanelSessionSaveData();
    exit(0);
}

static int ignore_x_err(Display *dpy, XErrorEvent *ev) {
    (void)dpy;
    (void)ev;
    return 0;
}

int main(int argc, char **argv) {
    unsigned char profile = XmAPPEARANCE_CLASSIC;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--appearance-profile") == 0 && i + 1 < argc) {
            profile = parse_profile_arg(argv[++i]);
        }
    }

    setlocale(LC_ALL, "");
    XtSetLanguageProc(nullptr, nullptr, nullptr);
    XSetErrorHandler(ignore_x_err);
    ensure_cde_search_paths(argv[0]);

    XtToolkitInitialize();
    wmGD.mwmAppContext = XtCreateApplicationContext();
    g_sigId = XtAppAddSignal(wmGD.mwmAppContext, on_xt_term_signal, nullptr);
    signal(SIGTERM, handle_term_sig);
    signal(SIGINT, handle_term_sig);
    Display *dpy = XtOpenDisplay(wmGD.mwmAppContext, nullptr, "dtwm", "Dtwm", nullptr, 0, &argc, argv);
    if (!dpy) {
        fprintf(stderr, "dtwm-panel: failed to open X11 display\n");
        return 1;
    }

    bootstrap_color_server_if_needed(dpy, profile);

    Widget topShell = XtVaAppCreateShell(
        "dtwm", "Dtwm", applicationShellWidgetClass, dpy,
        XmNmappedWhenManaged, False,
        XmNwidth, 1,
        XmNheight, 1,
        nullptr
    );

    if (!DtAppInitialize(wmGD.mwmAppContext, dpy, topShell, "dtwm", "Dtwm")) {
        fprintf(stderr, "dtwm-panel: DtAppInitialize failed\n");
        return 1;
    }
    DtDbLoad();

    memset(&g_fpScreen, 0, sizeof(g_fpScreen));
    g_fpScreen.screen = DefaultScreen(dpy);
    g_fpScreen.rootWindow = DefaultRootWindow(dpy);
    g_fpScreen.screenTopLevelW1 = topShell;
    g_fpScreen.managed = True;

    wmGD.display = dpy;
    wmGD.mwmName = "dtwm";
    wmGD.bMenuButton = Button3;
    wmGD.moveThreshold = 4;
    wmGD.numScreens = 1;
    wmGD.Screens = &g_fpScreen;
    wmGD.dtSD = &g_fpScreen;
    wmGD.useFrontPanel = True;
    wmGD.appearanceProfile = profile;
    wmGD.dpiScale = 1.0;
    wmGD.windowContextType = XUniqueContext();

    g_fpScreen.wPanelist = WmPanelistAllocate(topShell, (XtPointer)&wmGD, (XtPointer)&g_fpScreen);
    if (!g_fpScreen.wPanelist || !panel.shell) {
        fprintf(stderr, "dtwm-panel: FrontPanelReadDatabases found no PANEL definition\n");
        return 1;
    }

    Pixmap iconBitmap = XCreateBitmapFromData(
        dpy, g_fpScreen.rootWindow, (char *)fntpl_i_bm_bits, fntpl_i_bm_width, fntpl_i_bm_height
    );
    if (iconBitmap != None) {
        XtVaSetValues(panel.shell, XmNiconPixmap, iconBitmap, nullptr);
    }

    g_topShell = topShell;
    WmPanelistShow(g_fpScreen.wPanelist);
    register_wayland_wsm_tooltalk_handlers(topShell);
    if (!g_ttWsmRegistered) {
        XtAppAddTimeOut(wmGD.mwmAppContext, 500, retry_tt_wsm_timer_cb, nullptr);
    }
    ensure_shell_context(panel.shell, DtWM_BEHAVIOR_PANEL);
    register_all_subpanel_contexts();
    publish_embedded_client_names(dpy);

    /* Watch _DT_WORKSPACE_* on mwm_win and MapNotify on root for TYPE client windows */
    g_mwmWin = find_mwm_window(dpy);
    g_atomWsCurrent = XInternAtom(dpy, "_DT_WORKSPACE_CURRENT", False);
    g_atomWsList = XInternAtom(dpy, "_DT_WORKSPACE_LIST", False);
    if (g_mwmWin != None) {
        XSelectInput(dpy, g_mwmWin, PropertyChangeMask);
    }
    XSelectInput(dpy, g_fpScreen.rootWindow, SubstructureNotifyMask | PropertyChangeMask);
    scan_existing_embedded_clients(dpy);

    while (!XtAppGetExitFlag(wmGD.mwmAppContext)) {
        XEvent ev;
        XtAppNextEvent(wmGD.mwmAppContext, &ev);
        if (!g_ttWsmRegistered && ev.type == PropertyNotify &&
            ev.xproperty.window == g_fpScreen.rootWindow &&
            ev.xproperty.state == PropertyNewValue) {
            register_wayland_wsm_tooltalk_handlers(topShell);
        }
        if (ev.type == PropertyNotify && ev.xproperty.window == g_mwmWin &&
            ev.xproperty.state == PropertyNewValue) {
            sync_panel_workspaces_from_x11(dpy, ev.xproperty.atom);
        } else if (ev.type == MapNotify && ev.xmap.event == g_fpScreen.rootWindow) {
            register_all_subpanel_contexts();
            try_embed_x11_window(dpy, ev.xmap.window);
        } else if (ev.type == SelectionRequest && g_colorSrvWin != None &&
                   ev.xselectionrequest.owner == g_colorSrvWin) {
            Atom prop_out = None;
            Atom req_prop = ev.xselectionrequest.property != None
                                ? ev.xselectionrequest.property
                                : ev.xselectionrequest.target;
            Atom a_pixel_set = XInternAtom(dpy, XmSPIXEL_SET, False);
            Atom a_type_mon = XInternAtom(dpy, "Type Of Monitor", False);
            if (ev.xselectionrequest.target == a_pixel_set && g_colorSrvPixelSet[0]) {
                XChangeProperty(
                    dpy, ev.xselectionrequest.requestor, req_prop, XA_STRING, 8, PropModeReplace,
                    (const unsigned char *)g_colorSrvPixelSet, (int)strlen(g_colorSrvPixelSet)
                );
                prop_out = req_prop;
            } else if (ev.xselectionrequest.target == a_type_mon && g_colorSrvTypeMon[0]) {
                XChangeProperty(
                    dpy, ev.xselectionrequest.requestor, req_prop, XA_STRING, 8, PropModeReplace,
                    (const unsigned char *)g_colorSrvTypeMon, (int)strlen(g_colorSrvTypeMon)
                );
                prop_out = req_prop;
            }
            XSelectionEvent reply = {
                .type = SelectionNotify,
                .requestor = ev.xselectionrequest.requestor,
                .selection = ev.xselectionrequest.selection,
                .target = ev.xselectionrequest.target,
                .property = prop_out,
                .time = ev.xselectionrequest.time,
            };
            XSendEvent(dpy, reply.requestor, False, 0, (XEvent *)&reply);
            XFlush(dpy);
            continue;
        }
        XtDispatchEvent(&ev);
    }
    WmFrontPanelSessionSaveData();
    return 0;
}
