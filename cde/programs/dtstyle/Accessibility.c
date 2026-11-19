/*
 * CDE - Common Desktop Environment
 * Accessibility.c: Style Manager Accessibility dialog, ~/.dt/accessibility.conf
 * persistence, X11 XKB AccessX / Wayland compositor synchronization, Screen
 * Magnifier configuration, Visual Bell, Reduced Motion, and Motif AT-SPI2
 * screen-reader bridge opt-in (WP-20).
 */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <Xm/Form.h>
#include <Xm/Frame.h>
#include <Xm/Label.h>
#include <Xm/MwmUtil.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
#include <Xm/Scale.h>
#include <Xm/SeparatoG.h>
#include <Xm/ToggleB.h>

#include <Dt/DialogBox.h>
#include <Dt/HourGlass.h>
#include <Dt/Icon.h>
#include <Dt/SessionM.h>
#include <Dt/UserMsg.h>

#include "Accessibility.h"
#include "Help.h"
#include "Main.h"
#include "Protocol.h"
#include "SaveRestore.h"

#define A11Y_BTN_OK     1
#define A11Y_BTN_APPLY  2
#define A11Y_BTN_CANCEL 3
#define A11Y_BTN_HELP   4
#define A11Y_NUM_BTNS   4

typedef struct {
    Boolean sticky_keys;
    Boolean sticky_two_keys_off;
    Boolean slow_keys;
    int slow_keys_ms;
    Boolean bounce_keys;
    int bounce_keys_ms;
    int vbell_mode; /* 0=off, 1=window, 2=screen */
    Boolean reduced_motion;
    Boolean magnifier;
    int zoom_pct;   /* 125..400 in steps of 25 */
    Boolean atspi_bridge;
} A11yConfig;

static struct {
    Widget pictLabel;
    Widget defaultBtn;
    Widget stickyToggle;
    Widget stickyTwoKeysToggle;
    Widget slowToggle;
    Widget slowScale;
    Widget bounceToggle;
    Widget bounceScale;
    Widget vbellOption;
    Widget vbellPulldown;
    Widget vbellOffBtn;
    Widget vbellWinBtn;
    Widget vbellScreenBtn;
    Widget reducedMotionToggle;
    Widget magToggle;
    Widget magScale;
    Widget atspiToggle;
    A11yConfig current;
    A11yConfig orig;
} g_a11yUI;

static saveRestore g_a11ySave = { FALSE, 0, };

static int
ClampInt(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int
SnapZoom25(int z)
{
    z = ((z + 12) / 25) * 25;
    return ClampInt(z, 125, 400);
}

static const char *
VbellToStr(int mode)
{
    if (mode == 1) return "window";
    if (mode == 2) return "screen";
    return "off";
}

static int
ParseVbell(const char *s)
{
    if (!s) return 0;
    if (strcasecmp(s, "window") == 0 || strcasecmp(s, "active") == 0 || strcmp(s, "1") == 0)
        return 1;
    if (strcasecmp(s, "screen") == 0 || strcasecmp(s, "fullscreen") == 0 || strcmp(s, "2") == 0)
        return 2;
    return 0;
}

static Boolean
ParseBool(const char *s, Boolean fallback)
{
    if (!s || !*s) return fallback;
    if (strcmp(s, "1") == 0 || strcasecmp(s, "true") == 0 ||
        strcasecmp(s, "yes") == 0 || strcasecmp(s, "on") == 0)
        return True;
    if (strcmp(s, "0") == 0 || strcasecmp(s, "false") == 0 ||
        strcasecmp(s, "no") == 0 || strcasecmp(s, "off") == 0)
        return False;
    return fallback;
}

static void
InitDefaultA11yConfig(A11yConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->sticky_keys = False;
    cfg->sticky_two_keys_off = True;
    cfg->slow_keys = False;
    cfg->slow_keys_ms = 300;
    cfg->bounce_keys = False;
    cfg->bounce_keys_ms = 300;
    cfg->vbell_mode = 0;
    cfg->reduced_motion = False;
    cfg->magnifier = False;
    cfg->zoom_pct = 200;
    cfg->atspi_bridge = False;
}

static void
LoadA11yConfigFile(A11yConfig *cfg)
{
    const char *home = (style.home && style.home[0]) ? style.home : getenv("HOME");
    char path[1024];
    char line[512];
    FILE *fp;

    InitDefaultA11yConfig(cfg);
    if (!home || !home[0])
        return;

    snprintf(path, sizeof(path), "%s/.dt/accessibility.conf", home);
    fp = fopen(path, "r");
    if (!fp)
        return;

    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        char *nl, *eq, *k, *v;
        size_t klen;

        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '#' || *p == '!') continue;
        nl = strpbrk(p, "\r\n");
        if (nl) *nl = '\0';
        eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        k = p;
        v = eq + 1;
        while (*v && isspace((unsigned char)*v)) v++;
        klen = strlen(k);
        while (klen > 0 && isspace((unsigned char)k[klen - 1]))
            k[--klen] = '\0';

        if (strcasecmp(k, "sticky_keys") == 0 || strcasecmp(k, "sticky") == 0)
            cfg->sticky_keys = ParseBool(v, cfg->sticky_keys);
        else if (strcasecmp(k, "sticky_two_keys_off") == 0 || strcasecmp(k, "two_keys_off") == 0)
            cfg->sticky_two_keys_off = ParseBool(v, cfg->sticky_two_keys_off);
        else if (strcasecmp(k, "slow_keys") == 0 || strcasecmp(k, "slow") == 0)
            cfg->slow_keys = ParseBool(v, cfg->slow_keys);
        else if (strcasecmp(k, "slow_keys_ms") == 0 || strcasecmp(k, "slow_ms") == 0)
            cfg->slow_keys_ms = ClampInt(atoi(v), 100, 1000);
        else if (strcasecmp(k, "bounce_keys") == 0 || strcasecmp(k, "bounce") == 0)
            cfg->bounce_keys = ParseBool(v, cfg->bounce_keys);
        else if (strcasecmp(k, "bounce_keys_ms") == 0 || strcasecmp(k, "bounce_ms") == 0)
            cfg->bounce_keys_ms = ClampInt(atoi(v), 100, 1000);
        else if (strcasecmp(k, "visual_bell") == 0 || strcasecmp(k, "vbell") == 0)
            cfg->vbell_mode = ParseVbell(v);
        else if (strcasecmp(k, "reduced_motion") == 0)
            cfg->reduced_motion = ParseBool(v, cfg->reduced_motion);
        else if (strcasecmp(k, "magnifier") == 0)
            cfg->magnifier = ParseBool(v, cfg->magnifier);
        else if (strcasecmp(k, "zoom_pct") == 0)
            cfg->zoom_pct = SnapZoom25(atoi(v));
        else if (strcasecmp(k, "atspi_bridge") == 0 || strcasecmp(k, "xm_accessibility") == 0)
            cfg->atspi_bridge = ParseBool(v, cfg->atspi_bridge);
    }
    fclose(fp);
}

static Boolean
SaveA11yConfigFile(const A11yConfig *cfg)
{
    const char *home = (style.home && style.home[0]) ? style.home : getenv("HOME");
    char dir[1024];
    char tmp_path[1060];
    char final_path[1060];
    FILE *fp;

    if (!home || !home[0] || !cfg)
        return False;

    snprintf(dir, sizeof(dir), "%s/.dt", home);
    (void)mkdir(dir, 0755);
    snprintf(final_path, sizeof(final_path), "%s/accessibility.conf", dir);
    snprintf(tmp_path, sizeof(tmp_path), "%s/accessibility.conf.tmp.%d", dir, (int)getpid());

    fp = fopen(tmp_path, "w");
    if (!fp)
        return False;

    fprintf(fp, "# CDE Style Manager Accessibility Preferences (WP-20)\n");
    fprintf(fp, "sticky_keys=%d\n", cfg->sticky_keys ? 1 : 0);
    fprintf(fp, "sticky_two_keys_off=%d\n", cfg->sticky_two_keys_off ? 1 : 0);
    fprintf(fp, "slow_keys=%d\n", cfg->slow_keys ? 1 : 0);
    fprintf(fp, "slow_keys_ms=%d\n", ClampInt(cfg->slow_keys_ms, 100, 1000));
    fprintf(fp, "bounce_keys=%d\n", cfg->bounce_keys ? 1 : 0);
    fprintf(fp, "bounce_keys_ms=%d\n", ClampInt(cfg->bounce_keys_ms, 100, 1000));
    fprintf(fp, "visual_bell=%s\n", VbellToStr(cfg->vbell_mode));
    fprintf(fp, "reduced_motion=%d\n", cfg->reduced_motion ? 1 : 0);
    fprintf(fp, "magnifier=%d\n", cfg->magnifier ? 1 : 0);
    fprintf(fp, "zoom_pct=%d\n", SnapZoom25(cfg->zoom_pct));
    fprintf(fp, "atspi_bridge=%d\n", cfg->atspi_bridge ? 1 : 0);
    if (fclose(fp) != 0) {
        unlink(tmp_path);
        return False;
    }
    if (rename(tmp_path, final_path) != 0) {
        unlink(tmp_path);
        return False;
    }
    return True;
}

static void
SendWmRequestAnySession(const char *cmd)
{
    Window root;
    Atom mwm_info, req_atom, act_type = None;
    int act_fmt = 0;
    unsigned long nitems = 0, rem = 0;
    unsigned char *prop = nullptr;
    Window wm_win;

    if (!style.display || !cmd || !cmd[0])
        return;

    if (DtstyleWaylandActive()) {
        DtstyleSendWaylandRequest(cmd);
        return;
    }

    root = XRootWindow(style.display, 0);
    mwm_info = XInternAtom(style.display, "_MOTIF_WM_INFO", False);
    if (XGetWindowProperty(style.display, root, mwm_info, 0, 2, False,
                           AnyPropertyType, &act_type, &act_fmt,
                           &nitems, &rem, &prop) != Success ||
        !prop || nitems < 2) {
        if (prop) XFree(prop);
        return;
    }
    wm_win = (Window)((unsigned long *)prop)[1];
    XFree(prop);
    if (wm_win == None)
        return;

    req_atom = XInternAtom(style.display, "_DT_WM_REQUEST", False);
    XChangeProperty(style.display, wm_win, req_atom, XA_STRING, 8,
                    PropModeAppend, (const unsigned char *)cmd,
                    (int)strlen(cmd) + 1);
    XSync(style.display, False);
}

static void
ApplyX11XkbDirect(const A11yConfig *cfg)
{
    int op = 0, ev = 0, err = 0, maj = XkbMajorVersion, min = XkbMinorVersion;
    XkbDescPtr xkb;

    if (!style.display || !cfg || DtstyleWaylandActive())
        return;
    if (!XkbQueryExtension(style.display, &op, &ev, &err, &maj, &min))
        return;
    xkb = XkbGetMap(style.display, 0, XkbUseCoreKbd);
    if (!xkb)
        return;
    if (XkbGetControls(style.display, XkbAllControlsMask, xkb) == Success && xkb->ctrls) {
        unsigned long mask =
            XkbStickyKeysMask | XkbSlowKeysMask | XkbBounceKeysMask |
            XkbControlsEnabledMask | XkbAccessXFeedbackMask;
        if (cfg->sticky_keys) xkb->ctrls->enabled_ctrls |= XkbStickyKeysMask;
        else xkb->ctrls->enabled_ctrls &= ~XkbStickyKeysMask;
        if (cfg->slow_keys) xkb->ctrls->enabled_ctrls |= XkbSlowKeysMask;
        else xkb->ctrls->enabled_ctrls &= ~XkbSlowKeysMask;
        if (cfg->bounce_keys) xkb->ctrls->enabled_ctrls |= XkbBounceKeysMask;
        else xkb->ctrls->enabled_ctrls &= ~XkbBounceKeysMask;

        xkb->ctrls->slow_keys_delay = (unsigned short)ClampInt(cfg->slow_keys_ms, 100, 1000);
        xkb->ctrls->debounce_delay = (unsigned short)ClampInt(cfg->bounce_keys_ms, 100, 1000);
        if (cfg->sticky_two_keys_off)
            xkb->ctrls->ax_options |= XkbAX_TwoKeysMask;
        else
            xkb->ctrls->ax_options &= ~XkbAX_TwoKeysMask;
        xkb->ctrls->ax_options |= XkbAX_LatchToLockMask;
        xkb->ctrls->ax_options &= ~XkbAX_FBOptionsMask;

        XkbSetControls(style.display, mask, xkb);
        if (!cfg->sticky_keys) {
            XkbLatchModifiers(style.display, XkbUseCoreKbd, 0xff, 0);
            XkbLockModifiers(style.display, XkbUseCoreKbd,
                             ShiftMask | ControlMask | Mod1Mask | Mod4Mask, 0);
        }
        XSync(style.display, False);
    }
    XkbFreeKeyboard(xkb, XkbAllComponentsMask, True);
}

static void
SyncAtspiEnvironment(Boolean enabled)
{
    char xrdbBuf[128];
    pid_t pid;

    snprintf(xrdbBuf, sizeof(xrdbBuf), "*enableAccessibility: %s\n",
             enabled ? "True" : "False");
    if (style.display)
        _DtAddToResource(style.display, xrdbBuf);

    pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            const char *shCmd = enabled
                ? "XM_ACCESSIBILITY=1; export XM_ACCESSIBILITY; "
                  "command -v dbus-update-activation-environment >/dev/null 2>&1 && "
                  "(dbus-update-activation-environment --systemd XM_ACCESSIBILITY=1 >/dev/null 2>&1 || "
                  "dbus-update-activation-environment XM_ACCESSIBILITY=1 >/dev/null 2>&1 || true); "
                  "command -v gdbus >/dev/null 2>&1 && "
                  "gdbus call --session --dest org.a11y.Bus --object-path /org/a11y/bus "
                  "--method org.freedesktop.DBus.Properties.Set org.a11y.Status IsEnabled "
                  "\"<true>\" >/dev/null 2>&1 || true"
                : "XM_ACCESSIBILITY=0; export XM_ACCESSIBILITY; "
                  "command -v dbus-update-activation-environment >/dev/null 2>&1 && "
                  "(dbus-update-activation-environment --systemd XM_ACCESSIBILITY=0 >/dev/null 2>&1 || "
                  "dbus-update-activation-environment XM_ACCESSIBILITY=0 >/dev/null 2>&1 || true); "
                  "command -v gdbus >/dev/null 2>&1 && "
                  "gdbus call --session --dest org.a11y.Bus --object-path /org/a11y/bus "
                  "--method org.freedesktop.DBus.Properties.Set org.a11y.Status IsEnabled "
                  "\"<false>\" >/dev/null 2>&1 || true";
            setsid();
            execl("/bin/sh", "sh", "-c", shCmd, (char *)nullptr);
            _exit(127);
        }
        _exit(0);
    } else if (pid > 0) {
        int st = 0;
        (void)waitpid(pid, &st, 0);
    }
}

static void
ApplyLiveA11yConfig(const A11yConfig *cfg, Boolean persistFile)
{
    char cmd[512];
    if (!cfg) return;

    if (persistFile)
        (void)SaveA11yConfigFile(cfg);

    ApplyX11XkbDirect(cfg);

    snprintf(
        cmd, sizeof(cmd),
        "f.set_accessibility sticky=%d two_keys_off=%d slow=%d slow_ms=%d "
        "bounce=%d bounce_ms=%d vbell=%s reduced_motion=%d magnifier=%d zoom_pct=%d",
        cfg->sticky_keys ? 1 : 0,
        cfg->sticky_two_keys_off ? 1 : 0,
        cfg->slow_keys ? 1 : 0,
        ClampInt(cfg->slow_keys_ms, 100, 1000),
        cfg->bounce_keys ? 1 : 0,
        ClampInt(cfg->bounce_keys_ms, 100, 1000),
        VbellToStr(cfg->vbell_mode),
        cfg->reduced_motion ? 1 : 0,
        cfg->magnifier ? 1 : 0,
        SnapZoom25(cfg->zoom_pct)
    );
    SendWmRequestAnySession(cmd);
    SyncAtspiEnvironment(cfg->atspi_bridge);
}

int
ApplyAccessibilitySpecCLI(const char *spec)
{
    A11yConfig cfg;
    LoadA11yConfigFile(&cfg);

    if (spec && spec[0] && strcasecmp(spec, "reload") != 0) {
        char copy[512];
        char *saveptr = nullptr;
        char *tok;
        snprintf(copy, sizeof(copy), "%s", spec);
        for (tok = strtok_r(copy, " ,\t\r\n", &saveptr); tok;
             tok = strtok_r(nullptr, " ,\t\r\n", &saveptr)) {
            char *eq = strchr(tok, '=');
            if (!eq) continue;
            *eq = '\0';
            const char *k = tok;
            const char *v = eq + 1;
            if (strcasecmp(k, "sticky") == 0 || strcasecmp(k, "sticky_keys") == 0)
                cfg.sticky_keys = ParseBool(v, cfg.sticky_keys);
            else if (strcasecmp(k, "two_keys_off") == 0 || strcasecmp(k, "sticky_two_keys_off") == 0)
                cfg.sticky_two_keys_off = ParseBool(v, cfg.sticky_two_keys_off);
            else if (strcasecmp(k, "slow") == 0 || strcasecmp(k, "slow_keys") == 0)
                cfg.slow_keys = ParseBool(v, cfg.slow_keys);
            else if (strcasecmp(k, "slow_ms") == 0 || strcasecmp(k, "slow_keys_ms") == 0)
                cfg.slow_keys_ms = ClampInt(atoi(v), 100, 1000);
            else if (strcasecmp(k, "bounce") == 0 || strcasecmp(k, "bounce_keys") == 0)
                cfg.bounce_keys = ParseBool(v, cfg.bounce_keys);
            else if (strcasecmp(k, "bounce_ms") == 0 || strcasecmp(k, "bounce_keys_ms") == 0)
                cfg.bounce_keys_ms = ClampInt(atoi(v), 100, 1000);
            else if (strcasecmp(k, "vbell") == 0 || strcasecmp(k, "visual_bell") == 0)
                cfg.vbell_mode = ParseVbell(v);
            else if (strcasecmp(k, "reduced_motion") == 0)
                cfg.reduced_motion = ParseBool(v, cfg.reduced_motion);
            else if (strcasecmp(k, "magnifier") == 0)
                cfg.magnifier = ParseBool(v, cfg.magnifier);
            else if (strcasecmp(k, "zoom_pct") == 0)
                cfg.zoom_pct = SnapZoom25(atoi(v));
            else if (strcasecmp(k, "atspi_bridge") == 0 || strcasecmp(k, "xm_accessibility") == 0)
                cfg.atspi_bridge = ParseBool(v, cfg.atspi_bridge);
        }
        ApplyLiveA11yConfig(&cfg, True);
    } else {
        ApplyLiveA11yConfig(&cfg, False);
    }
    return 0;
}

/* --- Motif UI Synchronization & Callbacks --- */

static void
UpdateSensitivity(void)
{
    if (g_a11yUI.stickyTwoKeysToggle)
        XtSetSensitive(g_a11yUI.stickyTwoKeysToggle,
                       XmToggleButtonGetState(g_a11yUI.stickyToggle));
    if (g_a11yUI.slowScale)
        XtSetSensitive(g_a11yUI.slowScale,
                       XmToggleButtonGetState(g_a11yUI.slowToggle));
    if (g_a11yUI.bounceScale)
        XtSetSensitive(g_a11yUI.bounceScale,
                       XmToggleButtonGetState(g_a11yUI.bounceToggle));
    if (g_a11yUI.magScale)
        XtSetSensitive(g_a11yUI.magScale,
                       XmToggleButtonGetState(g_a11yUI.magToggle));
}

static void
PopulateWidgetsFromConfig(const A11yConfig *cfg)
{
    Widget hist = g_a11yUI.vbellOffBtn;
    if (!cfg || !style.accessibilityDialog)
        return;

    XmToggleButtonSetState(g_a11yUI.stickyToggle, cfg->sticky_keys, False);
    XmToggleButtonSetState(g_a11yUI.stickyTwoKeysToggle, cfg->sticky_two_keys_off, False);
    XmToggleButtonSetState(g_a11yUI.slowToggle, cfg->slow_keys, False);
    XmScaleSetValue(g_a11yUI.slowScale, ClampInt(cfg->slow_keys_ms, 100, 1000));
    XmToggleButtonSetState(g_a11yUI.bounceToggle, cfg->bounce_keys, False);
    XmScaleSetValue(g_a11yUI.bounceScale, ClampInt(cfg->bounce_keys_ms, 100, 1000));

    if (cfg->vbell_mode == 1) hist = g_a11yUI.vbellWinBtn;
    else if (cfg->vbell_mode == 2) hist = g_a11yUI.vbellScreenBtn;
    if (g_a11yUI.vbellOption && hist)
        XtVaSetValues(g_a11yUI.vbellOption, XmNmenuHistory, hist, NULL);

    XmToggleButtonSetState(g_a11yUI.reducedMotionToggle, cfg->reduced_motion, False);
    XmToggleButtonSetState(g_a11yUI.magToggle, cfg->magnifier, False);
    XmScaleSetValue(g_a11yUI.magScale, SnapZoom25(cfg->zoom_pct));
    XmToggleButtonSetState(g_a11yUI.atspiToggle, cfg->atspi_bridge, False);
    UpdateSensitivity();
}

static void
ReadConfigFromWidgets(A11yConfig *cfg)
{
    int val = 0;
    Widget hist = nullptr;
    if (!cfg) return;

    cfg->sticky_keys = XmToggleButtonGetState(g_a11yUI.stickyToggle);
    cfg->sticky_two_keys_off = XmToggleButtonGetState(g_a11yUI.stickyTwoKeysToggle);
    cfg->slow_keys = XmToggleButtonGetState(g_a11yUI.slowToggle);
    XmScaleGetValue(g_a11yUI.slowScale, &val);
    cfg->slow_keys_ms = ClampInt(val, 100, 1000);

    cfg->bounce_keys = XmToggleButtonGetState(g_a11yUI.bounceToggle);
    XmScaleGetValue(g_a11yUI.bounceScale, &val);
    cfg->bounce_keys_ms = ClampInt(val, 100, 1000);

    XtVaGetValues(g_a11yUI.vbellOption, XmNmenuHistory, &hist, NULL);
    if (hist == g_a11yUI.vbellWinBtn) cfg->vbell_mode = 1;
    else if (hist == g_a11yUI.vbellScreenBtn) cfg->vbell_mode = 2;
    else cfg->vbell_mode = 0;

    cfg->reduced_motion = XmToggleButtonGetState(g_a11yUI.reducedMotionToggle);
    cfg->magnifier = XmToggleButtonGetState(g_a11yUI.magToggle);
    XmScaleGetValue(g_a11yUI.magScale, &val);
    cfg->zoom_pct = SnapZoom25(val);
    cfg->atspi_bridge = XmToggleButtonGetState(g_a11yUI.atspiToggle);
}

static void
ToggleSensCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    UpdateSensitivity();
}

static void
MagScaleValueCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    XmScaleCallbackStruct *cbs = (XmScaleCallbackStruct *)call_data;
    (void)client_data;
    if (!cbs) return;
    int snapped = SnapZoom25(cbs->value);
    if (snapped != cbs->value)
        XmScaleSetValue(w, snapped);
}

static void
DefaultBtnCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    A11yConfig defCfg;
    (void)w;
    (void)client_data;
    (void)call_data;
    InitDefaultA11yConfig(&defCfg);
    g_a11yUI.current = defCfg;
    PopulateWidgetsFromConfig(&g_a11yUI.current);
}

static void
MapA11yDlgCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    Widget shell = (Widget)client_data;
    (void)w;
    (void)call_data;
    if (!g_a11ySave.restoreFlag && shell && style.accessibilityDialog)
        putDialog(shell, style.accessibilityDialog);
    XtRemoveCallback(style.accessibilityDialog, XmNmapCallback, MapA11yDlgCB, shell);
}

static void
DialogButtonCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    DtDialogBoxCallbackStruct *cb = (DtDialogBoxCallbackStruct *)call_data;
    (void)w;
    (void)client_data;
    if (!cb) return;

    switch (cb->button_position) {
    case A11Y_BTN_OK:
        ReadConfigFromWidgets(&g_a11yUI.current);
        g_a11yUI.orig = g_a11yUI.current;
        ApplyLiveA11yConfig(&g_a11yUI.current, True);
        XtUnmanageChild(style.accessibilityDialog);
        break;
    case A11Y_BTN_APPLY:
        ReadConfigFromWidgets(&g_a11yUI.current);
        g_a11yUI.orig = g_a11yUI.current;
        ApplyLiveA11yConfig(&g_a11yUI.current, True);
        break;
    case A11Y_BTN_CANCEL:
        g_a11yUI.current = g_a11yUI.orig;
        PopulateWidgetsFromConfig(&g_a11yUI.orig);
        XtUnmanageChild(style.accessibilityDialog);
        break;
    case A11Y_BTN_HELP:
        XtCallCallbacks(style.accessibilityDialog, XmNhelpCallback, nullptr);
        break;
    default:
        break;
    }
}

static Widget
CreateSectionFrame(Widget parent, const char *name, const char *titleText, Widget *outRC)
{
    Widget frame = XtVaCreateManagedWidget(
        name, xmFrameWidgetClass, parent,
        XmNshadowType, XmSHADOW_ETCHED_IN,
        XmNmarginWidth, 8,
        XmNmarginHeight, 6,
        NULL);
    XmString xms = CMPSTR((char *)titleText);
    XtVaCreateManagedWidget(
        "sectionTitle", xmLabelWidgetClass, frame,
        XmNchildType, XmFRAME_TITLE_CHILD,
        XmNlabelString, xms,
        NULL);
    XmStringFree(xms);
    *outRC = XtVaCreateManagedWidget(
        "sectionRC", xmRowColumnWidgetClass, frame,
        XmNorientation, XmVERTICAL,
        XmNpacking, XmPACK_TIGHT,
        XmNspacing, 4,
        NULL);
    return frame;
}

static Widget
BuildAccessibilityDialog(Widget shell)
{
    Arg args[MAX_ARGS];
    int n;
    XmString btnLabels[A11Y_NUM_BTNS];
    XmString xms;
    Widget form, mainCol, kbdRC, visRC, magRC, srRC, vbellRow;

    btnLabels[0] = CMPSTR((String)_DtOkString);
    btnLabels[1] = CMPSTR("Apply");
    btnLabels[2] = CMPSTR((String)_DtCancelString);
    btnLabels[3] = CMPSTR((String)_DtHelpString);

    XtSetArg(g_a11ySave.posArgs[g_a11ySave.poscnt], XmNbuttonCount, A11Y_NUM_BTNS);
    g_a11ySave.poscnt++;
    XtSetArg(g_a11ySave.posArgs[g_a11ySave.poscnt], XmNbuttonLabelStrings, btnLabels);
    g_a11ySave.poscnt++;
    XtSetArg(g_a11ySave.posArgs[g_a11ySave.poscnt], XmNdefaultPosition, False);
    g_a11ySave.poscnt++;

    style.accessibilityDialog = __DtCreateDialogBoxDialog(
        shell, "AccessibilityDialog", g_a11ySave.posArgs, g_a11ySave.poscnt);
    XtAddCallback(style.accessibilityDialog, XmNcallback, DialogButtonCB, nullptr);
    XtAddCallback(style.accessibilityDialog, XmNmapCallback, MapA11yDlgCB, shell);
    XtAddCallback(style.accessibilityDialog, XmNhelpCallback,
                  HelpRequestCB, (XtPointer)HELP_KEYBOARD_DIALOG);

    for (int i = 0; i < A11Y_NUM_BTNS; ++i)
        XmStringFree(btnLabels[i]);

    n = 0;
    XtSetArg(args[n], XmNautoUnmanage, False); n++;
    XtSetArg(args[n], XmNcancelButton, _DtDialogBoxGetButton(style.accessibilityDialog, A11Y_BTN_CANCEL)); n++;
    XtSetValues(style.accessibilityDialog, args, n);

    n = 0;
    XtSetArg(args[n], XmNtitle, "Style Manager - Accessibility"); n++;
    XtSetArg(args[n], XmNuseAsyncGeometry, True); n++;
    XtSetArg(args[n], XmNmwmFunctions, DIALOG_MWM_FUNC); n++;
    XtSetValues(XtParent(style.accessibilityDialog), args, n);

    n = 0;
    XtSetArg(args[n], XmNhorizontalSpacing, style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNverticalSpacing, style.verticalSpacing); n++;
    XtSetArg(args[n], XmNallowOverlap, False); n++;
    XtSetArg(args[n], XmNchildType, XmWORK_AREA); n++;
    form = XmCreateForm(style.accessibilityDialog, "accessibilityForm", args, n);

    n = 0;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNfillMode, XmFILL_SELF); n++;
    XtSetArg(args[n], XmNbehavior, XmICON_LABEL); n++;
    XtSetArg(args[n], XmNpixmapForeground, style.secBSCol); n++;
    XtSetArg(args[n], XmNpixmapBackground, style.secTSCol); n++;
    XtSetArg(args[n], XmNstring, NULL); n++;
    XtSetArg(args[n], XmNshadowThickness, 0); n++;
    XtSetArg(args[n], XmNimageName, KEYBOARD_ICON); n++;
    XtSetArg(args[n], XmNtraversalOn, False); n++;
    g_a11yUI.pictLabel = _DtCreateIcon(form, "accessibilityPict", args, n);
    XtManageChild(g_a11yUI.pictLabel);

    xms = CMPSTR("Default");
    g_a11yUI.defaultBtn = XtVaCreateManagedWidget(
        "systemDefault", xmPushButtonWidgetClass, form,
        XmNtopAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNmarginHeight, LB_MARGIN_HEIGHT,
        XmNmarginWidth, LB_MARGIN_WIDTH,
        XmNlabelString, xms,
        NULL);
    XmStringFree(xms);
    XtAddCallback(g_a11yUI.defaultBtn, XmNactivateCallback, DefaultBtnCB, nullptr);

    mainCol = XtVaCreateManagedWidget(
        "accessibilityMainCol", xmRowColumnWidgetClass, form,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, g_a11yUI.defaultBtn,
        XmNtopOffset, 6,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNorientation, XmVERTICAL,
        XmNpacking, XmPACK_TIGHT,
        XmNspacing, 8,
        NULL);

    /* 1. Keyboard Accessibility (AccessX) */
    CreateSectionFrame(mainCol, "accessxFrame", "Keyboard Accessibility (AccessX)", &kbdRC);

    xms = CMPSTR("Sticky Keys (latch/lock Shift, Ctrl, Alt, Super)");
    g_a11yUI.stickyToggle = XtVaCreateManagedWidget(
        "stickyKeysToggle", xmToggleButtonWidgetClass, kbdRC,
        XmNlabelString, xms,
        XmNalignment, XmALIGNMENT_BEGINNING,
        NULL);
    XmStringFree(xms);
    XtAddCallback(g_a11yUI.stickyToggle, XmNvalueChangedCallback, ToggleSensCB, nullptr);

    xms = CMPSTR("Two keys pressed together turn Sticky Keys off");
    g_a11yUI.stickyTwoKeysToggle = XtVaCreateManagedWidget(
        "stickyTwoKeysToggle", xmToggleButtonWidgetClass, kbdRC,
        XmNlabelString, xms,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNmarginLeft, 18,
        NULL);
    XmStringFree(xms);

    xms = CMPSTR("Slow Keys (require keys to be held before acceptance)");
    g_a11yUI.slowToggle = XtVaCreateManagedWidget(
        "slowKeysToggle", xmToggleButtonWidgetClass, kbdRC,
        XmNlabelString, xms,
        XmNalignment, XmALIGNMENT_BEGINNING,
        NULL);
    XmStringFree(xms);
    XtAddCallback(g_a11yUI.slowToggle, XmNvalueChangedCallback, ToggleSensCB, nullptr);

    xms = CMPSTR("Slow Keys Acceptance Delay (ms)");
    g_a11yUI.slowScale = XtVaCreateManagedWidget(
        "slowKeysScale", xmScaleWidgetClass, kbdRC,
        XmNtitleString, xms,
        XmNorientation, XmHORIZONTAL,
        XmNminimum, 100,
        XmNmaximum, 1000,
        XmNvalue, 300,
        XmNshowValue, True,
        XmNscaleMultiple, 50,
        NULL);
    XmStringFree(xms);

    xms = CMPSTR("Bounce Keys (ignore rapid duplicate keystrokes)");
    g_a11yUI.bounceToggle = XtVaCreateManagedWidget(
        "bounceKeysToggle", xmToggleButtonWidgetClass, kbdRC,
        XmNlabelString, xms,
        XmNalignment, XmALIGNMENT_BEGINNING,
        NULL);
    XmStringFree(xms);
    XtAddCallback(g_a11yUI.bounceToggle, XmNvalueChangedCallback, ToggleSensCB, nullptr);

    xms = CMPSTR("Bounce Keys Debounce Interval (ms)");
    g_a11yUI.bounceScale = XtVaCreateManagedWidget(
        "bounceKeysScale", xmScaleWidgetClass, kbdRC,
        XmNtitleString, xms,
        XmNorientation, XmHORIZONTAL,
        XmNminimum, 100,
        XmNmaximum, 1000,
        XmNvalue, 300,
        XmNshowValue, True,
        XmNscaleMultiple, 50,
        NULL);
    XmStringFree(xms);

    /* 2. Visual Alerts & Motion */
    CreateSectionFrame(mainCol, "visualMotionFrame", "Visual Alerts & Motion", &visRC);

    vbellRow = XtVaCreateManagedWidget(
        "vbellRow", xmRowColumnWidgetClass, visRC,
        XmNorientation, XmHORIZONTAL,
        XmNspacing, 6,
        NULL);
    g_a11yUI.vbellPulldown = XmCreatePulldownMenu(vbellRow, "vbellPulldown", nullptr, 0);

    xms = CMPSTR("Off");
    g_a11yUI.vbellOffBtn = XtVaCreateManagedWidget(
        "vbellOff", xmPushButtonWidgetClass, g_a11yUI.vbellPulldown,
        XmNlabelString, xms, NULL);
    XmStringFree(xms);

    xms = CMPSTR("Flash Active Window");
    g_a11yUI.vbellWinBtn = XtVaCreateManagedWidget(
        "vbellWindow", xmPushButtonWidgetClass, g_a11yUI.vbellPulldown,
        XmNlabelString, xms, NULL);
    XmStringFree(xms);

    xms = CMPSTR("Flash Screen");
    g_a11yUI.vbellScreenBtn = XtVaCreateManagedWidget(
        "vbellScreen", xmPushButtonWidgetClass, g_a11yUI.vbellPulldown,
        XmNlabelString, xms, NULL);
    XmStringFree(xms);

    xms = CMPSTR("Visual Bell:");
    n = 0;
    XtSetArg(args[n], XmNlabelString, xms); n++;
    XtSetArg(args[n], XmNsubMenuId, g_a11yUI.vbellPulldown); n++;
    g_a11yUI.vbellOption = XmCreateOptionMenu(vbellRow, "vbellOptionMenu", args, n);
    XtManageChild(g_a11yUI.vbellOption);
    XmStringFree(xms);

    xms = CMPSTR("Reduced Motion (disable minimize and restore animations)");
    g_a11yUI.reducedMotionToggle = XtVaCreateManagedWidget(
        "reducedMotionToggle", xmToggleButtonWidgetClass, visRC,
        XmNlabelString, xms,
        XmNalignment, XmALIGNMENT_BEGINNING,
        NULL);
    XmStringFree(xms);

    /* 3. Screen Magnifier */
    CreateSectionFrame(mainCol, "magnifierFrame", "Screen Magnifier (Super or Ctrl+Alt + = / - / 0)", &magRC);

    xms = CMPSTR("Enable Full-Screen Magnifier");
    g_a11yUI.magToggle = XtVaCreateManagedWidget(
        "magnifierToggle", xmToggleButtonWidgetClass, magRC,
        XmNlabelString, xms,
        XmNalignment, XmALIGNMENT_BEGINNING,
        NULL);
    XmStringFree(xms);
    XtAddCallback(g_a11yUI.magToggle, XmNvalueChangedCallback, ToggleSensCB, nullptr);

    xms = CMPSTR("Zoom Factor (%)");
    g_a11yUI.magScale = XtVaCreateManagedWidget(
        "magnifierZoomScale", xmScaleWidgetClass, magRC,
        XmNtitleString, xms,
        XmNorientation, XmHORIZONTAL,
        XmNminimum, 125,
        XmNmaximum, 400,
        XmNvalue, 200,
        XmNshowValue, True,
        XmNscaleMultiple, 25,
        NULL);
    XmStringFree(xms);
    XtAddCallback(g_a11yUI.magScale, XmNvalueChangedCallback, MagScaleValueCB, nullptr);

    /* 4. Screen Reader Bridge */
    CreateSectionFrame(mainCol, "atspiFrame", "Screen Reader & Assistive Technology", &srRC);

    xms = CMPSTR("Enable Motif AT-SPI2 Screen Reader Bridge (XM_ACCESSIBILITY=1)");
    g_a11yUI.atspiToggle = XtVaCreateManagedWidget(
        "atspiBridgeToggle", xmToggleButtonWidgetClass, srRC,
        XmNlabelString, xms,
        XmNalignment, XmALIGNMENT_BEGINNING,
        NULL);
    XmStringFree(xms);

    XtManageChild(form);
    return style.accessibilityDialog;
}

void
popup_accessibilityBB(Widget shell)
{
    LoadA11yConfigFile(&g_a11yUI.current);
    g_a11yUI.orig = g_a11yUI.current;

    if (style.accessibilityDialog == nullptr) {
        _DtTurnOnHourGlass(shell);
        BuildAccessibilityDialog(shell);
        PopulateWidgetsFromConfig(&g_a11yUI.current);
        XtManageChild(style.accessibilityDialog);
        _DtTurnOffHourGlass(shell);
    } else {
        PopulateWidgetsFromConfig(&g_a11yUI.current);
        XtManageChild(style.accessibilityDialog);
        raiseWindow(XtWindow(XtParent(style.accessibilityDialog)));
    }
}

void
restoreAccessibility(Widget shell, XrmDatabase db)
{
    XrmName xrm_name[5];
    XrmRepresentation rep_type;
    XrmValue value;

    xrm_name[0] = XrmStringToQuark("AccessibilityDialog");
    xrm_name[1] = XrmStringToQuark("ismapped");
    xrm_name[2] = 0;

    if (XrmQGetResource(db, xrm_name, xrm_name, &rep_type, &value) &&
        strcmp(value.addr, "True") == 0) {
        g_a11ySave.poscnt = 0;
        xrm_name[1] = XrmStringToQuark("x");
        if (XrmQGetResource(db, xrm_name, xrm_name, &rep_type, &value)) {
            XtSetArg(g_a11ySave.posArgs[g_a11ySave.poscnt], XmNx, atoi((char *)value.addr));
            g_a11ySave.poscnt++;
        }
        xrm_name[1] = XrmStringToQuark("y");
        if (XrmQGetResource(db, xrm_name, xrm_name, &rep_type, &value)) {
            XtSetArg(g_a11ySave.posArgs[g_a11ySave.poscnt], XmNy, atoi((char *)value.addr));
            g_a11ySave.poscnt++;
        }
        g_a11ySave.restoreFlag = True;
        popup_accessibilityBB(shell);
    }
}

void
saveAccessibility(int fd)
{
    char bufr[512];
    if (style.accessibilityDialog != nullptr) {
        Boolean wasMapped = XtIsManaged(style.accessibilityDialog);
        if (wasMapped)
            XtUnmanageChild(style.accessibilityDialog);
        snprintf(bufr, sizeof(bufr), "*AccessibilityDialog.ismapped: %s\n",
                 wasMapped ? "True" : "False");
        WRITE_STR2FD(fd, bufr);
        snprintf(bufr, sizeof(bufr), "*AccessibilityDialog.x: %d\n*AccessibilityDialog.y: %d\n",
                 (int)XtX(XtParent(style.accessibilityDialog)),
                 (int)XtY(XtParent(style.accessibilityDialog)));
        WRITE_STR2FD(fd, bufr);
    }
}
