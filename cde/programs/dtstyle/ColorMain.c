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
 **   File:        ColorMain.c
 **
 **   Project:     DT 3.0 
 **
 **   Description: Controls the Dtstyle Color dialog
 **
 **
 **  (c) Copyright Hewlett-Packard Company, 1990.  
 **
 **
 **
 ****************************************************************************
 ************************************<+>*************************************/
/* $TOG: ColorMain.c /main/8 1998/01/12 10:41:53 cshi $ */
/*+++++++++++++++++++++++++++++++++++++++*/
/* include files                         */
/*+++++++++++++++++++++++++++++++++++++++*/
#include <limits.h>

#include <locale.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <X11/Intrinsic.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <Xm/MwmUtil.h>

#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

#include <Xm/Xm.h>
#include <Xm/XmP.h>
#include <Xm/Appearance.h>
#include <Xm/DrawingA.h>
#include <Xm/Form.h>
#include <Xm/Frame.h>
#include <Xm/Label.h>
#include <Xm/LabelG.h>
#include <Xm/List.h>
#include <Xm/MessageB.h>
#include <Xm/Protocols.h>
#include <Xm/PushB.h>
#include <Xm/PushBG.h>
#include <Xm/ToggleBG.h>
#include <Xm/RowColumn.h>
#include <Xm/SelectioB.h>
#include <Xm/TextF.h>
#include <Xm/VendorSEP.h>

#include <Dt/DialogBox.h>
#include <Dt/Icon.h>
#include <Dt/TitleBox.h>
#include <Dt/UserMsg.h>

#include <Dt/HourGlass.h>
#include <Dt/Wsm.h>

#include "Help.h"
#include "Main.h"
#include "SaveRestore.h"
#include "ColorMain.h"
#include "ColorFile.h"
#include "ColorEdit.h"
#include "ColorPalette.h"

#ifdef sun
#include "OWsync.h"
#endif

/*+++++++++++++++++++++++++++++++++++++++*/
/* include extern functions              */
/*+++++++++++++++++++++++++++++++++++++++*/




/*+++++++++++++++++++++++++++++++++++++++*/
/* Local #defines                        */
/*+++++++++++++++++++++++++++++++++++++++*/
#define DEFAULT_PALETTE    "Default"

#define COLOR_BTN_OK       1
#define COLOR_BTN_APPLY    2
#define COLOR_BTN_CANCEL   3
#define COLOR_BTN_HELP     4
#define COLOR_NUM_BUTTONS  4

typedef enum {
    COLOR_STYLE_CLASSIC = 0,
    COLOR_STYLE_MODERN = 1,
    COLOR_STYLE_HIGH_CONTRAST = 2,
} ColorWidgetStyle;

static ColorWidgetStyle g_currentWidgetStyle = COLOR_STYLE_CLASSIC;
static ColorWidgetStyle g_origWidgetStyle = COLOR_STYLE_CLASSIC;
static XmAppearanceDensity g_currentDensity = XmAPPEARANCE_DENSITY_COMFORTABLE;
static XmAppearanceDensity g_origDensity = XmAPPEARANCE_DENSITY_COMFORTABLE;
static char g_origPaletteName[128] = {0};
static Boolean g_appliedSinceOpen = False;
static Boolean g_updatingStyleWidgets = False;

static Boolean
DtstyleIsHighContrastPalette(const palette *pal)
{
    if (!pal || !pal->name) return False;
    return (strcasecmp(pal->name, "HighContrast") == 0 ||
            strcasecmp(pal->name, "HighContrast.dp") == 0) ? True : False;
}

static double
DtstylePaletteLuminance(const palette *pal)
{
    if (!pal) return 0.60;
    if (pal->name && (strcasecmp(pal->name, "Black") == 0 ||
                      strcasecmp(pal->name, "Black.dp") == 0)) {
        return 0.0;
    }
    int n = pal->num_of_colors > 0 ? pal->num_of_colors : 2;
    int i1 = (n > 1) ? 1 : 0;
    int i4 = (n > 4) ? 4 : i1;
    int i3 = (n > 3) ? 3 : i1;
    double l1 = (0.2126 * pal->color[i1].bg.red + 0.7152 * pal->color[i1].bg.green + 0.0722 * pal->color[i1].bg.blue) / 65535.0;
    double l4 = (0.2126 * pal->color[i4].bg.red + 0.7152 * pal->color[i4].bg.green + 0.0722 * pal->color[i4].bg.blue) / 65535.0;
    double l3 = (0.2126 * pal->color[i3].bg.red + 0.7152 * pal->color[i3].bg.green + 0.0722 * pal->color[i3].bg.blue) / 65535.0;
    return 0.45 * l1 + 0.35 * l4 + 0.20 * l3;
}

static Boolean
DtstyleIsDarkPalette(const palette *pal)
{
    if (!pal) return False;
    if (DtstyleIsHighContrastPalette(pal)) return True;
    if (pal->name && (strcasecmp(pal->name, "ModernDark") == 0 ||
                      strcasecmp(pal->name, "ModernDark.dp") == 0)) {
        return True;
    }
    return (DtstylePaletteLuminance(pal) < 0.30) ? True : False;
}

static XmAppearanceProfile
DtstyleResolveMotifProfile(const palette *pal, ColorWidgetStyle wstyle)
{
    if (wstyle == COLOR_STYLE_HIGH_CONTRAST || DtstyleIsHighContrastPalette(pal)) {
        return XmAPPEARANCE_HIGH_CONTRAST;
    }
    if (wstyle == COLOR_STYLE_CLASSIC) {
        return XmAPPEARANCE_CLASSIC;
    }
    return DtstyleIsDarkPalette(pal)
               ? XmAPPEARANCE_MODERN_DARK
               : XmAPPEARANCE_MODERN_LIGHT;
}

static XmAppearanceProfile
DtstyleProfileForPaletteName(const char *name)
{
    if (name == nullptr)
        return XmAPPEARANCE_CLASSIC;
    if (strcmp(name, "ModernLight") == 0 ||
        strcmp(name, "ModernLight.dp") == 0)
        return XmAPPEARANCE_MODERN_LIGHT;
    if (strcmp(name, "ModernDark") == 0 ||
        strcmp(name, "ModernDark.dp") == 0)
        return XmAPPEARANCE_MODERN_DARK;
    if (strcmp(name, "HighContrast") == 0 ||
        strcmp(name, "HighContrast.dp") == 0)
        return XmAPPEARANCE_HIGH_CONTRAST;
    return XmAPPEARANCE_CLASSIC;
}

static const char *
DtstyleProfileNameString(XmAppearanceProfile profile)
{
    switch (profile)
    {
        case XmAPPEARANCE_MODERN_LIGHT:
            return "modern_light";
        case XmAPPEARANCE_MODERN_DARK:
            return "modern_dark";
        case XmAPPEARANCE_HIGH_CONTRAST:
            return "high_contrast";
        case XmAPPEARANCE_CLASSIC:
        default:
            return "classic";
    }
}

static const char *
DtstyleDensityNameString(XmAppearanceDensity density)
{
    return (density == XmAPPEARANCE_DENSITY_COMPACT) ? "compact" : "comfortable";
}

static Boolean
DtstyleIsWaylandSession(void)
{
    if (!style.display) return False;
    Atom a = XInternAtom(style.display, "_DT_WAYLAND_OUTPUTS", True);
    if (a == None) return False;
    Atom act_t = None;
    int act_f = 0;
    unsigned long nitems = 0, rem = 0;
    unsigned char *data = nullptr;
    Boolean ok = False;
    if (XGetWindowProperty(style.display, style.root, a, 0, 8, False,
                           AnyPropertyType, &act_t, &act_f, &nitems, &rem,
                           &data) == Success && data) {
        ok = (nitems > 0) ? True : False;
        XFree(data);
    }
    return ok;
}

static void
DtstyleSendWaylandWmRequest(const char *cmd)
{
    if (!style.display || !cmd || !cmd[0]) return;
    Atom mwm_info = XInternAtom(style.display, "_MOTIF_WM_INFO", True);
    if (mwm_info == None) return;
    Atom act_t = None;
    int act_f = 0;
    unsigned long nitems = 0, rem = 0;
    unsigned char *prop = nullptr;
    if (XGetWindowProperty(style.display, style.root, mwm_info, 0, 2, False,
                           AnyPropertyType, &act_t, &act_f, &nitems, &rem,
                           &prop) != Success || !prop || nitems < 2) {
        if (prop) XFree(prop);
        return;
    }
    Window wm_win = (Window)((unsigned long *)prop)[1];
    XFree(prop);
    if (wm_win == None) return;
    Atom req_atom = XInternAtom(style.display, "_DT_WM_REQUEST", False);
    XChangeProperty(style.display, wm_win, req_atom, XA_STRING, 8,
                    PropModeAppend, (const unsigned char *)cmd,
                    (int)strlen(cmd) + 1);
    XFlush(style.display);
}

static Pixel
DtstyleAllocColorPixel(const XColor *src, Pixel fallback)
{
    XColor xc;
    if (!style.display || !src)
        return fallback;
    xc = *src;
    xc.flags = DoRed | DoGreen | DoBlue;
    if (XAllocColor(style.display, style.colormap, &xc))
        return xc.pixel;
    return src->pixel ? src->pixel : fallback;
}

static void
DtstylePublishLivePixelSet(const palette *pal)
{
    if (!style.display || !pal) return;
    char cust_name[64];
    snprintf(cust_name, sizeof(cust_name), "%s%d", XmSCUSTOMIZE_DATA, style.screenNum);
    Atom cust_atom = XInternAtom(style.display, cust_name, True);
    if (cust_atom == None) return;
    Window owner = XGetSelectionOwner(style.display, cust_atom);
    if (owner == None) return;

    static const int cmap_idx[4][XmCO_MAX_NUM_COLORS] = {
        {0, 1, 2, 3, 4, 5, 6, 7},
        {0, 1, 2, 3, 1, 1, 2, 1},
        {0, 1, 1, 1, 1, 1, 1, 1},
        {0, 1, 1, 1, 1, 1, 1, 1},
    };
    int midx = 0;
    if (TypeOfMonitor == XmCO_MEDIUM_COLOR) midx = 1;
    else if (TypeOfMonitor == XmCO_LOW_COLOR) midx = 2;
    else if (TypeOfMonitor == XmCO_BLACK_WHITE) midx = 3;

    Pixel blk = BlackPixelOfScreen(style.screen);
    Pixel wht = WhitePixelOfScreen(style.screen);

    char buf[2048];
    int pos = snprintf(buf, sizeof(buf), "%x_", TypeOfMonitor);
    for (int i = 0; i < XmCO_MAX_NUM_COLORS; ++i) {
        int slot = cmap_idx[midx][i];
        if (slot >= pal->num_of_colors && pal->num_of_colors > 0)
            slot = slot % pal->num_of_colors;
        const ColorSet *cs = &pal->color[slot];
        Pixel p_bg = style.dynamicColor ? cs->bg.pixel : DtstyleAllocColorPixel(&cs->bg, wht);
        Pixel p_fg = style.dynamicColor ? cs->fg.pixel : DtstyleAllocColorPixel(&cs->fg, blk);
        Pixel p_ts = style.dynamicColor ? cs->ts.pixel : DtstyleAllocColorPixel(&cs->ts, wht);
        Pixel p_bs = style.dynamicColor ? cs->bs.pixel : DtstyleAllocColorPixel(&cs->bs, blk);
        Pixel p_sc = style.dynamicColor ? cs->sc.pixel : DtstyleAllocColorPixel(&cs->sc, wht);
        pos += snprintf(
            buf + pos, sizeof(buf) - (size_t)pos, "%lx_%lx_%lx_%lx_%lx_",
            (unsigned long)p_bg,
            (unsigned long)p_fg,
            (unsigned long)p_ts,
            (unsigned long)p_bs,
            (unsigned long)p_sc
        );
    }
    if (pos + 2 < (int)sizeof(buf)) {
        buf[pos++] = XmPIXEL_SET_PROP_VERSION;
        buf[pos] = '\0';
        Atom pset_atom = XInternAtom(style.display, XmSPIXEL_SET_PROP, False);
        XChangeProperty(style.display, owner, pset_atom, XA_STRING, 8,
                        PropModeReplace, (const unsigned char *)buf, pos);
        XFlush(style.display);
    }
}
#define TYPE_OF_MONITOR  "Type Of Monitor" /* also in dtsession/SrvPalette.c */

#define BORDER_WIDTH                3
#define COLOR_BUTTON_WIDTH         35
#define COLOR_BUTTON_HEIGHT        35
#define ADD_PALETTE_TOP_OFFSET     20
#define ADD_PALETTE_LEFT_POSITION  65
#define PALETTE_RC_RIGHT_POSITION  60

#define DEFAULT_COLOR  (XmCO_HIGH_COLOR + 1)

#define B_W_STR            "B_W"
#define LOW_COLOR_STR      "LOW_COLOR"
#define MEDIUM_COLOR_STR   "MEDIUM_COLOR"
#define HIGH_COLOR_STR     "HIGH_COLOR"
#define DEFAULT_COLOR_STR  "DEFAULT"

#define STR1 ((char *)GETMESSAGE(14, 29, "A palette named '%s' already exists.\nThis new palette will overwrite the old one.\nIs this what you want to do?"))
#define STR2 ((char *)GETMESSAGE(14, 23, "Delete palette '%s'?\n"))
#define NEXT_SESSION ((char *)GETMESSAGE(14, 28, "The selected palette will take effect\nat your next session."))
#define CANT_DELETE ((char *)GETMESSAGE(14, 18, "Can't delete the last palette.\n"))
#define COLORUSE_WHEN ((char *)GETMESSAGE(14, 27, "The new Color Use value will take effect\nat your next session."))

/*+++++++++++++++++++++++++++++++++++++++*/
/* Local typedefs                        */
/*+++++++++++++++++++++++++++++++++++++++*/

typedef struct {
    Widget           colorForm;
    Widget           leftColForm;
    Widget           rightColForm;
    Widget           paletteTB;
    Widget	     palettesForm;
    Widget           paletteModeLabel;
    Widget	     addPaletteButton;
    Widget	     deletePaletteButton;
    Widget           buttonsTB;
    Widget           styleTB;
    Widget           styleOptMenu;
    Widget           styleOptBtns[3];
    Widget           densityOptMenu;
    Widget           densityOptBtns[2];
    Widget           themeSummaryLabel;
    Widget           previewTB;
    Widget           previewCanvas;
    Widget           colorlabel;
    Widget           highColorTG;
    Widget           mediumColorTG;
    Widget           lowColorTG;
    Widget           blackWhiteTG;
    Widget           defaultTG;
    Widget	     dlg;
    int              origColorUse;
    char             *currentColorUseStr;
    int              currentColorUse;
} colorWidgets;

static void UpdateColorThemeSummaryAndPreview(void);
static void QueryCurrentAppearanceSettings(void);
static void PublishColorDialogWindowProp(void);
static void ApplyColorAndThemeToDesktop(Boolean restartWmAndPanel);

/*+++++++++++++++++++++++++++++++++++++++*/
/* Internal Functions                    */
/*+++++++++++++++++++++++++++++++++++++++*/

static int CreateMainWindow( Widget parent) ;
static void selectPaletteCB( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void selectColorCB( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void timeoutCB( 
                        XtPointer client_data,
                        XtIntervalId *id) ;
static void addPaletteCB( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void addCancelCB( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void addOkCB( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void setDlgOkCB( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void modifyColorCB( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void dialogBoxCB( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void AddName(    
                        palette *newPalette) ;
static void deletePaletteCB( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void deleteOkCB( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void deleteCancelCB( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void resourcesCB(
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void colorUseCB(
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void colorUseExitCB(
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void activateCBexitColor( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void _DtmapCB( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void _DtmapCB_colorUse( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void colorEditorCB(
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;

static void allocNewColors(void);
static Boolean ValidName( char *name) ;
void loadDatabase(void);



/*+++++++++++++++++++++++++++++++++++++++*/
/* Global Variables                      */
/*+++++++++++++++++++++++++++++++++++++++*/

Atom     XA_CUSTOMIZE;
Atom     XA_TYPE_MONITOR;

/*  Palettes exist in a linked list. */
palette *pHeadPalette;
palette *pCurrentPalette;
palette *pOldPalette;

Widget  modifyColorButton;
int     TypeOfMonitor;
Bool    UsePixmaps;
int	FgColor;
Widget  paletteList;
Widget  deleteButton;
char    *defaultName;
Bool    WaitSelection;
int     NumOfPalettes = 0;

/*+++++++++++++++++++++++++++++++++++++++*/
/* Internal Variables                    */
/*+++++++++++++++++++++++++++++++++++++++*/

/* palette names without the .dp */
static char *WHITE_BLACK = "WhiteBlack";
static char *BLACK_WHITE = "BlackWhite";
static char *WHITE_ONLY = "White";
static char *BLACK_ONLY = "Black";

static char *PALETTEDLG = "paletteDlg";
static saveRestore save = {FALSE, 0, {{0}}};

static Widget           colorButton[XmCO_MAX_NUM_COLORS];
static Widget           addDialog;
static Widget           deleteDialog;
static Widget           colorUseDialog;
static palette          OrgPalette;
static XtIntervalId     timeID;
static int              dclick_time;
static int              selected_button;
static int              selected_position;
static Atom             XA_WM_DELETE_WINDOW;
static char             defaultName_restore[50];
static palette         *loop_palette, *loop_palette2;
static int              loopcount = 0;
static colorWidgets     colorDialog; 
static Widget           gParent;


/*
 *   copy of the system palette description file for the 
 *   current locale in xrm form
 */
XrmDatabase sys_pl_DB = nullptr;

/*
 *   copy of the admin palette description file for the 
 *   current locale in xrm form
 */
XrmDatabase adm_pl_DB = nullptr;

/*
 *   final combination of the admin & system dta bases 
 */
XrmDatabase pl_DB = nullptr;

/*
 *   copy of the palette description file from the user's home 
 *   directory. This database is locale independent and will always
 *   be loaded.
 */
XrmDatabase hm_pl_DB = nullptr;


void 
loadDatabase(void)
{
  
  char   *lang;
  char   *pl_desc;

  /* load the palette description data base for the given locale*/
  /* from that locale's description file from the system location */

  lang = setlocale (LC_CTYPE,nullptr);

  pl_desc = (char *)XtMalloc(strlen("/usr/dt/palettes/desc.") + strlen(lang) + 1);
  strcpy (pl_desc,"/usr/dt/palettes/desc.");
  strcat (pl_desc, lang);
  if(sys_pl_DB = XrmGetFileDatabase (pl_desc))
    XrmMergeDatabases(sys_pl_DB, &pl_DB);
  XtFree(pl_desc);
  
  /* load the palette description data base for the given locale*/
  /* from that locale's description file from the admin location */
  pl_desc = (char *)XtMalloc(strlen("/etc/dt/palettes/desc.") + strlen(lang) + 1);
  strcpy (pl_desc,"/etc/dt/palettes/desc.");
  strcat (pl_desc, lang);
  if (adm_pl_DB = XrmGetFileDatabase (pl_desc))
    XrmMergeDatabases(adm_pl_DB, &pl_DB);
  XtFree(pl_desc);

  /* load the palette description data base regardless of locale*/
  pl_desc = (char *) XtMalloc(strlen(style.home) +(strlen("/.dt/palettes/desc.palettes") + 1));
  strcpy (pl_desc, style.home);
  strcat (pl_desc, "/.dt/palettes/desc.palettes");
  if (hm_pl_DB = XrmGetFileDatabase (pl_desc))
    XrmMergeDatabases(hm_pl_DB, &pl_DB);
  XtFree(pl_desc);

  /* load the palette description data base regardless of locale for later use*/
  pl_desc = (char *) XtMalloc(strlen(style.home) +(strlen("/.dt/palettes/desc.palettes") + 1));
  strcpy (pl_desc, style.home);
  strcat (pl_desc, "/.dt/palettes/desc.palettes");
  if (XrmCombineFileDatabase (pl_desc, &pl_DB, True))
    /* load a separate home data base to be later saved as a file  */
    /* if any palettes are added */
    hm_pl_DB = XrmGetFileDatabase (pl_desc);
  XtFree(pl_desc);
}



void 
Customize(
        Widget shell )
{
    int     i;

    /*  
    **  Main routine does the following:
    **   1. Creates the color dialog shell
    **   2. Checks which monitor the customizer is running on
    **   3. Initialize color palettes
    **
    */
  
    /* can the user access the Color portion of Style? */

    /* Not if Color Server is not running */
    if (style.colorSrv == FALSE)
    {
       ErrDialog(((char *)GETMESSAGE(14, 25, "The color portion of the Style Manager\nwill not operate because the color server\nis not running.  Check $HOME/.dt/errorlog.")), style.shell);
       return;
    }

    /* Not if useColorObj resource is False.  XmeUseColorObj will return false
       if color server is not running or if the resource UseColorObj is False.
       If the color server is not running, that case is caught above so if
       we get here, the resource UseColorObj must be False.  This is an
       undocumented resource. */
       
    if (XmeUseColorObj() == FALSE)
    {
       ErrDialog(((char *)GETMESSAGE(14, 26, "The color portion of the Style Manager\nwill not operate because the resource\n'useColorObj' is set to False.")), style.shell);
       return;
    }

    if (style.colorDialog == nullptr || style.count < 12)
    {
        _DtTurnOnHourGlass(shell);

	/* Create Main Color Dialog */
	if(CreateMainWindow(shell) == -1)
        {
           _DtTurnOffHourGlass(shell);
           return; 
        }
        QueryCurrentAppearanceSettings();
        XtManageChild(style.colorDialog);
        XtRealizeWidget(XtParent(style.colorDialog));
        PublishColorDialogWindowProp();

        _DtTurnOffHourGlass(shell);
    }
    else 
    {
        QueryCurrentAppearanceSettings();
        XtManageChild(style.colorDialog);

        if (XtIsRealized(style.colorDialog)) {
            PublishColorDialogWindowProp();
            raiseWindow(XtWindow(XtParent(style.colorDialog)));
        }
    }

   /* also determine if system uses long filenames, used by add palette only */
    CheckFileType();

    SaveOrgPalette();
    UpdateColorThemeSummaryAndPreview();
}


/*
**  This routine creates the Color Customizer Main Window.   It is passed
**  the top level shell.
*/
static int 
CreateMainWindow(
        Widget parent )
{

    /* Create the DialogBox dialog */
    CreateDialogBoxD(parent);
  
    /* add some more to the dialog box */
    AddToDialogBox();

    /* Create the top part of the color dialog */
    CreateTopColor1();
    CreateTopColor2();

    /* read in palettes */
    ReadInPalettes(style.xrdb.paletteDir);

    if (NumOfPalettes == 0)
    {
        /* error dialog - no palettes */
        ErrDialog(((char *)GETMESSAGE(14, 38, "The color portion of the Style Manager\n\
will not operate because there are no palette\n\
files available.  Check $HOME/.dt/errorlog.")), style.shell);
        return(-1);        
    }

    /* go get the list of palettes of the color dialog */
    InitializePaletteList(parent,paletteList, False);

    /* Allocate the pixels for the Current palette. 
     * Will get the pixel values from the color server. 
     */
    AllocatePaletteCells(parent);

    /* go create the bottom portion of the color dialog */
    CreateBottomColor();

/*
**  Create the color buttons.  Have to do it after
**  initialize palettes so the correct pixels would be used.
*/
    CreatePaletteButtons(style.buttonsForm);
    
    return(0);

}


static void
SyncPaletteSwatchBorders(void)
{
    if (!style.display || !style.screen || TypeOfMonitor == XmCO_BLACK_WHITE)
        return;
    if (!(style.dynamicColor || style.visualClass == TrueColor ||
          style.visualClass == DirectColor))
        return;

    Widget host = style.buttonsForm ? style.buttonsForm : colorDialog.colorForm;
    if (!host)
        return;

    Pixel bg_pixel = WhitePixelOfScreen(style.screen);
    XtVaGetValues(host, XmNbackground, &bg_pixel, NULL);

    XColor qcol;
    memset(&qcol, 0, sizeof(qcol));
    qcol.pixel = bg_pixel;
    XQueryColor(style.display, style.colormap, &qcol);
    unsigned int lum =
        (30U * qcol.red + 59U * qcol.green + 11U * qcol.blue) / 100U;
    Pixel sel_pixel = (lum < 30000U)
                          ? WhitePixelOfScreen(style.screen)
                          : BlackPixelOfScreen(style.screen);

    int count = (pCurrentPalette && pCurrentPalette->num_of_colors > 0)
                    ? pCurrentPalette->num_of_colors
                    : XmCO_MAX_NUM_COLORS;
    for (int bi = 0; bi < count && bi < XmCO_MAX_NUM_COLORS; ++bi) {
        if (colorButton[bi] != nullptr) {
            XtVaSetValues(colorButton[bi],
                          XmNborderColor,
                          (bi == selected_button) ? sel_pixel : bg_pixel,
                          NULL);
        }
    }
}

static void
SyncSwatchesDeferredCB(XtPointer client_data, XtIntervalId *id)
{
    (void)client_data;
    (void)id;
    SyncPaletteSwatchBorders();
}

void 
CreatePaletteButtons(
        Widget parent )
{
    int     i,n;
    Arg              args[16];
    XmString         string;
    Pixmap           pixmap100;
    Widget           paletteRc;

    if(style.count > 11)
       return;

    if (TypeOfMonitor == XmCO_BLACK_WHITE)      /* create pixmaps for top/bottom shadow */
    {
	edit.pixmap25 = XmGetPixmap (style.screen, "25_foreground",
	               BlackPixelOfScreen(style.screen),
		       WhitePixelOfScreen(style.screen));
	edit.pixmap75 = XmGetPixmap (style.screen, "75_foreground",
	               BlackPixelOfScreen(style.screen),
		       WhitePixelOfScreen(style.screen));
    }
    pixmap100 = XmGetPixmap (style.screen, "background",
             BlackPixelOfScreen(style.screen),
             pCurrentPalette->color[pCurrentPalette->active].bg.pixel); 
    n = 0;
    XtSetArg (args[n], XmNtopAttachment, XmATTACH_FORM);  n++;
    XtSetArg (args[n], XmNtopOffset, 0);  n++;
    XtSetArg (args[n], XmNleftAttachment, XmATTACH_FORM);  n++;
    XtSetArg (args[n], XmNleftOffset, 0);  n++;
    XtSetArg (args[n], XmNrightAttachment, XmATTACH_POSITION);  n++;
    XtSetArg (args[n], XmNrightPosition, PALETTE_RC_RIGHT_POSITION);  n++;
    XtSetArg (args[n], XmNbottomAttachment, XmATTACH_FORM);  n++;
    XtSetArg (args[n], XmNbottomOffset, 0);  n++;
    XtSetArg (args[n], XmNspacing, 0);  n++;
    XtSetArg (args[n], XmNmarginWidth, style.horizontalSpacing);  n++;
    XtSetArg (args[n], XmNmarginHeight, style.verticalSpacing);  n++;
    XtSetArg (args[n], XmNorientation, XmHORIZONTAL);  n++;
    XtSetArg (args[n], XmNpacking, XmPACK_COLUMN);  n++;
    XtSetArg (args[n], XmNadjustLast, False);  n++;
    if(TypeOfMonitor == XmCO_HIGH_COLOR)
    {
       XtSetArg (args[n], XmNnumColumns, 2);  n++;
    }
    else
    {
       XtSetArg (args[n], XmNnumColumns, 1);  n++;
    }
    paletteRc = XmCreateRowColumn(parent, "paletteRc", args, n);
    XtManageChild(paletteRc);

    for (i=0; i<XmCO_MAX_NUM_COLORS; i++)
    {
       n=0;
       XtSetArg (args[n], XmNrecomputeSize, False); n++; 
       XtSetArg (args[n], XmNwidth,  COLOR_BUTTON_WIDTH); n++; 
       XtSetArg (args[n], XmNheight, COLOR_BUTTON_HEIGHT); n++; 
       XtSetArg (args[n], XmNshadowThickness, 2); n++; 
       /* allow traversal only if editing is possible */
       if (style.dynamicColor || style.visualClass==TrueColor
			|| style.visualClass==DirectColor)
       {
           XtSetArg (args[n], XmNtraversalOn, False); n++; 
       }
       XtSetArg (args[n], XmNborderWidth, BORDER_WIDTH); n++; 
       XtSetArg (args[n], XmNborderColor, 
           pCurrentPalette->color[pCurrentPalette->secondary].bg.pixel); n++;
       XtSetArg (args[n], XmNforeground, 
                     pCurrentPalette->color[i].fg.pixel); n++;
       XtSetArg (args[n], XmNbackground,
                     pCurrentPalette->color[i].bg.pixel); n++;
       XtSetArg (args[n], XmNarmColor, pCurrentPalette->color[i].sc.pixel); n++;
       XtSetArg (args[n], XmNmultiClick, XmMULTICLICK_KEEP); n++;
       if (TypeOfMonitor == XmCO_LOW_COLOR)
       {
           XtSetArg (args[n], XmNhighlightColor, 
            pCurrentPalette->color[pCurrentPalette->secondary].fg.pixel); n++;
       }
       else
       {
           XtSetArg (args[n], XmNhighlightPixmap, pixmap100); n++;
       }
       if(UsePixmaps == FALSE && TypeOfMonitor != XmCO_BLACK_WHITE)
       {
          XtSetArg (args[n], XmNtopShadowColor, 
                            pCurrentPalette->color[i].ts.pixel); n++;
          XtSetArg (args[n], XmNbottomShadowColor, 
                            pCurrentPalette->color[i].bs.pixel); n++;
       }
       else if (TypeOfMonitor == XmCO_BLACK_WHITE)
       {
          XtSetArg (args[n], XmNtopShadowPixmap, edit.pixmap25);        n++;
          XtSetArg (args[n], XmNbottomShadowPixmap, edit.pixmap75);     n++;
       }
       string = CMPSTR("");
       XtSetArg (args[n], XmNlabelString, string); n++;
       colorButton[i] = XmCreatePushButton(paletteRc, "colorButton", args, n);
       /* allow access to modify functionality if available */
       if (style.dynamicColor || style.visualClass==TrueColor
			|| style.visualClass==DirectColor)
           XtAddCallback(colorButton[i], XmNactivateCallback, selectColorCB, 
                         (XtPointer) (intptr_t) i);  
       XmStringFree(string);
    }
    XtManageChildren(colorButton,pCurrentPalette->num_of_colors);

    if(!save.restoreFlag)
       selected_button = 0;

    if(style.visualClass==TrueColor || style.visualClass==DirectColor)
        allocNewColors();
    SyncPaletteSwatchBorders();

    QueryCurrentAppearanceSettings();
    UpdateColorThemeSummaryAndPreview();
    style.count++;
}

Boolean 
InitializePaletteList(
        Widget shell,
        Widget list,
#if NeedWidePrototypes
        int startup )
#else
        Boolean startup )
#endif
{
  int     n;
  Arg              args[4];
  XmString         string;
  XmStringTable    string_table;
  char            *name_str;
  char            *class_str;
  char            *str_type_return;
  XrmValue         value_return;
  
  if(style.count > 8)
    return(True);
  
  /* 
   **  Add the palettes read in from ReadInPalettes
   */
  if(loopcount == 0) {
    if (TypeOfMonitor != XmCO_BLACK_WHITE) {
      loop_palette = pHeadPalette;
      loop_palette2 = pHeadPalette;
    }
    else {
      loop_palette = pHeadPalette;
      loop_palette2 = nullptr;
      while(loop_palette != nullptr)
	if(strcmp(loop_palette->name, "Black") == 0) {
	  loop_palette2 = loop_palette;
	  loop_palette = nullptr;
	}
	else
	  loop_palette = loop_palette->next;
      loop_palette = pHeadPalette;
      if(loop_palette2 == nullptr)
	loop_palette2 = pHeadPalette;
    }
  }
  while( loop_palette != nullptr)
    {
      if (pl_DB !=nullptr)
	{
	  name_str = (char *) XtMalloc(strlen("palettes.") + 
				       strlen(loop_palette->name) +
				       strlen(".desc") + 1);
	  
	  class_str = (char *) XtMalloc(strlen("Palettes.") + 
					strlen(loop_palette->name) + 
					strlen(".Desc") + 1);
	  strcpy(name_str, "palettes.");
	  strcpy(class_str, "Palettes.");
	  strcat(name_str, loop_palette->name);
	  strcat(class_str, loop_palette->name);
	  strcat(name_str, ".desc");
	  strcat(class_str, ".Desc");
	  
	  if (XrmGetResource (pl_DB, name_str, class_str, &str_type_return, &value_return))
	    {
	      loop_palette->desc = (char *)XtMalloc(strlen (value_return.addr) + 1 );
	      strcpy(loop_palette->desc, value_return.addr);
	    }
	  else
	    {
	      loop_palette->desc = (char *)XtMalloc(strlen (loop_palette->name) + 1 );
	      strcpy(loop_palette->desc, loop_palette->name);
	    }
	  XtFree(name_str);
	  XtFree(class_str);
	}
      else
	{
	  loop_palette->desc = (char *)XtMalloc(strlen (loop_palette->name) + 1 );
	  strcpy(loop_palette->desc, loop_palette->name);
	}
      
      XmListAddItem(list, CMPSTR(loop_palette->desc), loop_palette->item_position);
      
      /* if the item is the same as the default name provided by the
	 color Server, save it */
      if(!save.restoreFlag || defaultName_restore[0] == '\0') {
	if (!(strcmp(loop_palette->name, defaultName)))
	  loop_palette2 = loop_palette;
      }
      else {
	if (!(strcmp(loop_palette->name, defaultName_restore)))
	  loop_palette2 = loop_palette;
      }
      loop_palette = loop_palette->next;
      loopcount++;
      if((loopcount % 5 == 0) && startup)
	{

	return(False);}
    }
  
  /*
   **  Make the palette named by the color Server the selected palette, if the
   **  palette returned by the color server doesn't match make it the head
   **  palette.
   */
  pOldPalette = nullptr;
  
  /* the default name is the name to be selected */
  if (loop_palette2->name == nullptr)
    string = CMPSTR("");
  else
    string = CMPSTR(loop_palette2->desc);
  string_table = &string;
  
  n=0;
  XtSetArg (args[n], XmNselectedItemCount, 1);  n++;
  XtSetArg (args[n], XmNselectedItems, string_table);  n++;
  XtSetValues (list, args, n);
  XmStringFree(string); 
  
  XmListSetPos(list, loop_palette2->item_position);
  XtManageChild(list);
  
  pCurrentPalette = loop_palette2;
  selected_position = pCurrentPalette->item_position;
  
  style.count++;
  return(True);
}

/*
 * Allocate new pixel values and update color palette buttons.
 * This is needed for screens without dynamicColor.
 */
static void allocNewColors(void)
{
	int i, n;
	Arg args[10];
	static unsigned long   pixels[XmCO_MAX_NUM_COLORS*5];
	static int       count = 0;

	if(count)
	{
		/* free the cells from last selection */
		XFreeColors(style.display, style.colormap, pixels, count, 0);
		count=0;
	}

	for (i=0; i<pCurrentPalette->num_of_colors; i++)
	{
		n=0;
		if (XAllocColor(style.display, style.colormap,
			&(pCurrentPalette->color[i].fg)) == 0) break;
		pixels[count++] = pCurrentPalette->color[i].fg.pixel;

		if (XAllocColor(style.display, style.colormap,
			&(pCurrentPalette->color[i].bg)) == 0) break;
		pixels[count++] = pCurrentPalette->color[i].bg.pixel;
		XtSetArg (args[n], XmNbackground,
			pCurrentPalette->color[i].bg.pixel); n++;

		if (XAllocColor(style.display, style.colormap,
			&(pCurrentPalette->color[i].sc)) == 0)	break;
		pixels[count++] = pCurrentPalette->color[i].sc.pixel;
		XtSetArg (args[n], XmNarmColor,
			pCurrentPalette->color[i].sc.pixel); n++;

		if (UsePixmaps == FALSE)
		{
			if (XAllocColor(style.display, style.colormap,
				&(pCurrentPalette->color[i].ts)) == 0) break;
			pixels[count++] = pCurrentPalette->color[i].ts.pixel;
			XtSetArg (args[n], XmNtopShadowColor,
				pCurrentPalette->color[i].ts.pixel); n++;

			if (XAllocColor(style.display, style.colormap,
				&(pCurrentPalette->color[i].bs)) == 0) break;
			pixels[count++] = pCurrentPalette->color[i].bs.pixel;
			XtSetArg (args[n], XmNbottomShadowColor,
				pCurrentPalette->color[i].bs.pixel); n++;
		}
		else	/* create pixmaps for top/bottom shadow */
		{
			XmDestroyPixmap(style.screen, edit.pixmap25);
			XmDestroyPixmap(style.screen, edit.pixmap75);

			edit.pixmap25 = XmGetPixmap (style.screen,
				"50_foreground",pCurrentPalette->color[i].bg.pixel,
				WhitePixelOfScreen(style.screen));

			edit.pixmap75 = XmGetPixmap (style.screen,
				"50_foreground",pCurrentPalette->color[i].bg.pixel,
				BlackPixelOfScreen(style.screen));

			XtSetArg (args[n], XmNtopShadowPixmap, edit.pixmap25); n++;
			XtSetArg (args[n], XmNbottomShadowPixmap, edit.pixmap75); n++;
		}
		XtSetValues(colorButton[i], args, n);
	}
}
    
/*
**  This is the selection callback for the Scrolled list.
**  The routine finds the item the user selected and changes 
**  the already allocated pixels to the colors of the selected palette.
*/
static void 
selectPaletteCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
    int     n;
    Arg              args[10];
    XmListCallbackStruct *cb = (XmListCallbackStruct *)call_data;
    palette         *tmp_palette;
    XmString         string;
    Pixel            white, black;
	static Boolean   First = True;

    white = WhitePixelOfScreen(style.screen);
    black = BlackPixelOfScreen(style.screen);

    if (((edit.DialogShell == nullptr) || (!XtIsManaged(edit.DialogShell))) &&
                                      selected_position != cb->item_position)
    {
	selected_position = cb->item_position;

	tmp_palette = pHeadPalette;
	while( tmp_palette != nullptr &&
	       tmp_palette->item_position != selected_position ) {
	     tmp_palette = tmp_palette->next;
	}

	if(tmp_palette != nullptr &&
	   tmp_palette->item_position == selected_position)
	{
	    pOldPalette = pCurrentPalette;
	    pCurrentPalette = tmp_palette;

	    n=0;
            string = CMPSTR(pCurrentPalette->desc);
	    XtSetArg (args[n], XmNtitleString, string); n++;
	    XtSetValues (colorDialog.buttonsTB, args, n);
            XmStringFree(string);

            if (style.dynamicColor)
	        ReColorPalette();
	    else 
            {     
               if (TypeOfMonitor != XmCO_BLACK_WHITE)
               {
                 allocNewColors();
               }
               else  /* XmCO_BLACK_WHITE */
               {
                  /* set color buttons for new palette - read only cells */
                  /* primary=color[1] secondary=color[0] */
                  if (strcmp(pCurrentPalette->name, WHITE_BLACK) == 0)
                  {
                      n=0;      /* secondary color white */
                      XtSetArg (args[n], XmNforeground, black); n++;
                      XtSetArg (args[n], XmNbackground, white); n++;
                      XtSetArg (args[n], XmNarmColor, white); n++;
                      XtSetValues(colorButton[0], args, n);
                      pCurrentPalette->color[0].fg.pixel = black;
                      pCurrentPalette->color[0].bg.pixel = white;
                      pCurrentPalette->color[0].sc.pixel = white;
                      pCurrentPalette->color[0].ts.pixel = black;
                      pCurrentPalette->color[0].bs.pixel = black;

                      n=0;      /* primary color black */
                      XtSetArg (args[n], XmNforeground, white); n++;
                      XtSetArg (args[n], XmNbackground, black); n++;
                      XtSetArg (args[n], XmNarmColor, black); n++;
                      XtSetValues(colorButton[1], args, n);
                      pCurrentPalette->color[1].fg.pixel = white;
                      pCurrentPalette->color[1].bg.pixel = black;
                      pCurrentPalette->color[1].sc.pixel = black;
                      pCurrentPalette->color[1].ts.pixel = white;
                      pCurrentPalette->color[1].bs.pixel = white;
                  }
                  else if (strcmp(pCurrentPalette->name, BLACK_WHITE) == 0)
                  {
                      n=0;      /* secondary color black */
                      XtSetArg (args[n], XmNforeground, white); n++;
                      XtSetArg (args[n], XmNbackground, black); n++;
                      XtSetArg (args[n], XmNarmColor, black); n++;
                      XtSetValues(colorButton[0], args, n);
                      pCurrentPalette->color[0].fg.pixel = white;
                      pCurrentPalette->color[0].bg.pixel = black;
                      pCurrentPalette->color[0].sc.pixel = black;
                      pCurrentPalette->color[0].ts.pixel = white;
                      pCurrentPalette->color[0].bs.pixel = white;

                      n=0;      /* primary color white */
                      XtSetArg (args[n], XmNforeground, black); n++;
                      XtSetArg (args[n], XmNbackground, white); n++;
                      XtSetArg (args[n], XmNarmColor, white); n++;
                      XtSetValues(colorButton[1], args, n);
                      pCurrentPalette->color[1].fg.pixel = black;
                      pCurrentPalette->color[1].bg.pixel = white;
                      pCurrentPalette->color[1].sc.pixel = white;
                      pCurrentPalette->color[1].ts.pixel = black;
                      pCurrentPalette->color[1].bs.pixel = black;
                  }
                  else if (strcmp(pCurrentPalette->name, BLACK_ONLY) == 0)
                  {
                      n=0;      /* primary and secondary color black */
                      XtSetArg (args[n], XmNforeground, white); n++;
                      XtSetArg (args[n], XmNbackground, black); n++;
                      XtSetArg (args[n], XmNarmColor, black); n++;
                      XtSetValues(colorButton[0], args, n);
                      pCurrentPalette->color[0].fg.pixel = white;
                      pCurrentPalette->color[0].bg.pixel = black;
                      pCurrentPalette->color[0].sc.pixel = black;
                      pCurrentPalette->color[0].ts.pixel = white;
                      pCurrentPalette->color[0].bs.pixel = white;
                      XtSetValues(colorButton[1], args, n);
                      pCurrentPalette->color[1].fg.pixel = white;
                      pCurrentPalette->color[1].bg.pixel = black;
                      pCurrentPalette->color[1].sc.pixel = black;
                      pCurrentPalette->color[1].ts.pixel = white;
                      pCurrentPalette->color[1].bs.pixel = white;
                  }
                  else     /* WHITE_ONLY */
                  {
                      n=0;      /* primary and secondary color white */
                      XtSetArg (args[n], XmNforeground, black); n++;
                      XtSetArg (args[n], XmNbackground, white); n++;
                      XtSetArg (args[n], XmNarmColor, white); n++;
                      XtSetValues(colorButton[0], args, n);
                      pCurrentPalette->color[0].fg.pixel = black;
                      pCurrentPalette->color[0].bg.pixel = white;
                      pCurrentPalette->color[0].sc.pixel = white;
                      pCurrentPalette->color[0].ts.pixel = black;
                      pCurrentPalette->color[0].bs.pixel = black;
                      XtSetValues(colorButton[1], args, n);
                      pCurrentPalette->color[1].fg.pixel = black;
                      pCurrentPalette->color[1].bg.pixel = white;
                      pCurrentPalette->color[1].sc.pixel = white;
                      pCurrentPalette->color[1].ts.pixel = black;
                      pCurrentPalette->color[1].bs.pixel = black;
                  }
              }                  
           }
           SyncPaletteSwatchBorders();
           UpdateColorThemeSummaryAndPreview();
        }
    }
}


static void 
selectColorCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
    intptr_t         i;
    int              n;
    Arg              args[4];
    ColorSet *color_set;
    XmPushButtonCallbackStruct *cb = (XmPushButtonCallbackStruct *)call_data;

    i = (intptr_t) client_data;

    /* if click_count == 1 .. first button press, set time out */
    if(cb->click_count == 1)
    {
       timeID = XtAppAddTimeOut(XtWidgetToApplicationContext(gParent), 
				(unsigned long) dclick_time, timeoutCB, 
				(XtPointer) i);
       return;
    }

    /* else .. second button press, remove the time out */
    XtRemoveTimeOut(timeID);

    if ((edit.DialogShell == nullptr) || (!XtIsManaged(edit.DialogShell)))
    {
        selected_button = i;
        SyncPaletteSwatchBorders();

        color_set = (ColorSet *) &pCurrentPalette->color[selected_button];
        ColorEditor(style.colorDialog,color_set);

		XtAddCallback(edit.DialogShell, XmNcallback, colorEditorCB, nullptr);
    }
}

/*
 * Color editor callback: updates swatch pixels and live preview/polarity.
 */
static void colorEditorCB(Widget w, XtPointer client_data, XtPointer call_data)
{
	(void)w;
	(void)client_data;
	(void)call_data;
	if(!style.dynamicColor)
		allocNewColors();
	UpdateColorThemeSummaryAndPreview();
	XtRemoveCallback(edit.DialogShell, XmNcallback, colorEditorCB, nullptr);
}

/*
**  This is the double click timeout callback.  If this routine is called
**  then there was only a single click on a colorbutton
*/
static void 
timeoutCB(
        XtPointer client_data,
        XtIntervalId *id )
{
    int     n;
    intptr_t         i;
    Arg              args[2];
	Pixel	bg_pixel;

    if (TypeOfMonitor == XmCO_BLACK_WHITE)
        return;

    i = (intptr_t) client_data;

    if ((edit.DialogShell == nullptr) || (!XtIsManaged(edit.DialogShell)))
    {
        selected_button = i;
        SyncPaletteSwatchBorders();
    }
}

static void 
addPaletteCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{

    int     n;
    Arg              args[10];
    XmString         string;
    XmString         string1;

    if (addDialog == nullptr)
    {
        n = 0;

        XtSetArg(args[n], XmNokLabelString, CMPSTR((String) _DtOkString)); n++;
        XtSetArg(args[n], XmNcancelLabelString, CMPSTR((String) _DtCancelString)); n++;
        XtSetArg(args[n], XmNhelpLabelString, CMPSTR((String) _DtHelpString)); n++;
	string =  CMPSTR(((char *)GETMESSAGE(14, 10, "New palette name:")));
        XtSetArg(args[n], XmNselectionLabelString, string); n++;
        string1 =  CMPSTR("");
        XtSetArg(args[n], XmNtextString, string1); n++;
        XtSetArg(args[n], XmNborderWidth, 3); n++;
        XtSetArg(args[n], XmNautoUnmanage, False); n++;
        addDialog = XmCreatePromptDialog(style.colorDialog,"AddDialog",
                                                                      args, n);
        XmStringFree(string);
        XmStringFree(string1);

        XtAddCallback(addDialog, XmNokCallback, addOkCB, 
                                 (XtPointer) nullptr);
        XtAddCallback(addDialog, XmNcancelCallback, addCancelCB, 
                                 (XtPointer) nullptr);
        XtAddCallback(addDialog, XmNhelpCallback,
            HelpRequestCB, (XtPointer)HELP_ADD_PALETTE_DIALOG);

        n = 0;
        XtSetArg (args[n], XmNmwmInputMode,
                        MWM_INPUT_PRIMARY_APPLICATION_MODAL); n++;
        XtSetArg (args[n], XmNuseAsyncGeometry, True); n++;
        XtSetArg (args[n], XmNtitle, ((char *)GETMESSAGE(14, 11, "Add Palette"))); n++;
        XtSetArg (args[n], XmNmwmFunctions, DIALOG_MWM_FUNC); n++;
        XtSetValues (XtParent (addDialog), args, n);
    }
    n=0;
    string =  CMPSTR("");
    XtSetArg(args[n], XmNtextString, string); n++;
    XtSetValues(addDialog, args, n);
    XmStringFree(string);

    XtManageChild(addDialog);

}

static void 
addCancelCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
        XtUnmanageChild(addDialog);
}

/*
 * Create the empty palette file "<directory>dtXXXXXX<PALETTE_SUFFIX>" with
 * mkstemps(3) and return its name without the suffix in XtMalloc'ed storage,
 * or NULL if the file cannot be made.  The file exists (mode 0600, owned by
 * the caller) when this returns, so no other user can plant a file or a
 * symbolic link at the name before WriteOutPalette() opens it.
 */
static char *
CreatePaletteFile(
        const char *directory )
{
  const char *base = "dtXXXXXX";
  size_t dirlen = strlen(directory);
  size_t baselen = strlen(base);
  size_t suffixlen = strlen(PALETTE_SUFFIX);
  char *path, *name;
  int fd;

  path = (char *)XtMalloc(dirlen + baselen + suffixlen + 1);
  strcpy(path, directory);
  strcat(path, base);
  strcat(path, PALETTE_SUFFIX);

  fd = mkstemps(path, (int)suffixlen);
  if (fd == -1)
    {
      XtFree(path);
      return nullptr;
    }
  close(fd);

  name = (char *)XtMalloc(baselen + 1);
  memcpy(name, path + dirlen, baselen);
  name[baselen] = '\0';
  XtFree(path);
  return name;
}

static void 
addOkCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
  int     n, i;
  Arg              args[6];
  XmString         string;
  char             *name, *filename;
  palette         *tmpPalette, *newPalette;
  int              count;
  int              ii, length;

  /* Get the text from the promp dialog */
  name = XmTextFieldGetString( XmSelectionBoxGetChild(addDialog, XmDIALOG_TEXT));
  
  /* see if the user typed in a valid palette name */
  if(!ValidName(name)) {
    ErrDialog(((char *)GETMESSAGE(14, 12, "The palette name cannot contain\nthese characters:\n\n * : ( ) [ ] { } < > ! | \" / \\")), 
	      style.colorDialog); 
    XtFree(name);
    return;
  }
  
  /* check the number of characters in name in a locale independent way */
  for ( ii = 0, count = 0; name[ii] != '\0'; ii += length ) {
    length = mblen( &(name[ii]), MB_CUR_MAX );
    if ( length <=0 )
      break;
    count++;
  }
  
  /* make sure the length of name is ok, short file names can only be 11 chars */
  if(count > 10 && !style.longfilename) {
    ErrDialog(((char *)GETMESSAGE(14, 13, "The palette name must be\n10 characters or less.\n")),style.colorDialog);
    XtFree(name);
    return;
  }
  
  /* Unmanage the promptDialog */
  XtUnmanageChild(addDialog);
  
  /* first search through palette descriptions and make sure the name to */
  /* add is not already in the list and go to the end of the palette list */
  for(tmpPalette = pHeadPalette; tmpPalette->next != nullptr;
      tmpPalette = tmpPalette->next)
    {
      if((strcmp(tmpPalette->desc, name) == 0)) {
	SameName(w, tmpPalette, name);
	XtFree(name);
	return;
      }
    }
  
  /* Check the last palette */
  if((strcmp(tmpPalette->desc, name) == 0)) {
    SameName(w, tmpPalette, name);
    XtFree(name);
    return;
  }
  
  /* allocate space for a new palette */
  newPalette = (palette *)XtMalloc(sizeof(palette) + 1 );
  
  /* set the previous last palatte to this new one, it is now the last one*/
  tmpPalette->next = newPalette;
  newPalette->next = nullptr;
  
  /* malloc space for the new palette desc */
  newPalette->desc = (char *)XtMalloc(strlen(name) + 1);
  for(i = 0; (size_t)i < strlen(name); i++)
    newPalette->desc[i] = name[i];
  newPalette->desc[i] = '\0';
  
  /* malloc space for the new palette name directory */
  newPalette->directory = (char *)XtMalloc(strlen(style.home) +
					   strlen(DT_PAL_DIR) + 1);
  strcpy(newPalette->directory, style.home);
  strcat(newPalette->directory, DT_PAL_DIR);
  
  /* makeup a new name for the palette and create its file */
  newPalette->name = CreatePaletteFile(newPalette->directory);
  if (newPalette->name == nullptr)
    {
      _DtSimpleErrnoError(progName, DtWarning, nullptr,
			  (char *)GETMESSAGE(16, 20, "Could not open %s."),
			  newPalette->directory);

      /*  remove palette from list */
      tmpPalette->next = nullptr;
      XtFree(newPalette->directory);
      XtFree(newPalette->desc);
      XtFree((char *)newPalette);
      XtFree(name);
      return;
    }

  /* the new palette is the next palette .. increase NumOfPalettes by one */
  newPalette->item_position = NumOfPalettes + 1;
  NumOfPalettes++;
  
  /* set all the new palette's color parameters to the current palette */
  newPalette->num_of_colors = pCurrentPalette->num_of_colors;
  for(i = 0; i < XmCO_MAX_NUM_COLORS; i++)
    {
      newPalette->color[i].fg.pixel = pCurrentPalette->color[i].fg.pixel;
      newPalette->color[i].fg.red = pCurrentPalette->color[i].fg.red;
      newPalette->color[i].fg.green = pCurrentPalette->color[i].fg.green;
      newPalette->color[i].fg.blue = pCurrentPalette->color[i].fg.blue;
      newPalette->color[i].fg.flags = pCurrentPalette->color[i].fg.flags;
      
      newPalette->color[i].bg.pixel = pCurrentPalette->color[i].bg.pixel;
      newPalette->color[i].bg.red = pCurrentPalette->color[i].bg.red;
      newPalette->color[i].bg.green = pCurrentPalette->color[i].bg.green;
      newPalette->color[i].bg.blue = pCurrentPalette->color[i].bg.blue;
      newPalette->color[i].bg.flags = pCurrentPalette->color[i].bg.flags;
      
      newPalette->color[i].ts.pixel = pCurrentPalette->color[i].ts.pixel;
      newPalette->color[i].ts.red = pCurrentPalette->color[i].ts.red;
      newPalette->color[i].ts.green = pCurrentPalette->color[i].ts.green;
      newPalette->color[i].ts.blue = pCurrentPalette->color[i].ts.blue;
      newPalette->color[i].ts.flags = pCurrentPalette->color[i].ts.flags;
      
      newPalette->color[i].bs.pixel = pCurrentPalette->color[i].bs.pixel;
      newPalette->color[i].bs.red = pCurrentPalette->color[i].bs.red;
      newPalette->color[i].bs.green = pCurrentPalette->color[i].bs.green;
      newPalette->color[i].bs.blue = pCurrentPalette->color[i].bs.blue;
      newPalette->color[i].bs.flags = pCurrentPalette->color[i].bs.flags;
      
      newPalette->color[i].sc.pixel = pCurrentPalette->color[i].sc.pixel;
      newPalette->color[i].sc.red = pCurrentPalette->color[i].sc.red;
      newPalette->color[i].sc.green = pCurrentPalette->color[i].sc.green;
      newPalette->color[i].sc.blue = pCurrentPalette->color[i].sc.blue;
      newPalette->color[i].sc.flags = pCurrentPalette->color[i].sc.flags;
    }
  
  /* Write out the palette */
  if ((WriteOutPalette(newPalette->name)) == -1)
    {
      XtFree(name);
      
      /*  remove palette from list */
      tmpPalette->next = nullptr;
      XtFree ((char *)newPalette);
      
      return;
    }
  else
    WriteOutDesc(newPalette);
      
    
  /* add the name to the scrolled window list and select it */
  AddName(newPalette);
  
  /* now check to see if there is a ~filename .. if there is delete it */
  /* use the $HOME environment varible then constuct the full file name */
  filename = (char *)XtMalloc(strlen(style.home) + strlen(DT_PAL_DIR) +
			      strlen(newPalette->name) + strlen(PALETTE_SUFFIX) + 2);
  
  /* create the full path name plus file name */
  strcpy(filename, style.home);
  strcat(filename, DT_PAL_DIR);
  strcat(filename, "~");
  strcat(filename, newPalette->name);
  strcat(filename, PALETTE_SUFFIX);
  
  unlink(filename);
  
  XtFree(filename);
  XtFree(name);
  
  /* Go write out the palette */
  pCurrentPalette = newPalette;
  
}

static void 
setDlgOkCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
   palette *tmpPalette = (palette *)client_data;
   int i;

  /* free the directory */
   XtFree(tmpPalette->directory);

  /* put the new (users) directory there */
   tmpPalette->directory = (char *)XtMalloc(strlen(style.home) +
                                          strlen(DT_PAL_DIR) + 1);
   strcpy(tmpPalette->directory, style.home);
   strcat(tmpPalette->directory, DT_PAL_DIR);

   for(i = 0; i < XmCO_MAX_NUM_COLORS; i++)
   {
       tmpPalette->color[i].fg.pixel = pCurrentPalette->color[i].fg.pixel;
       tmpPalette->color[i].fg.red = pCurrentPalette->color[i].fg.red;
       tmpPalette->color[i].fg.green = pCurrentPalette->color[i].fg.green;
       tmpPalette->color[i].fg.blue = pCurrentPalette->color[i].fg.blue;
       tmpPalette->color[i].fg.flags = pCurrentPalette->color[i].fg.flags;

       tmpPalette->color[i].bg.pixel = pCurrentPalette->color[i].bg.pixel;
       tmpPalette->color[i].bg.red = pCurrentPalette->color[i].bg.red;
       tmpPalette->color[i].bg.green = pCurrentPalette->color[i].bg.green;
       tmpPalette->color[i].bg.blue = pCurrentPalette->color[i].bg.blue;
       tmpPalette->color[i].bg.flags = pCurrentPalette->color[i].bg.flags;

       tmpPalette->color[i].ts.pixel = pCurrentPalette->color[i].ts.pixel;
       tmpPalette->color[i].ts.red = pCurrentPalette->color[i].ts.red;
       tmpPalette->color[i].ts.green = pCurrentPalette->color[i].ts.green;
       tmpPalette->color[i].ts.blue = pCurrentPalette->color[i].ts.blue;
       tmpPalette->color[i].ts.flags = pCurrentPalette->color[i].ts.flags;

       tmpPalette->color[i].bs.pixel = pCurrentPalette->color[i].bs.pixel;
       tmpPalette->color[i].bs.red = pCurrentPalette->color[i].bs.red;
       tmpPalette->color[i].bs.green = pCurrentPalette->color[i].bs.green;
       tmpPalette->color[i].bs.blue = pCurrentPalette->color[i].bs.blue;
       tmpPalette->color[i].bs.flags = pCurrentPalette->color[i].bs.flags;

       tmpPalette->color[i].sc.pixel = pCurrentPalette->color[i].sc.pixel;
       tmpPalette->color[i].sc.red = pCurrentPalette->color[i].sc.red;
       tmpPalette->color[i].sc.green = pCurrentPalette->color[i].sc.green;
       tmpPalette->color[i].sc.blue = pCurrentPalette->color[i].sc.blue;
       tmpPalette->color[i].sc.flags = pCurrentPalette->color[i].sc.flags;
   }

  /* Write out the palette */
   if ((WriteOutPalette(tmpPalette->name)) == -1)
      return;
   else
     WriteOutDesc(tmpPalette);

   pCurrentPalette = tmpPalette;

  /* select item in list as if user had selected it */
   XmListSelectPos (paletteList, tmpPalette->item_position, TRUE);
   XmListSetBottomPos(paletteList, tmpPalette->item_position);
   selected_position = tmpPalette->item_position;

}

static void 
modifyColorCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
    ColorSet *color_set;

    if(TypeOfMonitor == XmCO_BLACK_WHITE)
       return;
        
    color_set = (ColorSet *) &pCurrentPalette->color[selected_button];
    ColorEditor(style.colorDialog,color_set);

	XtAddCallback(edit.DialogShell, XmNcallback, colorEditorCB, nullptr);
}


/*
**  dialogBoxCB
**      Process callback from the Ok, Apply, Cancel and Help pushButtons in the 
**      DialogBox.
*/
static void 
dialogBoxCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
  palette *tmp_palette;
  Bool match = FALSE;
  DtDialogBoxCallbackStruct *cb = (DtDialogBoxCallbackStruct *) call_data;
  
  switch (cb->button_position)
    {
    case COLOR_BTN_OK:
      if ((edit.DialogShell != nullptr) && (XtIsManaged(edit.DialogShell)))
	XtUnmanageChild(edit.DialogShell);
      
      XtUnmanageChild(style.colorDialog);
      UpdateDefaultPalette();
      g_appliedSinceOpen = False;
      break;

    case COLOR_BTN_APPLY:
      if ((edit.DialogShell != nullptr) && (XtIsManaged(edit.DialogShell)))
	XtUnmanageChild(edit.DialogShell);
      ApplyColorAndThemeToDesktop(True);
      g_appliedSinceOpen = True;
      UpdateColorThemeSummaryAndPreview();
      break;
      
    case COLOR_BTN_CANCEL:
      if ((edit.DialogShell != nullptr) && (XtIsManaged(edit.DialogShell)))
	XtUnmanageChild(edit.DialogShell);
      
      if ((addDialog != nullptr) && (XtIsManaged(addDialog)))
	XtCallCallbacks(addDialog, XmNcancelCallback, (XtPointer)nullptr);
      
      if ((deleteDialog != nullptr) && (XtIsManaged(deleteDialog)))
	XtCallCallbacks(deleteDialog, XmNcancelCallback, (XtPointer)nullptr);
      
      if ((colorUseDialog != nullptr) && (XtIsManaged(colorUseDialog)))
	XtCallCallbacks(colorUseDialog, XmNcallback, (XtPointer)nullptr);
      
      XtUnmanageChild(style.colorDialog);
      
      tmp_palette = pHeadPalette;
      const char *target_orig = g_origPaletteName[0] ? g_origPaletteName : defaultName;
      for(tmp_palette = pHeadPalette; tmp_palette != nullptr; tmp_palette = tmp_palette->next)
	if(!(strcmp(tmp_palette->name, target_orig)))
	  {
	    match = TRUE;
	    break;
	  }
      
      if(match == FALSE)  /* the original palette was deleted */
	UpdateDefaultPalette();
      else 
	RestoreOrgPalette();
      
      break;
      
    case COLOR_BTN_HELP:
      XtCallCallbacks(style.colorDialog, XmNhelpCallback, (XtPointer)nullptr);
      break;
      
    default:
      break;
    }
}


static void 
AddName(
        palette *newPalette )
{
    XmString         string;

/*
**  Add the palette name to the list
*/
    string = CMPSTR(newPalette->desc);
    XmListAddItem(paletteList, string, newPalette->item_position);
    XmListSelectPos(paletteList, newPalette->item_position, TRUE);
    XmListSetBottomPos(paletteList, newPalette->item_position);
    selected_position = newPalette->item_position;
    XSync(style.display, 0);
    XmStringFree(string);
}

static void 
deletePaletteCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
    int     n;
    Arg              args[10];
    char            *tmpStr;
    palette         *tmp_palette;
    char            *string;
    char            *class_str;
    char            *str_type_return;
    XrmValue         value_return;

    tmp_palette = pHeadPalette;

    while( tmp_palette != nullptr
           && tmp_palette->item_position != selected_position ) {
	 tmp_palette = tmp_palette->next;
    }
    
    if (deleteDialog == nullptr)
    {
	n=0;
        XtSetArg(args[n], XmNokLabelString, CMPSTR((String) _DtOkString)); n++;
        XtSetArg(args[n], XmNcancelLabelString, CMPSTR((String) _DtCancelString)); n++;
        XtSetArg(args[n], XmNhelpLabelString, CMPSTR((String) _DtHelpString)); n++;
	XtSetArg(args[n], XmNdialogType, XmDIALOG_INFORMATION);             n++;
	XtSetArg(args[n], XmNborderWidth, 3);                               n++;
        XtSetArg(args[n], XmNdefaultPosition, False);                      n++;
	deleteDialog = XmCreateQuestionDialog(style.colorDialog,
                                              "deleteDialog", args, n);
        XtAddCallback(deleteDialog, XmNmapCallback, CenterMsgCB,
                      style.colorDialog);
	XtAddCallback(deleteDialog, XmNcancelCallback, deleteCancelCB, nullptr);
	XtAddCallback(deleteDialog, XmNokCallback, deleteOkCB, 
                      (XtPointer)(paletteList));
        XtAddCallback(deleteDialog, XmNhelpCallback,
                      HelpRequestCB,
                      (XtPointer)HELP_DELETE_PALETTE_WARNING_DIALOG);


        n = 0;
        XtSetArg (args[n], XmNmwmInputMode,
                        MWM_INPUT_PRIMARY_APPLICATION_MODAL); n++;
        XtSetArg (args[n], XmNuseAsyncGeometry, True); n++;
        XtSetArg (args[n], XmNtitle, ((char *)GETMESSAGE(14, 16, "Delete Palette"))); n++;
        XtSetArg (args[n], XmNmwmFunctions, DIALOG_MWM_FUNC); n++;
        XtSetValues (XtParent (deleteDialog), args, n);

    }

    n = 0;
    tmpStr = XtMalloc(strlen(STR2) + strlen(tmp_palette->desc) + 1);
    sprintf(tmpStr, STR2, tmp_palette->desc);
    XtSetArg(args[n], XmNmessageString, CMPSTR(tmpStr)); n++;
    XtSetValues(deleteDialog, args, n);
    XtFree (tmpStr);

    XtManageChild(deleteDialog);
}

static void 
deleteOkCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{

    XtUnmanageChild(deleteDialog);

    if(NumOfPalettes == 1)
    {
       InfoDialog(CANT_DELETE, style.colorDialog, False);
    } 
    else
    {
       if (RemovePalette() == True)
           DeletePaletteFromLinkList((Widget)client_data);
    }
}

static void 
deleteCancelCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
        XtUnmanageChild(deleteDialog);
}

static void 
resourcesCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{

    int     n;
    Arg              args[12];
    XmString         button_string[NUM_LABELS]; 
    XmString	     string;
    Widget           parent = (Widget) client_data;
    Widget           colorUseTB;
    Widget           form;
    Widget           widget_list[10];
    int              count=0;
    Widget           pictLabel;
    Widget           colorUseRC;

    if (colorUseDialog == nullptr)
    {
        n = 0;

        /* Set up DialogBox button labels. */
        button_string[0] = CMPSTR((String) _DtOkString);
        button_string[1] = CMPSTR((String) _DtCancelString);
        button_string[2] = CMPSTR((String) _DtHelpString);

        XtSetArg (args[n], XmNchildType, XmWORK_AREA);  n++;
        XtSetArg (args[n], XmNbuttonCount, NUM_LABELS);  n++;
        XtSetArg (args[n], XmNbuttonLabelStrings, button_string);  n++;
        XtSetArg (args[n], XmNdefaultPosition, False);  n++;
        colorUseDialog = __DtCreateDialogBoxDialog(parent, "colorUseDialog", 
                          args, n);
        XtAddCallback(colorUseDialog, XmNcallback, colorUseExitCB, nullptr);
        XtAddCallback(colorUseDialog, XmNmapCallback, _DtmapCB_colorUse, nullptr);
        XtAddCallback(colorUseDialog, XmNhelpCallback,
            HelpRequestCB, (XtPointer)HELP_COLOR_USE_DIALOG);

        XmStringFree(button_string[0]);
        XmStringFree(button_string[1]);
        XmStringFree(button_string[2]);

        widget_list[0] = _DtDialogBoxGetButton(colorUseDialog,2);
        n=0;
        XtSetArg(args[n], XmNautoUnmanage, False); n++;
        XtSetArg(args[n], XmNcancelButton, widget_list[0]); n++;
        XtSetValues (colorUseDialog, args, n);

        n = 0;
        XtSetArg (args[n], XmNuseAsyncGeometry, True); n++;
        XtSetArg (args[n], XmNtitle, 
            ((char *)GETMESSAGE(14, 39, "Number Of Colors To Use:"))); n++;
        XtSetArg (args[n], XmNmwmFunctions, DIALOG_MWM_FUNC); n++;
        XtSetValues (XtParent(colorUseDialog), args, n);

        n = 0;
        XtSetArg(args[n], XmNhorizontalSpacing, style.horizontalSpacing); n++;
        XtSetArg(args[n], XmNverticalSpacing, style.verticalSpacing); n++;
        XtSetArg(args[n], XmNallowOverlap, False); n++;
        XtSetArg(args[n], XmNchildType, XmWORK_AREA);  n++;
        form = XmCreateForm(colorUseDialog, "colorUseForm", args, n);
        XtManageChild(form);

        n = 0;
        XtSetArg (args[n], XmNtopAttachment, XmATTACH_FORM);  n++;
        XtSetArg (args[n], XmNleftAttachment, XmATTACH_FORM);  n++;
        XtSetArg (args[n], XmNrightAttachment, XmATTACH_NONE);  n++;
        XtSetArg (args[n], XmNbottomAttachment, XmATTACH_NONE);  n++;
        XtSetArg (args[n], XmNbehavior, XmICON_LABEL); n++;
        XtSetArg (args[n], XmNshadowThickness, 0); n++;  
        XtSetArg (args[n], XmNstring, NULL); n++;  
        XtSetArg (args[n], XmNpixmapForeground, style.primBSCol); n++;
        XtSetArg (args[n], XmNpixmapBackground, style.primTSCol); n++;
        XtSetArg (args[n], XmNimageName, COLOR_ICON); n++;  
        XtSetArg (args[n], XmNtraversalOn, False); n++;  
        pictLabel = _DtCreateIcon(form, "pictLabel", args, n);
        XtManageChild(pictLabel);

        n = 0;
        XtSetArg (args[n], XmNtopAttachment, XmATTACH_WIDGET);  n++;
        XtSetArg (args[n], XmNtopWidget, pictLabel);  n++;
        XtSetArg (args[n], XmNtopOffset, style.verticalSpacing);  n++;
        XtSetArg (args[n], XmNleftAttachment, XmATTACH_FORM);  n++;
        XtSetArg (args[n], XmNrightAttachment, XmATTACH_FORM);  n++;
        XtSetArg (args[n], XmNbottomAttachment, XmATTACH_FORM);  n++;
        colorUseTB 
            = XmCreateFrame(form, "colorUseTB", args, n);
        XtManageChild(colorUseTB);

        /* create a rowColumn for ColorUse selections */
        n = 0;
        colorUseRC = XmCreateRadioBox(colorUseTB, "colorUseRC", args, n);
        XtManageChild(colorUseRC);

        n = 0;
	string = CMPSTR(((char *)GETMESSAGE(14, 41, "More Colors for Desktop")));
        XtSetArg(args[n], XmNlabelString, string); n++;
	widget_list[count++] = colorDialog.highColorTG 
            =  XmCreateToggleButtonGadget(colorUseRC,"highColorTG", args, n);
        XtAddCallback(colorDialog.highColorTG, XmNvalueChangedCallback, 
                        colorUseCB, (XtPointer)XmCO_HIGH_COLOR);  
        XmStringFree(string);

        n = 0;
	string = CMPSTR(((char *)GETMESSAGE(14, 42, "More Colors for Applications")));
        XtSetArg(args[n], XmNlabelString, string); n++;
        widget_list[count++] = colorDialog.mediumColorTG 
            = XmCreateToggleButtonGadget(colorUseRC,"mediumColorTG", args, n);
        XmStringFree(string);
        XtAddCallback(colorDialog.mediumColorTG, XmNvalueChangedCallback, 
                        colorUseCB, (XtPointer)XmCO_MEDIUM_COLOR);  

        n = 0;
	string = CMPSTR(((char *)GETMESSAGE(14, 43, "Most Colors for Applications")));
        XtSetArg(args[n], XmNlabelString, string); n++;
        widget_list[count++] = colorDialog.lowColorTG 
            = XmCreateToggleButtonGadget(colorUseRC,"lowColorTG", args, n);
        XmStringFree(string);
        XtAddCallback(colorDialog.lowColorTG, XmNvalueChangedCallback, 
                        colorUseCB, (XtPointer)XmCO_LOW_COLOR);  

        n = 0;
	string = CMPSTR(((char *)GETMESSAGE(14, 34, "Black and White")));
        XtSetArg(args[n], XmNlabelString, string); n++;
        widget_list[count++] = colorDialog.blackWhiteTG 
            = XmCreateToggleButtonGadget(colorUseRC,"blackWhiteTG", args, n);
        XmStringFree(string);
        XtAddCallback(colorDialog.blackWhiteTG, XmNvalueChangedCallback, 
                        colorUseCB, (XtPointer)XmCO_BLACK_WHITE);  

        n = 0;
	string = CMPSTR(((char *)GETMESSAGE(14, 35, "Default")));
        XtSetArg(args[n], XmNlabelString, string); n++;
        widget_list[count++] = colorDialog.defaultTG 
            = XmCreateToggleButtonGadget(colorUseRC,"defaultTG", args, n);
        XmStringFree(string);
        XtAddCallback(colorDialog.defaultTG, XmNvalueChangedCallback, 
                        colorUseCB, (XtPointer)DEFAULT_COLOR);  

        XtManageChildren(widget_list,count);
        putDialog (XtParent(style.colorDialog), colorUseDialog); 
    }

    XtManageChild(colorUseDialog);
}

/*
**  colorUseCB
**      Process new ColorUse selection
*/
static void 
colorUseCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
    Arg              args[4];
    XmToggleButtonCallbackStruct *cb = 
            (XmToggleButtonCallbackStruct *)call_data;

    colorDialog.currentColorUse = (int) (intptr_t) client_data;
    switch (colorDialog.currentColorUse)
    {
        case XmCO_HIGH_COLOR:
            colorDialog.currentColorUseStr = HIGH_COLOR_STR;
            break;

        case XmCO_MEDIUM_COLOR:
            colorDialog.currentColorUseStr = MEDIUM_COLOR_STR;
            break;

        case XmCO_LOW_COLOR:
            colorDialog.currentColorUseStr = LOW_COLOR_STR;
            break;

        case XmCO_BLACK_WHITE:
            colorDialog.currentColorUseStr = B_W_STR;
            break;

        case DEFAULT_COLOR:
            colorDialog.currentColorUseStr = DEFAULT_COLOR_STR;
            break;
    }


}


/*
**  colorUseExitCB
**      Process callback from the Ok, Cancel and Help pushButtons in the 
**      Configure DT Colors DialogBox.
*/
static void 
colorUseExitCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{

    char        colorUseRes[64];
    DtDialogBoxCallbackStruct *cb = (DtDialogBoxCallbackStruct *) call_data;

    switch (cb->button_position)
    {
      case HELP_BUTTON:
          XtCallCallbacks(colorUseDialog, XmNhelpCallback, (XtPointer)nullptr);
          break;

      case OK_BUTTON:

         XtUnmanageChild(colorUseDialog);

         if (colorDialog.origColorUse != colorDialog.currentColorUse)
         {
             InfoDialog(COLORUSE_WHEN, style.colorDialog, False); 

             /* create the ColorUse resource spec for xrdb */
             /* remove ColorUse specification from database for DEFAULT_COLOR */

             sprintf(colorUseRes, "*%d*ColorUse: %s\n", 
                 style.screenNum,colorDialog.currentColorUseStr);

             switch (colorDialog.currentColorUse)
             {
                case XmCO_MEDIUM_COLOR:
                    sprintf(colorUseRes+strlen(colorUseRes), 
                             "*HelpColorUse: GRAY_SCALE\n");

                    break;

                case XmCO_LOW_COLOR:
                case XmCO_BLACK_WHITE:
                    sprintf(colorUseRes+strlen(colorUseRes), 
                             "*HelpColorUse: B_W\n");

                    break;

                case XmCO_HIGH_COLOR:
                case DEFAULT_COLOR:
                default:
                    sprintf(colorUseRes+strlen(colorUseRes), 
                             "*HelpColorUse: COLOR\n");
                    break;
             }

             _DtAddToResource(style.display, colorUseRes);

             colorDialog.origColorUse = colorDialog.currentColorUse;
         }

         break;

      case CANCEL_BUTTON:
      default:

         XtUnmanageChild(colorUseDialog);
         
         switch (colorDialog.origColorUse)
         {
           case XmCO_HIGH_COLOR:
             XmToggleButtonGadgetSetState(colorDialog.highColorTG, True, True);
             break;

           case XmCO_MEDIUM_COLOR:
             XmToggleButtonGadgetSetState(colorDialog.mediumColorTG, True, True);
             break;

            case XmCO_LOW_COLOR:
             XmToggleButtonGadgetSetState(colorDialog.lowColorTG, True, True);
             break;

            case XmCO_BLACK_WHITE:
             XmToggleButtonGadgetSetState(colorDialog.blackWhiteTG, True, True);
             break;

            case DEFAULT_COLOR:
             XmToggleButtonGadgetSetState(colorDialog.defaultTG, True, True);
             break;
          }
          break;

    }
}


static void 
activateCBexitColor(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
  DtDialogBoxCallbackStruct CancelBut;

  if(style.colorDialog != nullptr && XtIsManaged(style.colorDialog)) {
     CancelBut.button_position = COLOR_BTN_CANCEL;
     XtCallCallbacks(style.colorDialog, XmNcallback, &CancelBut);
  }
}

static void 
_DtmapCB(
        Widget w,
        XtPointer client_data,

        XtPointer call_data )
{

    DtWsmRemoveWorkspaceFunctions(style.display, XtWindow(XtParent(w)));

    if (!save.restoreFlag)
        putDialog ((Widget)client_data, XtParent(w));

    XtRemoveCallback(style.colorDialog, XmNmapCallback, _DtmapCB, nullptr);
}

static void 
_DtmapCB_colorUse(
        Widget w,
        XtPointer client_data,

        XtPointer call_data )
{

    char *str_type_return;
    XrmValue value_return;
    XrmValue    cvt_value;
    XrmDatabase db;
    Boolean status;
    char *string;
    char instanceString[24], nameString[24];

    DtWsmRemoveWorkspaceFunctions(style.display, XtWindow(XtParent(w)));

    db = XtDatabase(style.display);

    /* Get ColorUse value */
    sprintf (instanceString, "dtsession*%d*colorUse",style.screenNum);
    sprintf (nameString, "Dtsession*%d*ColorUse",style.screenNum);

    if (status = XrmGetResource (db, instanceString,
                                 nameString,
                                 &str_type_return, &value_return))
    {
        /* make local copy of string */
        string = (char *) XtMalloc( strlen(value_return.addr) + 1 );
        strcpy (string, value_return.addr);

        if (strcmp(string, HIGH_COLOR_STR) == 0)
        {
            XmToggleButtonGadgetSetState (colorDialog.highColorTG, True, True); 
            colorDialog.origColorUse = XmCO_HIGH_COLOR;
        }
        else if (strcmp(string, MEDIUM_COLOR_STR) == 0)
        {
            XmToggleButtonGadgetSetState (colorDialog.mediumColorTG, True, True); 
            colorDialog.origColorUse = XmCO_MEDIUM_COLOR;
        }
        else if (strcmp(string, LOW_COLOR_STR) == 0)
        {
            XmToggleButtonGadgetSetState (colorDialog.lowColorTG, True, True); 
            colorDialog.origColorUse = XmCO_LOW_COLOR;
        }
        else if (strcmp(string, B_W_STR) == 0)
        {
            XmToggleButtonGadgetSetState (colorDialog.blackWhiteTG, True, True); 
            colorDialog.origColorUse = XmCO_BLACK_WHITE;
        }
        else 
        {
            XmToggleButtonGadgetSetState (colorDialog.defaultTG, True, True); 
            colorDialog.origColorUse = DEFAULT_COLOR;
        }

        XtFree (string);
    }
    else /* ColorUse not specified */
    {
        XmToggleButtonGadgetSetState (colorDialog.defaultTG, True, True); 
        colorDialog.origColorUse = DEFAULT_COLOR;
    } 

    XtRemoveCallback(colorUseDialog, XmNmapCallback, _DtmapCB_colorUse, nullptr);
}

/************************************************************************
 *
 *  DeletePaletteFromLinkList - routine used to delete a palette from
 *       the link list of palettes.  The palette which is at the current
 *       selected_position is the palette that is going to be deleted.
 *       Special things have to happen if the selected palette is at the
 *       head of the list.
 *
 ************************************************************************/ 
void
DeletePaletteFromLinkList(
        Widget list )
{
    int n;
    Arg args[2];
    int i;
    XmString        string; 
    palette        *tmp_palette, *tmp2_palette;
    palette        *selected_palette;


    selected_palette = pHeadPalette;
    while( selected_palette != nullptr
           && selected_palette->item_position != selected_position ) {
	 selected_palette = selected_palette->next;
    }

    if (selected_palette == nullptr)
        return;		/* no palette at the selected position */

    XmListDeletePos (list, selected_palette->item_position);

    /* delete item from palette list structure */

    /* If the palette is at the head .. remove the head and the next
         palette becomes the new selected palette */
    if (selected_palette->item_position == 1)
    {
        pHeadPalette = selected_palette->next;
        tmp_palette = pHeadPalette;
        tmp_palette->item_position--;
       /* new current palette */
        pCurrentPalette = tmp_palette;
    }
    else  /* find the palette just above the palette to be deleted .. it
             will become the new selected palette */
    {
        tmp_palette = pHeadPalette;
        for (i=1; i < selected_palette->item_position-1; i++)
            tmp_palette = tmp_palette->next;

        tmp_palette->next = selected_palette->next;

       /* what is CurrentPalette now? prev or next item?
           special case empty list or NULL entry */
       if (tmp_palette->next != nullptr)
           pCurrentPalette = tmp_palette->next;
       else
           pCurrentPalette = tmp_palette;
    }

    /* decrement item_positions values in remaining palette entries */
    tmp2_palette = tmp_palette;
    while ((tmp2_palette = tmp2_palette->next) != nullptr)
    {
        tmp2_palette->item_position--;
    }

   /* go copy the pixel numbers to the new palette */
    CopyPixel(selected_palette->color, pCurrentPalette->color,
                                      selected_palette->num_of_colors);

    /* select item in list as if user had selected it */
    XmListSelectPos (list, tmp_palette->item_position, TRUE);
    /* Need to check to see if the first palette is being deleted if it is
       need to change colors and update title box */
    if(selected_position == tmp_palette->item_position) {
       pOldPalette = selected_palette;

       n=0;
       string = CMPSTR(pCurrentPalette->desc);
       XtSetArg (args[n], XmNtitleString, string); n++;
       XtSetValues (colorDialog.buttonsTB, args, n);
       XmStringFree(string);

       ReColorPalette();
    }

    XmListSetBottomPos(paletteList, tmp_palette->item_position);
    selected_position = tmp_palette->item_position;

    NumOfPalettes--;

   /* deallocate the palette structure */
    XtFree(selected_palette->name);
    XtFree(selected_palette->desc);
    XtFree(selected_palette->directory);
    XtFree((char *)selected_palette);
}

void
CopyPixel(
        ColorSet srcPixels[XmCO_MAX_NUM_COLORS],
        ColorSet dstPixels[XmCO_MAX_NUM_COLORS],
	int numOfColors )
{
   int i;

   for(i=0; i < numOfColors; i++)
   {
      dstPixels[i].bg.pixel = srcPixels[i].bg.pixel;
      dstPixels[i].fg.pixel = srcPixels[i].fg.pixel;
      dstPixels[i].ts.pixel = srcPixels[i].ts.pixel;
      dstPixels[i].bs.pixel = srcPixels[i].bs.pixel;
      dstPixels[i].sc.pixel = srcPixels[i].sc.pixel;
   }
}

void 
SaveOrgPalette( void )
{
   int i;
   palette  *tmp_palette = nullptr, *tmp2_palette = nullptr;

   if(save.restoreFlag && defaultName_restore[0] != 0) {
      tmp_palette = pHeadPalette;
      while(tmp_palette->next != nullptr )
         if(strcmp(tmp_palette->name, defaultName))
         {
            tmp_palette = tmp_palette->next;
         }
         else
         {
             break;
         }
      if(!strcmp(tmp_palette->name, defaultName)) {
         tmp2_palette = pCurrentPalette;
         pCurrentPalette = tmp_palette;
      }
      else
         tmp_palette = nullptr;
   }

   OrgPalette.item_position = pCurrentPalette->item_position;
   OrgPalette.num_of_colors = pCurrentPalette->num_of_colors;
   for(i = 0; i < XmCO_MAX_NUM_COLORS; i++)
   {
       OrgPalette.primary = pCurrentPalette->primary;
       OrgPalette.secondary = pCurrentPalette->secondary;
       OrgPalette.active = pCurrentPalette->active;
       OrgPalette.inactive = pCurrentPalette->inactive;

       if(save.restoreFlag && defaultName_restore[0] != 0)
          OrgPalette.color[i].bg.pixel = tmp2_palette->color[i].bg.pixel;
       else
          OrgPalette.color[i].bg.pixel = pCurrentPalette->color[i].bg.pixel;
       OrgPalette.color[i].bg.red = pCurrentPalette->color[i].bg.red;
       OrgPalette.color[i].bg.green = pCurrentPalette->color[i].bg.green;
       OrgPalette.color[i].bg.blue = pCurrentPalette->color[i].bg.blue;

       if(save.restoreFlag && defaultName_restore[0] != 0)
          OrgPalette.color[i].fg.pixel = tmp2_palette->color[i].fg.pixel;
       else
          OrgPalette.color[i].fg.pixel = pCurrentPalette->color[i].fg.pixel;
       OrgPalette.color[i].fg.red = pCurrentPalette->color[i].fg.red;
       OrgPalette.color[i].fg.green = pCurrentPalette->color[i].fg.green;
       OrgPalette.color[i].fg.blue = pCurrentPalette->color[i].fg.blue;

       if(save.restoreFlag && defaultName_restore[0] != 0)
          OrgPalette.color[i].ts.pixel = tmp2_palette->color[i].ts.pixel;
       else
          OrgPalette.color[i].ts.pixel = pCurrentPalette->color[i].ts.pixel;
       OrgPalette.color[i].ts.red = pCurrentPalette->color[i].ts.red;
       OrgPalette.color[i].ts.green = pCurrentPalette->color[i].ts.green;
       OrgPalette.color[i].ts.blue = pCurrentPalette->color[i].ts.blue;

       if(save.restoreFlag && defaultName_restore[0] != 0)
          OrgPalette.color[i].bs.pixel = tmp2_palette->color[i].bs.pixel;
       else
          OrgPalette.color[i].bs.pixel = pCurrentPalette->color[i].bs.pixel;
       OrgPalette.color[i].bs.red = pCurrentPalette->color[i].bs.red;
       OrgPalette.color[i].bs.green = pCurrentPalette->color[i].bs.green;
       OrgPalette.color[i].bs.blue = pCurrentPalette->color[i].bs.blue;

       if(save.restoreFlag && defaultName_restore[0] != 0)
          OrgPalette.color[i].sc.pixel = tmp2_palette->color[i].sc.pixel;
       else
          OrgPalette.color[i].sc.pixel = pCurrentPalette->color[i].sc.pixel;
       OrgPalette.color[i].sc.red = pCurrentPalette->color[i].sc.red;
       OrgPalette.color[i].sc.green = pCurrentPalette->color[i].sc.green;
       OrgPalette.color[i].sc.blue = pCurrentPalette->color[i].sc.blue;
   }

   if(save.restoreFlag && defaultName_restore[0] != 0) 
      if(tmp_palette != nullptr)
         pCurrentPalette = tmp2_palette;

   if (pCurrentPalette && pCurrentPalette->name) {
       snprintf(g_origPaletteName, sizeof(g_origPaletteName), "%s", pCurrentPalette->name);
   } else if (defaultName) {
       snprintf(g_origPaletteName, sizeof(g_origPaletteName), "%s", defaultName);
   }
   g_origWidgetStyle = g_currentWidgetStyle;
   g_origDensity = g_currentDensity;
   g_appliedSinceOpen = False;

}

static void
ApplyColorAndThemeToDesktop(Boolean restartWmAndPanel)
{
    if (!style.display || !pCurrentPalette || !pCurrentPalette->name) return;

    XmAppearanceProfile profile =
        DtstyleResolveMotifProfile(pCurrentPalette, g_currentWidgetStyle);
    const char *prof_str = DtstyleProfileNameString(profile);
    const char *dens_str = DtstyleDensityNameString(g_currentDensity);

    int act_idx = (pCurrentPalette->active >= 0 &&
                   pCurrentPalette->active < pCurrentPalette->num_of_colors)
                      ? pCurrentPalette->active
                      : 0;

    char acc_hex[16];
    snprintf(acc_hex, sizeof(acc_hex), "#%02X%02X%02X",
             (unsigned int)(pCurrentPalette->color[act_idx].bg.red >> 8),
             (unsigned int)(pCurrentPalette->color[act_idx].bg.green >> 8),
             (unsigned int)(pCurrentPalette->color[act_idx].bg.blue >> 8));

    /* Publish updated PIXEL_SET first so live listeners on XA_RESOURCE_MANAGER
     * immediately observe the new palette pixels on CUSTOMIZE_DATA. */
    DtstylePublishLivePixelSet(pCurrentPalette);

    char xrdb_string[1024];
    int pos = 0;
    if (TypeOfMonitor == XmCO_BLACK_WHITE) {
        pos += snprintf(xrdb_string + pos, sizeof(xrdb_string) - (size_t)pos,
                        "*%d*MonochromePalette: %s%s\n"
                        "*appearanceProfile: %s\n"
                        "*appearanceDensity: %s\n"
                        "*ckCoreMotif.accentColor: %s\n"
                        "*ckCoreMotif.surfaceBackground:\n",
                        style.screenNum, pCurrentPalette->name, PALETTE_SUFFIX,
                        prof_str, dens_str, acc_hex);
    } else {
        pos += snprintf(xrdb_string + pos, sizeof(xrdb_string) - (size_t)pos,
                        "*%d*ColorPalette: %s%s\n"
                        "*appearanceProfile: %s\n"
                        "*appearanceDensity: %s\n"
                        "*ckCoreMotif.accentColor: %s\n"
                        "*ckCoreMotif.surfaceBackground:\n",
                        style.screenNum, pCurrentPalette->name, PALETTE_SUFFIX,
                        prof_str, dens_str, acc_hex);
    }

    XtFree(defaultName);
    defaultName = (char *)XtMalloc(strlen(pCurrentPalette->name) + 1);
    strcpy(defaultName, pCurrentPalette->name);

    if (style.xrdb.writeXrdbColors) {
#ifdef sun
        OWsyncColorResources(style.display, TypeOfMonitor, pCurrentPalette->color);
#else
        int textchip = (TypeOfMonitor == XmCO_HIGH_COLOR ||
                        TypeOfMonitor == XmCO_MEDIUM_COLOR) ? 3 : 0;
        int chipnum = (TypeOfMonitor == XmCO_HIGH_COLOR) ? 4 : 1;
        pos += snprintf(
            xrdb_string + pos, sizeof(xrdb_string) - (size_t)pos,
            "*DtTerm*background: #%04X%04X%04X\n"
            "*DtTerm*foreground: #%04X%04X%04X\n"
            "*background: #%04X%04X%04X\n"
            "*foreground: #%04X%04X%04X\n",
            pCurrentPalette->color[textchip].bg.red,
            pCurrentPalette->color[textchip].bg.green,
            pCurrentPalette->color[textchip].bg.blue,
            pCurrentPalette->color[textchip].fg.red,
            pCurrentPalette->color[textchip].fg.green,
            pCurrentPalette->color[textchip].fg.blue,
            pCurrentPalette->color[chipnum].bg.red,
            pCurrentPalette->color[chipnum].bg.green,
            pCurrentPalette->color[chipnum].bg.blue,
            pCurrentPalette->color[chipnum].fg.red,
            pCurrentPalette->color[chipnum].fg.green,
            pCurrentPalette->color[chipnum].fg.blue
        );
#endif
    }
    _DtAddToResource(style.display, xrdb_string);

    if (!style.dynamicColor && TypeOfMonitor != XmCO_BLACK_WHITE) {
        allocNewColors();
    }

    /* Publish live summary atom on root for inspection & automated qualification */
    {
        Atom state_atom = XInternAtom(style.display, "_DT_COLOR_THEME_STATE", False);
        Boolean is_dark = DtstyleIsDarkPalette(pCurrentPalette);
        char st_buf[384];
        int sn = snprintf(
            st_buf, sizeof(st_buf),
            "palette=%s style=%s profile=%s density=%s polarity=%s",
            pCurrentPalette->name,
            (g_currentWidgetStyle == COLOR_STYLE_MODERN) ? "modern" :
            (g_currentWidgetStyle == COLOR_STYLE_HIGH_CONTRAST) ? "high_contrast" : "classic",
            prof_str,
            dens_str,
            DtstyleIsHighContrastPalette(pCurrentPalette) ? "high_contrast" :
            (is_dark ? "dark" : "light")
        );
        if (sn > 0) {
            XChangeProperty(style.display, style.root, state_atom, XA_STRING, 8,
                            PropModeReplace, (const unsigned char *)st_buf, sn);
        }
    }

    /* Synchronize GTK3/GTK4/GSettings/XSETTINGS/Portal dark-light scheme */
    {
        const char *sync_arg = "light";
        if (profile == XmAPPEARANCE_HIGH_CONTRAST || DtstyleIsHighContrastPalette(pCurrentPalette)) {
            sync_arg = "high_contrast";
        } else if (profile == XmAPPEARANCE_MODERN_DARK || DtstyleIsDarkPalette(pCurrentPalette)) {
            sync_arg = "dark";
        }
        pid_t spid = fork();
        if (spid == 0) {
            if (fork() == 0) {
                setsid();
                execlp("cde-color-sync", "cde-color-sync", "--apply", sync_arg, (char *)nullptr);
                execl(CDE_INSTALLATION_TOP "/bin/cde-color-sync", "cde-color-sync", "--apply", sync_arg, (char *)nullptr);
                _exit(127);
            }
            _exit(0);
        } else if (spid > 0) {
            int st = 0;
            (void)waitpid(spid, &st, 0);
        }
    }

    if (DtstyleIsWaylandSession()) {
        char wcmd[256];
        snprintf(wcmd, sizeof(wcmd), "f.set_palette %s %s", pCurrentPalette->name, prof_str);
        DtstyleSendWaylandWmRequest(wcmd);
        if (restartWmAndPanel) {
            DtstyleSendWaylandWmRequest("f.restart -noconfirm");
        }
    } else if (restartWmAndPanel) {
        DtstyleSendWaylandWmRequest("f.restart -noconfirm");
    }
    SyncPaletteSwatchBorders();
    UpdateColorThemeSummaryAndPreview();
    if (style.shell) {
        (void)XtAppAddTimeOut(XtWidgetToApplicationContext(style.shell),
                              25UL, SyncSwatchesDeferredCB, nullptr);
    }
    XSync(style.display, False);
}

void
RestoreOrgPalette( void )
{
   int i;
    palette  *tmp_palette;
   int         j=0;
   XColor      colors[XmCO_MAX_NUM_COLORS * 5];

   tmp_palette = pHeadPalette;
   while ( tmp_palette != nullptr &&
           tmp_palette->item_position != OrgPalette.item_position )
      tmp_palette = tmp_palette->next;

   if ( tmp_palette != nullptr &&
	tmp_palette->item_position == OrgPalette.item_position)
   {
      pCurrentPalette = tmp_palette;
      OrgPalette.num_of_colors = pCurrentPalette->num_of_colors;
      for(i = 0; i < XmCO_MAX_NUM_COLORS; i++)
      {
          pCurrentPalette->primary = OrgPalette.primary;
          pCurrentPalette->secondary = OrgPalette.secondary;
          pCurrentPalette->inactive = OrgPalette.inactive;
          pCurrentPalette->active = OrgPalette.active;

          pCurrentPalette->color[i].bg.pixel = OrgPalette.color[i].bg.pixel;
          pCurrentPalette->color[i].bg.red = OrgPalette.color[i].bg.red;
          pCurrentPalette->color[i].bg.green = OrgPalette.color[i].bg.green;
          pCurrentPalette->color[i].bg.blue = OrgPalette.color[i].bg.blue;
          if(i < OrgPalette.num_of_colors && TypeOfMonitor != XmCO_BLACK_WHITE)
             colors[j++] =  pCurrentPalette->color[i].bg;

          pCurrentPalette->color[i].sc.pixel = OrgPalette.color[i].sc.pixel;
          pCurrentPalette->color[i].sc.red = OrgPalette.color[i].sc.red;
          pCurrentPalette->color[i].sc.green = OrgPalette.color[i].sc.green;
          pCurrentPalette->color[i].sc.blue = OrgPalette.color[i].sc.blue;
          if(i < OrgPalette.num_of_colors && TypeOfMonitor != XmCO_BLACK_WHITE)
             colors[j++] =  pCurrentPalette->color[i].sc;

          pCurrentPalette->color[i].fg.pixel = OrgPalette.color[i].fg.pixel;
          pCurrentPalette->color[i].fg.red = OrgPalette.color[i].fg.red;
          pCurrentPalette->color[i].fg.green = OrgPalette.color[i].fg.green;
          pCurrentPalette->color[i].fg.blue = OrgPalette.color[i].fg.blue;
          if(i < OrgPalette.num_of_colors && TypeOfMonitor != XmCO_BLACK_WHITE)
             if(FgColor == DYNAMIC)
                colors[j++] =  pCurrentPalette->color[i].fg;

          pCurrentPalette->color[i].ts.pixel = OrgPalette.color[i].ts.pixel;
          pCurrentPalette->color[i].ts.red = OrgPalette.color[i].ts.red;
          pCurrentPalette->color[i].ts.green = OrgPalette.color[i].ts.green;
          pCurrentPalette->color[i].ts.blue = OrgPalette.color[i].ts.blue;
          if(i < OrgPalette.num_of_colors && TypeOfMonitor != XmCO_BLACK_WHITE)
             if(UsePixmaps == FALSE)
                colors[j++] =  pCurrentPalette->color[i].ts;

          pCurrentPalette->color[i].bs.pixel = OrgPalette.color[i].bs.pixel;
          pCurrentPalette->color[i].bs.red = OrgPalette.color[i].bs.red;
          pCurrentPalette->color[i].bs.green = OrgPalette.color[i].bs.green;
          pCurrentPalette->color[i].bs.blue = OrgPalette.color[i].bs.blue;
          if(i < OrgPalette.num_of_colors && TypeOfMonitor != XmCO_BLACK_WHITE)
             if(UsePixmaps == FALSE)
                colors[j++] =  pCurrentPalette->color[i].bs;

      }

      if (style.dynamicColor)
          XStoreColors (style.display, style.colormap, colors, j);
      else if (TypeOfMonitor != XmCO_BLACK_WHITE)
          allocNewColors();

      g_currentWidgetStyle = g_origWidgetStyle;
      g_currentDensity = g_origDensity;
      selected_position = OrgPalette.item_position;

      XmListSelectPos (paletteList, OrgPalette.item_position, False);
      XmListSetBottomPos(paletteList, OrgPalette.item_position);
      if (colorDialog.buttonsTB && pCurrentPalette->desc) {
          XmString xs = CMPSTR(pCurrentPalette->desc);
          XtVaSetValues(colorDialog.buttonsTB, XmNtitleString, xs, NULL);
          XmStringFree(xs);
      }

      if (g_appliedSinceOpen) {
          ApplyColorAndThemeToDesktop(True);
          g_appliedSinceOpen = False;
      }
      SyncPaletteSwatchBorders();
      UpdateColorThemeSummaryAndPreview();
   }
}

void 
     UpdateDefaultPalette( void )
{
   ApplyColorAndThemeToDesktop(True);
   SaveOrgPalette();
}
 


void 
show_selection(
        Widget w,
        XtPointer client_data,
        Atom *selection,
        Atom *type,
        XtPointer value,
        unsigned long *length,
        int *format )
{
    int      dynamic_color;

    style.colorSrv = True;
    if(value != nullptr)
    {
       if((intptr_t) client_data == GET_TYPE_MONITOR)
       {
          sscanf ((char *)value, "%x_%x_%x_%x", (unsigned int *) &(TypeOfMonitor),
                                     (unsigned int *) &(UsePixmaps), (unsigned int *) &(FgColor), (unsigned int *) &dynamic_color);
          if(dynamic_color == FALSE)
             style.dynamicColor = False;
          else
             style.dynamicColor = True;
       }

       WaitSelection = FALSE;
       free(value);
    }
    else /* no response from Color Server - it must not be there */
    {
       style.colorSrv = False;
       WaitSelection = FALSE;
    }
}

/************************************************************************
 * restoreColor()
 *
 * restore any state information saved with saveBackdrop.
 * This is called from restoreSession with the application
 * shell and the special xrm database retrieved for restore.
 ************************************************************************/
void 
restoreColor(
        Widget shell,
        XrmDatabase db )
{
    XrmName xrm_name[5];
    XrmRepresentation rep_type;
    XrmValue value;
    palette *tmp_palette;

    /*"paletteDlg" is the resource name of the dialog shell we are saving for.*/
    xrm_name [0] = XrmStringToQuark (PALETTEDLG);
    xrm_name [2] = 0;

    /* get x position */
    xrm_name [1] = XrmStringToQuark ("x");
    if (XrmQGetResource (db, xrm_name, xrm_name, &rep_type, &value)){
      XtSetArg (save.posArgs[save.poscnt], XmNx, atoi((char *)value.addr)); save.poscnt++;
    }

    /* get y position */
    xrm_name [1] = XrmStringToQuark ("y");
    if (XrmQGetResource (db, xrm_name, xrm_name, &rep_type, &value)){
      XtSetArg (save.posArgs[save.poscnt], XmNy, atoi((char *)value.addr)); save.poscnt++;
    }

    /* get selected palette */
    xrm_name [1] = XrmStringToQuark ("selected_palette");
    if (XrmQGetResource (db, xrm_name, xrm_name, &rep_type, &value) &&
        value.addr != nullptr){
       snprintf(defaultName_restore, sizeof(defaultName_restore), "%s", value.addr);
    }
    else
       defaultName_restore[0] = 0;

    /* get selected button */
    xrm_name [1] = XrmStringToQuark ("selected_button");
    if (XrmQGetResource (db, xrm_name, xrm_name, &rep_type, &value) &&
        value.addr != nullptr){
      int btn = atoi((char *)value.addr);
      if (btn >= 0 && btn < XmCO_MAX_NUM_COLORS)
          selected_button = btn;
    }

   /* need to have some variables initialized before creating the
        Color's dialog ... */
    InitializeAtoms();
    CheckMonitor(shell);
    GetDefaultPal(shell);

    xrm_name [1] = XrmStringToQuark ("ismapped");
    if (XrmQGetResource (db, xrm_name, xrm_name, &rep_type, &value) &&
        value.addr != nullptr &&
        strcmp(value.addr, "True") == 0) {
      save.restoreFlag = True;
      Customize(shell);
    }
}


/************************************************************************
 * saveColor()
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
saveColor(
        int fd )
{
    Position x,y;
    char bufr[1024];     /* size=[1024], make bigger if needed */
    XmVendorShellExtObject  vendorExt;
    XmWidgetExtData         extData;

    if (style.colorDialog != nullptr) 
    {
	if (XtIsManaged(style.colorDialog))
	  sprintf(bufr, "*paletteDlg.ismapped: True\n");
	else
	  sprintf(bufr, "*paletteDlg.ismapped: False\n");

        WRITE_STR2FD(fd, bufr);

	/* Get and write out the geometry info for our Window */
	x = XtX(XtParent(style.colorDialog));
	y = XtY(XtParent(style.colorDialog));

        /* Modify x & y to take into account window mgr frames
         * This is pretty bogus, but I don't know a better way to do it.
         */
        extData = _XmGetWidgetExtData(style.shell, XmSHELL_EXTENSION);
        vendorExt = (XmVendorShellExtObject)extData->widget;
        x -= vendorExt->vendor.xOffset;
        y -= vendorExt->vendor.yOffset;

	snprintf(bufr, sizeof(bufr), "*paletteDlg.x: %d\n", x);
        WRITE_STR2FD(fd, bufr);
	snprintf(bufr, sizeof(bufr), "*paletteDlg.y: %d\n", y);
        WRITE_STR2FD(fd, bufr);
	snprintf(bufr, sizeof(bufr), "*paletteDlg.selected_palette: %s\n",
                 pCurrentPalette->name);
        WRITE_STR2FD(fd, bufr);
	snprintf(bufr, sizeof(bufr), "*paletteDlg.selected_button: %d\n",
                 selected_button);
        WRITE_STR2FD(fd, bufr);
    }
}

/**************************************************************************
 *
 * SameName - procedure used by the Add palette .. if the palette desc the 
 *            user selects is the same name as a palette already in the
 *            linked list this procedure gets called.  It set up a
 *            Warning dialog asking the user if they really want to add
 *            a palette with the same name as an existing palette.
 *
 **************************************************************************/
void
SameName(
        Widget w,
        palette *tmpPalette,
        char *name )
{
   char    *tmpStr;
   int n=0;
   Arg args[10];

   if (colorDialog.dlg == nullptr) {
     tmpStr = (char *)XtMalloc(strlen(STR1) + strlen(name) + 1);
     sprintf(tmpStr, STR1, name);
     XtSetArg(args[n], XmNmessageString, CMPSTR(tmpStr)); n++;
     XtSetArg(args[n], XmNokLabelString, CMPSTR((String) _DtOkString)); n++;
     XtSetArg(args[n], XmNcancelLabelString, CMPSTR((String) _DtCancelString)); n++;
     XtSetArg(args[n], XmNhelpLabelString, CMPSTR((String) _DtHelpString)); n++;
     XtSetArg(args[n], XmNmwmFunctions, DIALOG_MWM_FUNC ); n++;
     XtSetArg(args[n], XmNdialogTitle, CMPSTR(((char *)GETMESSAGE(14, 21, "Warning")))); n++;
     colorDialog.dlg = XmCreateWarningDialog(style.colorDialog, "QNotice", args, n);
     XtAddCallback(colorDialog.dlg, XmNokCallback, setDlgOkCB, (XtPointer)tmpPalette);
     XtAddCallback(colorDialog.dlg, XmNhelpCallback,
              HelpRequestCB, (XtPointer)HELP_ADD_PALETTE_WARNING_DIALOG);

      n=0;
      XtSetArg (args[n], XmNmwmInputMode,
                        MWM_INPUT_PRIMARY_APPLICATION_MODAL); n++;
      XtSetValues (XtParent(colorDialog.dlg), args, n);
      XtFree(tmpStr);
   }
   XtManageChild(colorDialog.dlg);
}

/****************************************************************************
 *
 * ValidName - procedure which checks to make sure the name being passed
 *             in is a valid filename.  Weeds out many of the non 
 *             alphabit characters.
 *
 ***************************************************************************/
static Boolean 
ValidName(
        char *name )
{
   int i;
   Boolean valid = True;
#ifdef NLS16
   int chlen;
#endif  /* NLS16 */

   if (!name || strlen(name) == 0)
   {
      valid = False;
   }

#ifdef NLS16
   if (valid)
   {
      chlen = mblen (&name[0], MB_CUR_MAX);
      for (i = 0; chlen > 0 && valid && (size_t)i < strlen(name); i += chlen)
      {
         chlen = mblen (&name[i], MB_CUR_MAX);
         if (chlen <= 0)
         {
            valid = False;
            break;
         }
      if ((chlen == 1) &&
          ((name[i] == '*') ||
           (name[i] == '"') ||
           (name[i] == '\\') ||
           (name[i] == '[') ||
           (name[i] == ']') ||
           (name[i] == '{') ||
           (name[i] == '}') ||
           (name[i] == '>') ||
           (name[i] == '<') ||
           (name[i] == '(') ||
           (name[i] == ')') ||
           (name[i] == '!') ||
           (name[i] == '|') ||
           (name[i] == ':') ||
           (name[i] == '/'))) 
      {
           valid = False;
      }
      }
   }
#else /* NLS16 */
   for (i = 0; valid && ((size_t)i < strlen(name)); i++)
   {
      if (strchr ("|!(){}[]<>*:\"\\", (int) name[i]))
      {
          valid = False;
      }
   }
#endif /* NLS16 */
   return (valid);
}

void 
InitializeAtoms( void )
{
    char             cust_str[24];

    sprintf(cust_str,"%s%d", XmSCUSTOMIZE_DATA, style.screenNum);
    XA_CUSTOMIZE = XInternAtom(style.display, cust_str, FALSE);
    XA_TYPE_MONITOR = XInternAtom(style.display, TYPE_OF_MONITOR, FALSE);
    XA_WM_DELETE_WINDOW = XInternAtom(style.display, "WM_DELETE_WINDOW", False);
}

void 
GetDefaultPal(
        Widget shell )
{
    char *str_type_return;
    XrmValue value_return;
    XrmDatabase db;
    Boolean status;
    char *p;
    char *string;
    char instanceName[30], instanceClass[30];

    /* get the current default palette from the Reource Manager Property */

    db = XtDatabase(style.display);

    if (TypeOfMonitor == XmCO_BLACK_WHITE)   
    {
        sprintf(instanceName,"dtsession.%d.monochromePalette", style.screenNum);
        sprintf(instanceClass,"Dtsession.%d.MonochromePalette", style.screenNum);
    }
    else
    {
        sprintf(instanceName,"dtsession.%d.colorPalette", style.screenNum);
        sprintf(instanceClass,"Dtsession.%d.ColorPalette", style.screenNum);
    }

    if (status = XrmGetResource (db, instanceName, instanceClass,
                                     &str_type_return, &value_return))
    {
        /* copy string to defaultName */
        defaultName = (char *) XtMalloc( strlen(value_return.addr) + 1 );
        strcpy (defaultName, value_return.addr);
        p = strstr (defaultName, PALETTE_SUFFIX);
        if (p) *p = '\0';
    }
    else
    {
        /* set defaultName to default palette */
        defaultName = (char *) XtMalloc( strlen(DEFAULT_PALETTE) + 1 );
        strcpy (defaultName, DEFAULT_PALETTE);
    }
}

static void
QueryCurrentAppearanceSettings(void)
{
    XmAppearanceProfile prof = XmAPPEARANCE_CLASSIC;
    XmAppearanceDensity dens = XmAPPEARANCE_DENSITY_COMFORTABLE;
    if (style.display) {
        (void)XmGetAppearanceProfile(style.display, &prof);
        (void)XmGetAppearanceDensity(style.display, &dens);
    }
    if (prof == XmAPPEARANCE_HIGH_CONTRAST) {
        g_currentWidgetStyle = COLOR_STYLE_HIGH_CONTRAST;
    } else if (prof == XmAPPEARANCE_MODERN_LIGHT || prof == XmAPPEARANCE_MODERN_DARK) {
        g_currentWidgetStyle = COLOR_STYLE_MODERN;
    } else {
        g_currentWidgetStyle = COLOR_STYLE_CLASSIC;
    }
    g_currentDensity = (dens == XmAPPEARANCE_DENSITY_COMPACT)
                           ? XmAPPEARANCE_DENSITY_COMPACT
                           : XmAPPEARANCE_DENSITY_COMFORTABLE;
}

static void
 xcol_to_rgb(const XColor *xc, double *r, double *g, double *b)
{
    *r = (double)xc->red / 65535.0;
    *g = (double)xc->green / 65535.0;
    *b = (double)xc->blue / 65535.0;
}

static void
draw_preview_bevel(cairo_t *cr, double x, double y, double w, double h,
                   double t, const XColor *tl, const XColor *br)
{
    double r, g, b;
    xcol_to_rgb(tl, &r, &g, &b);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_new_path(cr);
    cairo_move_to(cr, x, y);
    cairo_line_to(cr, x + w, y);
    cairo_line_to(cr, x + w - t, y + t);
    cairo_line_to(cr, x + t, y + t);
    cairo_line_to(cr, x + t, y + h - t);
    cairo_line_to(cr, x, y + h);
    cairo_close_path(cr);
    cairo_fill(cr);

    xcol_to_rgb(br, &r, &g, &b);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_new_path(cr);
    cairo_move_to(cr, x + w, y);
    cairo_line_to(cr, x + w, y + h);
    cairo_line_to(cr, x, y + h);
    cairo_line_to(cr, x + t, y + h - t);
    cairo_line_to(cr, x + w - t, y + h - t);
    cairo_line_to(cr, x + w - t, y + t);
    cairo_close_path(cr);
    cairo_fill(cr);
}

static Window g_previewHostWin = None;
static pid_t  g_previewHelperPid = -1;
static char   g_lastPreviewCfg[2048] = "";

static int
DtstyleGetCurrentDpi(void)
{
    int dpi = 96;
    if (!style.display)
        return dpi;
    Atom actual_t = None;
    int actual_f = 0;
    unsigned long nitems = 0, rem = 0;
    unsigned char *prop = nullptr;
    if (XGetWindowProperty(style.display, style.root, XA_RESOURCE_MANAGER,
                           0L, 65536L, False, XA_STRING, &actual_t, &actual_f,
                           &nitems, &rem, &prop) == Success &&
        prop != nullptr) {
        char *buf = (char *)XtMalloc((Cardinal)(nitems + 1));
        if (buf) {
            memcpy(buf, prop, nitems);
            buf[nitems] = '\0';
            XrmDatabase db = XrmGetStringDatabase(buf);
            XtFree(buf);
            if (db) {
                char *type = nullptr;
                XrmValue val = {0, nullptr};
                if (XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &type, &val) &&
                    val.addr != nullptr) {
                    int parsed = atoi(val.addr);
                    if (parsed >= 72 && parsed <= 384)
                        dpi = parsed;
                }
                XrmDestroyDatabase(db);
            }
        }
        XFree(prop);
    }
    return dpi;
}

static double
DtstyleGetPreviewDpiScale(void)
{
    double s = (double)DtstyleGetCurrentDpi() / 96.0;
    if (s < 1.0) s = 1.0;
    if (s > 2.25) s = 2.25;
    return s;
}

static void
EnsurePreviewCanvasRoom(void)
{
    if (!colorDialog.previewCanvas || !style.colorDialog)
        return;
    double sc = DtstyleGetPreviewDpiScale();
    Dimension min_w = (Dimension)(468.0 * sc + 0.5);
    Dimension min_h = (Dimension)(344.0 * sc + 0.5);

    if (!XtIsRealized(colorDialog.previewCanvas)) {
        XtVaSetValues(colorDialog.previewCanvas,
                      XmNwidth, min_w, XmNheight, min_h, NULL);
        return;
    }

    Dimension cw = 0, ch = 0;
    XtVaGetValues(colorDialog.previewCanvas, XmNwidth, &cw, XmNheight, &ch, NULL);
    if (cw >= min_w && ch >= min_h)
        return;

    Widget shell = XtParent(style.colorDialog);
    if (shell && XtIsRealized(shell)) {
        Dimension sw = 0, sh = 0;
        XtVaGetValues(shell, XmNwidth, &sw, XmNheight, &sh, NULL);
        Dimension dw = (cw < min_w) ? (Dimension)(min_w - cw) : 0;
        Dimension dh = (ch < min_h) ? (Dimension)(min_h - ch) : 0;
        XtVaSetValues(shell,
                      XmNallowShellResize, True,
                      XmNwidth, (Dimension)(sw + dw),
                      XmNheight, (Dimension)(sh + dh),
                      NULL);
    }
    XtVaSetValues(colorDialog.previewCanvas,
                  XmNwidth, (cw < min_w) ? min_w : cw,
                  XmNheight, (ch < min_h) ? min_h : ch,
                  NULL);
}

static void
draw_authentic_dtwm_frame(cairo_t *cr, double wx, double wy, double ww, double wh,
                          double sc, const ColorSet *cs, const ColorSet *prim_cs,
                          const ColorSet *sec_cs, const char *title,
                          bool is_modern, bool is_hc, bool fill_dummy_client,
                          double *out_cx, double *out_cy,
                          double *out_cw, double *out_ch)
{
    double r, g, b;
    double fb = (is_modern ? 2.0 : 5.0) * sc;
    double th = (is_modern ? 25.0 : 22.0) * sc;
    double btn_w = th;

    /* Base window frame fill */
    if (is_hc) {
        cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    } else {
        xcol_to_rgb(&cs->bg, &r, &g, &b);
        cairo_set_source_rgb(cr, r, g, b);
    }
    cairo_rectangle(cr, wx, wy, ww, wh);
    cairo_fill(cr);

    if (is_modern) {
        /* Modern flat outer border + subtle inner highlight + header bar */
        xcol_to_rgb(&cs->bs, &r, &g, &b);
        cairo_set_source_rgb(cr, r, g, b);
        cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, wx + 0.5, wy + 0.5, ww - 1.0, wh - 1.0);
        cairo_stroke(cr);

        xcol_to_rgb(&cs->ts, &r, &g, &b);
        cairo_set_source_rgba(cr, r, g, b, 0.35);
        cairo_rectangle(cr, wx + 1.5, wy + 1.5, ww - 3.0, wh - 3.0);
        cairo_stroke(cr);

        /* Header bottom hairline */
        xcol_to_rgb(&cs->bs, &r, &g, &b);
        cairo_set_source_rgba(cr, r, g, b, 0.55);
        cairo_move_to(cr, wx + fb, wy + fb + th - 0.5);
        cairo_line_to(cr, wx + ww - fb, wy + fb + th - 0.5);
        cairo_stroke(cr);

        /* Left menu glyph (—), Right minimize (_) and maximize (□) glyphs */
        xcol_to_rgb(&cs->fg, &r, &g, &b);
        cairo_set_source_rgb(cr, r, g, b);
        cairo_set_line_width(cr, 1.5 * sc);
        double gy = wy + fb + th * 0.5;
        cairo_move_to(cr, wx + fb + 7.0 * sc, gy);
        cairo_line_to(cr, wx + fb + 17.0 * sc, gy);
        cairo_stroke(cr);

        double min_x = wx + ww - fb - 2.0 * btn_w;
        cairo_move_to(cr, min_x + 8.0 * sc, wy + fb + th - 7.5 * sc);
        cairo_line_to(cr, min_x + 17.0 * sc, wy + fb + th - 7.5 * sc);
        cairo_stroke(cr);

        double max_x = wx + ww - fb - btn_w;
        cairo_set_line_width(cr, 1.3 * sc);
        cairo_rectangle(cr, max_x + 8.0 * sc, wy + fb + 7.5 * sc, 9.5 * sc, 9.5 * sc);
        cairo_stroke(cr);

        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 11.0 * sc);
        cairo_text_extents_t te = {0};
        cairo_text_extents(cr, title, &te);
        cairo_move_to(cr, wx + (ww - te.width) * 0.5, wy + fb + 16.5 * sc);
        cairo_show_text(cr, title);
    } else {
        XColor hc_wht = {0, 0xFFFF, 0xFFFF, 0xFFFF, DoRed | DoGreen | DoBlue, 0};
        const XColor *ts = is_hc ? &hc_wht : &cs->ts;
        const XColor *bs = is_hc ? &hc_wht : &cs->bs;

        /* Authentic CDE outer 3D mitered bevel + inner reverse bevel */
        draw_preview_bevel(cr, wx, wy, ww, wh, 1.2 * sc, ts, bs);
        draw_preview_bevel(cr, wx + fb - 1.0 * sc, wy + fb - 1.0 * sc,
                           ww - 2.0 * (fb - 1.0 * sc), wh - 2.0 * (fb - 1.0 * sc),
                           1.0 * sc, bs, ts);

        /* Authentic CDE L-corner resize handle grooves at fb + th */
        double clen = fb + th;
        if (!is_hc && ww > 2.0 * clen + 20.0 * sc && wh > 2.0 * clen + 20.0 * sc) {
            cairo_set_line_width(cr, 1.0);
            xcol_to_rgb(bs, &r, &g, &b);
            cairo_set_source_rgb(cr, r, g, b);
            cairo_move_to(cr, wx + clen - 0.5, wy + 1.0); cairo_line_to(cr, wx + clen - 0.5, wy + fb - 1.0);
            cairo_move_to(cr, wx + ww - clen - 0.5, wy + 1.0); cairo_line_to(cr, wx + ww - clen - 0.5, wy + fb - 1.0);
            cairo_move_to(cr, wx + clen - 0.5, wy + wh - fb + 1.0); cairo_line_to(cr, wx + clen - 0.5, wy + wh - 1.0);
            cairo_move_to(cr, wx + ww - clen - 0.5, wy + wh - fb + 1.0); cairo_line_to(cr, wx + ww - clen - 0.5, wy + wh - 1.0);
            cairo_move_to(cr, wx + 1.0, wy + clen - 0.5); cairo_line_to(cr, wx + fb - 1.0, wy + clen - 0.5);
            cairo_move_to(cr, wx + ww - fb + 1.0, wy + clen - 0.5); cairo_line_to(cr, wx + ww - 1.0, wy + clen - 0.5);
            cairo_move_to(cr, wx + 1.0, wy + wh - clen - 0.5); cairo_line_to(cr, wx + fb - 1.0, wy + wh - clen - 0.5);
            cairo_move_to(cr, wx + ww - fb + 1.0, wy + wh - clen - 0.5); cairo_line_to(cr, wx + ww - 1.0, wy + wh - clen - 0.5);
            cairo_stroke(cr);

            xcol_to_rgb(ts, &r, &g, &b);
            cairo_set_source_rgb(cr, r, g, b);
            cairo_move_to(cr, wx + clen + 0.5, wy + 1.0); cairo_line_to(cr, wx + clen + 0.5, wy + fb - 1.0);
            cairo_move_to(cr, wx + ww - clen + 0.5, wy + 1.0); cairo_line_to(cr, wx + ww - clen + 0.5, wy + fb - 1.0);
            cairo_move_to(cr, wx + clen + 0.5, wy + wh - fb + 1.0); cairo_line_to(cr, wx + clen + 0.5, wy + wh - 1.0);
            cairo_move_to(cr, wx + ww - clen + 0.5, wy + wh - fb + 1.0); cairo_line_to(cr, wx + ww - clen + 0.5, wy + wh - 1.0);
            cairo_move_to(cr, wx + 1.0, wy + clen + 0.5); cairo_line_to(cr, wx + fb - 1.0, wy + clen + 0.5);
            cairo_move_to(cr, wx + ww - fb + 1.0, wy + clen + 0.5); cairo_line_to(cr, wx + ww - 1.0, wy + clen + 0.5);
            cairo_move_to(cr, wx + 1.0, wy + wh - clen + 0.5); cairo_line_to(cr, wx + fb - 1.0, wy + wh - clen + 0.5);
            cairo_move_to(cr, wx + ww - fb + 1.0, wy + wh - clen + 0.5); cairo_line_to(cr, wx + ww - 1.0, wy + wh - clen + 0.5);
            cairo_stroke(cr);
        }

        double mx = wx + fb;
        double my = wy + fb;
        draw_preview_bevel(cr, mx, my, btn_w, th, 1.0 * sc, ts, bs);
        draw_preview_bevel(cr, mx + 5.0 * sc, my + 9.0 * sc, btn_w - 10.0 * sc, 4.0 * sc, 1.0 * sc, ts, bs);

        double max_x = wx + ww - fb - btn_w;
        draw_preview_bevel(cr, max_x, my, btn_w, th, 1.0 * sc, ts, bs);
        draw_preview_bevel(cr, max_x + 5.0 * sc, my + 5.0 * sc, btn_w - 10.0 * sc, th - 10.0 * sc, 1.0 * sc, ts, bs);

        double min_x = max_x - btn_w;
        draw_preview_bevel(cr, min_x, my, btn_w, th, 1.0 * sc, ts, bs);
        draw_preview_bevel(cr, min_x + 9.0 * sc, my + 9.0 * sc, 4.0 * sc, 4.0 * sc, 1.0 * sc, ts, bs);

        double tx = mx + btn_w;
        double tw = min_x - tx;
        if (tw > 20.0 * sc) {
            draw_preview_bevel(cr, tx, my, tw, th, 1.0 * sc, ts, bs);
            if (is_hc) {
                cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
            } else {
                xcol_to_rgb(&cs->fg, &r, &g, &b);
                cairo_set_source_rgb(cr, r, g, b);
            }
            cairo_select_font_face(cr, "serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, 11.5 * sc);
            cairo_text_extents_t te = {0};
            cairo_text_extents(cr, title, &te);
            cairo_move_to(cr, tx + (tw - te.width) * 0.5, my + 15.0 * sc);
            cairo_show_text(cr, title);
        }
    }

    double cx = wx + fb;
    double cy = wy + fb + th;
    double cw_c = ww - 2.0 * fb;
    double ch_c = wh - 2.0 * fb - th;

    if (fill_dummy_client && prim_cs && sec_cs) {
        if (is_hc) {
            cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
        } else {
            xcol_to_rgb(&prim_cs->bg, &r, &g, &b);
            cairo_set_source_rgb(cr, r, g, b);
        }
        cairo_rectangle(cr, cx, cy, cw_c, ch_c);
        cairo_fill(cr);

        double mbh = 20.0 * sc;
        if (!is_hc) {
            xcol_to_rgb(&sec_cs->bg, &r, &g, &b);
            cairo_set_source_rgb(cr, r, g, b);
            cairo_rectangle(cr, cx, cy, cw_c, mbh);
            cairo_fill(cr);
            if (!is_modern)
                draw_preview_bevel(cr, cx, cy, cw_c, mbh, 1.2 * sc, &sec_cs->ts, &sec_cs->bs);
            xcol_to_rgb(&sec_cs->fg, &r, &g, &b);
            cairo_set_source_rgb(cr, r, g, b);
        } else {
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        }
        cairo_select_font_face(cr, is_modern ? "sans-serif" : "serif",
                               CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 10.5 * sc);
        cairo_move_to(cr, cx + 8.0 * sc, cy + 14.0 * sc);
        cairo_show_text(cr, "File   Selected   View");
    }

    if (out_cx) *out_cx = cx;
    if (out_cy) *out_cy = cy;
    if (out_cw) *out_cw = cw_c;
    if (out_ch) *out_ch = ch_c;
}

static void
SyncEmbeddedPreviewWindow(Widget canvas, int cx, int cy, int cw, int ch)
{
    if (!canvas || !XtIsRealized(canvas) || !style.display || !pCurrentPalette)
        return;
    if (cw < 60 || ch < 60)
        return;

    if (g_previewHostWin == None) {
        Pixel init_bg = pCurrentPalette->color[0].bg.pixel;
        g_previewHostWin = XCreateSimpleWindow(
            style.display, XtWindow(canvas),
            cx, cy, (unsigned int)cw, (unsigned int)ch,
            0, init_bg, init_bg
        );
        XMapWindow(style.display, g_previewHostWin);
        XFlush(style.display);
        g_lastPreviewCfg[0] = '\0';
    } else {
        XMoveResizeWindow(style.display, g_previewHostWin,
                          cx, cy, (unsigned int)cw, (unsigned int)ch);
    }

    XmAppearanceProfile resolved =
        DtstyleResolveMotifProfile(pCurrentPalette, g_currentWidgetStyle);
    const char *prof_str = DtstyleProfileNameString(resolved);
    const char *dens_str = DtstyleDensityNameString(g_currentDensity);
    int dpi_val = DtstyleGetCurrentDpi();

    static const int cmap_idx[4][XmCO_MAX_NUM_COLORS] = {
        {0, 1, 2, 3, 4, 5, 6, 7},
        {0, 1, 2, 3, 1, 1, 2, 1},
        {0, 1, 1, 1, 1, 1, 1, 1},
        {0, 1, 1, 1, 1, 1, 1, 1},
    };
    int midx = 0;
    if (TypeOfMonitor == XmCO_MEDIUM_COLOR) midx = 1;
    else if (TypeOfMonitor == XmCO_LOW_COLOR) midx = 2;
    else if (TypeOfMonitor == XmCO_BLACK_WHITE) midx = 3;

    char cfg[2048];
    int pos = snprintf(cfg, sizeof(cfg), "profile=%s\ndensity=%s\ndpi=%d\n",
                       prof_str, dens_str, dpi_val);
    int nc = pCurrentPalette->num_of_colors > 0 ? pCurrentPalette->num_of_colors : 2;
    for (int i = 0; i < 8 && pos + 64 < (int)sizeof(cfg); ++i) {
        int slot = cmap_idx[midx][i];
        if (slot >= nc) slot = slot % nc;
        const ColorSet *cs = &pCurrentPalette->color[slot];
        pos += snprintf(
            cfg + pos, sizeof(cfg) - (size_t)pos,
            "c%d=#%02X%02X%02X,#%02X%02X%02X,#%02X%02X%02X,#%02X%02X%02X,#%02X%02X%02X\n",
            i,
            (unsigned int)(cs->bg.red >> 8), (unsigned int)(cs->bg.green >> 8), (unsigned int)(cs->bg.blue >> 8),
            (unsigned int)(cs->fg.red >> 8), (unsigned int)(cs->fg.green >> 8), (unsigned int)(cs->fg.blue >> 8),
            (unsigned int)(cs->ts.red >> 8), (unsigned int)(cs->ts.green >> 8), (unsigned int)(cs->ts.blue >> 8),
            (unsigned int)(cs->bs.red >> 8), (unsigned int)(cs->bs.green >> 8), (unsigned int)(cs->bs.blue >> 8),
            (unsigned int)(cs->sc.red >> 8), (unsigned int)(cs->sc.green >> 8), (unsigned int)(cs->sc.blue >> 8)
        );
    }

    if (strcmp(cfg, g_lastPreviewCfg) != 0) {
        snprintf(g_lastPreviewCfg, sizeof(g_lastPreviewCfg), "%s", cfg);
        Atom cfg_atom = XInternAtom(style.display, "_DTSTYLE_PREVIEW_CFG", False);
        XChangeProperty(style.display, g_previewHostWin, cfg_atom, XA_STRING, 8,
                        PropModeReplace, (const unsigned char *)cfg, pos);
        XFlush(style.display);
    }

    if (g_previewHelperPid > 0) {
        int st = 0;
        pid_t r = waitpid(g_previewHelperPid, &st, WNOHANG);
        if (r == g_previewHelperPid || r < 0)
            g_previewHelperPid = -1;
    }
    if (g_previewHelperPid <= 0) {
        char xid_buf[32];
        snprintf(xid_buf, sizeof(xid_buf), "0x%lx", (unsigned long)g_previewHostWin);
        pid_t pid = fork();
        if (pid == 0) {
            /* Connect as an X11 client to parent dtstyle's display (including on
             * native Wayland where DISPLAY points to parent's ck-wlx11 socket)
             * so XReparentWindow embeds real Motif preview widgets in-window. */
            setenv("CK_MOTIF_BACKEND", "x11", 1);
            execl("/proc/self/exe", "dtstyle", "--preview-helper", xid_buf, (char *)nullptr);
            execl(CDE_INSTALLATION_TOP "/bin/dtstyle", "dtstyle", "--preview-helper", xid_buf, (char *)nullptr);
            execlp("dtstyle", "dtstyle", "--preview-helper", xid_buf, (char *)nullptr);
            _exit(127);
        } else if (pid > 0) {
            g_previewHelperPid = pid;
        }
    }
}

static void
DrawLiveThemePreviewCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)client_data;
    (void)call_data;
    if (!w || !XtIsRealized(w) || !style.display || !pCurrentPalette) return;

    EnsurePreviewCanvasRoom();

    Dimension cw = 0, ch = 0;
    XtVaGetValues(w, XmNwidth, &cw, XmNheight, &ch, NULL);
    if (cw < 120 || ch < 100) return;

    double sc = DtstyleGetPreviewDpiScale();
    Visual *vis = DefaultVisual(style.display, style.screenNum);
    cairo_surface_t *surf = cairo_xlib_surface_create(style.display, XtWindow(w), vis, cw, ch);
    cairo_t *cr = cairo_create(surf);

    const palette *pal = pCurrentPalette;
    int nc = pal->num_of_colors > 0 ? pal->num_of_colors : 2;
    const ColorSet *cs_act   = &pal->color[0];
    const ColorSet *cs_inact = &pal->color[(nc > 1) ? 1 : 0];
    const ColorSet *cs_ws0   = &pal->color[(nc > 2) ? 2 : 0];
    const ColorSet *cs_prim  = &pal->color[(nc > 4) ? 4 : ((nc > 1) ? 1 : 0)];
    const ColorSet *cs_sec   = &pal->color[(nc > 5) ? 5 : ((nc > 1) ? 1 : 0)];

    XmAppearanceProfile resolved = DtstyleResolveMotifProfile(pal, g_currentWidgetStyle);
    bool is_modern = (resolved == XmAPPEARANCE_MODERN_LIGHT || resolved == XmAPPEARANCE_MODERN_DARK);
    bool is_hc = (resolved == XmAPPEARANCE_HIGH_CONTRAST);

    double r, g, b;
    /* 1. Desktop workspace backdrop with subtle CDE granite weave */
    if (is_hc) {
        cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    } else {
        xcol_to_rgb(&cs_ws0->bg, &r, &g, &b);
        cairo_set_source_rgb(cr, r, g, b);
    }
    cairo_paint(cr);

    if (!is_hc) {
        cairo_set_line_width(cr, 1.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.05);
        for (int gy = 2; gy < (int)ch; gy += 6) {
            cairo_move_to(cr, 0, gy + 0.5);
            cairo_line_to(cr, cw, gy + 0.5);
        }
        cairo_stroke(cr);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.05);
        for (int gx = 3; gx < (int)cw; gx += 8) {
            cairo_move_to(cr, gx + 0.5, 0);
            cairo_line_to(cr, gx + 0.5, ch);
        }
        cairo_stroke(cr);
    }

    /* 2. Authentic inactive background window peeking top-left */
    draw_authentic_dtwm_frame(
        cr, 8.0 * sc, 6.0 * sc, cw * 0.66, ch * 0.50,
        sc, cs_inact, cs_prim, cs_sec,
        "File Manager", is_modern, is_hc, true,
        nullptr, nullptr, nullptr, nullptr
    );

    /* 3. Authentic active foreground window hosting real Motif preview dialog */
    double fph = 22.0 * sc;
    double wx = 22.0 * sc;
    double wy = 22.0 * sc;
    double ww = (double)cw - 32.0 * sc;
    double wh = (double)ch - wy - fph - 10.0 * sc;
    double cx = 0.0, cy = 0.0, cwidth = 0.0, cheight = 0.0;

    draw_authentic_dtwm_frame(
        cr, wx, wy, ww, wh,
        sc, cs_act, cs_prim, cs_sec,
        "Sample Dialog", is_modern, is_hc, false,
        &cx, &cy, &cwidth, &cheight
    );

    /* 4. Authentic miniature CDE Front Panel along the bottom */
    double fpw = cw * 0.68;
    double fpx = (cw - fpw) * 0.5;
    double fpy = ch - fph - 4.0 * sc;
    xcol_to_rgb(&cs_prim->bg, &r, &g, &b);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_rectangle(cr, fpx, fpy, fpw, fph);
    cairo_fill(cr);
    if (is_modern) {
        xcol_to_rgb(&cs_prim->bs, &r, &g, &b);
        cairo_set_source_rgb(cr, r, g, b);
        cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, fpx + 0.5, fpy + 0.5, fpw - 1.0, fph - 1.0);
        cairo_stroke(cr);
    } else {
        draw_preview_bevel(cr, fpx, fpy, fpw, fph, 1.5 * sc, &cs_prim->ts, &cs_prim->bs);
    }

    static const int ws_map[4] = {2, 4, 5, 6};
    double sw_area = fpw - 44.0 * sc;
    double sw_w = sw_area / 4.0;
    for (int wi = 0; wi < 4; ++wi) {
        int cidx = (nc > ws_map[wi]) ? ws_map[wi] : (wi % nc);
        const ColorSet *wcs = &pal->color[cidx];
        double sx = fpx + 22.0 * sc + wi * sw_w;
        xcol_to_rgb(&wcs->bg, &r, &g, &b);
        cairo_set_source_rgb(cr, r, g, b);
        cairo_rectangle(cr, sx + 1.5 * sc, fpy + 3.5 * sc, sw_w - 3.0 * sc, fph - 7.0 * sc);
        cairo_fill(cr);
        if (is_modern) {
            xcol_to_rgb((wi == 0) ? &cs_act->bg : &wcs->bs, &r, &g, &b);
            cairo_set_source_rgb(cr, r, g, b);
            cairo_set_line_width(cr, (wi == 0) ? 1.5 : 1.0);
            cairo_rectangle(cr, sx + 2.0 * sc, fpy + 4.0 * sc, sw_w - 4.0 * sc, fph - 8.0 * sc);
            cairo_stroke(cr);
        } else {
            draw_preview_bevel(cr, sx + 1.5 * sc, fpy + 3.5 * sc, sw_w - 3.0 * sc, fph - 7.0 * sc, 1.2 * sc,
                               (wi == 0) ? &wcs->bs : &wcs->ts,
                               (wi == 0) ? &wcs->ts : &wcs->bs);
        }
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surf);

    /* Position & update the real embedded Motif client window inside (cx, cy, cwidth, cheight) */
    SyncEmbeddedPreviewWindow(w, (int)(cx + 0.5), (int)(cy + 0.5),
                              (int)(cwidth + 0.5), (int)(cheight + 0.5));
}

static void
PreviewCanvasInputEH(Widget w, XtPointer client_data, XEvent *ev, Boolean *cont)
{
    (void)w;
    (void)client_data;
    (void)ev;
    (void)cont;
}

static void
SelectWidgetStyleCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    if (g_updatingStyleWidgets) return;
    int idx = (int)(intptr_t)client_data;
    if (idx >= 0 && idx <= 2) {
        g_currentWidgetStyle = (ColorWidgetStyle)idx;
        UpdateColorThemeSummaryAndPreview();
    }
}

static void
SelectDensityCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    if (g_updatingStyleWidgets) return;
    int idx = (int)(intptr_t)client_data;
    g_currentDensity = (idx == 1)
                           ? XmAPPEARANCE_DENSITY_COMPACT
                           : XmAPPEARANCE_DENSITY_COMFORTABLE;
    UpdateColorThemeSummaryAndPreview();
}

static void
UpdateColorThemeSummaryAndPreview(void)
{
    g_updatingStyleWidgets = True;

    if (colorDialog.styleOptMenu && colorDialog.styleOptBtns[(int)g_currentWidgetStyle]) {
        XtVaSetValues(colorDialog.styleOptMenu,
                      XmNmenuHistory, colorDialog.styleOptBtns[(int)g_currentWidgetStyle],
                      NULL);
    }
    int didx = (g_currentDensity == XmAPPEARANCE_DENSITY_COMPACT) ? 1 : 0;
    if (colorDialog.densityOptMenu && colorDialog.densityOptBtns[didx]) {
        XtVaSetValues(colorDialog.densityOptMenu,
                      XmNmenuHistory, colorDialog.densityOptBtns[didx],
                      NULL);
    }

    if (pCurrentPalette) {
        Boolean is_hc_pal = DtstyleIsHighContrastPalette(pCurrentPalette);
        Boolean is_dark = DtstyleIsDarkPalette(pCurrentPalette);
        if (colorDialog.paletteModeLabel) {
            const char *mode_txt =
                is_hc_pal ? "Color Mode: High Contrast (Accessible)" :
                is_dark   ? "Color Mode: Dark Palette  ->  Dark Mode" :
                            "Color Mode: Bright Palette  ->  Light Mode";
            XmString xs = CMPSTR((char *)mode_txt);
            XtVaSetValues(colorDialog.paletteModeLabel, XmNlabelString, xs, NULL);
            XmStringFree(xs);
        }

        if (colorDialog.themeSummaryLabel) {
            XmAppearanceProfile res = DtstyleResolveMotifProfile(pCurrentPalette, g_currentWidgetStyle);
            const char *prof_human =
                (res == XmAPPEARANCE_MODERN_DARK)   ? "Modern Motif (Dark Mode)" :
                (res == XmAPPEARANCE_MODERN_LIGHT)  ? "Modern Motif (Light Mode)" :
                (res == XmAPPEARANCE_HIGH_CONTRAST) ? "High Contrast (Accessible)" :
                                                      "Classic CDE (3D Bevel)";
            char sum_buf[256];
            snprintf(sum_buf, sizeof(sum_buf),
                     "Effective Theme: %s   |   Spacing: %s",
                     prof_human,
                     (g_currentDensity == XmAPPEARANCE_DENSITY_COMPACT) ? "Compact" : "Comfortable");
            XmString xs = CMPSTR(sum_buf);
            XtVaSetValues(colorDialog.themeSummaryLabel, XmNlabelString, xs, NULL);
            XmStringFree(xs);
        }
    }

    g_updatingStyleWidgets = False;

    if (colorDialog.previewCanvas && XtIsRealized(colorDialog.previewCanvas)) {
        DrawLiveThemePreviewCB(colorDialog.previewCanvas, nullptr, nullptr);
    }
}

static void
ColorDialogIpcEH(Widget w, XtPointer client_data, XEvent *ev, Boolean *cont)
{
    (void)client_data;
    (void)cont;
    if (!ev || ev->type != PropertyNotify || !style.display) return;
    Atom ipc_atom = XInternAtom(style.display, "_DTSTYLE_COLOR_IPC", False);
    if (ev->xproperty.atom != ipc_atom || ev->xproperty.state != PropertyNewValue) return;

    Atom act = None;
    int fmt = 0;
    unsigned long ni = 0, rem = 0;
    unsigned char *data = nullptr;
    if (XGetWindowProperty(style.display, XtWindow(w), ipc_atom, 0, 512, True,
                           AnyPropertyType, &act, &fmt, &ni, &rem, &data) != Success || !data) {
        return;
    }
    char cmd[512];
    size_t cl = ni < sizeof(cmd) - 1 ? (size_t)ni : sizeof(cmd) - 1;
    memcpy(cmd, data, cl);
    cmd[cl] = '\0';
    XFree(data);

    if (strncmp(cmd, "set-palette ", 12) == 0) {
        const char *pname = cmd + 12;
        while (*pname == ' ') pname++;
        for (palette *it = pHeadPalette; it != nullptr; it = it->next) {
            if (strcasecmp(it->name, pname) == 0 ||
                (it->desc && strcasecmp(it->desc, pname) == 0)) {
                XmListSelectPos(paletteList, it->item_position, True);
                XmListSetBottomPos(paletteList, it->item_position);
                break;
            }
        }
    } else if (strncmp(cmd, "set-style ", 10) == 0) {
        const char *st = cmd + 10;
        while (*st == ' ') st++;
        if (strcasecmp(st, "modern") == 0) g_currentWidgetStyle = COLOR_STYLE_MODERN;
        else if (strcasecmp(st, "high_contrast") == 0) g_currentWidgetStyle = COLOR_STYLE_HIGH_CONTRAST;
        else g_currentWidgetStyle = COLOR_STYLE_CLASSIC;
        UpdateColorThemeSummaryAndPreview();
    } else if (strncmp(cmd, "set-density ", 12) == 0) {
        const char *dn = cmd + 12;
        while (*dn == ' ') dn++;
        g_currentDensity = (strcasecmp(dn, "compact") == 0)
                               ? XmAPPEARANCE_DENSITY_COMPACT
                               : XmAPPEARANCE_DENSITY_COMFORTABLE;
        UpdateColorThemeSummaryAndPreview();
    } else if (strcasecmp(cmd, "apply") == 0) {
        DtDialogBoxCallbackStruct cbs = {0};
        cbs.button_position = COLOR_BTN_APPLY;
        dialogBoxCB(style.colorDialog, nullptr, &cbs);
    } else if (strcasecmp(cmd, "ok") == 0) {
        DtDialogBoxCallbackStruct cbs = {0};
        cbs.button_position = COLOR_BTN_OK;
        dialogBoxCB(style.colorDialog, nullptr, &cbs);
    } else if (strcasecmp(cmd, "cancel") == 0) {
        DtDialogBoxCallbackStruct cbs = {0};
        cbs.button_position = COLOR_BTN_CANCEL;
        dialogBoxCB(style.colorDialog, nullptr, &cbs);
    }
}

static void
PublishColorDialogWindowProp(void)
{
    if (!style.display || !style.colorDialog) return;
    Widget sh = XtParent(style.colorDialog);
    if (!sh || !XtIsRealized(sh)) return;
    Window w = XtWindow(sh);
    if (w == None) return;
    Atom a = XInternAtom(style.display, "_DTSTYLE_COLOR_DLG_WIN", False);
    XChangeProperty(style.display, style.root, a, XA_WINDOW, 32,
                    PropModeReplace, (const unsigned char *)&w, 1);
    XFlush(style.display);
}

void 
CreateDialogBoxD(
        Widget parent )
{
    XmString         button_string[COLOR_NUM_BUTTONS]; 
    Arg              args[4];
    int		     n;
    Widget           w;

   if(style.count > 0)
      return;

    /* Get the default value of multiclick */
    dclick_time = XtGetMultiClickTime(style.display);

    gParent = parent;
    if(!XtIsRealized(parent))
       XtRealizeWidget(parent);

   /* Set up DialogBox button labels: OK, Apply, Cancel, Help */
    button_string[0] = CMPSTR((String) _DtOkString);
    button_string[1] = CMPSTR("Apply");
    button_string[2] = CMPSTR((String) _DtCancelString);
    button_string[3] = CMPSTR((String) _DtHelpString);

   /* saveRestore
    * Note that save.poscnt has been initialized elsewhere.  
    * save.posArgs may contain information from restoreColor().*/

    XtSetArg (save.posArgs[save.poscnt], XmNchildType, XmWORK_AREA);  save.poscnt++;
    XtSetArg (save.posArgs[save.poscnt], XmNbuttonCount, COLOR_NUM_BUTTONS);  save.poscnt++;
    XtSetArg (save.posArgs[save.poscnt], XmNbuttonLabelStrings, button_string);  save.poscnt++;
    XtSetArg (save.posArgs[save.poscnt], XmNdefaultPosition, False); save.poscnt++;
    style.colorDialog = __DtCreateDialogBoxDialog(parent,PALETTEDLG, save.posArgs, save.poscnt);
    XtAddCallback(style.colorDialog, XmNcallback, dialogBoxCB, nullptr);
    XtAddCallback(style.colorDialog, XmNmapCallback, _DtmapCB, parent);
    XtAddCallback(style.colorDialog, XmNhelpCallback,
            HelpRequestCB, (XtPointer)HELP_COLOR_DIALOG);

    XmStringFree(button_string[0]);
    XmStringFree(button_string[1]);
    XmStringFree(button_string[2]);
    XmStringFree(button_string[3]);

    w = _DtDialogBoxGetButton(style.colorDialog, COLOR_BTN_CANCEL);
    n=0;
    XtSetArg(args[n], XmNautoUnmanage, False); n++;
    XtSetArg(args[n], XmNcancelButton, w); n++;
    XtSetValues (style.colorDialog, args, n);

    style.count++;
}

void 
AddToDialogBox( void )
{
    int     n;
    Arg              args[8];

   if(style.count > 1)
      return;

    n = 0;
    XtSetArg (args[n], XmNtitle, "Style Manager - Color & Theme"); n++;
    XtSetArg (args[n], XmNuseAsyncGeometry, True); n++;
    XtSetArg (args[n], XmNallowShellResize, True); n++;
    XtSetArg (args[n], XmNmwmFunctions, DIALOG_MWM_FUNC); n++;
    XtSetValues (XtParent(style.colorDialog), args, n);
    
    /* Add save session property & IPC listener to the dialog shell */
    XmAddWMProtocolCallback(XtParent(style.colorDialog), 
                           XA_WM_DELETE_WINDOW, activateCBexitColor, nullptr);
    XtAddEventHandler(
        XtParent(style.colorDialog), PropertyChangeMask, False,
        ColorDialogIpcEH, nullptr
    );

/*
**  Create a main two-column form for the Color & Theme dialog
*/
    n = 0;
    XtSetArg(args[n], XmNhorizontalSpacing, style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNverticalSpacing, style.verticalSpacing); n++;
    XtSetArg(args[n], XmNallowOverlap, False); n++;
    colorDialog.colorForm = XmCreateForm(style.colorDialog, "colorForm", args, n);
    XtManageChild(colorDialog.colorForm);

    colorDialog.leftColForm = XtVaCreateManagedWidget(
        "leftColForm", xmFormWidgetClass, colorDialog.colorForm,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNhorizontalSpacing, style.horizontalSpacing,
        XmNverticalSpacing, style.verticalSpacing,
        NULL
    );

    colorDialog.rightColForm = XtVaCreateManagedWidget(
        "rightColForm", xmFormWidgetClass, colorDialog.colorForm,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_WIDGET,
        XmNleftWidget, colorDialog.leftColForm,
        XmNleftOffset, 12,
        XmNrightAttachment, XmATTACH_FORM,
        XmNhorizontalSpacing, style.horizontalSpacing,
        XmNverticalSpacing, style.verticalSpacing,
        NULL
    );
    
    style.count++;
}

void 
CreateTopColor1( void )
{
    int     n;
    Arg              args[6];
    XmString         string; 

    if(style.count > 2)
       return;
/*
**  titlebox as child of the left column form
*/
    n = 0;
    XtSetArg (args[n], XmNtopAttachment, XmATTACH_FORM);  n++;
    XtSetArg (args[n], XmNleftAttachment, XmATTACH_FORM);  n++;
    XtSetArg (args[n], XmNrightAttachment, XmATTACH_FORM);  n++;
    XtSetArg (args[n], XmNmarginWidth, 0);  n++;
    XtSetArg (args[n], XmNmarginHeight, 0);  n++;
    string = CMPSTR(((char *)GETMESSAGE(14, 5, "Palettes")));
    XtSetArg (args[n], XmNtitleString, string); n++;
    colorDialog.paletteTB = _DtCreateTitleBox(colorDialog.leftColForm, "paletteTB", args, n);
    XtManageChild(colorDialog.paletteTB);
    XmStringFree(string);

/* 
**  Create a form inside palette titlebox 
*/
    n = 0;
    XtSetArg(args[n], XmNhorizontalSpacing, style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNverticalSpacing, style.verticalSpacing); n++;
    colorDialog.palettesForm = XmCreateForm(colorDialog.paletteTB, "palettesForm", args, n);
    XtManageChild(colorDialog.palettesForm);

    style.count++;
}

void 
CreateTopColor2( void )
{
    int     n;
    Arg              args[16];

    if(style.count > 3)
       return;
/*
**  Create a scrolled list widget.  This widget will contain the list of
**  palettes currently loaded (by ReadPalettes) in the customizer.
*/
    n=0;
    XtSetArg (args[n], XmNselectionPolicy, XmBROWSE_SELECT); n++;
    XtSetArg (args[n], XmNautomaticSelection, True); n++;
    XtSetArg (args[n], XmNvisibleItemCount, 9); n++;
    paletteList = XmCreateScrolledList(colorDialog.palettesForm,"paletteList",args,n);
    XtAddCallback(paletteList, XmNbrowseSelectionCallback,
                                              selectPaletteCB, nullptr);

    XmString mode_xs = CMPSTR("Color Mode: Bright Palette  ->  Light Mode");
    colorDialog.paletteModeLabel = XtVaCreateManagedWidget(
        "paletteModeLabel", xmLabelGadgetClass, colorDialog.palettesForm,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNlabelString, mode_xs,
        XmNleftAttachment, XmATTACH_FORM,
        XmNleftOffset, style.horizontalSpacing,
        XmNrightAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        NULL
    );
    XmStringFree(mode_xs);

    n=0;
    XtSetArg (args[n], XmNtopAttachment, XmATTACH_FORM);  n++;
    XtSetArg (args[n], XmNtopOffset, style.horizontalSpacing);  n++;
    XtSetArg (args[n], XmNleftAttachment, XmATTACH_FORM);  n++;
    XtSetArg (args[n], XmNleftOffset, style.horizontalSpacing);  n++;
    XtSetArg (args[n], XmNrightAttachment, XmATTACH_POSITION);  n++;
    XtSetArg (args[n], XmNrightPosition, 62);  n++;
    XtSetArg (args[n], XmNbottomAttachment, XmATTACH_WIDGET);  n++;
    XtSetArg (args[n], XmNbottomWidget, colorDialog.paletteModeLabel);  n++;
    XtSetArg (args[n], XmNbottomOffset, 6);  n++;
    XtSetValues (XtParent(paletteList), args, n);

    style.count++;
}

void 
CreateBottomColor( void )
{
    int     n;
    Arg              args[12];
    XmString         string; 
    Widget           addDeleteForm;
    Widget           resourcesPB;

    colorDialog.dlg = nullptr;

    if(style.count > 10)
       return;

    if(TypeOfMonitor != XmCO_BLACK_WHITE)
    {
        /* Create form for Add and Delete buttons */
        n = 0;
        XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM);  n++;
        XtSetArg(args[n], XmNleftAttachment, XmATTACH_WIDGET);  n++;
        XtSetArg(args[n], XmNleftWidget, XtParent(paletteList));  n++;
        XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM);  n++;
        XtSetArg(args[n], XmNrightOffset, 0);  n++;
        XtSetArg(args[n], XmNhorizontalSpacing, style.horizontalSpacing); n++;
        XtSetArg(args[n], XmNverticalSpacing, style.verticalSpacing); n++;
        XtSetArg(args[n], XmNallowOverlap, False); n++;
        XtSetArg(args[n], XmNchildType, XmWORK_AREA);  n++;
        addDeleteForm = 
            XmCreateForm(colorDialog.palettesForm, "addDeleteForm", args, n);
        XtManageChild(addDeleteForm);

        /* Create Add button */
        n = 0;
        XtSetArg (args[n], XmNtopAttachment, XmATTACH_FORM);  n++;
        XtSetArg (args[n], XmNtopOffset, ADD_PALETTE_TOP_OFFSET);  n++;
        XtSetArg (args[n], XmNleftAttachment, XmATTACH_FORM);  n++;
        XtSetArg (args[n], XmNleftOffset, style.horizontalSpacing);  n++;
        XtSetArg (args[n], XmNrightAttachment, XmATTACH_FORM);  n++;
        string = CMPSTR(((char *)GETMESSAGE(14, 6, "Add...")));
        XtSetArg (args[n], XmNlabelString, string); n++;
        colorDialog.addPaletteButton =
                XmCreatePushButtonGadget(addDeleteForm, "addPalette", args, n);
        XmStringFree(string);
        XtManageChild(colorDialog.addPaletteButton);
        XtAddCallback(colorDialog.addPaletteButton, XmNactivateCallback, addPaletteCB, 
                            (XtPointer) nullptr);

       /* Create Delete button */
        n = 0;
        XtSetArg (args[n], XmNtopAttachment, XmATTACH_WIDGET);  n++;
        XtSetArg (args[n], XmNtopWidget, colorDialog.addPaletteButton);  n++;
        XtSetArg (args[n], XmNleftAttachment, XmATTACH_FORM);  n++;
        XtSetArg (args[n], XmNleftOffset, style.horizontalSpacing);  n++;
        XtSetArg (args[n], XmNrightAttachment, XmATTACH_FORM);  n++;
        string = CMPSTR(((char *)GETMESSAGE(14, 7, "Delete...")));
        XtSetArg (args[n], XmNlabelString, string); n++;
        colorDialog.deletePaletteButton =
        XmCreatePushButtonGadget(addDeleteForm,"deletePalette",args,n);
        XmStringFree(string);
        XtManageChild(colorDialog.deletePaletteButton);
        XtAddCallback(colorDialog.deletePaletteButton, XmNactivateCallback, deletePaletteCB,
                               (XtPointer) nullptr);
    }
    
/*
**  Create a pushbutton at the bottom of the left column for Number Of Colors
*/
    n=0;
    XtSetArg (args[n], XmNtopAttachment, XmATTACH_NONE);  n++;
    XtSetArg (args[n], XmNleftAttachment, XmATTACH_FORM);  n++;
    XtSetArg (args[n], XmNrightAttachment, XmATTACH_FORM);  n++;
    XtSetArg (args[n], XmNbottomAttachment, XmATTACH_FORM);  n++;
    XtSetArg (args[n], XmNmarginHeight, 5);  n++;
    XtSetArg (args[n], XmNnavigationType, XmTAB_GROUP);  n++;
    string = CMPSTR(((char *)GETMESSAGE(14, 40, "Number Of Colors...")));
    XtSetArg (args[n], XmNlabelString, string); n++;
    resourcesPB = XmCreatePushButtonGadget(colorDialog.leftColForm, "resourcesPB", args, n);
    XtManageChild(resourcesPB);
    XtAddCallback(resourcesPB, XmNactivateCallback, resourcesCB,
                               (XtPointer) style.colorDialog);
    XmStringFree(string);

/*
**  Create a title box for palette color buttons above Number Of Colors
*/
    n=0;
    XtSetArg (args[n], XmNtopAttachment, XmATTACH_NONE);  n++;
    XtSetArg (args[n], XmNleftAttachment, XmATTACH_FORM);  n++;
    XtSetArg (args[n], XmNrightAttachment, XmATTACH_FORM);  n++;
    XtSetArg (args[n], XmNbottomAttachment, XmATTACH_WIDGET);  n++;
    XtSetArg (args[n], XmNbottomWidget, resourcesPB);  n++;
    XtSetArg (args[n], XmNbottomOffset, style.verticalSpacing);  n++;
    XtSetArg (args[n], XmNmarginWidth, 0);  n++;
    XtSetArg (args[n], XmNmarginHeight, 0);  n++;
    string = CMPSTR(pCurrentPalette->desc);
    XtSetArg (args[n], XmNtitleString, string); n++;
    colorDialog.buttonsTB = _DtCreateTitleBox(colorDialog.leftColForm, "ButtonsTB", args, n);
    XtManageChild(colorDialog.buttonsTB);
    XmStringFree(string);

    XtVaSetValues(
        colorDialog.paletteTB,
        XmNbottomAttachment, XmATTACH_WIDGET,
        XmNbottomWidget, colorDialog.buttonsTB,
        XmNbottomOffset, style.verticalSpacing,
        NULL
    );

    /* Create a form inside palette buttons titlebox */
    n = 0;
    XtSetArg(args[n], XmNhorizontalSpacing, style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNverticalSpacing, style.verticalSpacing); n++;
    style.buttonsForm = XmCreateForm(colorDialog.buttonsTB, "buttonsForm", args, n);
    XtManageChild(style.buttonsForm);
    
    /* Create Modify... button */
    if(TypeOfMonitor != XmCO_BLACK_WHITE)
    {
        n = 0;
        XtSetArg (args[n], XmNtopAttachment, XmATTACH_FORM);  n++;
        XtSetArg (args[n], XmNtopOffset, style.horizontalSpacing+BORDER_WIDTH);  n++;
        XtSetArg (args[n], XmNleftAttachment, XmATTACH_POSITION);  n++;
        XtSetArg (args[n], XmNleftPosition, ADD_PALETTE_LEFT_POSITION);  n++;
        XtSetArg (args[n], XmNrightAttachment, XmATTACH_FORM);  n++;
        string = CMPSTR(((char *)GETMESSAGE(14, 8, "Modify...")));
        XtSetArg (args[n], XmNlabelString, string); n++;
        modifyColorButton = 
        XmCreatePushButtonGadget(style.buttonsForm, "modifyColorButton", args, n);
        XmStringFree(string);
        XtManageChild(modifyColorButton);
        XtAddCallback(modifyColorButton, XmNactivateCallback, modifyColorCB, 
                            (XtPointer) nullptr);
    }

/*
**  Right Column 1: Motif Component Style & Layout Spacing TitleBox
*/
    XmString st_title = CMPSTR("Motif Component Style");
    n = 0;
    XtSetArg(args[n], XmNtitleString, st_title); n++;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
    colorDialog.styleTB = _DtCreateTitleBox(colorDialog.rightColForm, "styleTB", args, n);
    XmStringFree(st_title);
    XtManageChild(colorDialog.styleTB);

    Widget style_rc = XtVaCreateManagedWidget(
        "styleRC", xmRowColumnWidgetClass, colorDialog.styleTB,
        XmNchildType, XmWORK_AREA,
        XmNorientation, XmVERTICAL,
        XmNspacing, 6,
        XmNmarginWidth, 6,
        XmNmarginHeight, 4,
        NULL
    );

    Widget style_pd = XmCreatePulldownMenu(style_rc, "stylePD", nullptr, 0);
    static const char *const k_style_labels[3] = {
        "Classic CDE (3D Bevel)",
        "Modern Motif (Flat & Rounded)",
        "High Contrast (Accessible)",
    };
    for (int si = 0; si < 3; ++si) {
        XmString xs = CMPSTR((char *)k_style_labels[si]);
        colorDialog.styleOptBtns[si] = XtVaCreateManagedWidget(
            "styleItem", xmPushButtonGadgetClass, style_pd,
            XmNlabelString, xs,
            NULL
        );
        XmStringFree(xs);
        XtAddCallback(colorDialog.styleOptBtns[si], XmNactivateCallback,
                      SelectWidgetStyleCB, (XtPointer)(intptr_t)si);
    }
    XmString st_lbl = CMPSTR("Widget Style:");
    n = 0;
    XtSetArg(args[n], XmNlabelString, st_lbl); n++;
    XtSetArg(args[n], XmNsubMenuId, style_pd); n++;
    colorDialog.styleOptMenu = XmCreateOptionMenu(style_rc, "styleOptMenu", args, n);
    XmStringFree(st_lbl);
    XtManageChild(colorDialog.styleOptMenu);

    Widget dens_pd = XmCreatePulldownMenu(style_rc, "densPD", nullptr, 0);
    static const char *const k_dens_labels[2] = {
        "Comfortable (Standard)",
        "Compact (Dense)",
    };
    for (int di = 0; di < 2; ++di) {
        XmString xs = CMPSTR((char *)k_dens_labels[di]);
        colorDialog.densityOptBtns[di] = XtVaCreateManagedWidget(
            "densItem", xmPushButtonGadgetClass, dens_pd,
            XmNlabelString, xs,
            NULL
        );
        XmStringFree(xs);
        XtAddCallback(colorDialog.densityOptBtns[di], XmNactivateCallback,
                      SelectDensityCB, (XtPointer)(intptr_t)di);
    }
    XmString dn_lbl = CMPSTR("Layout Spacing:");
    n = 0;
    XtSetArg(args[n], XmNlabelString, dn_lbl); n++;
    XtSetArg(args[n], XmNsubMenuId, dens_pd); n++;
    colorDialog.densityOptMenu = XmCreateOptionMenu(style_rc, "densityOptMenu", args, n);
    XmStringFree(dn_lbl);
    XtManageChild(colorDialog.densityOptMenu);

    XmString sum_xs = CMPSTR("Effective Theme: Classic CDE (3D Bevel)   |   Spacing: Comfortable");
    colorDialog.themeSummaryLabel = XtVaCreateManagedWidget(
        "themeSummaryLabel", xmLabelGadgetClass, style_rc,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNlabelString, sum_xs,
        NULL
    );
    XmStringFree(sum_xs);

/*
**  Right Column 2: Live Desktop & Component Preview TitleBox
*/
    XmString pv_title = CMPSTR("Live Desktop & Component Preview");
    n = 0;
    XtSetArg(args[n], XmNtitleString, pv_title); n++;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNtopWidget, colorDialog.styleTB); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
    colorDialog.previewTB = _DtCreateTitleBox(colorDialog.rightColForm, "previewTB", args, n);
    XmStringFree(pv_title);
    XtManageChild(colorDialog.previewTB);

    Widget pv_frame = XtVaCreateManagedWidget(
        "previewFrame", xmFrameWidgetClass, colorDialog.previewTB,
        XmNchildType, XmWORK_AREA,
        XmNshadowType, XmSHADOW_IN,
        XmNshadowThickness, 2,
        NULL
    );

    colorDialog.previewCanvas = XtVaCreateManagedWidget(
        "previewCanvas", xmDrawingAreaWidgetClass, pv_frame,
        XmNwidth, 436,
        XmNheight, 292,
        NULL
    );
    XtAddCallback(colorDialog.previewCanvas, XmNexposeCallback, DrawLiveThemePreviewCB, nullptr);
    XtAddCallback(colorDialog.previewCanvas, XmNresizeCallback, DrawLiveThemePreviewCB, nullptr);
    XtAddEventHandler(colorDialog.previewCanvas, ButtonPressMask, False, PreviewCanvasInputEH, nullptr);

    style.count++;
}
 

