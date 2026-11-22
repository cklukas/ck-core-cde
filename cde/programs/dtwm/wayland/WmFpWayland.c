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
#include <Dt/SessionM.h>
#include <Dt/SvcTT.h>
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
#include <dirent.h>
#include <limits.h>
#include <locale.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
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
static Atom g_atomSunTtSession = None;
static Atom g_atomTtSession = None;
static Atom g_atomTrashWin = None;
static Atom g_atomSubpanelTorn = None;
static Atom g_atomPanelBusyStartSeq = None;
static Atom g_atomPanelBusyStopSeq = None;
static Window g_watchedTrashWin = None;

extern void WorkspaceModifyCB(Widget, Atom, int, XtPointer);
extern void SubpanelUnmapCB(Widget, XtPointer, XtPointer);
extern void UpdateFileTypeControlFields(void);

void WmStopWaiting(void) {
    if (wmGD.dtSD) {
        WmFrontPanelSetBusy(False);
        if (wmGD.display) {
            XFlush(wmGD.display);
        }
    }
}

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

static Window g_localMwmWin = None;

static void send_dt_wm_request(Display *dpy, const char *req) {
    if (!dpy || !req || !req[0]) return;
    if (g_atomWmRequest == None) {
        g_atomWmRequest = XInternAtom(dpy, "_DT_WM_REQUEST", False);
    }
    const char *wl_act = getenv("CK_MOTIF_WAYLAND_ACTIVE");
    if (wl_act && strcmp(wl_act, "1") == 0) {
        XChangeProperty(
            dpy, DefaultRootWindow(dpy), g_atomWmRequest, XA_STRING, 8, PropModeReplace,
            (const unsigned char *)req, (int)strlen(req) + 1
        );
        XFlush(dpy);
        return;
    }
    if (g_mwmWin == None) {
        g_mwmWin = find_mwm_window(dpy);
    }
    if (g_mwmWin != None) {
        XChangeProperty(
            dpy, g_mwmWin, g_atomWmRequest, XA_STRING, 8, PropModeAppend,
            (const unsigned char *)req, (int)strlen(req) + 1
        );
        XFlush(dpy);
    }
}

static void bootstrap_local_wsm_properties_if_needed(Display *dpy) {
    if (!dpy) return;
    Window root = DefaultRootWindow(dpy);
    Atom *existing_list = nullptr;
    int existing_cnt = 0;
    if (DtWsmGetWorkspaceList(dpy, root, &existing_list, &existing_cnt) == Success &&
        existing_list && existing_cnt > 0) {
        XFree(existing_list);
        return;
    }
    if (existing_list) XFree(existing_list);

    g_localMwmWin = find_mwm_window(dpy);
    if (g_localMwmWin == None) {
        g_localMwmWin = XCreateSimpleWindow(dpy, root, -100, -100, 1, 1, 0, 0, 0);
        if (g_localMwmWin == None) return;
        Atom mwm_info_atom = XInternAtom(dpy, "_MOTIF_WM_INFO", False);
        long mwm_info[2] = {2L /* MWM_INFO_STARTUP_CUSTOM */, (long)g_localMwmWin};
        XChangeProperty(
            dpy, root, mwm_info_atom, mwm_info_atom, 32, PropModeReplace,
            (const unsigned char *)mwm_info, 2
        );
    }

    static const struct {
        const char *id;
        const char *title;
        int cs_id;
        const char *bg;
        const char *fg;
        const char *backdrop;
    } defs[4] = {
        {"ws0", "One",   3, "0x8998aa", "0xffffff", "KnitLight"},
        {"ws1", "Two",   8, "0x686f82", "0xffffff", "WaterDrops"},
        {"ws2", "Three", 6, "0x7c8498", "0xffffff", "SkyDark"},
        {"ws3", "Four",  7, "0x727a8e", "0xffffff", "Paver"},
    };

    Atom ws_atoms[4];
    for (int i = 0; i < 4; ++i) {
        ws_atoms[i] = XInternAtom(dpy, defs[i].id, False);
        Atom b_atom = XInternAtom(dpy, defs[i].backdrop, False);
        char info_name[96];
        snprintf(info_name, sizeof(info_name), "_DT_WORKSPACE_INFO_%s", defs[i].id);
        Atom info_atom = XInternAtom(dpy, info_name, False);

        char buf[512];
        int off = snprintf(buf, sizeof(buf), "%s", defs[i].title) + 1;
        off += snprintf(buf + off, sizeof(buf) - (size_t)off, "%d", defs[i].cs_id) + 1;
        off += snprintf(buf + off, sizeof(buf) - (size_t)off, "%s", defs[i].bg) + 1;
        off += snprintf(buf + off, sizeof(buf) - (size_t)off, "%s", defs[i].fg) + 1;
        off += snprintf(buf + off, sizeof(buf) - (size_t)off, "%lu", (unsigned long)b_atom) + 1;
        off += snprintf(buf + off, sizeof(buf) - (size_t)off, "0") + 1;
        off += snprintf(buf + off, sizeof(buf) - (size_t)off, "0") + 1;
        XChangeProperty(
            dpy, g_localMwmWin, info_atom, XA_STRING, 8, PropModeReplace,
            (const unsigned char *)buf, off
        );
    }

    Atom list_atom = XInternAtom(dpy, "_DT_WORKSPACE_LIST", False);
    XChangeProperty(
        dpy, g_localMwmWin, list_atom, XA_ATOM, 32, PropModeReplace,
        (const unsigned char *)ws_atoms, 4
    );

    Atom cur_ws = ws_atoms[0];
    Atom a_cur_name = XInternAtom(dpy, "_DT_WORKSPACE_CURRENT_NAME", False);
    Atom act_t = None;
    int act_f = 0;
    unsigned long nitems = 0, rem = 0;
    unsigned char *wname = nullptr;
    if (XGetWindowProperty(
            dpy, root, a_cur_name, 0, 64, False, AnyPropertyType,
            &act_t, &act_f, &nitems, &rem, &wname
        ) == Success && wname && nitems > 0) {
        char wbuf[64];
        size_t wn = nitems < sizeof(wbuf) - 1 ? (size_t)nitems : sizeof(wbuf) - 1;
        memcpy(wbuf, wname, wn);
        wbuf[wn] = '\0';
        cur_ws = XInternAtom(dpy, wbuf, False);
        XFree(wname);
    } else if (wname) {
        XFree(wname);
    }

    Atom cur_atom = XInternAtom(dpy, "_DT_WORKSPACE_CURRENT", False);
    XChangeProperty(
        dpy, g_localMwmWin, cur_atom, XA_ATOM, 32, PropModeReplace,
        (const unsigned char *)&cur_ws, 1
    );
    XSync(dpy, False);
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
    Window existing = XGetSelectionOwner(dpy, cust_atom);
    if (existing != None) {
        XWindowAttributes wa;
        memset(&wa, 0, sizeof(wa));
        if (XGetWindowAttributes(dpy, existing, &wa)) {
            /* Live Color Server is already active on this display */
            return;
        }
    }

    static const char *const classic_bgs[8] = {
        "#ed00a8007000", "#9900991b99fe", "#89559808aa00", "#68006f008200",
        "#c600b2d2a87e", "#49009200a700", "#b70087008d00", "#938eab73bf00",
    };
    static const char *const modern_light_bgs[8] = {
        "#4a007900b000", "#9d00a600b500", "#7c009300ad00", "#f400f600fa00",
        "#d900de00e600", "#66008a00ab00", "#7800a1009300", "#a80088009a00",
    };
    static const char *const modern_dark_bgs[8] = {
        "#3d007400b800", "#350039004200", "#250031004200", "#160018001d00",
        "#210024002b00", "#2d003c005200", "#470036005400", "#24004a005000",
    };
    static const char *const high_contrast_bgs[8] = {
        "#ff00ff000000", "#000000000000", "#000000000000", "#000000000000",
        "#000000000000", "#000000000000", "#000000000000", "#000000000000",
    };

    const char *const *bgs = classic_bgs;
    if (profile == XmAPPEARANCE_MODERN_LIGHT) bgs = modern_light_bgs;
    else if (profile == XmAPPEARANCE_MODERN_DARK) bgs = modern_dark_bgs;
    else if (profile == XmAPPEARANCE_HIGH_CONTRAST) bgs = high_contrast_bgs;

    /* Respect *ColorPalette in RESOURCE_MANAGER if already configured */
    char file_bgs[8][48];
    const char *file_bg_ptrs[8];
    {
        char *rms = XResourceManagerString(dpy);
        if (rms && *rms) {
            XrmDatabase rdb = XrmGetStringDatabase(rms);
            if (rdb) {
                char qname[64], qclass[64];
                snprintf(qname, sizeof(qname), "dtwm.%d.colorPalette", scr_num);
                snprintf(qclass, sizeof(qclass), "Dtwm.%d.ColorPalette", scr_num);
                char *rtype = nullptr;
                XrmValue rval = {0, nullptr};
                if ((XrmGetResource(rdb, qname, qclass, &rtype, &rval) ||
                     XrmGetResource(rdb, "colorPalette", "ColorPalette", &rtype, &rval)) &&
                    rval.addr && rval.addr[0]) {
                    char pname[128];
                    snprintf(pname, sizeof(pname), "%s", rval.addr);
                    if (!strstr(pname, ".dp") && strlen(pname) + 4 < sizeof(pname))
                        strcat(pname, ".dp");
                    const char *home = getenv("HOME");
                    char cpaths[3][PATH_MAX];
                    int ncp = 0;
                    if (home && home[0])
                        snprintf(cpaths[ncp++], sizeof(cpaths[0]), "%s/.dt/palettes/%s", home, pname);
                    snprintf(cpaths[ncp++], sizeof(cpaths[0]), "/etc/dt/palettes/%s", pname);
                    snprintf(cpaths[ncp++], sizeof(cpaths[0]), CDE_INSTALLATION_TOP "/palettes/%s", pname);
                    for (int ci = 0; ci < ncp; ++ci) {
                        FILE *fp = fopen(cpaths[ci], "r");
                        if (!fp) continue;
                        int got = 0;
                        char line[128];
                        while (got < 8 && fgets(line, sizeof(line), fp)) {
                            size_t ll = strcspn(line, "\r\n:");
                            line[ll] = '\0';
                            if (ll > 0) {
                                snprintf(file_bgs[got], sizeof(file_bgs[got]), "%s", line);
                                file_bg_ptrs[got] = file_bgs[got];
                                ++got;
                            }
                        }
                        fclose(fp);
                        if (got == 8) {
                            bgs = file_bg_ptrs;
                            break;
                        }
                    }
                }
                XrmDestroyDatabase(rdb);
            }
        }
    }

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
    XSelectInput(dpy, g_colorSrvWin, PropertyChangeMask);
    Atom pixel_set_prop = XInternAtom(dpy, XmSPIXEL_SET_PROP, False);
    XChangeProperty(
        dpy, g_colorSrvWin, pixel_set_prop, XA_STRING, 8, PropModeReplace,
        (const unsigned char *)buf, pos
    );
    Window root_win = DefaultRootWindow(dpy);
    if (root_win != None) {
        Atom type_mon_prop = XInternAtom(dpy, "_DT_COLOR_TYPE_MONITOR", False);
        if (type_mon_prop != None) {
            XChangeProperty(
                dpy, root_win, type_mon_prop, XA_STRING, 8, PropModeReplace,
                (const unsigned char *)g_colorSrvTypeMon,
                (int)strlen(g_colorSrvTypeMon)
            );
        }
        XChangeProperty(
            dpy, root_win, pixel_set_prop, XA_STRING, 8, PropModeReplace,
            (const unsigned char *)buf, pos
        );
    }
    XSetSelectionOwner(dpy, cust_atom, g_colorSrvWin, CurrentTime);
    XFlush(dpy);
}

/* --- Discover appconfig search paths for both installed (/usr/dt) and build-tree runs --- */
static void ensure_appmanager_gathered(void) {
    const char *usess = getenv("DTUSERSESSION");
    if (!usess || !usess[0] || strchr(usess, '/')) {
        return;
    }
    char check_path[PATH_MAX];
    snprintf(
        check_path, sizeof(check_path),
        "/var/dt/appconfig/appmanager/%s/Desktop_Apps", usess
    );
    struct stat st;
    if (stat(check_path, &st) == 0) {
        return;
    }
    const char *gather_bin = CDE_INSTALLATION_TOP "/bin/dtappgather";
    if (access(gather_bin, X_OK) != 0) {
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        execl(gather_bin, "dtappgather", (char *)nullptr);
        _exit(127);
    } else if (pid > 0) {
        int status = 0;
        (void)waitpid(pid, &status, 0);
    }
}

static void ensure_cde_search_paths(const char *argv0) {
    struct stat st;
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
        char dt_dir[PATH_MAX], sess_dir[PATH_MAX], types_dir[PATH_MAX], dyn_dir[PATH_MAX], app_dir[PATH_MAX];
        snprintf(dt_dir, sizeof(dt_dir), "%s/.dt", home);
        snprintf(sess_dir, sizeof(sess_dir), "%s/.dt/sessions", home);
        snprintf(types_dir, sizeof(types_dir), "%s/.dt/types", home);
        snprintf(dyn_dir, sizeof(dyn_dir), "%s/.dt/types/fp_dynamic", home);
        snprintf(app_dir, sizeof(app_dir), "%s/.dt/appmanager", home);
        mkdir(dt_dir, 0755);
        mkdir(sess_dir, 0755);
        mkdir(types_dir, 0755);
        mkdir(dyn_dir, 0755);
        mkdir(app_dir, 0755);
    }

    if (!getenv("DTUSERSESSION") || !getenv("DTUSERSESSION")[0]) {
        const char *u = getenv("USER");
        if (!u || !u[0]) u = getenv("LOGNAME");
        struct passwd *pw = (!u || !u[0]) ? getpwuid(getuid()) : nullptr;
        if (pw && pw->pw_name && pw->pw_name[0]) u = pw->pw_name;
        if (!u || !u[0]) u = "user";
        char usess[256];
        snprintf(usess, sizeof(usess), "%s-wayland", u);
        for (char *p = usess; *p; ++p) {
            if (*p == '/') *p = '_';
        }
        setenv("DTUSERSESSION", usess, 1);
    }

    if (!getenv("DTAPPSEARCHPATH") || !getenv("DTAPPSEARCHPATH")[0]) {
        char apppath[PATH_MAX * 2];
        if (home && home[0]) {
            snprintf(
                apppath, sizeof(apppath),
                "%s/.dt/appmanager:%s/appmanager/%%L:%s/appmanager/C",
                home, appcfg, appcfg
            );
        } else {
            snprintf(
                apppath, sizeof(apppath),
                "%s/appmanager/%%L:%s/appmanager/C",
                appcfg, appcfg
            );
        }
        setenv("DTAPPSEARCHPATH", apppath, 1);
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

    if (!getenv("XBMLANGPATH")) {
        char xbmpath[PATH_MAX * 4];
        if (home && home[0]) {
            snprintf(
                xbmpath, sizeof(xbmpath),
                "%s/.dt/icons/%%B%%M.bm:%s/.dt/icons/%%B:"
                "%s/icons/%%L/%%B%%M.bm:%s/icons/%%L/%%B:"
                "%s/icons/C/%%B%%M.bm:%s/icons/C/%%B:"
                CDE_INSTALLATION_TOP "/include/X11/bitmaps/%%B:/usr/include/X11/bitmaps/%%B:%%D",
                home, home, appcfg, appcfg, appcfg, appcfg
            );
        } else {
            snprintf(
                xbmpath, sizeof(xbmpath),
                "%s/icons/%%L/%%B%%M.bm:%s/icons/%%L/%%B:"
                "%s/icons/C/%%B%%M.bm:%s/icons/C/%%B:"
                CDE_INSTALLATION_TOP "/include/X11/bitmaps/%%B:/usr/include/X11/bitmaps/%%B:%%D",
                appcfg, appcfg, appcfg, appcfg
            );
        }
        setenv("XBMLANGPATH", xbmpath, 1);
    }

    if (!getenv("DTHELPSEARCHPATH") || !getenv("DTHELPSEARCHPATH")[0]) {
        char helppath[PATH_MAX * 4];
        if (home && home[0]) {
            snprintf(
                helppath, sizeof(helppath),
                "%s/.dt/help/%%H:%s/.dt/help/%%H.sdl:%s/.dt/help/%%H.hv:"
                "%s/help/%%L/%%H:%s/help/%%L/%%H.sdl:%s/help/%%L/%%H.hv:"
                "%s/help/C/%%H:%s/help/C/%%H.sdl:%s/help/C/%%H.hv",
                home, home, home, appcfg, appcfg, appcfg, appcfg, appcfg, appcfg
            );
        } else {
            snprintf(
                helppath, sizeof(helppath),
                "%s/help/%%L/%%H:%s/help/%%L/%%H.sdl:%s/help/%%L/%%H.hv:"
                "%s/help/C/%%H:%s/help/C/%%H.sdl:%s/help/C/%%H.hv",
                appcfg, appcfg, appcfg, appcfg, appcfg, appcfg
            );
        }
        setenv("DTHELPSEARCHPATH", helppath, 1);
    }

    if (!getenv("DTINFOLIBSEARCHPATH")) {
        setenv(
            "DTINFOLIBSEARCHPATH",
            CDE_INSTALLATION_TOP "/infolib/%L/%I.dti:" CDE_INSTALLATION_TOP "/infolib/C/%I.dti",
            1
        );
    }
    if (!getenv("DTINFOLIBDEFAULT")) {
        setenv("DTINFOLIBDEFAULT", "cde", 1);
    }

    ensure_appmanager_gathered();
}

static void ensure_ttsession_running(Display *dpy, const char *argv0) {
    if (!dpy) return;
    Window root = DefaultRootWindow(dpy);
    Atom a_sun_tt = XInternAtom(dpy, "_SUN_TT_SESSION", False);
    Atom a_dt_tt = XInternAtom(dpy, "TT_SESSION", False);

    const char *env_tt = getenv("TT_SESSION");
    if (env_tt && env_tt[0]) {
        setenv("_SUN_TT_SESSION", env_tt, 1);
        XChangeProperty(
            dpy, root, a_sun_tt, XA_STRING, 8, PropModeReplace,
            (const unsigned char *)env_tt, (int)strlen(env_tt)
        );
        XChangeProperty(
            dpy, root, a_dt_tt, XA_STRING, 8, PropModeReplace,
            (const unsigned char *)env_tt, (int)strlen(env_tt)
        );
        XFlush(dpy);
        return;
    }

    const char *prop_names[2] = {"_SUN_TT_SESSION", "TT_SESSION"};
    for (int i = 0; i < 2; ++i) {
        Atom a = XInternAtom(dpy, prop_names[i], True);
        if (a == None) continue;
        Atom actual = None;
        int fmt = 0;
        unsigned long nitems = 0, after = 0;
        unsigned char *data = nullptr;
        if (XGetWindowProperty(
                dpy, root, a, 0, 128, False, AnyPropertyType,
                &actual, &fmt, &nitems, &after, &data
            ) == Success && data) {
            if (nitems > 0 && data[0] != '\0') {
                char sbuf[256];
                size_t sn = nitems < sizeof(sbuf) - 1 ? (size_t)nitems : sizeof(sbuf) - 1;
                memcpy(sbuf, data, sn);
                sbuf[sn] = '\0';
                setenv("TT_SESSION", sbuf, 1);
                setenv("_SUN_TT_SESSION", sbuf, 1);
                XFree(data);
                return;
            }
            XFree(data);
        }
    }

    char build_tt_bin[PATH_MAX] = {0};
    const char *tt_bin = CDE_INSTALLATION_TOP "/bin/ttsession";
    struct stat st;
    if ((stat(tt_bin, &st) != 0 || !(st.st_mode & S_IXUSR)) && argv0) {
        char resolved[PATH_MAX];
        if (realpath(argv0, resolved)) {
            char *slash = strrchr(resolved, '/');
            if (slash) {
                *slash = '\0';
                static const char *const tt_rels[2] = {
                    "../../lib/tt/bin/ttsession/ttsession",
                    "../../../lib/tt/bin/ttsession/ttsession"
                };
                for (int r = 0; r < 2; ++r) {
                    snprintf(build_tt_bin, sizeof(build_tt_bin), "%s/%s", resolved, tt_rels[r]);
                    if (stat(build_tt_bin, &st) == 0 && (st.st_mode & S_IXUSR)) {
                        tt_bin = build_tt_bin;
                        break;
                    }
                }
            }
        }
    }
    if (stat(tt_bin, &st) != 0 || !(st.st_mode & S_IXUSR)) {
        return;
    }

    const char *wl_act = getenv("CK_MOTIF_WAYLAND_ACTIVE");
    if (wl_act && strcmp(wl_act, "1") == 0) {
        int pfd[2] = {-1, -1};
        if (pipe(pfd) != 0) return;
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            close(pfd[0]);
            dup2(pfd[1], STDOUT_FILENO);
            close(pfd[1]);
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                dup2(devnull, STDERR_FILENO);
                if (devnull > 2) close(devnull);
            }
            unsetenv("DISPLAY");
            char *const args[] = {(char *)tt_bin, (char *)"-p", nullptr};
            execv(tt_bin, args);
            _exit(127);
        }
        close(pfd[1]);
        if (pid > 0) {
            char line[256] = {0};
            size_t len = 0;
            while (len + 1 < sizeof(line)) {
                char ch = 0;
                ssize_t r = read(pfd[0], &ch, 1);
                if (r <= 0 || ch == '\n' || ch == '\r') break;
                line[len++] = ch;
            }
            line[len] = '\0';
            close(pfd[0]);
            if (len > 0) {
                setenv("TT_SESSION", line, 1);
                setenv("_SUN_TT_SESSION", line, 1);
                XChangeProperty(
                    dpy, root, a_sun_tt, XA_STRING, 8, PropModeReplace,
                    (const unsigned char *)line, (int)len
                );
                XChangeProperty(
                    dpy, root, a_dt_tt, XA_STRING, 8, PropModeReplace,
                    (const unsigned char *)line, (int)len
                );
                XSync(dpy, False);
            }
        } else {
            close(pfd[0]);
        }
        return;
    }

    const char *dpy_str = DisplayString(dpy);
    pid_t pid = fork();
    if (pid == 0) {
        if (dpy_str && dpy_str[0]) {
            setenv("DISPLAY", dpy_str, 1);
        }
        char *const args[] = {(char *)tt_bin, (char *)"-s", nullptr};
        execv(tt_bin, args);
        _exit(127);
    }
    if (pid > 0) {
        int status = 0;
        (void)waitpid(pid, &status, 0);
        XSync(dpy, False);
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
                Window sp_win = XtWindow(cd->subpanel_data->shell);
                XClassHint cur_hint = {nullptr, nullptr};
                bool has_sp_hint = false;
                if (XGetClassHint(wmGD.display, sp_win, &cur_hint)) {
                    if (cur_hint.res_name && strncmp(cur_hint.res_name, "dtwm-subpanel", 13) == 0) {
                        has_sp_hint = true;
                    }
                    if (cur_hint.res_name) XFree(cur_hint.res_name);
                    if (cur_hint.res_class) XFree(cur_hint.res_class);
                }
                if (!has_sp_hint) {
                    XClassHint sp_hint = {
                        .res_name = (char *)"dtwm-subpanel",
                        .res_class = (char *)"Dtwm",
                    };
                    XSetClassHint(wmGD.display, sp_win, &sp_hint);
                }
                ensure_shell_context(
                    cd->subpanel_data->shell, DtWM_BEHAVIOR_PANEL | DtWM_BEHAVIOR_SUBPANEL
                );
                XSelectInput(
                    wmGD.display, sp_win,
                    StructureNotifyMask | PropertyChangeMask
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
                int cw = (int)XtWidth(ecd->wControl);
                int ch_h = (int)XtHeight(ecd->wControl);
                int tw = (ecd->width > 8) ? (int)ecd->width : (cw > 6 ? cw - 6 : 48);
                int th = (ecd->height > 8) ? (int)ecd->height : (ch_h > 4 ? ch_h - 4 : 48);
                if (cw > 8 && tw > cw) tw = cw;
                if (ch_h > 8 && th > ch_h) th = ch_h;
                ecd->width = (unsigned int)tw;
                ecd->height = (unsigned int)th;
                ecd->x = (int)XtX(ecd->wControl) + (cw - tw) / 2;
                ecd->y = (int)XtY(ecd->wControl) + (ch_h - th) / 2;
            }
            unsigned int w = ecd->width > 8 ? (unsigned int)ecd->width : 48u;
            unsigned int h = ecd->height > 8 ? (unsigned int)ecd->height : 48u;
            XWindowAttributes pattr;
            if (XGetWindowAttributes(dpy, ecd->winParent, &pattr)) {
                XSelectInput(dpy, ecd->winParent, pattr.your_event_mask | SubstructureNotifyMask);
            }
            XUnmapWindow(dpy, win);
            XSetWindowBorderWidth(dpy, win, 0);
            XReparentWindow(dpy, win, ecd->winParent, ecd->x, ecd->y);
            XMoveResizeWindow(dpy, win, ecd->x, ecd->y, w, h);
            XMapWindow(dpy, win);
            XFlush(dpy);
            WmStopWaiting();
            matched = true;
            break;
        }
    }

    if (ch.res_name) XFree(ch.res_name);
    if (ch.res_class) XFree(ch.res_class);
    return matched;
}

static void clear_destroyed_embedded_client(Window destroyed_win) {
    if (destroyed_win == None || panel.embedded_client_count <= 0) return;
    WmFpEmbeddedClientList list = (WmFpEmbeddedClientList)panel.embedded_client_list;
    for (int i = 0; i < panel.embedded_client_count; ++i) {
        WmFpEmbeddedClientData *ecd = &list[i];
        if (ecd->pCD && ecd->pCD->client == destroyed_win) {
            XtFree((char *)ecd->pCD);
            ecd->pCD = nullptr;
            return;
        }
    }
}

static void enforce_embedded_client_geometry(Display *dpy, const XConfigureEvent *cev) {
    if (!dpy || !cev || panel.embedded_client_count <= 0) return;
    WmFpEmbeddedClientList list = (WmFpEmbeddedClientList)panel.embedded_client_list;
    for (int i = 0; i < panel.embedded_client_count; ++i) {
        WmFpEmbeddedClientData *ecd = &list[i];
        if (!ecd->pCD || ecd->pCD->client != cev->window || ecd->winParent == None) {
            continue;
        }
        if (ecd->wControl != nullptr) {
            int cw = (int)XtWidth(ecd->wControl);
            int ch_h = (int)XtHeight(ecd->wControl);
            int tw = (ecd->width > 8) ? (int)ecd->width : 48;
            int th = (ecd->height > 8) ? (int)ecd->height : 48;
            if (cw > 8 && tw > cw) tw = cw;
            if (ch_h > 8 && th > ch_h) th = ch_h;
            ecd->x = (int)XtX(ecd->wControl) + (cw - tw) / 2;
            ecd->y = (int)XtY(ecd->wControl) + (ch_h - th) / 2;
        }
        unsigned int w = ecd->width > 8 ? (unsigned int)ecd->width : 48u;
        unsigned int h = ecd->height > 8 ? (unsigned int)ecd->height : 48u;
        if (cev->x != ecd->x || cev->y != ecd->y ||
            cev->width != (int)w || cev->height != (int)h) {
            XMoveResizeWindow(dpy, cev->window, ecd->x, ecd->y, w, h);
            XFlush(dpy);
        }
        return;
    }
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
    WmActionArg *ap = (WmActionArg *)actionName;
    if (!ap->actionName || !ap->actionName[0]) return False;
    if (!g_ttWsmRegistered && wmGD.dtSD) {
        WmFrontPanelSetBusy(True);
    }
    DtActionInvoke(
        panel.shell, ap->actionName, ap->aap, ap->numArgs,
        ap->szExecParms, nullptr, nullptr, 1, nullptr, nullptr
    );
    return True;
}

Boolean F_Kill(String args, ClientData *pCD, XEvent *event) {
    (void)args;
    (void)event;
    if (!pCD) return False;
    if (pCD->dtwmBehaviors & DtWM_BEHAVIOR_SUBPANEL) {
        Widget sub_shell = XtWindowToWidget(wmGD.display, pCD->client);
        if (sub_shell) {
            /* Unmanage the XmForm child (matching X11 SlideSubpanelBackIn) so
             * XmNunmapCallback -> SubpanelUnmapCB resets the arrow to Fpup! */
            for (int i = 0; i < panel.box_data_count; ++i) {
                BoxData *bd = panel.box_data[i];
                if (!bd) continue;
                for (int j = 0; j < bd->control_data_count; ++j) {
                    ControlData *cd = bd->control_data[j];
                    if (cd && cd->subpanel_data && cd->subpanel_data->shell == sub_shell) {
                        if (cd->subpanel_data->form && XtIsManaged(cd->subpanel_data->form)) {
                            XtUnmanageChild(cd->subpanel_data->form);
                        } else {
                            SubpanelUnmapCB(cd->subpanel_data->form, (XtPointer)cd, nullptr);
                        }
                        return True;
                    }
                }
            }
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

void WmFpWaylandInvokeExitSession(Widget w);

static void fp_menu_exit_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)client_data;
    (void)call_data;
    WmFpWaylandInvokeExitSession(panel.shell);
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

static bool window_has_managed_wm_state(Display *dpy, Window win, Atom wm_state_atom) {
    if (!dpy || win == None || wm_state_atom == None) return false;
    Atom act_type = None;
    int act_fmt = 0;
    unsigned long nitems = 0, rem = 0;
    unsigned char *data = nullptr;
    bool ok = false;
    if (XGetWindowProperty(
            dpy, win, wm_state_atom, 0, 2, False, AnyPropertyType,
            &act_type, &act_fmt, &nitems, &rem, &data
        ) == Success && data) {
        if (act_fmt == 32 && nitems >= 1) {
            unsigned long st = *(unsigned long *)data;
            /* NormalState (1) or IconicState (3), never WithdrawnState (0) */
            ok = (st == 1UL || st == 3UL);
        }
        XFree(data);
    }
    return ok;
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
            Display *dpy = wmGD.display;
            Window root = DefaultRootWindow(dpy);
            bool found = false;
            Window matched_x11_win = None;

            /* 1. Check dtwm-wayland's live registry of mapped Wayland & Xwayland app IDs */
            bool has_wayland_registry = false;
            Atom ids_atom = XInternAtom(dpy, "_DT_WAYLAND_CLIENT_IDS", True);
            if (ids_atom != None) {
                Atom act_type = None;
                int act_fmt = 0;
                unsigned long nitems = 0, rem = 0;
                unsigned char *prop = nullptr;
                if (XGetWindowProperty(
                        dpy, root, ids_atom, 0, 2048, False, XA_STRING,
                        &act_type, &act_fmt, &nitems, &rem, &prop
                    ) == Success) {
                    if (act_type == XA_STRING && act_fmt == 8) {
                        has_wayland_registry = true;
                        if (prop && nitems > 0) {
                            const char *p = (const char *)prop;
                            const char *end = p + nitems;
                            while (p < end) {
                                size_t slen = strnlen(p, (size_t)(end - p));
                                if (slen > 0 && strcasecmp(p, res_name) == 0) {
                                    found = true;
                                    break;
                                }
                                p += slen + 1;
                            }
                        }
                    }
                    if (prop) {
                        XFree(prop);
                    }
                }
            }

            if (has_wayland_registry) {
                if (found) {
                    char req[192];
                    snprintf(req, sizeof(req), "f.recall_client %s", res_name);
                    send_dt_wm_request(dpy, req);
                    XFlush(dpy);
                    return True;
                }
            } else {
                /* 2. Standalone X11 fallback: check managed X11 windows carrying WM_STATE */
                Atom wm_state_atom = XInternAtom(dpy, "WM_STATE", True);
                Window root_ret = None, parent_ret = None, *children = nullptr;
                unsigned int nchildren = 0;
                Window panel_win = XtIsRealized(panel.shell) ? XtWindow(panel.shell) : None;
                Window class_fallback_win = None;
                if (XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &nchildren) && children) {
                    for (unsigned int i = 0; i < nchildren; ++i) {
                        if (children[i] == panel_win) continue;
                        XClassHint ch = {nullptr, nullptr};
                        if (XGetClassHint(dpy, children[i], &ch)) {
                            XWindowAttributes wattr;
                            bool inst_match = (ch.res_name && strcasecmp(ch.res_name, res_name) == 0);
                            bool cls_match = (ch.res_class && strcasecmp(ch.res_class, res_name) == 0);
                            if (ch.res_name) XFree(ch.res_name);
                            if (ch.res_class) XFree(ch.res_class);
                            if ((inst_match || cls_match) &&
                                XGetWindowAttributes(dpy, children[i], &wattr) &&
                                !wattr.override_redirect &&
                                window_has_managed_wm_state(dpy, children[i], wm_state_atom)) {
                                if (inst_match) {
                                    matched_x11_win = children[i];
                                    break;
                                }
                                if (class_fallback_win == None) {
                                    class_fallback_win = children[i];
                                }
                            }
                        }
                    }
                    XFree(children);
                }
                if (matched_x11_win == None) {
                    matched_x11_win = class_fallback_win;
                }
                if (matched_x11_win != None) {
                    Atom net_active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
                    XEvent aev;
                    memset(&aev, 0, sizeof(aev));
                    aev.xclient.type = ClientMessage;
                    aev.xclient.window = matched_x11_win;
                    aev.xclient.message_type = net_active;
                    aev.xclient.format = 32;
                    aev.xclient.data.l[0] = 2;
                    aev.xclient.data.l[1] = CurrentTime;
                    XSendEvent(
                        dpy, root, False,
                        SubstructureRedirectMask | SubstructureNotifyMask, &aev
                    );
                    XFlush(dpy);
                    return True;
                }
            }
        }
    }

    if (prp->wmFunc && prp->pArgs) {
        return prp->wmFunc(prp->pArgs, pCD, event);
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

static bool g_smXerrTrapped = false;

static int trap_sm_x_error(Display *dpy, XErrorEvent *ev) {
    (void)dpy;
    (void)ev;
    g_smXerrTrapped = true;
    return 0;
}

static bool is_dtsession_alive(Display *dpy, Window *out_sm_win) {
    if (out_sm_win) *out_sm_win = None;
    if (!dpy) return false;
    Window sm_win = None;
    if (_DtGetSmWindow(dpy, DefaultRootWindow(dpy), &sm_win) != Success || sm_win == None) {
        return false;
    }
    XWindowAttributes wa;
    SmStateInfo sm_state;
    memset(&wa, 0, sizeof(wa));
    memset(&sm_state, 0, sizeof(sm_state));
    g_smXerrTrapped = false;
    int (*prev)(Display *, XErrorEvent *) = XSetErrorHandler(trap_sm_x_error);
    Status ok = XGetWindowAttributes(dpy, sm_win, &wa);
    Status state_ok = ok ? _DtGetSmState(dpy, sm_win, &sm_state) : BadWindow;
    XSync(dpy, False);
    XSetErrorHandler(prev);
    if (!ok || state_ok != Success || g_smXerrTrapped) {
        return false;
    }
    if (out_sm_win) *out_sm_win = sm_win;
    return true;
}

static void fallback_exit_without_tt_sm(Display *dpy) {
    if (!dpy) return;
    Window sm_win = None;
    if (is_dtsession_alive(dpy, &sm_win) && sm_win != None) {
        Atom proto = XInternAtom(dpy, _XA_DT_SM_WM_PROTOCOL, False);
        Atom exit_a = XInternAtom(dpy, _XA_DT_WM_EXIT_SESSION, False);
        if (proto != None && exit_a != None) {
            XClientMessageEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = ClientMessage;
            ev.window = sm_win;
            ev.message_type = proto;
            ev.format = 32;
            ev.data.l[0] = (long)exit_a;
            ev.data.l[1] = (long)CurrentTime;
            g_smXerrTrapped = false;
            int (*prev)(Display *, XErrorEvent *) = XSetErrorHandler(trap_sm_x_error);
            XSendEvent(dpy, sm_win, False, NoEventMask, (XEvent *)&ev);
            XSync(dpy, False);
            XSetErrorHandler(prev);
            if (!g_smXerrTrapped) {
                return;
            }
        }
    }
    send_dt_wm_request(dpy, "f.quit_mwm");
}

static Tt_callback_action exit_session_tt_reply_cb(Tt_message m, Tt_pattern p) {
    (void)p;
    Tt_state st = tt_message_state(m);
    if (st == TT_HANDLED) {
        tttk_message_destroy(m);
        return TT_CALLBACK_PROCESSED;
    }
    if (st == TT_FAILED) {
        Tt_status status = tt_message_status(m);
        tttk_message_destroy(m);
        if (status != TT_DESKTOP_ECANCELED) {
            fallback_exit_without_tt_sm(wmGD.display);
        }
        return TT_CALLBACK_PROCESSED;
    }
    return TT_CALLBACK_CONTINUE;
}

static void resolve_ws_arg_name(Display *dpy, const char *arg, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!arg || !arg[0]) return;

    char *endp = nullptr;
    unsigned long raw_atom = strtoul(arg, &endp, 0);
    if (endp != arg && *endp == '\0' && raw_atom != 0 && dpy) {
        char *aname = XGetAtomName(dpy, (Atom)raw_atom);
        if (aname) {
            if (aname[0]) {
                snprintf(out, out_sz, "%s", aname);
                XFree(aname);
                return;
            }
            XFree(aname);
        }
    }
    snprintf(out, out_sz, "%s", arg);
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
            char ws_name[64];
            char req[128];
            resolve_ws_arg_name(dpy, pch, ws_name, sizeof(ws_name));
            snprintf(req, sizeof(req), "f.goto_workspace %s", ws_name);
            send_dt_wm_request(dpy, req);
            tt_free(pch);
        }
        handled = true;
    } else if (strcmp(op, "DtWorkspace_Title_Set") == 0) {
        char *pch_ws = tt_message_arg_val(m, 1);
        char *pch_title = tt_message_arg_val(m, 2);
        if (tt_ptr_error(pch_ws) == TT_OK && pch_ws &&
            tt_ptr_error(pch_title) == TT_OK && pch_title) {
            char ws_name[64];
            char req[256];
            resolve_ws_arg_name(dpy, pch_ws, ws_name, sizeof(ws_name));
            snprintf(req, sizeof(req), "f.set_workspace_title %s %s", ws_name, pch_title);
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
            char ws_name[64];
            char req[128];
            resolve_ws_arg_name(dpy, pch_ws, ws_name, sizeof(ws_name));
            snprintf(req, sizeof(req), "f.delete_workspace %s", ws_name);
            send_dt_wm_request(dpy, req);
            tt_free(pch_ws);
        }
        handled = true;
    } else if (strcmp(op, "DtPanel_Restore") == 0) {
        send_dt_wm_request(dpy, "f.restart");
        handled = true;
    } else if (strcmp(op, "GetWsmClients") == 0) {
        handled = true;
    } else if (strcmp(op, "XSession_Exit") == 0) {
        /* Fallback when dtsession's TT_HANDLE_PUSH handler is absent */
        tt_free(op);
        tt_message_reply(m);
        tt_message_destroy(m);
        fallback_exit_without_tt_sm(dpy);
        return TT_CALLBACK_PROCESSED;
    }

    tt_free(op);
    if (!handled) {
        return TT_CALLBACK_CONTINUE;
    }
    tt_message_reply(m);
    tt_message_destroy(m);
    return TT_CALLBACK_PROCESSED;
}

static Tt_callback_action wayland_wsm_tt_notice_cb(Tt_message m, Tt_pattern p) {
    (void)p;
    if (tt_message_state(m) != TT_SENT) {
        return TT_CALLBACK_CONTINUE;
    }
    char *op = tt_message_op(m);
    if (tt_ptr_error(op) != TT_OK || !op) {
        return TT_CALLBACK_CONTINUE;
    }

    bool handled = false;
    if (strcmp(op, "DtActivity_Began") == 0) {
        WmStopWaiting();
        handled = true;
    } else if (strcmp(op, "DtActivity_Beginning") == 0) {
        if (wmGD.dtSD) {
            WmFrontPanelSetBusy(True);
            if (wmGD.display) {
                XFlush(wmGD.display);
            }
        }
        handled = true;
    } else if (strcmp(op, "DtTypes_Reloaded") == 0) {
        WmFrontPanelSetBusy(True);
        DtDbLoad();
        UpdateFileTypeControlFields();
        WmFrontPanelSetBusy(False);
        if (wmGD.display) {
            XFlush(wmGD.display);
        }
        handled = true;
    }

    tt_free(op);
    if (!handled) {
        return TT_CALLBACK_CONTINUE;
    }
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
    if (_DtSvcInitToolTalk(topShell) != TT_OK) {
        return;
    }

    char *sess = tt_default_session();
    if (tt_ptr_error(sess) != TT_OK || !sess) {
        return;
    }
    Tt_pattern notice_pat = tt_pattern_create();
    if (tt_ptr_error(notice_pat) != TT_OK) {
        tt_free(sess);
        return;
    }
    Tt_pattern pat = tt_pattern_create();
    if (tt_ptr_error(pat) != TT_OK) {
        tt_pattern_destroy(notice_pat);
        tt_free(sess);
        return;
    }
    tt_pattern_category_set(notice_pat, TT_OBSERVE);
    tt_pattern_scope_add(notice_pat, TT_SESSION);
    tt_pattern_session_add(notice_pat, sess);
    tt_pattern_class_add(notice_pat, TT_NOTICE);
    tt_pattern_state_add(notice_pat, TT_SENT);
    tt_pattern_op_add(notice_pat, "DtActivity_Beginning");
    tt_pattern_op_add(notice_pat, "DtActivity_Began");
    tt_pattern_op_add(notice_pat, "DtTypes_Reloaded");
    tt_pattern_callback_add(notice_pat, wayland_wsm_tt_notice_cb);

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
    tt_pattern_op_add(pat, "XSession_Exit");
    tt_pattern_callback_add(pat, wayland_wsm_tt_request_cb);
    Tt_status notice_st = tt_pattern_register(notice_pat);
    Tt_status req_st = tt_pattern_register(pat);
    if (notice_st == TT_OK && req_st == TT_OK) {
        g_ttWsmRegistered = true;
        /* Also re-register WorkspaceModifyCB on ToolTalk if WmPanelistShow ran before ttsession */
        SwitchData *sw = find_panel_switch_data();
        if (sw && panel.shell) {
            DtWsmAddWorkspaceModifiedCallback(panel.shell, WorkspaceModifyCB, (XtPointer)sw);
        }
    } else {
        tt_pattern_destroy(notice_pat);
        tt_pattern_destroy(pat);
    }
}

void WmFpWaylandInvokeExitSession(Widget w) {
    Display *dpy = wmGD.display ? wmGD.display : (w ? XtDisplay(w) : nullptr);
    if (!dpy) return;
    if (!is_dtsession_alive(dpy, nullptr)) {
        send_dt_wm_request(dpy, "f.quit_mwm");
        return;
    }
    Widget shell = w ? w : panel.shell;
    if (!g_ttWsmRegistered && shell) {
        ensure_ttsession_running(dpy, nullptr);
        register_wayland_wsm_tooltalk_handlers(shell);
    }
    if (shell && _DtSvcInitToolTalk(shell) == TT_OK) {
        Tt_message req = tttk_message_create(
            nullptr, TT_REQUEST, TT_SESSION, nullptr, "XSession_Exit", exit_session_tt_reply_cb
        );
        if (tt_ptr_error(req) == TT_OK && req) {
            if (tt_message_send(req) == TT_OK) {
                return;
            }
            tttk_message_destroy(req);
        }
    }
    fallback_exit_without_tt_sm(dpy);
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
    if (changed_atom == g_atomPanelBusyStartSeq) {
        if (wmGD.dtSD) {
            WmFrontPanelSetBusy(True);
            XFlush(dpy);
        }
        return;
    }
    if (changed_atom == g_atomPanelBusyStopSeq) {
        WmStopWaiting();
        return;
    }

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

static void watch_trash_window_events(Display *dpy) {
    if (!dpy || g_atomTrashWin == None) return;
    Atom actual = None;
    int fmt = 0;
    unsigned long nitems = 0, after = 0;
    unsigned char *data = nullptr;
    Window twin = None;
    if (XGetWindowProperty(
            dpy, g_fpScreen.rootWindow, g_atomTrashWin, 0, 1, False, XA_WINDOW,
            &actual, &fmt, &nitems, &after, &data
        ) == Success && data) {
        if (fmt == 32 && nitems >= 1) {
            twin = *(Window *)data;
        }
        XFree(data);
    }
    if (twin != None && twin != g_watchedTrashWin) {
        g_watchedTrashWin = twin;
        XSelectInput(dpy, twin, StructureNotifyMask | PropertyChangeMask);
    } else if (twin == None) {
        g_watchedTrashWin = None;
    }
    WmFpUpdateTrashOpenIcon(dpy);
}

static void handle_subpanel_x11_event(Display *dpy, const XEvent *ev) {
    if (!dpy || !ev || !panel.shell) return;
    for (int i = 0; i < panel.box_data_count; ++i) {
        BoxData *bd = panel.box_data[i];
        if (!bd) continue;
        for (int j = 0; j < bd->control_data_count; ++j) {
            ControlData *cd = bd->control_data[j];
            if (!cd || !cd->subpanel_data || !cd->subpanel_data->shell ||
                !XtIsRealized(cd->subpanel_data->shell)) {
                continue;
            }
            Window swin = XtWindow(cd->subpanel_data->shell);
            if (ev->type == UnmapNotify && ev->xunmap.window == swin) {
                if (cd->subpanel_data->form && XtIsManaged(cd->subpanel_data->form)) {
                    XtUnmanageChild(cd->subpanel_data->form);
                }
                return;
            }
            if (ev->type == PropertyNotify && ev->xproperty.window == swin &&
                ev->xproperty.atom == g_atomSubpanelTorn &&
                ev->xproperty.state == PropertyNewValue) {
                cd->subpanel_data->torn = True;
                return;
            }
        }
    }
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

    ensure_ttsession_running(dpy, argv[0]);
    bootstrap_color_server_if_needed(dpy, profile);
    bootstrap_local_wsm_properties_if_needed(dpy);

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
    if (!XtIsRealized(panel.shell)) {
        XtRealizeWidget(panel.shell);
    }
    if (XtWindow(panel.shell) != None) {
        XClassHint fp_hint = {
            .res_name = (char *)"dtwm-front-panel",
            .res_class = (char *)"Dtwm",
        };
        XSetClassHint(dpy, XtWindow(panel.shell), &fp_hint);
    }
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
    g_atomSunTtSession = XInternAtom(dpy, "_SUN_TT_SESSION", False);
    g_atomTtSession = XInternAtom(dpy, "TT_SESSION", False);
    g_atomTrashWin = XInternAtom(dpy, "_DT_TRASH_WINDOW", False);
    g_atomSubpanelTorn = XInternAtom(dpy, "_DT_SUBPANEL_TORN_OFF", False);
    g_atomPanelBusyStartSeq = XInternAtom(dpy, "_DT_PANEL_BUSY_START_SEQ", False);
    g_atomPanelBusyStopSeq = XInternAtom(dpy, "_DT_PANEL_BUSY_STOP_SEQ", False);
    if (g_mwmWin != None) {
        XSelectInput(dpy, g_mwmWin, PropertyChangeMask);
    }
    XSelectInput(
        dpy, g_fpScreen.rootWindow,
        StructureNotifyMask | SubstructureNotifyMask | PropertyChangeMask
    );
    scan_existing_embedded_clients(dpy);
    watch_trash_window_events(dpy);

    while (!XtAppGetExitFlag(wmGD.mwmAppContext)) {
        XEvent ev;
        XtAppNextEvent(wmGD.mwmAppContext, &ev);
        if (ev.type == ConfigureNotify && ev.xconfigure.window == g_fpScreen.rootWindow) {
            Screen *scr = ScreenOfDisplay(dpy, g_fpScreen.screen);
            if (scr && ev.xconfigure.width > 0 && ev.xconfigure.height > 0) {
                scr->width = ev.xconfigure.width;
                scr->height = ev.xconfigure.height;
            }
        }
        if (ev.type == PropertyNotify &&
            ev.xproperty.window == g_fpScreen.rootWindow &&
            ev.xproperty.state == PropertyNewValue) {
            if (g_mwmWin == None) {
                g_mwmWin = find_mwm_window(dpy);
                if (g_mwmWin != None) {
                    XSelectInput(dpy, g_mwmWin, PropertyChangeMask);
                }
            }
            if (ev.xproperty.atom == g_atomTrashWin) {
                watch_trash_window_events(dpy);
            }
            Atom a_ws_cur_name = XInternAtom(dpy, "_DT_WORKSPACE_CURRENT_NAME", False);
            if (ev.xproperty.atom == a_ws_cur_name) {
                Atom act_t = None;
                int act_f = 0;
                unsigned long nitems = 0, rem = 0;
                unsigned char *wname = nullptr;
                if (XGetWindowProperty(
                        dpy, g_fpScreen.rootWindow, a_ws_cur_name, 0, 64, False,
                        AnyPropertyType, &act_t, &act_f, &nitems, &rem, &wname
                    ) == Success && wname && nitems > 0) {
                    char wbuf[64];
                    size_t wn = nitems < sizeof(wbuf) - 1 ? (size_t)nitems : sizeof(wbuf) - 1;
                    memcpy(wbuf, wname, wn);
                    wbuf[wn] = '\0';
                    SwitchData *sw = find_panel_switch_data();
                    Atom ws_atom = XInternAtom(dpy, wbuf, False);
                    if (g_localMwmWin != None && ws_atom != None && g_atomWsCurrent != None) {
                        XChangeProperty(
                            dpy, g_localMwmWin, g_atomWsCurrent, XA_ATOM, 32, PropModeReplace,
                            (const unsigned char *)&ws_atom, 1
                        );
                    }
                    if (sw && panel.shell && ws_atom != None) {
                        WorkspaceModifyCB(panel.shell, ws_atom, DtWSM_REASON_CURRENT, (XtPointer)sw);
                        emit_tt_workspace_modified(DefaultScreen(dpy), ws_atom, DtWSM_REASON_CURRENT);
                    }
                    XFree(wname);
                } else if (wname) {
                    XFree(wname);
                }
            }
            if (g_ttWsmRegistered &&
                (ev.xproperty.atom == g_atomSunTtSession || ev.xproperty.atom == g_atomTtSession)) {
                g_ttWsmRegistered = false;
            }
            if (!g_ttWsmRegistered) {
                register_wayland_wsm_tooltalk_handlers(topShell);
            }
        }
        if (ev.type == PropertyNotify && g_colorSrvWin != None &&
            ev.xproperty.window == g_colorSrvWin &&
            ev.xproperty.state == PropertyNewValue) {
            Atom pset_prop = XInternAtom(dpy, XmSPIXEL_SET_PROP, True);
            if (pset_prop != None && ev.xproperty.atom == pset_prop) {
                Atom act_t = None;
                int act_f = 0;
                unsigned long nitems = 0, rem = 0;
                unsigned char *pdata = nullptr;
                if (XGetWindowProperty(dpy, g_colorSrvWin, pset_prop, 0, 2048, False,
                                       AnyPropertyType, &act_t, &act_f, &nitems,
                                       &rem, &pdata) == Success && pdata && nitems > 2) {
                    size_t slen = (size_t)nitems;
                    if (pdata[slen - 1] == XmPIXEL_SET_PROP_VERSION)
                        --slen;
                    if (slen < sizeof(g_colorSrvPixelSet)) {
                        memcpy(g_colorSrvPixelSet, pdata, slen);
                        g_colorSrvPixelSet[slen] = '\0';
                    }
                    XFree(pdata);
                } else if (pdata) {
                    XFree(pdata);
                }
            }
        }
        if (ev.type == PropertyNotify && g_watchedTrashWin != None &&
            ev.xproperty.window == g_watchedTrashWin) {
            WmFpUpdateTrashOpenIcon(dpy);
        }
        handle_subpanel_x11_event(dpy, &ev);
        if (ev.type == ConfigureNotify) {
            enforce_embedded_client_geometry(dpy, &ev.xconfigure);
        } else if (ev.type == DestroyNotify) {
            clear_destroyed_embedded_client(ev.xdestroywindow.window);
        }
        if (ev.type == PropertyNotify && ev.xproperty.window == g_mwmWin &&
            ev.xproperty.state == PropertyNewValue) {
            sync_panel_workspaces_from_x11(dpy, ev.xproperty.atom);
        } else if (ev.type == MapNotify && ev.xmap.window != g_fpScreen.rootWindow) {
            register_all_subpanel_contexts();
            bool embedded = try_embed_x11_window(dpy, ev.xmap.window);
            if (!embedded && g_mwmWin == None &&
                ev.xmap.event == g_fpScreen.rootWindow && !ev.xmap.override_redirect &&
                ev.xmap.window != g_colorSrvWin &&
                XtWindowToWidget(dpy, ev.xmap.window) == nullptr) {
                WmStopWaiting();
            }
            WmFpUpdateTrashOpenIcon(dpy);
        } else if (ev.type == UnmapNotify && ev.xunmap.event == g_fpScreen.rootWindow) {
            WmFpUpdateTrashOpenIcon(dpy);
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
