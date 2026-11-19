/*
 * CDE - Common Desktop Environment
 * ColorPreviewHelper.c: Isolated real Motif widget preview worker for dtstyle.
 *
 * Runs as `dtstyle --preview-helper <host_xid_hex>` with `*useColorObj: False`
 * and `*ckCoreMotif.liveAppearanceSync: False` so its XrmDatabase, ColorSets,
 * XmAppearanceProfile, and XmAppearanceDensity exist virtually only for this
 * embedded preview window without mutating the live desktop until Apply/OK.
 */

#include <ctype.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include <X11/Intrinsic.h>
#include <X11/Shell.h>
#include <X11/StringDefs.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>

#include <Xm/Appearance.h>
#include <Xm/CascadeBG.h>
#include <Xm/Form.h>
#include <Xm/LabelG.h>
#include <Xm/List.h>
#include <Xm/PushB.h>
#include <Xm/PushBG.h>
#include <Xm/RowColumn.h>
#include <Xm/Scale.h>
#include <Xm/ScrollBar.h>
#include <Xm/SeparatoG.h>
#include <Xm/TabStack.h>
#include <Xm/TextF.h>
#include <Xm/ToggleB.h>
#include <Xm/Xm.h>

#include "ColorPreviewHelper.h"

typedef struct {
    Pixel bg;
    Pixel fg;
    Pixel ts;
    Pixel bs;
    Pixel sc;
    char bg_hex[16];
    char fg_hex[16];
    char ts_hex[16];
    char bs_hex[16];
    char sc_hex[16];
} PreviewColorSet;

typedef struct {
    XtAppContext app;
    Display *dpy;
    Window host_win;
    Atom cfg_atom;
    Widget shell;
    Widget root_form;
    Widget tab_stack;
    Widget tab_pages[3];
    int active_tab_idx;
    Boolean chk_state;
    int scale_value;
    char text_value[128];
    Dimension cur_w;
    Dimension cur_h;
    int cur_dpi;
    XmAppearanceProfile cur_profile;
    XmAppearanceDensity cur_density;
    PreviewColorSet cset[8];
    PreviewColorSet eff[8];
    Pixel eff_btn_bg;
    Pixel eff_shell_bg;
    XrmDatabase owned_db;
    Boolean has_cfg;
    Boolean quit;
} PreviewHelperCtx;

static PreviewHelperCtx g_pv;

static int
pv_ignore_x_err(Display *dpy, XErrorEvent *ev)
{
    (void)dpy;
    (void)ev;
    return 0;
}

static Pixel
pv_alloc_hex(Display *dpy, Colormap cmap, const char *hex, Pixel fallback)
{
    XColor xc;
    if (!hex || hex[0] != '#')
        return fallback;
    memset(&xc, 0, sizeof(xc));
    if (XParseColor(dpy, cmap, hex, &xc) && XAllocColor(dpy, cmap, &xc))
        return xc.pixel;
    return fallback;
}

static void
pv_init_default_colors(PreviewHelperCtx *ctx)
{
    static const char *const defs[8][5] = {
        {"#EEA86B", "#FFFFFF", "#F9DFC7", "#7F5939", "#CA8E5B"},
        {"#999999", "#FFFFFF", "#D3D3D3", "#4F4F4F", "#828282"},
        {"#8B98AB", "#FFFFFF", "#CCD3DC", "#49505A", "#768191"},
        {"#606575", "#FFFFFF", "#B7BAC3", "#31343C", "#525663"},
        {"#C2B2A8", "#000000", "#E7E1DD", "#665E58", "#A5978F"},
        {"#468899", "#FFFFFF", "#B0CFD7", "#23454E", "#3B7482"},
        {"#B5878D", "#FFFFFF", "#E2CFD1", "#5F474A", "#9A7378"},
        {"#93ABB8", "#000000", "#D3DEE4", "#4B575E", "#7D919C"},
    };
    Colormap cmap = DefaultColormap(ctx->dpy, DefaultScreen(ctx->dpy));
    Pixel blk = BlackPixel(ctx->dpy, DefaultScreen(ctx->dpy));
    Pixel wht = WhitePixel(ctx->dpy, DefaultScreen(ctx->dpy));
    for (int i = 0; i < 8; ++i) {
        snprintf(ctx->cset[i].bg_hex, sizeof(ctx->cset[i].bg_hex), "%s", defs[i][0]);
        snprintf(ctx->cset[i].fg_hex, sizeof(ctx->cset[i].fg_hex), "%s", defs[i][1]);
        snprintf(ctx->cset[i].ts_hex, sizeof(ctx->cset[i].ts_hex), "%s", defs[i][2]);
        snprintf(ctx->cset[i].bs_hex, sizeof(ctx->cset[i].bs_hex), "%s", defs[i][3]);
        snprintf(ctx->cset[i].sc_hex, sizeof(ctx->cset[i].sc_hex), "%s", defs[i][4]);
        ctx->cset[i].bg = pv_alloc_hex(ctx->dpy, cmap, defs[i][0], wht);
        ctx->cset[i].fg = pv_alloc_hex(ctx->dpy, cmap, defs[i][1], blk);
        ctx->cset[i].ts = pv_alloc_hex(ctx->dpy, cmap, defs[i][2], wht);
        ctx->cset[i].bs = pv_alloc_hex(ctx->dpy, cmap, defs[i][3], blk);
        ctx->cset[i].sc = pv_alloc_hex(ctx->dpy, cmap, defs[i][4], wht);
    }
}

static void
pv_put_pixel(XrmDatabase *db, const char *res, Pixel *px)
{
    XrmValue v;
    v.size = sizeof(Pixel);
    v.addr = (XPointer)px;
    XrmPutResource(db, res, XmRPixel, &v);
}

static void
pv_parse_hex_rgb(const char *hex, unsigned int out[3])
{
    unsigned int r = 128, g = 128, b = 128;
    if (hex && hex[0] == '#' && strlen(hex) >= 7)
        (void)sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b);
    out[0] = r;
    out[1] = g;
    out[2] = b;
}

static void
pv_blend_rgb(const unsigned int a[3], const unsigned int b[3], double t,
             unsigned int out[3])
{
    for (int i = 0; i < 3; ++i) {
        double v = (1.0 - t) * (double)a[i] + t * (double)b[i];
        if (v < 0.0) v = 0.0;
        if (v > 255.0) v = 255.0;
        out[i] = (unsigned int)(v + 0.5);
    }
}

static Pixel
pv_alloc_rgb8(Display *dpy, Colormap cmap, const unsigned int rgb[3],
              Pixel fallback)
{
    char hex[16];
    snprintf(hex, sizeof(hex), "#%02X%02X%02X", rgb[0], rgb[1], rgb[2]);
    return pv_alloc_hex(dpy, cmap, hex, fallback);
}

static void
pv_harmonize_slot(Display *dpy, Colormap cmap, const PreviewColorSet *raw,
                  int slot, const unsigned int acc_rgb[3],
                  XmAppearanceProfile profile, PreviewColorSet *out,
                  Pixel *btn_bg_out)
{
    static const unsigned int white[3] = {255, 255, 255};
    static const unsigned int black[3] = {0, 0, 0};
    unsigned int raw_rgb[3], surf_rgb[3], fg_rgb[3], border_rgb[3], arm_rgb[3], btn_rgb[3];
    Pixel blk = BlackPixel(dpy, DefaultScreen(dpy));
    Pixel wht = WhitePixel(dpy, DefaultScreen(dpy));

    *out = *raw;
    if (profile != XmAPPEARANCE_MODERN_LIGHT &&
        profile != XmAPPEARANCE_MODERN_DARK) {
        if (btn_bg_out) *btn_bg_out = raw->bg;
        return;
    }

    pv_parse_hex_rgb(raw->bg_hex, raw_rgb);
    if (profile == XmAPPEARANCE_MODERN_LIGHT) {
        static const unsigned int base_ink[3] = {0x1d, 0x21, 0x29};
        static const unsigned int base_well[3] = {0xfb, 0xfc, 0xfe};
        static const unsigned int base_prim[3] = {0xf1, 0xf3, 0xf7};
        static const unsigned int base_sec[3] = {0xe6, 0xeb, 0xf2};
        unsigned int dark_tint[3];

        pv_blend_rgb(raw_rgb, black, 0.72, dark_tint);
        pv_blend_rgb(base_ink, dark_tint, 0.14, fg_rgb);
        if (slot == 3)
            pv_blend_rgb(base_well, raw_rgb, 0.10, surf_rgb);
        else if (slot == 4)
            pv_blend_rgb(base_prim, raw_rgb, 0.25, surf_rgb);
        else
            pv_blend_rgb(base_sec, raw_rgb, 0.28, surf_rgb);

        double lum = (0.30 * surf_rgb[0] + 0.59 * surf_rgb[1] + 0.11 * surf_rgb[2]) / 255.0;
        if (lum < 0.62)
            pv_blend_rgb(surf_rgb, white, 0.35, surf_rgb);
        if (lum > 0.88)
            pv_blend_rgb(surf_rgb, fg_rgb, 0.07, btn_rgb);
        else
            pv_blend_rgb(surf_rgb, white, 0.48, btn_rgb);
        pv_blend_rgb(surf_rgb, fg_rgb, 0.25, border_rgb);
        pv_blend_rgb(surf_rgb, acc_rgb, 0.22, arm_rgb);
    } else {
        static const unsigned int base_fg[3] = {0xee, 0xf2, 0xf7};
        static const unsigned int base_well[3] = {0x16, 0x19, 0x20};
        static const unsigned int base_prim[3] = {0x21, 0x25, 0x2f};
        static const unsigned int base_sec[3] = {0x2b, 0x31, 0x3d};
        unsigned int tint_rgb[3];

        fg_rgb[0] = base_fg[0]; fg_rgb[1] = base_fg[1]; fg_rgb[2] = base_fg[2];
        double rlum = (0.30 * raw_rgb[0] + 0.59 * raw_rgb[1] + 0.11 * raw_rgb[2]) / 255.0;
        if (rlum > 0.24)
            pv_blend_rgb(base_prim, raw_rgb, 0.28, tint_rgb);
        else {
            tint_rgb[0] = raw_rgb[0]; tint_rgb[1] = raw_rgb[1]; tint_rgb[2] = raw_rgb[2];
        }

        if (slot == 3)
            pv_blend_rgb(base_well, tint_rgb, 0.45, surf_rgb);
        else if (slot == 4)
            pv_blend_rgb(base_prim, tint_rgb, 0.55, surf_rgb);
        else
            pv_blend_rgb(base_sec, tint_rgb, 0.55, surf_rgb);

        pv_blend_rgb(surf_rgb, fg_rgb, 0.13, btn_rgb);
        pv_blend_rgb(surf_rgb, fg_rgb, 0.25, border_rgb);
        pv_blend_rgb(surf_rgb, acc_rgb, 0.28, arm_rgb);
    }

    out->bg = pv_alloc_rgb8(dpy, cmap, surf_rgb, wht);
    out->fg = pv_alloc_rgb8(dpy, cmap, fg_rgb, blk);
    out->ts = pv_alloc_rgb8(dpy, cmap, border_rgb, wht);
    out->bs = pv_alloc_rgb8(dpy, cmap, border_rgb, blk);
    out->sc = pv_alloc_rgb8(dpy, cmap, arm_rgb, wht);
    if (btn_bg_out)
        *btn_bg_out = pv_alloc_rgb8(dpy, cmap, btn_rgb, out->bg);
}

static void
pv_apply_virtual_xrm(PreviewHelperCtx *ctx)
{
    const char *prof_str =
        (ctx->cur_profile == XmAPPEARANCE_MODERN_LIGHT) ? "modern_light" :
        (ctx->cur_profile == XmAPPEARANCE_MODERN_DARK) ? "modern_dark" :
        (ctx->cur_profile == XmAPPEARANCE_HIGH_CONTRAST) ? "high_contrast" :
        "classic";
    const char *dens_str =
        (ctx->cur_density == XmAPPEARANCE_DENSITY_COMPACT) ? "compact" : "comfortable";

    char dpi_line[64] = "";
    if (ctx->cur_dpi >= 72 && ctx->cur_dpi <= 384)
        snprintf(dpi_line, sizeof(dpi_line), "Xft.dpi: %d\n", ctx->cur_dpi);

    Colormap cmap = DefaultColormap(ctx->dpy, DefaultScreen(ctx->dpy));
    unsigned int acc_rgb[3];
    pv_parse_hex_rgb(ctx->cset[0].bg_hex, acc_rgb);
    for (int i = 0; i < 8; ++i)
        ctx->eff[i] = ctx->cset[i];
    ctx->eff_btn_bg = ctx->cset[4].bg;
    pv_harmonize_slot(ctx->dpy, cmap, &ctx->cset[3], 3, acc_rgb,
                      ctx->cur_profile, &ctx->eff[3], NULL);
    pv_harmonize_slot(ctx->dpy, cmap, &ctx->cset[4], 4, acc_rgb,
                      ctx->cur_profile, &ctx->eff[4], &ctx->eff_btn_bg);
    pv_harmonize_slot(ctx->dpy, cmap, &ctx->cset[5], 5, acc_rgb,
                      ctx->cur_profile, &ctx->eff[5], NULL);
    ctx->eff_shell_bg = (ctx->cur_profile == XmAPPEARANCE_HIGH_CONTRAST)
                            ? BlackPixel(ctx->dpy, DefaultScreen(ctx->dpy))
                            : ctx->eff[4].bg;

    char spec[4096];
    snprintf(
        spec, sizeof(spec),
        "*useColorObj: False\n"
        "*ckCoreMotif.liveAppearanceSync: False\n"
        "*ckCoreMotif.elevateFlatButtons: True\n"
        "%s"
        "*ckCoreMotif.appearanceProfile: %s\n"
        "*appearanceProfile: %s\n"
        "*ckCoreMotif.appearanceDensity: %s\n"
        "*appearanceDensity: %s\n"
        "*ckCoreMotif.accentColor: %s\n"
        "%s",
        dpi_line,
        prof_str, prof_str, dens_str, dens_str,
        ctx->cset[0].bg_hex,
        (ctx->cur_profile == XmAPPEARANCE_CLASSIC)
            ? "*FontList: -dt-interface user-medium-r-normal-s*-*-*-*-*-*-*-*-*:\n"
            : ""
    );

    XrmDatabase db = XrmGetStringDatabase(spec);
    if (!db)
        return;

    if (ctx->cur_profile != XmAPPEARANCE_HIGH_CONTRAST) {
        /* Primary surface (ColorSet 5 -> index 4) */
        pv_put_pixel(&db, "*background", &ctx->eff[4].bg);
        pv_put_pixel(&db, "*foreground", &ctx->eff[4].fg);
        pv_put_pixel(&db, "*topShadowColor", &ctx->eff[4].ts);
        pv_put_pixel(&db, "*bottomShadowColor", &ctx->eff[4].bs);
        pv_put_pixel(&db, "*selectColor", &ctx->cset[0].bg);
        pv_put_pixel(&db, "*armColor", &ctx->eff[4].sc);
        pv_put_pixel(&db, "*troughColor", &ctx->eff[3].bg);
        pv_put_pixel(&db, "*unselectColor", &ctx->eff[3].bg);
        pv_put_pixel(&db, "*highlightColor", &ctx->cset[0].bg);

        if (ctx->cur_profile == XmAPPEARANCE_MODERN_LIGHT ||
            ctx->cur_profile == XmAPPEARANCE_MODERN_DARK) {
            pv_put_pixel(&db, "*XmPushButton.background", &ctx->eff_btn_bg);
            pv_put_pixel(&db, "*XmPushButtonGadget.background", &ctx->eff_btn_bg);
            pv_put_pixel(&db, "*OptionButton.background", &ctx->eff_btn_bg);
        }

        /* MenuBar / Secondary (ColorSet 6 -> index 5) */
        pv_put_pixel(&db, "*pvMenuBar*background", &ctx->eff[5].bg);
        pv_put_pixel(&db, "*pvMenuBar*foreground", &ctx->eff[5].fg);
        pv_put_pixel(&db, "*pvMenuBar*topShadowColor", &ctx->eff[5].ts);
        pv_put_pixel(&db, "*pvMenuBar*bottomShadowColor", &ctx->eff[5].bs);
        pv_put_pixel(&db, "*XmMenuShell*background", &ctx->eff[5].bg);
        pv_put_pixel(&db, "*XmMenuShell*foreground", &ctx->eff[5].fg);
        pv_put_pixel(&db, "*XmMenuShell*topShadowColor", &ctx->eff[5].ts);
        pv_put_pixel(&db, "*XmMenuShell*bottomShadowColor", &ctx->eff[5].bs);
        pv_put_pixel(&db, "*XmMenuShell*armColor", &ctx->eff[5].sc);

        /* Text & List areas (ColorSet 4 -> index 3) */
        pv_put_pixel(&db, "*XmTextField*background", &ctx->eff[3].bg);
        pv_put_pixel(&db, "*XmTextField*foreground", &ctx->eff[3].fg);
        pv_put_pixel(&db, "*XmTextField*topShadowColor", &ctx->eff[3].ts);
        pv_put_pixel(&db, "*XmTextField*bottomShadowColor", &ctx->eff[3].bs);
        pv_put_pixel(&db, "*XmList*background", &ctx->eff[3].bg);
        pv_put_pixel(&db, "*XmList*foreground", &ctx->eff[3].fg);
        pv_put_pixel(&db, "*XmList*topShadowColor", &ctx->eff[3].ts);
        pv_put_pixel(&db, "*XmList*bottomShadowColor", &ctx->eff[3].bs);
    }

    XrmSetDatabase(ctx->dpy, db);
    if (ctx->owned_db && ctx->owned_db != db)
        XrmDestroyDatabase(ctx->owned_db);
    ctx->owned_db = db;
}

static void
pv_on_tab_selected(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    PreviewHelperCtx *ctx = (PreviewHelperCtx *)client_data;
    XmTabStackCallbackStruct *cbs = (XmTabStackCallbackStruct *)call_data;
    if (!ctx || !cbs || !cbs->selected_child)
        return;
    for (int i = 0; i < 3; ++i) {
        if (ctx->tab_pages[i] == cbs->selected_child) {
            ctx->active_tab_idx = i;
            break;
        }
    }
}

static void
pv_on_toggle_changed(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    PreviewHelperCtx *ctx = (PreviewHelperCtx *)client_data;
    XmToggleButtonCallbackStruct *cbs = (XmToggleButtonCallbackStruct *)call_data;
    if (ctx && cbs)
        ctx->chk_state = cbs->set ? True : False;
}

static void
pv_on_scale_changed(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    PreviewHelperCtx *ctx = (PreviewHelperCtx *)client_data;
    XmScaleCallbackStruct *cbs = (XmScaleCallbackStruct *)call_data;
    if (ctx && cbs)
        ctx->scale_value = cbs->value;
}

static void
pv_on_text_changed(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)call_data;
    PreviewHelperCtx *ctx = (PreviewHelperCtx *)client_data;
    if (!ctx || !w)
        return;
    char *s = XmTextFieldGetString(w);
    if (s) {
        snprintf(ctx->text_value, sizeof(ctx->text_value), "%s", s);
        XtFree(s);
    }
}

static void
pv_rebuild_dialog_contents(PreviewHelperCtx *ctx)
{
    if (!ctx || !ctx->shell)
        return;

    if (ctx->cur_dpi >= 72 && ctx->cur_dpi <= 384)
        (void)XmSetDisplayScaleOverride(ctx->dpy, (double)ctx->cur_dpi / 96.0);
    pv_apply_virtual_xrm(ctx);
    (void)XmApplyAppearanceDensity(ctx->dpy, ctx->cur_density);
    (void)XmApplyAppearanceProfile(ctx->dpy, ctx->cur_profile);

    Widget old_root = ctx->root_form;
    ctx->root_form = NULL;
    if (old_root)
        XtDestroyWidget(old_root);

    Pixel surf_bg = ctx->eff_shell_bg;
    XtVaSetValues(ctx->shell,
                  XmNbackground, surf_bg,
                  XmNwidth, ctx->cur_w,
                  XmNheight, ctx->cur_h,
                  NULL);

    int pad = (ctx->cur_density == XmAPPEARANCE_DENSITY_COMPACT) ? 4 : 6;

    Widget root = XtVaCreateWidget(
        "pvRootForm", xmFormWidgetClass, ctx->shell,
        XmNwidth, ctx->cur_w,
        XmNheight, ctx->cur_h,
        XmNmarginWidth, 0,
        XmNmarginHeight, 0,
        NULL
    );
    ctx->root_form = root;

    /* 1. Real Motif MenuBar */
    Widget mbar = XmCreateMenuBar(root, "pvMenuBar", nullptr, 0);
    XtVaSetValues(
        mbar,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );

    Widget file_pd = XmCreatePulldownMenu(mbar, "filePD", nullptr, 0);
    static const char *const file_items[] = {"New", "Open...", "Save", "Close"};
    for (int i = 0; i < 4; ++i) {
        XmString xs = XmStringCreateLocalized((char *)file_items[i]);
        XtVaCreateManagedWidget("fileItem", xmPushButtonGadgetClass, file_pd,
                                XmNlabelString, xs, NULL);
        XmStringFree(xs);
    }

    XmString xs_file = XmStringCreateLocalized("File");
    XtVaCreateManagedWidget("fileCascade", xmCascadeButtonGadgetClass, mbar,
                            XmNlabelString, xs_file, XmNsubMenuId, file_pd, NULL);
    XmStringFree(xs_file);

    XmString xs_edit = XmStringCreateLocalized("Edit");
    XtVaCreateManagedWidget("editCascade", xmCascadeButtonGadgetClass, mbar,
                            XmNlabelString, xs_edit, NULL);
    XmStringFree(xs_edit);

    XmString xs_view = XmStringCreateLocalized("View");
    XtVaCreateManagedWidget("viewCascade", xmCascadeButtonGadgetClass, mbar,
                            XmNlabelString, xs_view, NULL);
    XmStringFree(xs_view);

    XmString xs_help = XmStringCreateLocalized("Help");
    Widget help_cb = XtVaCreateManagedWidget(
        "helpCascade", xmCascadeButtonGadgetClass, mbar,
        XmNlabelString, xs_help, NULL
    );
    XmStringFree(xs_help);
    XtVaSetValues(mbar, XmNmenuHelpWidget, help_cb, NULL);
    XtManageChild(mbar);

    /* 2. Bottom Dialog Action Buttons (OK + Cancel + Separator) */
    Widget btn_row = XtVaCreateManagedWidget(
        "pvBtnRow", xmFormWidgetClass, root,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNbottomOffset, pad,
        XmNleftAttachment, XmATTACH_FORM,
        XmNleftOffset, pad + 2,
        XmNrightAttachment, XmATTACH_FORM,
        XmNrightOffset, pad + 2,
        NULL
    );

    int btn_mh = (ctx->cur_profile == XmAPPEARANCE_CLASSIC) ? 1 : 3;
    XmString xs_ok = XmStringCreateLocalized("OK");
    Widget ok_btn = XtVaCreateManagedWidget(
        "pvOkBtn", xmPushButtonWidgetClass, btn_row,
        XmNlabelString, xs_ok,
        XmNleftAttachment, XmATTACH_POSITION,
        XmNleftPosition, 18,
        XmNrightAttachment, XmATTACH_POSITION,
        XmNrightPosition, 46,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNmarginHeight, btn_mh,
        NULL
    );
    XmStringFree(xs_ok);

    XmString xs_cancel = XmStringCreateLocalized("Cancel");
    XtVaCreateManagedWidget(
        "pvCancelBtn", xmPushButtonWidgetClass, btn_row,
        XmNlabelString, xs_cancel,
        XmNleftAttachment, XmATTACH_POSITION,
        XmNleftPosition, 54,
        XmNrightAttachment, XmATTACH_POSITION,
        XmNrightPosition, 82,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNmarginHeight, btn_mh,
        NULL
    );
    XmStringFree(xs_cancel);
    XtVaSetValues(root, XmNdefaultButton, ok_btn, NULL);

    Widget sep = XtVaCreateManagedWidget(
        "pvSep", xmSeparatorGadgetClass, root,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_WIDGET,
        XmNbottomWidget, btn_row,
        XmNbottomOffset, pad - 1,
        NULL
    );

    /* 3. Real Motif XmTabStack in work area */
    Widget tabs = XmCreateTabStack(root, "pvTabStack", nullptr, 0);
    ctx->tab_stack = tabs;
    XtVaSetValues(
        tabs,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, mbar,
        XmNtopOffset, pad,
        XmNleftAttachment, XmATTACH_FORM,
        XmNleftOffset, pad,
        XmNrightAttachment, XmATTACH_FORM,
        XmNrightOffset, pad,
        XmNbottomAttachment, XmATTACH_WIDGET,
        XmNbottomWidget, sep,
        XmNbottomOffset, pad,
        NULL
    );

    /* Tab 1: General (OptionMenu + ToggleButton + Scale) */
    XmString tab1_xs = XmStringCreateLocalized("General");
    Widget page1 = XtVaCreateManagedWidget(
        "tabGeneral", xmFormWidgetClass, tabs,
        XmNtabLabelString, tab1_xs,
        XmNmarginWidth, pad + 2,
        XmNmarginHeight, pad,
        NULL
    );
    XmStringFree(tab1_xs);
    ctx->tab_pages[0] = page1;

    Widget mode_pd = XmCreatePulldownMenu(page1, "modePD", nullptr, 0);
    static const char *const mode_opts[] = {"Standard Layout", "Workstation Pro"};
    for (int i = 0; i < 2; ++i) {
        XmString mxs = XmStringCreateLocalized((char *)mode_opts[i]);
        XtVaCreateManagedWidget("modeOpt", xmPushButtonGadgetClass, mode_pd,
                                XmNlabelString, mxs, NULL);
        XmStringFree(mxs);
    }
    XmString mode_lbl = XmStringCreateLocalized("Preset:");
    Arg oargs[4];
    Cardinal on = 0;
    XtSetArg(oargs[on], XmNlabelString, mode_lbl); on++;
    XtSetArg(oargs[on], XmNsubMenuId, mode_pd); on++;
    XtSetArg(oargs[on], XmNtopAttachment, XmATTACH_FORM); on++;
    XtSetArg(oargs[on], XmNleftAttachment, XmATTACH_FORM); on++;
    Widget opt_menu = XmCreateOptionMenu(page1, "pvOptMenu", oargs, on);
    XmStringFree(mode_lbl);
    XtManageChild(opt_menu);

    XmString chk_xs = XmStringCreateLocalized("Live theme sync");
    Widget chk = XtVaCreateManagedWidget(
        "pvCheck", xmToggleButtonWidgetClass, page1,
        XmNlabelString, chk_xs,
        XmNset, ctx->chk_state,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, opt_menu,
        XmNtopOffset, 4,
        XmNleftAttachment, XmATTACH_FORM,
        NULL
    );
    XmStringFree(chk_xs);
    XtAddCallback(chk, XmNvalueChangedCallback, pv_on_toggle_changed, ctx);

    Widget scale = XtVaCreateManagedWidget(
        "pvScale", xmScaleWidgetClass, page1,
        XmNorientation, XmHORIZONTAL,
        XmNminimum, 0,
        XmNmaximum, 100,
        XmNvalue, ctx->scale_value,
        XmNshowValue, False,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, chk,
        XmNtopOffset, 4,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );
    XtAddCallback(scale, XmNvalueChangedCallback, pv_on_scale_changed, ctx);

    /* Tab 2: Input & ScrollBar */
    XmString tab2_xs = XmStringCreateLocalized("Controls");
    Widget page2 = XtVaCreateManagedWidget(
        "tabControls", xmFormWidgetClass, tabs,
        XmNtabLabelString, tab2_xs,
        XmNmarginWidth, pad + 2,
        XmNmarginHeight, pad + 2,
        NULL
    );
    XmStringFree(tab2_xs);
    ctx->tab_pages[1] = page2;

    Widget tf = XtVaCreateManagedWidget(
        "pvTextField", xmTextFieldWidgetClass, page2,
        XmNvalue, ctx->text_value,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );
    XtAddCallback(tf, XmNvalueChangedCallback, pv_on_text_changed, ctx);

    XtVaCreateManagedWidget(
        "pvScrollBar", xmScrollBarWidgetClass, page2,
        XmNorientation, XmHORIZONTAL,
        XmNvalue, 30,
        XmNsliderSize, 35,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, tf,
        XmNtopOffset, 8,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );

    /* Tab 3: ScrolledList */
    XmString tab3_xs = XmStringCreateLocalized("Items");
    Widget page3 = XtVaCreateManagedWidget(
        "tabItems", xmFormWidgetClass, tabs,
        XmNtabLabelString, tab3_xs,
        XmNmarginWidth, pad,
        XmNmarginHeight, pad,
        NULL
    );
    XmStringFree(tab3_xs);
    ctx->tab_pages[2] = page3;

    Widget slist = XmCreateScrolledList(page3, "pvList", nullptr, 0);
    XtVaSetValues(
        XtParent(slist),
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );
    static const char *const list_items[] = {
        "Workspace One (Active)", "Workspace Two", "Workspace Three"
    };
    for (int i = 0; i < 3; ++i) {
        XmString lxs = XmStringCreateLocalized((char *)list_items[i]);
        XmListAddItem(slist, lxs, 0);
        XmStringFree(lxs);
    }
    XmListSelectPos(slist, 1, False);
    XtManageChild(slist);

    XtAddCallback(tabs, XmNtabSelectedCallback, pv_on_tab_selected, ctx);
    XtManageChild(tabs);
    XtManageChild(root);

    /* Re-apply appearance pass on freshly created subtree so Modern/HC roles & fonts settle */
    if (ctx->cur_profile != XmAPPEARANCE_CLASSIC) {
        (void)XmApplyAppearanceDensity(ctx->dpy, ctx->cur_density);
        (void)XmApplyAppearanceProfile(ctx->dpy, ctx->cur_profile);
    }

    if (ctx->active_tab_idx >= 0 && ctx->active_tab_idx < 3 &&
        ctx->tab_pages[ctx->active_tab_idx] != NULL) {
        XmTabStackSelectTab(ctx->tab_pages[ctx->active_tab_idx], False);
    }
    XFlush(ctx->dpy);
}

static void
pv_load_cfg_from_host(PreviewHelperCtx *ctx)
{
    Atom act_t = None;
    int act_f = 0;
    unsigned long nitems = 0, rem = 0;
    unsigned char *prop = NULL;

    if (XGetWindowProperty(ctx->dpy, ctx->host_win, ctx->cfg_atom, 0L, 16384L,
                           False, XA_STRING, &act_t, &act_f, &nitems, &rem,
                           &prop) != Success ||
        !prop || nitems == 0) {
        if (prop)
            XFree(prop);
        return;
    }

    char *buf = (char *)malloc(nitems + 1);
    if (!buf) {
        XFree(prop);
        return;
    }
    memcpy(buf, prop, nitems);
    buf[nitems] = '\0';
    XFree(prop);

    Colormap cmap = DefaultColormap(ctx->dpy, DefaultScreen(ctx->dpy));
    char *saveptr = NULL;
    for (char *line = strtok_r(buf, "\n", &saveptr); line != NULL;
         line = strtok_r(NULL, "\n", &saveptr)) {
        if (strncmp(line, "profile=", 8) == 0) {
            const char *v = line + 8;
            if (strcmp(v, "modern_light") == 0)
                ctx->cur_profile = XmAPPEARANCE_MODERN_LIGHT;
            else if (strcmp(v, "modern_dark") == 0)
                ctx->cur_profile = XmAPPEARANCE_MODERN_DARK;
            else if (strcmp(v, "high_contrast") == 0)
                ctx->cur_profile = XmAPPEARANCE_HIGH_CONTRAST;
            else
                ctx->cur_profile = XmAPPEARANCE_CLASSIC;
        } else if (strncmp(line, "density=", 8) == 0) {
            const char *v = line + 8;
            ctx->cur_density = (strcmp(v, "compact") == 0)
                                   ? XmAPPEARANCE_DENSITY_COMPACT
                                   : XmAPPEARANCE_DENSITY_COMFORTABLE;
        } else if (strncmp(line, "dpi=", 4) == 0) {
            int d = atoi(line + 4);
            if (d >= 72 && d <= 384)
                ctx->cur_dpi = d;
        } else if (line[0] == 'c' && line[1] >= '0' && line[1] <= '7' &&
                   line[2] == '=') {
            int idx = line[1] - '0';
            char h_bg[16] = "", h_fg[16] = "", h_ts[16] = "", h_bs[16] = "", h_sc[16] = "";
            if (sscanf(line + 3, "%15[^,],%15[^,],%15[^,],%15[^,],%15s",
                       h_bg, h_fg, h_ts, h_bs, h_sc) == 5) {
                snprintf(ctx->cset[idx].bg_hex, sizeof(ctx->cset[idx].bg_hex), "%s", h_bg);
                snprintf(ctx->cset[idx].fg_hex, sizeof(ctx->cset[idx].fg_hex), "%s", h_fg);
                snprintf(ctx->cset[idx].ts_hex, sizeof(ctx->cset[idx].ts_hex), "%s", h_ts);
                snprintf(ctx->cset[idx].bs_hex, sizeof(ctx->cset[idx].bs_hex), "%s", h_bs);
                snprintf(ctx->cset[idx].sc_hex, sizeof(ctx->cset[idx].sc_hex), "%s", h_sc);
                ctx->cset[idx].bg = pv_alloc_hex(ctx->dpy, cmap, h_bg, ctx->cset[idx].bg);
                ctx->cset[idx].fg = pv_alloc_hex(ctx->dpy, cmap, h_fg, ctx->cset[idx].fg);
                ctx->cset[idx].ts = pv_alloc_hex(ctx->dpy, cmap, h_ts, ctx->cset[idx].ts);
                ctx->cset[idx].bs = pv_alloc_hex(ctx->dpy, cmap, h_bs, ctx->cset[idx].bs);
                ctx->cset[idx].sc = pv_alloc_hex(ctx->dpy, cmap, h_sc, ctx->cset[idx].sc);
            }
        }
    }
    free(buf);
    ctx->has_cfg = True;
    pv_rebuild_dialog_contents(ctx);
}

int
DtstyleRunPreviewHelper(int argc, char **argv)
{
    if (argc < 3)
        return 1;

#ifdef __linux__
    (void)prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif

    unsigned long raw_xid = strtoul(argv[2], NULL, 0);
    if (raw_xid == 0)
        return 1;

    memset(&g_pv, 0, sizeof(g_pv));
    g_pv.host_win = (Window)raw_xid;
    g_pv.chk_state = True;
    g_pv.scale_value = 68;
    snprintf(g_pv.text_value, sizeof(g_pv.text_value), "Motif ABI 9 + CDE Palette");
    g_pv.cur_w = 386;
    g_pv.cur_h = 200;
    g_pv.cur_profile = XmAPPEARANCE_CLASSIC;
    g_pv.cur_density = XmAPPEARANCE_DENSITY_COMFORTABLE;

    XSetErrorHandler(pv_ignore_x_err);
    XtToolkitInitialize();
    g_pv.app = XtCreateApplicationContext();

    static String fallback_res[] = {
        (String)"*useColorObj: False",
        (String)"*ckCoreMotif.liveAppearanceSync: False",
        (String)"*ckCoreMotif.elevateFlatButtons: True",
        NULL
    };
    XtAppSetFallbackResources(g_pv.app, fallback_res);

    int d_argc = 1;
    char *d_argv[2] = {argv[0], NULL};
    g_pv.dpy = XtOpenDisplay(g_pv.app, NULL, "dtstylePreview", "DtstylePreview",
                             NULL, 0, &d_argc, d_argv);
    if (!g_pv.dpy)
        return 1;

    XWindowAttributes wa;
    memset(&wa, 0, sizeof(wa));
    if (!XGetWindowAttributes(g_pv.dpy, g_pv.host_win, &wa))
        return 1;
    if (wa.width > 40 && wa.height > 40) {
        g_pv.cur_w = (Dimension)wa.width;
        g_pv.cur_h = (Dimension)wa.height;
    }

    g_pv.cfg_atom = XInternAtom(g_pv.dpy, "_DTSTYLE_PREVIEW_CFG", False);
    XSelectInput(g_pv.dpy, g_pv.host_win,
                 PropertyChangeMask | StructureNotifyMask);

    pv_init_default_colors(&g_pv);
    pv_apply_virtual_xrm(&g_pv);

    g_pv.shell = XtVaAppCreateShell(
        "dtstylePreview", "DtstylePreview", topLevelShellWidgetClass, g_pv.dpy,
        XmNoverrideRedirect, True,
        XmNmappedWhenManaged, False,
        XmNwidth, g_pv.cur_w,
        XmNheight, g_pv.cur_h,
        XmNborderWidth, 0,
        NULL
    );

    XtRealizeWidget(g_pv.shell);
    XReparentWindow(g_pv.dpy, XtWindow(g_pv.shell), g_pv.host_win, 0, 0);
    XMapWindow(g_pv.dpy, XtWindow(g_pv.shell));
    XFlush(g_pv.dpy);

    pv_load_cfg_from_host(&g_pv);
    if (!g_pv.has_cfg)
        pv_rebuild_dialog_contents(&g_pv);

    while (!g_pv.quit) {
        XEvent ev;
        XtAppNextEvent(g_pv.app, &ev);
        if (ev.xany.window == g_pv.host_win) {
            if (ev.type == DestroyNotify) {
                break;
            }
            if (ev.type == ConfigureNotify) {
                Dimension nw = (Dimension)(ev.xconfigure.width > 40 ? ev.xconfigure.width : 40);
                Dimension nh = (Dimension)(ev.xconfigure.height > 40 ? ev.xconfigure.height : 40);
                if (nw != g_pv.cur_w || nh != g_pv.cur_h) {
                    g_pv.cur_w = nw;
                    g_pv.cur_h = nh;
                    XtVaSetValues(g_pv.shell, XmNwidth, nw, XmNheight, nh, NULL);
                    if (g_pv.root_form)
                        XtVaSetValues(g_pv.root_form, XmNwidth, nw, XmNheight, nh, NULL);
                }
                continue;
            }
            if (ev.type == PropertyNotify &&
                ev.xproperty.atom == g_pv.cfg_atom &&
                ev.xproperty.state == PropertyNewValue) {
                pv_load_cfg_from_host(&g_pv);
                continue;
            }
        }
        XtDispatchEvent(&ev);
    }

    return 0;
}
