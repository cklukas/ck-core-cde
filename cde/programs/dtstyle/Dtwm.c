/*
 * CDE - Common Desktop Environment
 *
 * Copyright (c) 1993-2012, The Open Group. All rights reserved.
 *
 * These libraries and programs are free software; you can
 * redistribute them and/or modify them under the terms of the GNU
 * Lesser General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * These libraries and programs are distributed in the hope that
 * they will be useful, but WITHOUT ANY WARRANTY; without even the
 * implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE. See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with these libraries and programs; if not, write
 * to the Free Software Foundation, Inc., 51 Franklin Street, Fifth
 * Floor, Boston, MA 02110-1301 USA
 */
/************************************<+>*************************************
 ****************************************************************************
 **
 **   File:        Dtwm.c
 **
 **   Project:     DT 3.0
 **
 **   Description: Controls the Dtstyle Window Manager customization dialog
 **
 **
 **  (c) Copyright Hewlett-Packard Company, 1990, 1993.  
 **
 **
 **
 ****************************************************************************
 ************************************<+>*************************************/
/* $XConsortium: Dtwm.c /main/4 1995/10/30 13:09:13 rswiston $ */
/*+++++++++++++++++++++++++++++++++++++++*/
/* include files                         */
/*+++++++++++++++++++++++++++++++++++++++*/

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <Xm/MwmUtil.h>

#include <Xm/XmP.h>
#include <Xm/Xm.h>
#include <Xm/Form.h>
#include <Xm/LabelG.h>
#include <Xm/PushBG.h>
#include <Xm/RowColumn.h>
#include <Xm/ToggleBG.h>
#include <Xm/SeparatoG.h>
#include <Xm/TextF.h>
#include <Xm/VendorSEP.h>
#include <Xm/MessageB.h>

#include <Dt/DialogBox.h>
#include <Dt/Icon.h>
#include <Dt/GetDispRes.h>
#include <Dt/TitleBox.h>

#include <Dt/Message.h>
#include <Dt/HourGlass.h>
#include <Dt/Wsm.h>

#include "Help.h"
#include "Main.h"
#include "SaveRestore.h"
#include "Protocol.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>

/*+++++++++++++++++++++++++++++++++++++++*/
/* include extern functions              */
/*+++++++++++++++++++++++++++++++++++++++*/
#include "Dtwm.h"

// DtUtil1/WmRestart.c
int _DtWmRestartNoConfirm (Display *display, Window root);


/*+++++++++++++++++++++++++++++++++++++++*/
/* Local #defines                        */
/*+++++++++++++++++++++++++++++++++++++++*/
#define POINTER 0    
#define EXPLICIT 1    

#define POINTER_STR         "pointer"
#define EXPLICIT_STR        "explicit"

#define NUM_PLACEMENT_ICONS 8
#define TOP_LEFT            0
#define TOP_RIGHT           1
#define BOTTOM_LEFT         2
#define BOTTOM_RIGHT        3
#define LEFT_TOP            4
#define LEFT_BOTTOM         5
#define RIGHT_TOP           6
#define RIGHT_BOTTOM        7
#define WARN_MSG  ((char *)GETMESSAGE(18, 12, "The workspace manager must be restarted in\n\
order for your changes to take effect.\n\
Press OK to restart the workspace manager\n\
or Cancel to reset the changes."))

/*+++++++++++++++++++++++++++++++++++++++*/
/* Internal Functions                    */
/*+++++++++++++++++++++++++++++++++++++++*/

static Widget build_dtwmDlg ( Widget shell ) ;
static void formLayoutCB ( Widget, XtPointer, XtPointer ) ;
static void _DtmapCB_dtwmDlg ( Widget, XtPointer, XtPointer ) ;
static void systemDefaultCB ( Widget, XtPointer, XtPointer ) ;
static void ButtonCB ( Widget, XtPointer, XtPointer ) ;
static void getDtwmValues (void) ;
static void _DtWmParseToLower (unsigned char  *string) ;
static void okWarnCB ( Widget, XtPointer, XtPointer ) ;
static void cancelWarnCB ( Widget, XtPointer, XtPointer ) ;
static void ReadIconImageMaximum(XrmDatabase db, const char *name,
                                 const char *class, int *width, int *height);

/*+++++++++++++++++++++++++++++++++++++++*/
/* Internal Variables                    */
/*+++++++++++++++++++++++++++++++++++++++*/

#define DTWM_UI_ANIM_NONE      0
#define DTWM_UI_ANIM_BEVEL     1
#define DTWM_UI_ANIM_TITLEBAR  2
#define DTWM_UI_ANIM_WIREFRAME 3
#define DTWM_UI_ANIM_COUNT     4

typedef struct {
    Widget      pictLabel;
    Widget      systemDefault;
    Widget      windowFocusTB;
    Widget      focusPolicyRC;
    Widget      focusSeparator;
    Widget      autoRaiseTG;
    Widget      secStackTG;
    Widget      moveOpaqueTG;
    Widget      minAnimPD;
    Widget      minAnimOM;
    Widget      minAnimBtns[DTWM_UI_ANIM_COUNT];
    Widget      maxAnimPD;
    Widget      maxAnimOM;
    Widget      maxAnimBtns[DTWM_UI_ANIM_COUNT];
    Widget      showTaskSwitcherTG;
    Widget      iconPlacementTB;
    Widget      useIconBoxRC;
    Widget      pointerTG;
    Widget      explicitTG;
    Widget      iconBoxTG;
    Widget	desktopTG;
    Widget      increaseIconSizeTG;
    Widget      showOpenIconsTG;
    Widget      iconGapLabel;
    Widget      iconGapText;
    Boolean     systemDefaultFlag;
    Boolean     waylandMode;
    int         origKeyboardFocusPolicy;
    int         origFocusAutoRaise;
    int         origSecStack;
    int         origMoveOpaque;
    int         origMinAnimStyle;
    int         origMaxAnimStyle;
    int         curMinAnimStyle;
    int         curMaxAnimStyle;
    int         origShowTaskSwitcher;
    int         origUseIconBox;
    Boolean     origIncreaseIconSize;
    Boolean     origShowOpenIcons;
    int         origIconPlacementMargin;
    int         iconImageBaseWidth;
    int         iconImageBaseHeight;
    Widget      warnDialog;
} Dtwm, *DtwmPtr;

static Dtwm dtwm;
static saveRestore save = {FALSE, 0, };

static char dtwmRes[2048]="";

static const char *
anim_style_to_token(int style_idx)
{
    switch (style_idx)
    {
        case DTWM_UI_ANIM_NONE:      return "none";
        case DTWM_UI_ANIM_BEVEL:     return "bevel";
        case DTWM_UI_ANIM_TITLEBAR:  return "titlebar";
        case DTWM_UI_ANIM_WIREFRAME: return "wireframe";
        default:                     return "auto";
    }
}

static int
DtstyleResolveWindowAnimStyle(const char *raw_style,
                              Boolean is_wayland,
                              Boolean is_maximize,
                              Boolean has_legacy_min,
                              Boolean legacy_min_enabled)
{
    char buf[32];
    size_t i = 0;

    if (raw_style != nullptr)
    {
        while (*raw_style && isspace((unsigned char)*raw_style))
            raw_style++;
        while (raw_style[i] != '\0' && !isspace((unsigned char)raw_style[i]) &&
               i + 1 < sizeof(buf))
        {
            buf[i] = (char)tolower((unsigned char)raw_style[i]);
            i++;
        }
    }
    buf[i] = '\0';

    if (strcmp(buf, "none") == 0 || strcmp(buf, "false") == 0 ||
        strcmp(buf, "off") == 0 || strcmp(buf, "0") == 0)
    {
        return DTWM_UI_ANIM_NONE;
    }
    if (strcmp(buf, "bevel") == 0 || strcmp(buf, "bevel_morph") == 0 ||
        strcmp(buf, "bevel_glide") == 0 || strcmp(buf, "bevelmorph") == 0 ||
        strcmp(buf, "bevelglide") == 0)
    {
        if (is_wayland)
            return DTWM_UI_ANIM_BEVEL;
        /* Graceful fallback when roaming a Wayland session's home directory into X11 */
        if (is_maximize)
            return DTWM_UI_ANIM_NONE;
        return (has_legacy_min && !legacy_min_enabled)
                   ? DTWM_UI_ANIM_NONE
                   : DTWM_UI_ANIM_WIREFRAME;
    }
    if (strcmp(buf, "titlebar") == 0 || strcmp(buf, "titlebar_shuttle") == 0 ||
        strcmp(buf, "titlebarshuttle") == 0)
    {
        return DTWM_UI_ANIM_TITLEBAR;
    }
    if (strcmp(buf, "wireframe") == 0)
    {
        return DTWM_UI_ANIM_WIREFRAME;
    }

    /* "auto", unset, or unrecognized: apply backend-native defaults */
    if (is_wayland)
    {
        if (!is_maximize && has_legacy_min && !legacy_min_enabled)
            return DTWM_UI_ANIM_NONE;
        return DTWM_UI_ANIM_BEVEL;
    }
    if (is_maximize)
        return DTWM_UI_ANIM_NONE;
    return (has_legacy_min && !legacy_min_enabled)
               ? DTWM_UI_ANIM_NONE
               : DTWM_UI_ANIM_WIREFRAME;
}

static void
append_dtwm_res(char *dst, size_t dst_sz, const char *fmt, ...)
{
    size_t used;
    va_list ap;

    if (!dst || dst_sz == 0 || !fmt)
        return;
    used = strnlen(dst, dst_sz);
    if (used + 1 >= dst_sz)
        return;
    va_start(ap, fmt);
    vsnprintf(dst + used, dst_sz - used, fmt, ap);
    va_end(ap);
}

static Boolean
is_wayland_dtwm_session(void)
{
    Atom wl_atom;
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *prop = nullptr;

    if (!style.display || style.root == None)
        return False;
    wl_atom = XInternAtom(style.display, "_DT_WAYLAND_OUTPUTS", True);
    if (wl_atom == None)
        return False;
    if (XGetWindowProperty(style.display, style.root, wl_atom, 0, 1, False,
                           AnyPropertyType, &actual_type, &actual_format,
                           &nitems, &bytes_after, &prop) != Success)
        return False;
    if (prop)
        XFree(prop);
    return (actual_type != None) ? True : False;
}

static void
set_widget_label_str(Widget w, const char *text)
{
    XmString xs;
    if (!w || !text)
        return;
    xs = CMPSTR((char *)text);
    XtVaSetValues(w, XmNlabelString, xs, NULL);
    XmStringFree(xs);
}

static void
set_anim_menu_selection(Boolean is_max, int style_idx)
{
    if (style_idx < 0 || style_idx >= DTWM_UI_ANIM_COUNT)
        style_idx = DTWM_UI_ANIM_NONE;
    if (!dtwm.waylandMode && style_idx == DTWM_UI_ANIM_BEVEL)
        style_idx = is_max ? DTWM_UI_ANIM_NONE : DTWM_UI_ANIM_WIREFRAME;

    if (is_max)
    {
        dtwm.curMaxAnimStyle = style_idx;
        if (dtwm.maxAnimOM && dtwm.maxAnimBtns[style_idx])
            XtVaSetValues(dtwm.maxAnimOM, XmNmenuHistory, dtwm.maxAnimBtns[style_idx], NULL);
    }
    else
    {
        dtwm.curMinAnimStyle = style_idx;
        if (dtwm.minAnimOM && dtwm.minAnimBtns[style_idx])
            XtVaSetValues(dtwm.minAnimOM, XmNmenuHistory, dtwm.minAnimBtns[style_idx], NULL);
    }
}

static void
refresh_anim_menus_for_backend(Boolean wayland_mode)
{
    const char *lbl_none = (char *)GETMESSAGE(18, 24, "None");
    const char *lbl_bevel_min = (char *)GETMESSAGE(18, 25, "Bevel Morph");
    const char *lbl_bevel_max = (char *)GETMESSAGE(18, 26, "Bevel Glide");
    const char *lbl_title = wayland_mode
                                ? (char *)GETMESSAGE(18, 27, "Titlebar Shuttle")
                                : (char *)GETMESSAGE(18, 28, "Titlebar");
    const char *lbl_wire = (char *)GETMESSAGE(18, 29, "Wireframe");

    dtwm.waylandMode = wayland_mode;

    set_widget_label_str(dtwm.minAnimBtns[DTWM_UI_ANIM_NONE], lbl_none);
    set_widget_label_str(dtwm.minAnimBtns[DTWM_UI_ANIM_BEVEL], lbl_bevel_min);
    set_widget_label_str(dtwm.minAnimBtns[DTWM_UI_ANIM_TITLEBAR], lbl_title);
    set_widget_label_str(dtwm.minAnimBtns[DTWM_UI_ANIM_WIREFRAME], lbl_wire);

    set_widget_label_str(dtwm.maxAnimBtns[DTWM_UI_ANIM_NONE], lbl_none);
    set_widget_label_str(dtwm.maxAnimBtns[DTWM_UI_ANIM_BEVEL], lbl_bevel_max);
    set_widget_label_str(dtwm.maxAnimBtns[DTWM_UI_ANIM_TITLEBAR], lbl_title);
    set_widget_label_str(dtwm.maxAnimBtns[DTWM_UI_ANIM_WIREFRAME], lbl_wire);

    if (wayland_mode)
    {
        if (dtwm.minAnimBtns[DTWM_UI_ANIM_BEVEL])
            XtManageChild(dtwm.minAnimBtns[DTWM_UI_ANIM_BEVEL]);
        if (dtwm.maxAnimBtns[DTWM_UI_ANIM_BEVEL])
            XtManageChild(dtwm.maxAnimBtns[DTWM_UI_ANIM_BEVEL]);
    }
    else
    {
        /* Ensure XmNmenuHistory never points to the Wayland-only Bevel item before unmanaging */
        if (dtwm.minAnimOM && dtwm.minAnimBtns[DTWM_UI_ANIM_WIREFRAME])
            XtVaSetValues(dtwm.minAnimOM, XmNmenuHistory, dtwm.minAnimBtns[DTWM_UI_ANIM_WIREFRAME], NULL);
        if (dtwm.maxAnimOM && dtwm.maxAnimBtns[DTWM_UI_ANIM_NONE])
            XtVaSetValues(dtwm.maxAnimOM, XmNmenuHistory, dtwm.maxAnimBtns[DTWM_UI_ANIM_NONE], NULL);

        if (dtwm.minAnimBtns[DTWM_UI_ANIM_BEVEL])
            XtUnmanageChild(dtwm.minAnimBtns[DTWM_UI_ANIM_BEVEL]);
        if (dtwm.maxAnimBtns[DTWM_UI_ANIM_BEVEL])
            XtUnmanageChild(dtwm.maxAnimBtns[DTWM_UI_ANIM_BEVEL]);
    }
}

static void
minAnimSelectCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    dtwm.curMinAnimStyle = (int)(intptr_t)client_data;
}

static void
maxAnimSelectCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    dtwm.curMaxAnimStyle = (int)(intptr_t)client_data;
}

static void
commit_orig_dtwm_values(void)
{
    char *gapStr = XmTextFieldGetString(dtwm.iconGapText);
    dtwm.origKeyboardFocusPolicy = XmToggleButtonGadgetGetState(dtwm.explicitTG);
    dtwm.origFocusAutoRaise = XmToggleButtonGadgetGetState(dtwm.autoRaiseTG);
    dtwm.origSecStack = !XmToggleButtonGadgetGetState(dtwm.secStackTG);
    dtwm.origMoveOpaque = XmToggleButtonGadgetGetState(dtwm.moveOpaqueTG);
    dtwm.origMinAnimStyle = dtwm.curMinAnimStyle;
    dtwm.origMaxAnimStyle = dtwm.curMaxAnimStyle;
    dtwm.origShowTaskSwitcher = XmToggleButtonGadgetGetState(dtwm.showTaskSwitcherTG);
    dtwm.origUseIconBox = XmToggleButtonGadgetGetState(dtwm.iconBoxTG);
    dtwm.origIncreaseIconSize = XmToggleButtonGadgetGetState(dtwm.increaseIconSizeTG);
    dtwm.origShowOpenIcons = XmToggleButtonGadgetGetState(dtwm.showOpenIconsTG);
    dtwm.origIconPlacementMargin = atoi(gapStr ? gapStr : "0");
    if (gapStr)
        XtFree(gapStr);
}

static char *icon[] = {
    "iconTL",
    "iconTR",
    "iconBL",
    "iconBR",
    "iconLT",
    "iconLB",
    "iconRT",
    "iconRB"
};

static char *placementStr[] = {
    "top left",
    "top right",
    "bottom left",
    "bottom right",
    "left top",
    "left bottom",
    "right top",
    "right bottom"
};

/*+++++++++++++++++++++++++++++++++++++++*/
/* popup_dtwmBB                         */
/*+++++++++++++++++++++++++++++++++++++++*/

void 
popup_dtwmBB(
        Widget shell )
{
    if (style.dtwmDialog == nullptr) {
      _DtTurnOnHourGlass(shell);  
      build_dtwmDlg(shell);
      getDtwmValues();
      XtManageChild(style.dtwmDialog);
      _DtTurnOffHourGlass(shell);  
    }
    else 
    {
        getDtwmValues();
        XtManageChild(style.dtwmDialog);
        raiseWindow(XtWindow(XtParent(style.dtwmDialog)));
    }
    dtwm.systemDefaultFlag = False;
}

/*+++++++++++++++++++++++++++++++++++++++*/
/* getDtwmValues                        */
/*+++++++++++++++++++++++++++++++++++++++*/

static void
ReadIconImageMaximum(XrmDatabase db, const char *name,
                     const char *class, int *width, int *height)
{
    char *type_return;
    XrmValue value_return;

    if (!db || !name || !class || !width || !height)
        return;

    if (!XrmGetResource(db, name, class, &type_return, &value_return))
        return;

    if (!value_return.addr)
        return;

    if (value_return.size >= (int)(sizeof(int) * 2))
    {
        int *ints = (int *) value_return.addr;
        *width = ints[0];
        *height = ints[1];
        return;
    }

    char buffer[64];
    int len = (int) value_return.size;
    if (len >= (int) sizeof(buffer))
        len = sizeof(buffer) - 1;
    memcpy(buffer, value_return.addr, len);
    buffer[len] = '\0';

    sscanf(buffer, "%dx%d", width, height);
}

static void
merge_dtwm_res_into_xt_db(const char *res_text)
{
    XrmDatabase patch_db;
    XrmDatabase xt_db;

    if (!style.display || !res_text || !res_text[0])
        return;

    patch_db = XrmGetStringDatabase((char *)res_text);
    if (!patch_db)
        return;

    xt_db = XtDatabase(style.display);
    if (xt_db)
        XrmCombineDatabase(patch_db, &xt_db, True);
    else
        XrmDestroyDatabase(patch_db);
}

static XrmDatabase
open_live_resource_manager_db(void)
{
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *prop = nullptr;
    XrmDatabase live_db = nullptr;

    if (!style.display || style.root == None)
        return nullptr;

    if (XGetWindowProperty(style.display, style.root, XA_RESOURCE_MANAGER,
                           0L, 100000L, False, XA_STRING,
                           &actual_type, &actual_format,
                           &nitems, &bytes_after, &prop) == Success &&
        prop != nullptr)
    {
        if (actual_type == XA_STRING && actual_format == 8 && nitems > 0)
            live_db = XrmGetStringDatabase((char *)prop);
        XFree(prop);
    }
    return live_db;
}

static Boolean
query_dtwm_res(XrmDatabase live_db, XrmDatabase fallback_db,
               const char *res_name, const char *res_class,
               XrmValue *value_out)
{
    char full_name_cap[128];
    char full_name_low[128];
    char full_class[128];
    char *type_ret = nullptr;

    snprintf(full_name_cap, sizeof(full_name_cap), "Dtwm.%s", res_name);
    snprintf(full_name_low, sizeof(full_name_low), "dtwm.%s", res_name);
    snprintf(full_class, sizeof(full_class), "Dtwm.%s", res_class);

    if (live_db)
    {
        if (XrmGetResource(live_db, full_name_cap, full_class, &type_ret, value_out) &&
            value_out->addr != nullptr)
            return True;
        if (XrmGetResource(live_db, full_name_low, full_class, &type_ret, value_out) &&
            value_out->addr != nullptr)
            return True;
    }
    if (fallback_db)
    {
        if (XrmGetResource(fallback_db, full_name_cap, full_class, &type_ret, value_out) &&
            value_out->addr != nullptr)
            return True;
        if (XrmGetResource(fallback_db, full_name_low, full_class, &type_ret, value_out) &&
            value_out->addr != nullptr)
            return True;
    }
    return False;
}

static Boolean
query_dtwm_string_lower(XrmDatabase live_db, XrmDatabase fallback_db,
                        const char *res_name, const char *res_class,
                        char *buf, size_t buf_sz)
{
    XrmValue val;
    size_t len;

    if (!buf || buf_sz == 0)
        return False;
    buf[0] = '\0';
    if (!query_dtwm_res(live_db, fallback_db, res_name, res_class, &val) || !val.addr)
        return False;

    len = strnlen((const char *)val.addr, val.size > 0 ? (size_t)val.size : buf_sz - 1);
    if (len >= buf_sz)
        len = buf_sz - 1;
    memcpy(buf, val.addr, len);
    buf[len] = '\0';
    _DtWmParseToLower((unsigned char *)buf);
    /* Trim trailing whitespace */
    while (len > 0 && isspace((unsigned char)buf[len - 1]))
        buf[--len] = '\0';
    return True;
}

static void 
getDtwmValues(void)
{
    XrmValue value_return;
    XrmDatabase db;
    XrmDatabase live_db;
    char sbuf[128];

    db = XtDatabase(style.display);
    live_db = open_live_resource_manager_db();

    /* Get KeyboardFocusPolicy value */
    if (query_dtwm_string_lower(live_db, db, "keyboardFocusPolicy",
                                "KeyboardFocusPolicy", sbuf, sizeof(sbuf)) &&
        strcmp(sbuf, "pointer") == 0)
    {
        dtwm.origKeyboardFocusPolicy = POINTER;
        XmToggleButtonGadgetSetState(dtwm.pointerTG, True, False);
        XmToggleButtonGadgetSetState(dtwm.explicitTG, False, False);
    }
    else
    {
        dtwm.origKeyboardFocusPolicy = EXPLICIT;
        XmToggleButtonGadgetSetState(dtwm.pointerTG, False, False);
        XmToggleButtonGadgetSetState(dtwm.explicitTG, True, False);
    }

    /* Get FocusAutoRaise value */
    if (query_dtwm_string_lower(live_db, db, "focusAutoRaise",
                                "FocusAutoRaise", sbuf, sizeof(sbuf)))
    {
        dtwm.origFocusAutoRaise = (strcmp(sbuf, "true") == 0) ? True : False;
    }
    else
    {
        dtwm.origFocusAutoRaise =
            (dtwm.origKeyboardFocusPolicy == EXPLICIT) ? True : False;
    }
    XmToggleButtonGadgetSetState(dtwm.autoRaiseTG, dtwm.origFocusAutoRaise, False);

    /* Get SecStack value from secondariesOnTop resource */
    if (query_dtwm_string_lower(live_db, db, "secondariesOnTop",
                                "SecondariesOnTop", sbuf, sizeof(sbuf)) ||
        query_dtwm_string_lower(live_db, db, "secondariesOnTop",
                                "secondariesOnTop", sbuf, sizeof(sbuf)))
    {
        dtwm.origSecStack = (strcmp(sbuf, "true") == 0) ? True : False;
    }
    else
    {
#ifdef sun
        dtwm.origSecStack = False;
#else
        dtwm.origSecStack = True;
#endif
    }
    XmToggleButtonGadgetSetState(dtwm.secStackTG, !dtwm.origSecStack, False);

    /* Get UseIconBox value */
    if (query_dtwm_string_lower(live_db, db, "useIconBox",
                                "UseIconBox", sbuf, sizeof(sbuf)))
    {
        dtwm.origUseIconBox = (strcmp(sbuf, "true") == 0) ? True : False;
    }
    else
    {
        dtwm.origUseIconBox = False;
    }
    XmToggleButtonGadgetSetState(dtwm.iconBoxTG, dtwm.origUseIconBox, False);
    XmToggleButtonGadgetSetState(dtwm.desktopTG, !dtwm.origUseIconBox, False);

    /* Get MoveOpaque value */
    if (query_dtwm_string_lower(live_db, db, "moveOpaque",
                                "MoveOpaque", sbuf, sizeof(sbuf)))
    {
        dtwm.origMoveOpaque = (strcmp(sbuf, "true") == 0) ? True : False;
    }
    else
    {
        /* Wayland compositor defaults to smooth opaque moves; X11 defaults to wireframe */
        dtwm.origMoveOpaque = is_wayland_dtwm_session() ? True : False;
    }
    XmToggleButtonGadgetSetState(dtwm.moveOpaqueTG, dtwm.origMoveOpaque, False);

    /* Get Backend-Aware Minimize & Maximize Animation Styles */
    {
        Boolean wayland_mode = is_wayland_dtwm_session();
        Boolean has_legacy_min = False;
        Boolean legacy_min_enabled = True;
        char min_style_buf[64] = "";
        char max_style_buf[64] = "";

        refresh_anim_menus_for_backend(wayland_mode);

        if (query_dtwm_string_lower(live_db, db, "animateMinimizeRestore",
                                    "AnimateMinimizeRestore", sbuf, sizeof(sbuf)))
        {
            has_legacy_min = True;
            legacy_min_enabled = (strcmp(sbuf, "true") == 0) ? True : False;
        }
        query_dtwm_string_lower(live_db, db, "minimizeAnimation",
                                "MinimizeAnimation", min_style_buf, sizeof(min_style_buf));
        query_dtwm_string_lower(live_db, db, "maximizeAnimation",
                                "MaximizeAnimation", max_style_buf, sizeof(max_style_buf));

        dtwm.origMinAnimStyle = DtstyleResolveWindowAnimStyle(
            min_style_buf, wayland_mode, False, has_legacy_min, legacy_min_enabled);
        dtwm.origMaxAnimStyle = DtstyleResolveWindowAnimStyle(
            max_style_buf, wayland_mode, True, has_legacy_min, legacy_min_enabled);

        set_anim_menu_selection(False, dtwm.origMinAnimStyle);
        set_anim_menu_selection(True, dtwm.origMaxAnimStyle);
    }

    /* Get ShowTaskSwitcher value */
    if (query_dtwm_string_lower(live_db, db, "showTaskSwitcher",
                                "ShowTaskSwitcher", sbuf, sizeof(sbuf)))
    {
        dtwm.origShowTaskSwitcher = (strcmp(sbuf, "true") == 0) ? True : False;
    }
    else
    {
        dtwm.origShowTaskSwitcher = True;
    }
    XmToggleButtonGadgetSetState(dtwm.showTaskSwitcherTG, dtwm.origShowTaskSwitcher, False);

    /* Determine icon image maximum and double state */
    {
        int iconWidth;
        int iconHeight;
        int resolution = _DtGetDisplayResolution(style.display, style.screenNum);
        int defaultSize =
            ((resolution == LOW_RES_DISPLAY) || (resolution == VGA_RES_DISPLAY)) ? 32 : 48;

        iconWidth = defaultSize;
        iconHeight = defaultSize;
        ReadIconImageMaximum(db, "dtwm.iconImageMaximum",
                             "Dtwm.IconImageMaximum",
                             &iconWidth, &iconHeight);
        if (live_db)
        {
            ReadIconImageMaximum(live_db, "Dtwm.iconImageMaximum",
                                 "Dtwm.IconImageMaximum",
                                 &iconWidth, &iconHeight);
            ReadIconImageMaximum(live_db, "dtwm.iconImageMaximum",
                                 "Dtwm.IconImageMaximum",
                                 &iconWidth, &iconHeight);
        }

        if (iconWidth <= 0)
            iconWidth = defaultSize;
        if (iconHeight <= 0)
            iconHeight = defaultSize;

        dtwm.iconImageBaseWidth = iconWidth;
        dtwm.iconImageBaseHeight = iconHeight;

        dtwm.origIncreaseIconSize = False;
        if (query_dtwm_string_lower(live_db, db, "doubleIconSize",
                                    "DoubleIconSize", sbuf, sizeof(sbuf)))
        {
            dtwm.origIncreaseIconSize = (strcmp(sbuf, "true") == 0);
        }

        if (dtwm.origIncreaseIconSize)
        {
            if (dtwm.iconImageBaseWidth > 1)
                dtwm.iconImageBaseWidth = (dtwm.iconImageBaseWidth + 1) / 2;
            if (dtwm.iconImageBaseHeight > 1)
                dtwm.iconImageBaseHeight = (dtwm.iconImageBaseHeight + 1) / 2;
        }

        XmToggleButtonGadgetSetState(dtwm.increaseIconSizeTG,
                                     dtwm.origIncreaseIconSize, False);
    }

    /* Get ShowIconsForOpenWindows value */
    {
        dtwm.origShowOpenIcons = True;
        if (query_dtwm_string_lower(live_db, db, "showOpenWindowIcons",
                                    "ShowOpenWindowIcons", sbuf, sizeof(sbuf)))
        {
            dtwm.origShowOpenIcons = (strcmp(sbuf, "true") == 0) ? True : False;
        }

        XmToggleButtonGadgetSetState(dtwm.showOpenIconsTG,
                                     dtwm.origShowOpenIcons, False);
    }

    /* Get IconPlacementMargin value */
    {
        dtwm.origIconPlacementMargin = -1;
        if (query_dtwm_res(live_db, db, "iconPlacementMargin",
                           "IconPlacementMargin", &value_return) &&
            value_return.addr != nullptr)
        {
            dtwm.origIconPlacementMargin = atoi((char *)value_return.addr);
        }

        {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", dtwm.origIconPlacementMargin);
            XmTextFieldSetString(dtwm.iconGapText, buf);
        }
    }

    if (live_db)
        XrmDestroyDatabase(live_db);
}

/*+++++++++++++++++++++++++++++++++++++++*/
/* build__dtwmDlg                        */
/*+++++++++++++++++++++++++++++++++++++++*/
static Widget 
build_dtwmDlg(
        Widget shell )
{
    int     i, n;
    Arg              args[MAX_ARGS];
    XmString         button_string[NUM_LABELS]; 
    XmString         string; 
    Widget           form;
    Widget           windowFocusForm;
    Widget           iconPlacementForm;
    int              count = 0;
    Widget           widgetList1[6];

    /* get dtwm resource values  */

    /* Set up DialogBoxDialog button labels */
    button_string[0] = CMPSTR((String) _DtOkString);
    button_string[1] = CMPSTR((String) _DtCancelString);
    button_string[2] = CMPSTR((String) _DtHelpString);

    /* Create toplevel DialogBox */
    /* saveRestore
     * Note that save.poscnt has been initialized elsewhere.  
     * save.posArgs may contain information from restoreBeep().*/

    XtSetArg(save.posArgs[save.poscnt], XmNbuttonCount, NUM_LABELS);  save.poscnt++;
    XtSetArg(save.posArgs[save.poscnt], XmNbuttonLabelStrings, button_string);  save.poscnt++;
    XtSetArg(save.posArgs[save.poscnt], XmNdefaultPosition, False);
    save.poscnt++;
    style.dtwmDialog = 
        __DtCreateDialogBoxDialog(shell, "dtwmDialog", save.posArgs, save.poscnt);
    XtAddCallback(style.dtwmDialog, XmNhelpCallback,
            HelpRequestCB, (XtPointer)HELP_DTWM_DIALOG);

    XmStringFree(button_string[0]);
    XmStringFree(button_string[1]);
    XmStringFree(button_string[2]);

    widgetList1[0] = _DtDialogBoxGetButton(style.dtwmDialog,2);
    n=0;
    XtSetArg(args[n], XmNautoUnmanage, False); n++;
    XtSetArg(args[n], XmNcancelButton, widgetList1[0]); n++;
    XtSetValues (style.dtwmDialog, args, n);

    n=0;
    XtSetArg(args[n], XmNtitle, ((char *)GETMESSAGE(18, 1, "Style Manager - Window"))); n++;
    XtSetArg (args[n], XmNuseAsyncGeometry, True); n++;
    XtSetArg(args[n], XmNmwmFunctions, DIALOG_MWM_FUNC); n++;
    XtSetValues (XtParent(style.dtwmDialog), args, n);

    n = 0;
    XtSetArg(args[n], XmNallowOverlap, False); n++;
    XtSetArg(args[n], XmNchildType, XmWORK_AREA);  n++;
    form = XmCreateForm(style.dtwmDialog, "dtwmForm", args, n);

    n = 0;
    XtSetArg(args[n], XmNfillMode, XmFILL_SELF); n++;
    XtSetArg(args[n], XmNbehavior, XmICON_LABEL); n++;
    XtSetArg(args[n], XmNpixmapForeground, style.secBSCol); n++;
    XtSetArg(args[n], XmNpixmapBackground, style.secTSCol); n++;
    XtSetArg(args[n], XmNstring, NULL); n++;  
    XtSetArg(args[n], XmNshadowThickness, 0); n++;  
    XtSetArg(args[n], XmNimageName, DTWM_ICON); n++;  
    XtSetArg(args[n], XmNtraversalOn, False); n++;  
    widgetList1[count++] = 
    dtwm.pictLabel = _DtCreateIcon(form, "dtwmpictLabel", args, n);

    n = 0;
    XtSetArg(args[n], XmNmarginHeight, LB_MARGIN_HEIGHT);  n++;
    XtSetArg(args[n], XmNmarginWidth, LB_MARGIN_WIDTH);  n++;
    string = CMPSTR(((char *)GETMESSAGE(18, 2, "Default")));
    XtSetArg(args[n], XmNlabelString, string); n++;
    widgetList1[count++] = dtwm.systemDefault = 
        XmCreatePushButtonGadget(form, "systemDefault", args, n);
    XmStringFree(string);

    n = 0;
    string = CMPSTR((char *)GETMESSAGE(18, 3, "Window Behavior"));
    XtSetArg(args[n], XmNtitleString, string);  n++;
    widgetList1[count++] = dtwm.windowFocusTB
        = _DtCreateTitleBox(form, "windowFocusTB", args, n);
    XmStringFree(string);
    
    n = 0;
    XtSetArg(args[n], XmNallowOverlap, False); n++;
    XtSetArg(args[n], XmNchildType, XmWORK_AREA);  n++;
    windowFocusForm = 
        XmCreateForm(dtwm.windowFocusTB, "windowFocusForm", args, n);

    n = 0;
    string = CMPSTR((char *)GETMESSAGE(18, 4, "Window Icons"));
    XtSetArg(args[n], XmNtitleString, string);  n++;
    widgetList1[count++] = dtwm.iconPlacementTB
        = _DtCreateTitleBox(form, "iconPlacementTB", args, n);
    XmStringFree(string);

    n = 0;
    XtSetArg(args[n], XmNallowOverlap, False); n++;
    XtSetArg(args[n], XmNchildType, XmWORK_AREA);  n++;
    iconPlacementForm = 
        XmCreateForm(dtwm.iconPlacementTB, "iconPlacementForm", args, n);

    n = 0;
    XtSetArg(args[n], XmNmarginWidth, 0); n++;
    XtSetArg(args[n], XmNmarginHeight, 0); n++;
    dtwm.focusPolicyRC = 
        XmCreateRadioBox(windowFocusForm, "focusPolicyRC", args, n);

    n = 0;
    string = CMPSTR((char *)GETMESSAGE(18, 13, "Point In Window To Make Active"));
    XtSetArg(args[n], XmNlabelString, string); n++;
    dtwm.pointerTG = 
        XmCreateToggleButtonGadget(dtwm.focusPolicyRC, "pointerTG", args, n);
    XmStringFree(string);

    n = 0;
    string = CMPSTR((char *)GETMESSAGE(18, 14, "Click In Window To Make Active"));
    XtSetArg(args[n], XmNlabelString, string); n++;
    dtwm.explicitTG = 
        XmCreateToggleButtonGadget(dtwm.focusPolicyRC, "explicitTG", args, n);
    XmStringFree(string);

    n = 0;
    XtSetArg(args[n], XmNmargin, 0); n++;
    dtwm.focusSeparator = 
        XmCreateSeparatorGadget(windowFocusForm, "focusSeparator", args, n);

    n = 0;
    string = CMPSTR((char *)GETMESSAGE(18, 15, "Raise Window When Made Active"));
    XtSetArg(args[n], XmNnavigationType, XmTAB_GROUP); n++;  
    XtSetArg(args[n], XmNlabelString, string);  n++;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING);  n++;
    dtwm.autoRaiseTG = 
        XmCreateToggleButtonGadget(windowFocusForm, "autoRaiseTG", args, n);
    XmStringFree(string);


    n = 0;
    string = CMPSTR((char *)GETMESSAGE(18, 11, "Allow Primary Windows On Top"));
    XtSetArg(args[n], XmNnavigationType, XmTAB_GROUP); n++;  
    XtSetArg(args[n], XmNlabelString, string);  n++;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING);  n++;
    dtwm.secStackTG = 
        XmCreateToggleButtonGadget(windowFocusForm, "secStackTG", args, n);
    XmStringFree(string);

    n = 0;
    string = CMPSTR((char *)GETMESSAGE(18, 16, "Show Contents During Move"));
    XtSetArg(args[n], XmNnavigationType, XmTAB_GROUP); n++;  
    XtSetArg(args[n], XmNlabelString, string);  n++;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING);  n++;
    dtwm.moveOpaqueTG = 
        XmCreateToggleButtonGadget(windowFocusForm, "moveOpaqueTG", args, n);
    XmStringFree(string);

    dtwm.minAnimPD = XmCreatePulldownMenu(windowFocusForm, "minAnimPD", nullptr, 0);
    for (i = 0; i < DTWM_UI_ANIM_COUNT; i++)
    {
        string = CMPSTR("Wireframe");
        dtwm.minAnimBtns[i] = XtVaCreateManagedWidget(
            "minAnimItem", xmPushButtonGadgetClass, dtwm.minAnimPD,
            XmNlabelString, string,
            NULL);
        XmStringFree(string);
        XtAddCallback(dtwm.minAnimBtns[i], XmNactivateCallback,
                      minAnimSelectCB, (XtPointer)(intptr_t)i);
    }
    n = 0;
    string = CMPSTR((char *)GETMESSAGE(18, 22, "Minimize / Restore Animation:"));
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNsubMenuId, dtwm.minAnimPD); n++;
    XtSetArg(args[n], XmNnavigationType, XmTAB_GROUP); n++;
    XtSetArg(args[n], XmNmarginWidth, 0); n++;
    XtSetArg(args[n], XmNmarginHeight, 0); n++;
    dtwm.minAnimOM = XmCreateOptionMenu(windowFocusForm, "minAnimOM", args, n);
    XmStringFree(string);

    dtwm.maxAnimPD = XmCreatePulldownMenu(windowFocusForm, "maxAnimPD", nullptr, 0);
    for (i = 0; i < DTWM_UI_ANIM_COUNT; i++)
    {
        string = CMPSTR("Wireframe");
        dtwm.maxAnimBtns[i] = XtVaCreateManagedWidget(
            "maxAnimItem", xmPushButtonGadgetClass, dtwm.maxAnimPD,
            XmNlabelString, string,
            NULL);
        XmStringFree(string);
        XtAddCallback(dtwm.maxAnimBtns[i], XmNactivateCallback,
                      maxAnimSelectCB, (XtPointer)(intptr_t)i);
    }
    n = 0;
    string = CMPSTR((char *)GETMESSAGE(18, 23, "Maximize / Restore Animation:"));
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNsubMenuId, dtwm.maxAnimPD); n++;
    XtSetArg(args[n], XmNnavigationType, XmTAB_GROUP); n++;
    XtSetArg(args[n], XmNmarginWidth, 0); n++;
    XtSetArg(args[n], XmNmarginHeight, 0); n++;
    dtwm.maxAnimOM = XmCreateOptionMenu(windowFocusForm, "maxAnimOM", args, n);
    XmStringFree(string);

    n = 0;
    string = CMPSTR((char *)GETMESSAGE(18, 21, "Show Task Switcher on Alt+Tab"));
    XtSetArg(args[n], XmNnavigationType, XmTAB_GROUP); n++;
    XtSetArg(args[n], XmNlabelString, string);  n++;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING);  n++;
    dtwm.showTaskSwitcherTG =
        XmCreateToggleButtonGadget(windowFocusForm, "showTaskSwitcherTG", args, n);
    XmStringFree(string);

    n = 0;
    XtSetArg(args[n], XmNmarginWidth, 0); n++;
    XtSetArg(args[n], XmNmarginHeight, 0); n++;
    dtwm.useIconBoxRC = 
        XmCreateRadioBox(iconPlacementForm, "useIconBoxRC", args, n);

    n = 0;
    string = CMPSTR((char *)GETMESSAGE(18, 9, "Use Icon Box"));
    XtSetArg(args[n], XmNlabelString, string); n++;
    dtwm.iconBoxTG = 
        XmCreateToggleButtonGadget(dtwm.useIconBoxRC, "iconBoxTG", args, n);
    XmStringFree(string);

    n = 0;
    string = CMPSTR((char *)GETMESSAGE(18, 10, "Place On Workspace"));
    XtSetArg(args[n], XmNlabelString, string); n++;
    dtwm.desktopTG = 
        XmCreateToggleButtonGadget(dtwm.useIconBoxRC, "desktopTG", args, n);
    XmStringFree(string);

    n = 0;
    string = CMPSTR((char *)GETMESSAGE(18, 17, "Increase Icon Size"));
    XtSetArg(args[n], XmNnavigationType, XmTAB_GROUP); n++;
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING); n++;
    dtwm.increaseIconSizeTG =
        XmCreateToggleButtonGadget(iconPlacementForm, "increaseIconSizeTG", args, n);
    XmStringFree(string);

    n = 0;
    string = CMPSTR((char *)GETMESSAGE(18, 18, "Show Icons for Open Windows"));
    XtSetArg(args[n], XmNnavigationType, XmTAB_GROUP); n++;
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING); n++;
    dtwm.showOpenIconsTG =
        XmCreateToggleButtonGadget(iconPlacementForm, "showOpenIconsTG", args, n);
    XmStringFree(string);

    n = 0;
    XtSetArg(args[n], XmNorientation, XmHORIZONTAL); n++;
    XtSetArg(args[n], XmNpacking, XmPACK_TIGHT); n++;
    XtSetArg(args[n], XmNspacing, style.horizontalSpacing); n++;
    dtwm.iconGapLabel = XmCreateRowColumn(iconPlacementForm, "iconGapRC", args, n);

    string = CMPSTR((char *)GETMESSAGE(18, 19, "Icon Grid Margin"));
    n = 0;
    XtSetArg(args[n], XmNlabelString, string); n++;
    {
        Widget iconGapLabelText =
            XmCreateLabelGadget(dtwm.iconGapLabel, "iconGapLabel", args, n);
        XtManageChild(iconGapLabelText);
    }
    XmStringFree(string);

    n = 0;
    XtSetArg(args[n], XmNcolumns, 4); n++;
    dtwm.iconGapText = XmCreateTextField(dtwm.iconGapLabel, "iconGapText", args, n);
    XtManageChild(dtwm.iconGapText);
    XtManageChild(dtwm.iconGapLabel);

    XtAddCallback(style.dtwmDialog, XmNmapCallback, _DtmapCB_dtwmDlg, shell);
    XtAddCallback(style.dtwmDialog, XmNcallback, ButtonCB, nullptr);
    XtAddCallback(dtwm.systemDefault, XmNactivateCallback, systemDefaultCB, nullptr);

    formLayoutCB(style.dtwmDialog, nullptr, nullptr);

    XtManageChild(dtwm.focusPolicyRC);
    XtManageChild(dtwm.pointerTG);
    XtManageChild(dtwm.explicitTG);

    XtManageChild(dtwm.focusSeparator);
    XtManageChild(dtwm.autoRaiseTG);
    XtManageChild(dtwm.secStackTG);
    XtManageChild(dtwm.moveOpaqueTG);
    XtManageChild(dtwm.minAnimOM);
    XtManageChild(dtwm.maxAnimOM);
    XtManageChild(dtwm.showTaskSwitcherTG);

    XtManageChild(dtwm.useIconBoxRC);
    XtManageChild(dtwm.iconBoxTG);
    XtManageChild(dtwm.desktopTG);
    XtManageChild(dtwm.increaseIconSizeTG);
    XtManageChild(dtwm.showOpenIconsTG);
    XtManageChild(dtwm.iconGapLabel);

    XtManageChild(windowFocusForm);
    XtManageChild(iconPlacementForm);
    XtManageChildren(widgetList1,count); 
    XtManageChild(form);

    return(style.dtwmDialog);
}


/*+++++++++++++++++++++++++++++++++++++++*/
/* formLayoutCB                          */
/*+++++++++++++++++++++++++++++++++++++++*/
static void 
formLayoutCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
    int              n;
    Arg              args[MAX_ARGS];

    /* Picture Label */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,      XmATTACH_FORM);       n++;
    XtSetArg(args[n], XmNtopOffset,          style.verticalSpacing);    n++;
    XtSetArg(args[n], XmNbottomAttachment,   XmATTACH_NONE);       n++;
    XtSetArg(args[n], XmNleftAttachment,     XmATTACH_FORM);       n++;
    XtSetArg(args[n], XmNleftOffset,         style.horizontalSpacing);  n++;
    XtSetArg(args[n], XmNrightAttachment,    XmATTACH_NONE);       n++;
    XtSetValues (dtwm.pictLabel, args, n);

    /* system Default */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,      XmATTACH_FORM);       n++;
    XtSetArg(args[n], XmNtopOffset,          style.verticalSpacing);    n++;
    XtSetArg(args[n], XmNbottomAttachment,   XmATTACH_NONE);       n++;
    XtSetArg(args[n], XmNleftAttachment,     XmATTACH_NONE);       n++;
    XtSetArg(args[n], XmNrightAttachment,    XmATTACH_FORM);       n++;
    XtSetArg(args[n], XmNrightOffset,        style.horizontalSpacing);  n++;
    XtSetValues (dtwm.systemDefault, args, n);

    /* Window Focus TitleBox */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,      XmATTACH_WIDGET);     n++;
    XtSetArg(args[n], XmNtopWidget,          dtwm.pictLabel);     n++;
    XtSetArg(args[n], XmNtopOffset,          style.horizontalSpacing);  n++;
    XtSetArg(args[n], XmNbottomAttachment,   XmATTACH_NONE);       n++;
    XtSetArg(args[n], XmNleftAttachment,     XmATTACH_FORM);       n++;
    XtSetArg(args[n], XmNleftOffset,         style.horizontalSpacing);  n++;
    XtSetArg(args[n], XmNrightAttachment,    XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightOffset,        style.horizontalSpacing);  n++;
    XtSetValues (dtwm.windowFocusTB, args, n);

    /* Icon Placement TitleBox */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,      XmATTACH_WIDGET);     n++;
    XtSetArg(args[n], XmNtopWidget,          dtwm.windowFocusTB); n++;
    XtSetArg(args[n], XmNtopOffset,          style.verticalSpacing);    n++;
    XtSetArg(args[n], XmNbottomAttachment,   XmATTACH_FORM);       n++;
    XtSetArg(args[n], XmNbottomOffset,       style.verticalSpacing);    n++;
    XtSetArg(args[n], XmNleftAttachment,     XmATTACH_FORM);       n++;
    XtSetArg(args[n], XmNleftOffset,         style.horizontalSpacing);  n++;
    XtSetArg(args[n], XmNrightAttachment,    XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightOffset,        style.horizontalSpacing);  n++;
    XtSetValues (dtwm.iconPlacementTB, args, n);

    /* FocusPolicy RC */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,      XmATTACH_FORM);       n++;
    XtSetArg(args[n], XmNtopOffset,          style.verticalSpacing);    n++;
    XtSetArg(args[n], XmNbottomAttachment,   XmATTACH_NONE);       n++;
    XtSetArg(args[n], XmNleftAttachment,     XmATTACH_FORM);       n++;
    XtSetArg(args[n], XmNleftOffset,         0);  n++;
    XtSetArg(args[n], XmNrightAttachment,    XmATTACH_FORM);       n++;
    XtSetArg(args[n], XmNrightOffset,        0);  n++;
    XtSetValues (dtwm.focusPolicyRC, args, n);

    /* FocusSeparator */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,      XmATTACH_WIDGET);       n++;
    XtSetArg(args[n], XmNtopWidget,          dtwm.focusPolicyRC);   n++;
    XtSetArg(args[n], XmNtopOffset,          style.verticalSpacing); n++;
    XtSetArg(args[n], XmNbottomAttachment,   XmATTACH_NONE);         n++;
    XtSetArg(args[n], XmNleftAttachment,     XmATTACH_FORM);         n++;
    XtSetArg(args[n], XmNleftOffset,         0);  n++;
    XtSetArg(args[n], XmNrightAttachment,    XmATTACH_FORM);         n++;
    XtSetArg(args[n], XmNrightOffset,        0);  n++;
    XtSetValues (dtwm.focusSeparator, args, n);

    /* AutoRaise TG */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,      XmATTACH_WIDGET);       n++;
    XtSetArg(args[n], XmNtopWidget,          dtwm.focusSeparator);  n++;
    XtSetArg(args[n], XmNtopOffset,          style.verticalSpacing); n++;
    XtSetArg(args[n], XmNbottomAttachment,   XmATTACH_NONE);         n++;
    XtSetArg(args[n], XmNleftAttachment,     XmATTACH_FORM);         n++;
    XtSetArg(args[n], XmNleftOffset,         0);  n++;
    XtSetArg(args[n], XmNrightAttachment,    XmATTACH_FORM);         n++;
    XtSetArg(args[n], XmNrightOffset,        0);  n++;
    XtSetValues (dtwm.autoRaiseTG, args, n);

    /* secStack TG */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,      XmATTACH_WIDGET);       n++;
    XtSetArg(args[n], XmNtopWidget,          dtwm.autoRaiseTG);     n++;
    XtSetArg(args[n], XmNtopOffset,          style.verticalSpacing-3); n++;
    XtSetArg(args[n], XmNbottomAttachment,   XmATTACH_NONE);         n++;
    XtSetArg(args[n], XmNbottomOffset,       style.verticalSpacing); n++;
    XtSetArg(args[n], XmNleftAttachment,     XmATTACH_FORM);         n++;
    XtSetArg(args[n], XmNleftOffset,         0);  n++;
    XtSetArg(args[n], XmNrightAttachment,    XmATTACH_FORM);         n++;
    XtSetArg(args[n], XmNrightOffset,        0);  n++;
    XtSetValues (dtwm.secStackTG, args, n);


    /* MoveOpaque TG */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,      XmATTACH_WIDGET);       n++;
    XtSetArg(args[n], XmNtopWidget,          dtwm.secStackTG);     n++;
    XtSetArg(args[n], XmNtopOffset,          style.verticalSpacing-3); n++;
    XtSetArg(args[n], XmNbottomAttachment,   XmATTACH_NONE);         n++;
    XtSetArg(args[n], XmNbottomOffset,       style.verticalSpacing); n++;
    XtSetArg(args[n], XmNleftAttachment,     XmATTACH_FORM);         n++;
    XtSetArg(args[n], XmNleftOffset,         0);  n++;
    XtSetArg(args[n], XmNrightAttachment,    XmATTACH_FORM);         n++;
    XtSetArg(args[n], XmNrightOffset,        0);  n++;
    XtSetValues (dtwm.moveOpaqueTG, args, n);

    /* Minimize / Restore Animation OM */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,      XmATTACH_WIDGET);       n++;
    XtSetArg(args[n], XmNtopWidget,          dtwm.moveOpaqueTG);     n++;
    XtSetArg(args[n], XmNtopOffset,          style.verticalSpacing-2); n++;
    XtSetArg(args[n], XmNbottomAttachment,   XmATTACH_NONE);         n++;
    XtSetArg(args[n], XmNleftAttachment,     XmATTACH_FORM);         n++;
    XtSetArg(args[n], XmNleftOffset,         0);  n++;
    XtSetArg(args[n], XmNrightAttachment,    XmATTACH_FORM);         n++;
    XtSetArg(args[n], XmNrightOffset,        0);  n++;
    XtSetValues (dtwm.minAnimOM, args, n);

    /* Maximize / Restore Animation OM */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,      XmATTACH_WIDGET);       n++;
    XtSetArg(args[n], XmNtopWidget,          dtwm.minAnimOM);        n++;
    XtSetArg(args[n], XmNtopOffset,          style.verticalSpacing-3); n++;
    XtSetArg(args[n], XmNbottomAttachment,   XmATTACH_NONE);         n++;
    XtSetArg(args[n], XmNleftAttachment,     XmATTACH_FORM);         n++;
    XtSetArg(args[n], XmNleftOffset,         0);  n++;
    XtSetArg(args[n], XmNrightAttachment,    XmATTACH_FORM);         n++;
    XtSetArg(args[n], XmNrightOffset,        0);  n++;
    XtSetValues (dtwm.maxAnimOM, args, n);

    /* Show Task Switcher TG */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,      XmATTACH_WIDGET);       n++;
    XtSetArg(args[n], XmNtopWidget,          dtwm.maxAnimOM);        n++;
    XtSetArg(args[n], XmNtopOffset,          style.verticalSpacing-3); n++;
    XtSetArg(args[n], XmNbottomAttachment,   XmATTACH_FORM);         n++;
    XtSetArg(args[n], XmNbottomOffset,       style.verticalSpacing); n++;
    XtSetArg(args[n], XmNleftAttachment,     XmATTACH_FORM);         n++;
    XtSetArg(args[n], XmNleftOffset,         0);  n++;
    XtSetArg(args[n], XmNrightAttachment,    XmATTACH_FORM);         n++;
    XtSetArg(args[n], XmNrightOffset,        0);  n++;
    XtSetValues (dtwm.showTaskSwitcherTG, args, n);

    /* Use Icon Box RC */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,      XmATTACH_FORM);       n++;
    XtSetArg(args[n], XmNtopOffset,          style.verticalSpacing);    n++;
    XtSetArg(args[n], XmNbottomAttachment,   XmATTACH_NONE);       n++;
    XtSetArg(args[n], XmNleftAttachment,     XmATTACH_FORM);       n++;
    XtSetArg(args[n], XmNleftOffset,         0);  n++;
    XtSetArg(args[n], XmNrightAttachment,    XmATTACH_FORM);       n++;
    XtSetArg(args[n], XmNrightOffset,        0);  n++;
    XtSetValues (dtwm.useIconBoxRC, args, n);

    n=0;
    XtSetArg(args[n], XmNtopAttachment,      XmATTACH_WIDGET);       n++;
    XtSetArg(args[n], XmNtopWidget,          dtwm.useIconBoxRC);     n++;
    XtSetArg(args[n], XmNtopOffset,          style.verticalSpacing); n++;
    XtSetArg(args[n], XmNbottomAttachment,   XmATTACH_NONE);         n++;
    XtSetArg(args[n], XmNleftAttachment,     XmATTACH_FORM);         n++;
    XtSetArg(args[n], XmNleftOffset,         0);  n++;
    XtSetArg(args[n], XmNrightAttachment,    XmATTACH_FORM);         n++;
    XtSetArg(args[n], XmNrightOffset,        0);  n++;
    XtSetValues (dtwm.increaseIconSizeTG, args, n);

    /* Show open icons TG */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,      XmATTACH_WIDGET);       n++;
    XtSetArg(args[n], XmNtopWidget,          dtwm.increaseIconSizeTG); n++;
    XtSetArg(args[n], XmNtopOffset,          style.verticalSpacing-3); n++;
    XtSetArg(args[n], XmNbottomAttachment,   XmATTACH_NONE);         n++;
    XtSetArg(args[n], XmNleftAttachment,     XmATTACH_FORM);         n++;
    XtSetArg(args[n], XmNleftOffset,         0);  n++;
    XtSetArg(args[n], XmNrightAttachment,    XmATTACH_FORM);         n++;
    XtSetArg(args[n], XmNrightOffset,        0);  n++;
    XtSetValues (dtwm.showOpenIconsTG, args, n);

    /* Icon grid margin */
    n=0;
    XtSetArg(args[n], XmNtopAttachment,      XmATTACH_WIDGET);       n++;
    XtSetArg(args[n], XmNtopWidget,          dtwm.showOpenIconsTG);   n++;
    XtSetArg(args[n], XmNtopOffset,          style.verticalSpacing-3); n++;
    XtSetArg(args[n], XmNbottomAttachment,   XmATTACH_FORM);         n++;
    XtSetArg(args[n], XmNbottomOffset,       style.verticalSpacing); n++;
    XtSetArg(args[n], XmNleftAttachment,     XmATTACH_FORM);         n++;
    XtSetArg(args[n], XmNleftOffset,         0);  n++;
    XtSetArg(args[n], XmNrightAttachment,    XmATTACH_FORM);         n++;
    XtSetArg(args[n], XmNrightOffset,        0);  n++;
    XtSetValues (dtwm.iconGapLabel, args, n);
}

/*+++++++++++++++++++++++++++++++++++++++*/
/* _DtmapCB_dtwmDlg                        */
/*+++++++++++++++++++++++++++++++++++++++*/
static void 
_DtmapCB_dtwmDlg(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{

    static int  first_time = 1;
    int         n;
    Arg         args[MAX_ARGS];


    DtWsmRemoveWorkspaceFunctions(style.display, XtWindow(XtParent(w)));

    if (!save.restoreFlag)
        putDialog ((Widget)client_data, w);
   
    XtRemoveCallback(style.dtwmDialog, XmNmapCallback, _DtmapCB_dtwmDlg, nullptr);
  
}


/*+++++++++++++++++++++++++++++++++++++++*/
/* systemDefaultCB                       */
/*+++++++++++++++++++++++++++++++++++++++*/
static void 
systemDefaultCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{

    /* KeyboardFocusPolicy:  explicit */
    XmToggleButtonGadgetSetState (dtwm.explicitTG, True, True); 

    /* FocusAutoRaise:  True */
    XmToggleButtonGadgetSetState (dtwm.autoRaiseTG, True, True); 
   
#ifdef sun
    /* Sun default is secStack: False */
    XmToggleButtonGadgetSetState (dtwm.secStackTG, True, True);
#else
    /* secStack:  True */
    XmToggleButtonGadgetSetState (dtwm.secStackTG, False, True); 
#endif

    /* OpaqueMove: True on Wayland, False on classic X11 */
    {
        Boolean wayland_mode = is_wayland_dtwm_session();
        XmToggleButtonGadgetSetState (dtwm.moveOpaqueTG, wayland_mode ? True : False, True); 
        refresh_anim_menus_for_backend(wayland_mode);
        set_anim_menu_selection(False, wayland_mode ? DTWM_UI_ANIM_BEVEL : DTWM_UI_ANIM_WIREFRAME);
        set_anim_menu_selection(True, wayland_mode ? DTWM_UI_ANIM_BEVEL : DTWM_UI_ANIM_NONE);
    }

    /* ShowTaskSwitcher: True */
    XmToggleButtonGadgetSetState (dtwm.showTaskSwitcherTG, True, True);

    /* PlaceOnDesktop:  True */
    XmToggleButtonGadgetSetState (dtwm.desktopTG, True, True); 

    XmToggleButtonGadgetSetState(dtwm.increaseIconSizeTG, False, True);
    XmToggleButtonGadgetSetState(dtwm.showOpenIconsTG, True, True);
    XmTextFieldSetString(dtwm.iconGapText, "-1");

}

/*+++++++++++++++++++++++++++++++++++++++*/
/* ButtonCB                              */
/* callback for PushButtons in DialogBox */
/*+++++++++++++++++++++++++++++++++++++++*/
static void 
ButtonCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
  int         n;
  Arg         args[MAX_ARGS];
  char        *resPtr;
  Boolean     state;
  Boolean     changeFlag = 0;
  DtDialogBoxCallbackStruct *cb = (DtDialogBoxCallbackStruct *) call_data;
  
  switch (cb->button_position)
    {
    case OK_BUTTON:
      
      XtUnmanageChild (w);
      dtwmRes[0] = '\0';
      
      /* create the Dtwm resource specs for xrdb */
      
      state = XmToggleButtonGadgetGetState (dtwm.explicitTG);
      if ( state != dtwm.origKeyboardFocusPolicy)
	{
	  append_dtwm_res(dtwmRes, sizeof(dtwmRes), "Dtwm.keyboardFocusPolicy: %s\n",
			  XmToggleButtonGadgetGetState (dtwm.pointerTG)
			  ? POINTER_STR : EXPLICIT_STR);
	  changeFlag = 1;
	}
      
      /* write out FocusAutoRaise if toggle has changed or if
       * KeyboardFocusPolicy has changed.  FAR default value is 
       * based off of KFP so must set explicitly or visible toggle 
       * will be inacurate */
      state = XmToggleButtonGadgetGetState (dtwm.autoRaiseTG);
      if (state !=  dtwm.origFocusAutoRaise || changeFlag)
	{
	  append_dtwm_res(dtwmRes, sizeof(dtwmRes), "Dtwm*focusAutoRaise: %s\n",
			  XmToggleButtonGadgetGetState (dtwm.autoRaiseTG)
			  ? "True" : "False");
	  changeFlag = 1;
	}
      
      state = !XmToggleButtonGadgetGetState (dtwm.secStackTG);
      if ( state != dtwm.origSecStack)
	{
	  append_dtwm_res(dtwmRes, sizeof(dtwmRes), "Dtwm*secondariesOnTop: %s\n",
			  XmToggleButtonGadgetGetState (dtwm.secStackTG)
			  ? "False" : "True");
	  changeFlag = 1;
	}
      
      state = XmToggleButtonGadgetGetState (dtwm.moveOpaqueTG);
      if ( state != dtwm.origMoveOpaque)
	{
	  append_dtwm_res(dtwmRes, sizeof(dtwmRes), "Dtwm*moveOpaque: %s\n",
			  XmToggleButtonGadgetGetState (dtwm.moveOpaqueTG)
			  ? "True" : "False");
	  changeFlag = 1;
	}

      if (dtwm.curMinAnimStyle != dtwm.origMinAnimStyle)
	{
	  append_dtwm_res(dtwmRes, sizeof(dtwmRes),
			  "Dtwm*minimizeAnimation: %s\n"
			  "Dtwm*animateMinimizeRestore: %s\n",
			  anim_style_to_token(dtwm.curMinAnimStyle),
			  (dtwm.curMinAnimStyle != DTWM_UI_ANIM_NONE) ? "True" : "False");
	  changeFlag = 1;
	}

      if (dtwm.curMaxAnimStyle != dtwm.origMaxAnimStyle)
	{
	  append_dtwm_res(dtwmRes, sizeof(dtwmRes),
			  "Dtwm*maximizeAnimation: %s\n",
			  anim_style_to_token(dtwm.curMaxAnimStyle));
	  changeFlag = 1;
	}

      state = XmToggleButtonGadgetGetState (dtwm.showTaskSwitcherTG);
      if ( state != dtwm.origShowTaskSwitcher)
	{
	  append_dtwm_res(dtwmRes, sizeof(dtwmRes), "Dtwm*showTaskSwitcher: %s\n",
			  XmToggleButtonGadgetGetState (dtwm.showTaskSwitcherTG)
			  ? "True" : "False");
	  changeFlag = 1;
	}
      
      state = XmToggleButtonGadgetGetState (dtwm.iconBoxTG);
      if (state != dtwm.origUseIconBox)
	{
	  append_dtwm_res(dtwmRes, sizeof(dtwmRes), "Dtwm*useIconBox: %s\n",
			  XmToggleButtonGadgetGetState (dtwm.iconBoxTG)
			  ? "True" : "False");
	  changeFlag = 1;
	}

      state = XmToggleButtonGadgetGetState (dtwm.increaseIconSizeTG);
      if (state != dtwm.origIncreaseIconSize)
	{
	  int baseWidth = (dtwm.iconImageBaseWidth > 0) ? dtwm.iconImageBaseWidth : 32;
	  int baseHeight = (dtwm.iconImageBaseHeight > 0) ? dtwm.iconImageBaseHeight : 32;
	  int width = state ? baseWidth * 2 : baseWidth;
	  int height = state ? baseHeight * 2 : baseHeight;

	  if (width <= 0)
	    width = baseWidth;
	  if (height <= 0)
	    height = baseHeight;

	  append_dtwm_res(dtwmRes, sizeof(dtwmRes),
			  "Dtwm*iconImageMaximum: %dx%d\n"
			  "Dtwm*doubleIconSize: %s\n",
			  width, height, state ? "True" : "False");
	  changeFlag = 1;
	}

      state = XmToggleButtonGadgetGetState (dtwm.showOpenIconsTG);
      if (state != dtwm.origShowOpenIcons)
	{
	  append_dtwm_res(dtwmRes, sizeof(dtwmRes), "Dtwm*showOpenWindowIcons: %s\n",
			  state ? "True" : "False");
	  changeFlag = 1;
	}

      {
        char *gapStr = XmTextFieldGetString(dtwm.iconGapText);
        int gap = atoi(gapStr ? gapStr : "0");
        if (gapStr) XtFree(gapStr);
        if (gap != dtwm.origIconPlacementMargin)
	  {
	    append_dtwm_res(dtwmRes, sizeof(dtwmRes), "Dtwm*iconPlacementMargin: %d\n", gap);
	    changeFlag = 1;
	  }
      }
      
      if (changeFlag)
	{
	  if (is_wayland_dtwm_session())
	    {
	      /* dtwm-wayland applies RESOURCE_MANAGER changes live without restarting */
	      commit_orig_dtwm_values();
	      merge_dtwm_res_into_xt_db(dtwmRes);
	      _DtAddToResource(style.display, dtwmRes);
	      XFlush(style.display);
	      break;
	    }
	  if (dtwm.warnDialog == nullptr)
	    {
	      n = 0;
	      XtSetArg(args[n], XmNokLabelString, CMPSTR((String) _DtOkString)); n++;
	      XtSetArg(args[n], XmNcancelLabelString, CMPSTR((String) _DtCancelString)); n++;
	      XtSetArg(args[n], XmNborderWidth, 3); n++;
	      XtSetArg(args[n], XmNmwmFunctions, DIALOG_MWM_FUNC ); n++;
	      XtSetArg(args[n], XmNdialogStyle, XmDIALOG_FULL_APPLICATION_MODAL); n++; 
	      XtSetArg(args[n], XmNdialogTitle, CMPSTR((char *)GETMESSAGE(2,2, "Notice"))); n++;
	      XtSetArg(args[n], XmNmessageString, CMPSTR(WARN_MSG)); n++;
              dtwm.warnDialog = XmCreateInformationDialog(style.shell, "warnDialog", args, n);
	      
	      XtUnmanageChild (XmMessageBoxGetChild(dtwm.warnDialog, XmDIALOG_HELP_BUTTON)); 
	      
	      XtAddCallback(dtwm.warnDialog, XmNokCallback, okWarnCB, 
			    client_data); 
	      XtAddCallback(dtwm.warnDialog, XmNcancelCallback, cancelWarnCB, 
			    client_data); 
	    }
	  XtManageChild(dtwm.warnDialog);  
	   
	}
      break;
      
      
    case CANCEL_BUTTON:
    
      /* reset to dtwm values from when dialog is mapped */ 
      XtUnmanageChild(w);
      
      XmToggleButtonGadgetSetState (dtwm.pointerTG, 
				    dtwm.origKeyboardFocusPolicy ? POINTER : EXPLICIT , True); 
      
      XmToggleButtonGadgetSetState (dtwm.explicitTG, 
				    dtwm.origKeyboardFocusPolicy ? EXPLICIT : POINTER , True); 
      
      XmToggleButtonGadgetSetState (dtwm.autoRaiseTG, 
				    dtwm.origFocusAutoRaise ? True : False , True); 
      
      XmToggleButtonGadgetSetState (dtwm.secStackTG, 
				    dtwm.origSecStack ? False : True , True); 
      
      XmToggleButtonGadgetSetState (dtwm.moveOpaqueTG, 
				    dtwm.origMoveOpaque ? True : False , True); 

      set_anim_menu_selection(False, dtwm.origMinAnimStyle);
      set_anim_menu_selection(True, dtwm.origMaxAnimStyle);

      XmToggleButtonGadgetSetState (dtwm.showTaskSwitcherTG,
				    dtwm.origShowTaskSwitcher ? True : False , True);
      
      XmToggleButtonGadgetSetState (dtwm.iconBoxTG, 
				    dtwm.origUseIconBox ? True : False , True); 
      
      XmToggleButtonGadgetSetState (dtwm.desktopTG, 
				    dtwm.origUseIconBox ? False : True , True); 

      XmToggleButtonGadgetSetState (dtwm.increaseIconSizeTG,
				    dtwm.origIncreaseIconSize ? True : False , True); 
      
      XmToggleButtonGadgetSetState (dtwm.showOpenIconsTG,
				    dtwm.origShowOpenIcons ? True : False , True); 

      {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", dtwm.origIconPlacementMargin);
        XmTextFieldSetString(dtwm.iconGapText, buf);
      }
      
      break;
    
  case HELP_BUTTON:
    XtCallCallbacks(style.dtwmDialog, XmNhelpCallback, (XtPointer)nullptr);
    break;
    
  default:
    break;
  }
}


/************************************************************************
 * restoreDtwm()
 * /u1/shakib/ncode
 * restore any state information saved with savedtwm.
 * This is called from restoreSession with the application
 * shell and the special xrm database retrieved for restore.
 ************************************************************************/
void 
restoreDtwm(
        Widget shell,
        XrmDatabase db )
{
    XrmName xrm_name[5];
    XrmRepresentation rep_type;
    XrmValue value;

    xrm_name [0] = XrmStringToQuark ("dtwmDlg");
    xrm_name [2] = 0;

    /* get x position */
    xrm_name [1] = XrmStringToQuark ("x");
    if (XrmQGetResource (db, xrm_name, xrm_name, &rep_type, &value)){
      XtSetArg (save.posArgs[save.poscnt], XmNx, atoi((char *)value.addr)); save.poscnt++;
      save.restoreFlag = True;
    }

    /* get y position */
    xrm_name [1] = XrmStringToQuark ("y");
    if (XrmQGetResource (db, xrm_name, xrm_name, &rep_type, &value)){
      XtSetArg (save.posArgs[save.poscnt], XmNy, atoi((char *)value.addr)); save.poscnt++;
    }

    xrm_name [1] = XrmStringToQuark ("ismapped");
    if (XrmQGetResource (db, xrm_name, xrm_name, &rep_type, &value) &&
        value.addr != nullptr &&
        strcmp(value.addr, "True") == 0) 
      popup_dtwmBB(shell);
}

/************************************************************************
 * saveDtwm()
 *
 * This routine will write out to the passed file descriptor any state
 * information this dialog needs.  It is called from saveSessionCB with the
 * file already opened.
 * All information is saved in xrm format.  There is no restriction
 * on what can be saved.  It doesn't have to be defined or be part of any
 * widget or Xt definition.  Just name and save it here and recover it in
 * restoreBackdrop.  The suggested minimum is whether you are mapped, and your
 * location.
 ************************************************************************/
void 
saveDtwm(
        int fd )
{
    Position x,y;
    Dimension width, height;
    char bufr[1024];     /* size=[1024], make bigger if needed */
    XmVendorShellExtObject  vendorExt;
    XmWidgetExtData         extData;

    if (style.dtwmDialog != nullptr) 
    {
        if (XtIsManaged(style.dtwmDialog))
            sprintf(bufr, "*dtwmDlg.ismapped: True\n");
        else
            sprintf(bufr, "*dtwmDlg.ismapped: False\n");

        WRITE_STR2FD(fd, bufr);

	/* Get and write out the geometry info for our Window */
	x = XtX(XtParent(style.dtwmDialog));
	y = XtY(XtParent(style.dtwmDialog));

        /* Modify x & y to take into account window mgr frames
         * This is pretty bogus, but I don't know a better way to do it.
         */
        extData = _XmGetWidgetExtData(style.shell, XmSHELL_EXTENSION);
        vendorExt = (XmVendorShellExtObject)extData->widget;
        x -= vendorExt->vendor.xOffset;
        y -= vendorExt->vendor.yOffset;

	width = XtWidth(style.dtwmDialog);
	height = XtHeight(style.dtwmDialog);

	snprintf(bufr, sizeof(bufr), "*dtwmDlg.x: %d\n", x);
        WRITE_STR2FD(fd, bufr);
	snprintf(bufr, sizeof(bufr), "*dtwmDlg.y: %d\n", y);
        WRITE_STR2FD(fd, bufr);
	snprintf(bufr, sizeof(bufr), "*dtwmDlg.width: %d\n", width);
        WRITE_STR2FD(fd, bufr);
	snprintf(bufr, sizeof(bufr), "*dtwmDlg.height: %d\n", height);
        WRITE_STR2FD(fd, bufr);
    }
}

/*************************************<->*************************************
 *
 *  _DtWmParseToLower (string)
 *
 *
 *  Description:
 *  -----------
 *  Lower all characters in a string.
 *
 *
 *  Inputs:
 *  ------
 *  string = NULL-terminated character string or NULL
 *
 * 
 *  Outputs:
 *  -------
 *  string = NULL-terminated lower case character string or NULL
 *
 *
 *  Comments:
 *  --------
 *  Can handle multi-byte characters
 * 
 *************************************<->***********************************/

static
void _DtWmParseToLower (unsigned char  *string)
{
    unsigned char *pch = string;
#ifdef MULTIBYTE
    int            chlen;

    while ((chlen = mblen ((char *)pch, MB_CUR_MAX)) > 0)
    {
        if ((chlen == 1) && (isupper (*pch)))
	{
	    *pch = tolower(*pch);
	}
	pch += chlen;
    }
#else
    while (*pch != nullptr)
    {
        if (isupper (*pch))
	{
	    *pch = tolower(*pch);
	}
	pch++;
    }
#endif

} /* END OF FUNCTION _DtWmParseToLower */




/*+++++++++++++++++++++++++++++++++++++++*/
/* cancelWarnCB - callback for the      */
/* cancel button of the warnDialog      */
/*+++++++++++++++++++++++++++++++++++++++*/

static void 
cancelWarnCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
  XmToggleButtonGadgetSetState (dtwm.pointerTG, 
				dtwm.origKeyboardFocusPolicy ? POINTER : EXPLICIT , True); 
  
  XmToggleButtonGadgetSetState (dtwm.explicitTG, 
            dtwm.origKeyboardFocusPolicy ? EXPLICIT : POINTER , True); 
  
  XmToggleButtonGadgetSetState (dtwm.autoRaiseTG, 
				dtwm.origFocusAutoRaise ? True : False , True); 
  
  XmToggleButtonGadgetSetState (dtwm.secStackTG, 
            dtwm.origSecStack ? False : True , True); 
  
  XmToggleButtonGadgetSetState (dtwm.moveOpaqueTG, 
				dtwm.origMoveOpaque ? True : False , True); 

  set_anim_menu_selection(False, dtwm.origMinAnimStyle);
  set_anim_menu_selection(True, dtwm.origMaxAnimStyle);

  XmToggleButtonGadgetSetState (dtwm.showTaskSwitcherTG,
				dtwm.origShowTaskSwitcher ? True : False , True);
  
  XmToggleButtonGadgetSetState (dtwm.iconBoxTG, 
            dtwm.origUseIconBox ? True : False , True); 
  
  XmToggleButtonGadgetSetState (dtwm.desktopTG, 
				dtwm.origUseIconBox ? False : True , True); 
}




/*+++++++++++++++++++++++++++++++++++++++*/
/* okWarnCB - callback for the           */
/* OK button of the warnDialog           */
/*+++++++++++++++++++++++++++++++++++++++*/

static void 
okWarnCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
  commit_orig_dtwm_values();
  merge_dtwm_res_into_xt_db(dtwmRes);

  /* write out resources to xrdb */
  _DtAddToResource(style.display, dtwmRes);
  
  /* force the workspace manager to restart */
  _DtWmRestartNoConfirm(style.display, style.root);
}
