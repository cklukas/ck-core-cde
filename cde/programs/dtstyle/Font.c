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
 **   File:        Font.c
 **
 **   Project:     DT 3.0
 **
 **   Description: Controls the Dtstyle Font dialog
 **
 **
 **  (c) Copyright Hewlett-Packard Company, 1990.  
 **
 **
 **
 ****************************************************************************
 ************************************<+>*************************************/
/* $XConsortium: Font.c /main/7 1996/10/30 11:14:15 drk $ */

/*+++++++++++++++++++++++++++++++++++++++*/
/* include files                         */
/*+++++++++++++++++++++++++++++++++++++++*/

#include <X11/Xlib.h>
#include <Xm/MwmUtil.h>

#include <Xm/Xm.h>
#include <Xm/XmP.h>
#include <Xm/MessageB.h>
#include <Xm/Form.h>
#include <Xm/Frame.h>
#include <Xm/LabelG.h>
#include <Xm/List.h>
#include <Xm/Scale.h>
#include <Xm/ToggleBG.h>
#include <Xm/PanedW.h>
#include <Xm/ComboBox.h>
#include <Xm/Text.h>
#include <Xm/TextF.h>
#include <Xm/VendorSEP.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

#include <Dt/DialogBox.h>
#include <Dt/Icon.h>
#include <Dt/TitleBox.h>

#include <Dt/Message.h>
#include <Dt/SessionM.h>
#include <Dt/HourGlass.h>
#include <Dt/Wsm.h>
#include <Dt/GetDispRes.h>
#include <string.h>
#include <errno.h>
#include "Help.h"
#include "Main.h"
#include "Resource.h"
#include "Protocol.h"
#include "SaveRestore.h"
#include "FontFaceCatalog.h"

#ifndef XmFONT_IS_FONTLIST
#define XmFONT_IS_FONTLIST XmFONT_IS_FONTSET
#endif

/*+++++++++++++++++++++++++++++++++++++++*/
/* Local #defines                        */
/*+++++++++++++++++++++++++++++++++++++++*/

#define FONT_MSG   ((char *)GETMESSAGE(5, 23, "Style Manager - Font"))
#define PREVIEW    ((char *)GETMESSAGE(5, 17, "Preview"))
#define IMMEDIATE  ((char *)GETMESSAGE(5, 18, "The changes to fonts will show up in some\napplications the next time they are started.\nOther applications, such as file manager and\napplication manager, will not show the font\nchanges until you Exit the desktop and then log\nback in.")) 
#define LATER      ((char *)GETMESSAGE(5, 19, "The selected font will be used when\n you restart this session."))
#define INFO_MSG   ((char *)GETMESSAGE(5, 24, "The font that is currently used for your desktop is not\navailable in the font list. If a new font is selected and\napplied, you will not be able to return to the current font\nusing the Style Manager - Font dialog."))
#define SYSTEM_MSG ((char *)GETMESSAGE(5, 20, "AaBbCcDdEeFfGg0123456789"))
#define USER_MSG   ((char *)GETMESSAGE(5, 21, "AaBbCcDdEeFfGg0123456789"))
#define BLANK_MSG  "                          "
#define FONT_SELECTION ((char *)GETMESSAGE(5, 25, "Font selection"))
#define FONT_LIST_TAG "dtstyle-font-dialog"


/*+++++++++++++++++++++++++++++++++++++++*/
/* Internal Variables                    */
/*+++++++++++++++++++++++++++++++++++++++*/

typedef struct {
    Widget fontWkarea;
    Widget fontpictLabel;
    Widget previewTB;
    Widget previewForm;
    Widget systemLabel;
    Widget userText;
    Widget sizeTB;
    Widget catalogForm;
    Widget familyList;
    Widget variantList;
    Widget charsetList;
    Widget sizeList;
    Widget fontTypeLabel;
    Widget dtwmApplyTG;
    Widget dtwmIconTG;
    Widget dtwmGlobalTG;
    Widget dtwmPanelTG;
    Widget dtwmRestartTG;
    Widget dtHelpApplyTG;
    int    originalFontIndex;
    int    selectedFontIndex;
    int    selectedFamilyIndex;
    int    selectedVariantIndex;
    int    selectedCharsetIndex;
    String selectedFontStr;
    Boolean userTextChanged;
    Boolean dirty;
    FontDescriptor *descriptorBackup;
    int             backupIndex;
} FontData;
static FontData font;
static FontFaceCatalog *faceCatalog = NULL;

int _DtWmRestartNoConfirm (Display *display, Window root);

static void
UpdateDtwmToggleSensitivity(
        void )
{
    Boolean enabled = False;
    if (font.dtwmApplyTG)
        enabled = XmToggleButtonGadgetGetState(font.dtwmApplyTG);

    if (font.dtwmIconTG)
        XtSetSensitive(font.dtwmIconTG, enabled);
    if (font.dtwmGlobalTG)
        XtSetSensitive(font.dtwmGlobalTG, enabled);
    if (font.dtwmPanelTG)
        XtSetSensitive(font.dtwmPanelTG, enabled);

    if (font.dtwmRestartTG) {
        Boolean canRestart = enabled && style.xrdb.writeXrdbImmediate;
        XtSetSensitive(font.dtwmRestartTG, canRestart);
        if (!canRestart)
            XmToggleButtonGadgetSetState(font.dtwmRestartTG, False, False);
    }
}

static void
dtwmApplyToggleCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
    (void)w;
    (void)client_data;
    (void)call_data;
    UpdateDtwmToggleSensitivity();
}

static void
AppendResourceLine(
        char *buffer,
        size_t bufferSize,
        const char *fmt, ... )
{
    size_t used;
    va_list ap;

    if (!buffer || bufferSize == 0)
        return;

    used = strlen(buffer);
    if (used >= bufferSize - 1)
        return;

    va_start(ap, fmt);
    (void)vsnprintf(buffer + used, bufferSize - used, fmt, ap);
    va_end(ap);
}

static const char *
GetFontChoiceString(
        int index )
{
    FontDescriptor *desc = NULL;
    if (index >= 0 && index < style.xrdb.numFonts)
        desc = style.xrdb.fontChoice[index].descriptor;

    if (desc && desc->raw && *desc->raw)
        return desc->raw;

    if (index >= 0 && index < style.xrdb.numFonts) {
        if (style.xrdb.fontChoice[index].sysStr)
            return style.xrdb.fontChoice[index].sysStr;
        if (style.xrdb.fontChoice[index].userStr)
            return style.xrdb.fontChoice[index].userStr;
    }

    return NULL;
}

static void
ClearDescriptorBackup(
        void )
{
    if (font.descriptorBackup) {
        FreeFontDescriptor(font.descriptorBackup);
        font.descriptorBackup = NULL;
    }
    font.backupIndex = -1;
}

static void
RestoreDescriptorBackup(
        void )
{
    if (!font.descriptorBackup || font.backupIndex < 0)
        return;

    int idx = font.backupIndex;
    FreeFontDescriptor(style.xrdb.fontChoice[idx].descriptor);
    style.xrdb.fontChoice[idx].descriptor =
        DuplicateFontDescriptor(font.descriptorBackup);
    ClearDescriptorBackup();
}

static saveRestore save = {FALSE, 0, };

/*+++++++++++++++++++++++++++++++++++++++*/
/* Internal Functions                    */
/*+++++++++++++++++++++++++++++++++++++++*/


static void CreateFontDlg( 
                        Widget parent) ;
static void _DtmapCB_fontBB( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void ButtonCB( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static const char *GetFontChoiceString( int index );
static void RestoreDescriptorBackup( void );
static void ClearDescriptorBackup( void );
static void fontFaceSelectedCB(
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data ) ;
static void fontSizeSelectedCB(
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data ) ;
static void fontFamilySelectedCB(
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data ) ;
static void fontVariantSelectedCB(
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data ) ;
static void fontCharsetSelectedCB(
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data ) ;
static void PopulateCharsetList(
                        int familyIndex,
                        int variantIndex );
static void valueChangedCB( 
                        Widget w,
                        XtPointer client_data,
                        XtPointer call_data) ;
static void ClearSizeList( void );
static void PopulateFamilyList( void );
static void PopulateVariantList( int familyIndex );
static void PopulateVariantSizeList(
                        int familyIndex,
                        int variantIndex,
                        int charsetIndex,
                        int selectedSize );
static void ApplyVariantAndCharset(
                        int familyIndex,
                        int variantIndex,
                        int charsetIndex,
                        int desiredSize );
static void UpdateFontTypeLabel(
                        int index );
static FontFaceCatalog *EnsureFaceCatalog( void );
static void UpdatePreviewFonts(
                        int index );
static void RebuildFontListsForIndex(
                        int index );
static void RefreshFontDialogState( void );

typedef struct {
    XmFontList sysFont;
    XmFontList userFont;
    XFontSet sysFontSet;
    XFontSet userFontSet;
    XFontStruct *sysFontStruct;
    XFontStruct *userFontStruct;
    Boolean pending;
} FontResourceTrash;

static FontResourceTrash fontTrash[10];

static void
FontResourceFreeTimeout(
        XtPointer client_data,
        XtIntervalId *id )
{
    FontResourceTrash *trash = (FontResourceTrash *)client_data;
    if (!trash)
        return;
    if (trash->sysFont)
        XmFontListFree(trash->sysFont);
    if (trash->userFont)
        XmFontListFree(trash->userFont);
    if (trash->sysFontSet)
        XFreeFontSet(style.display, trash->sysFontSet);
    if (trash->userFontSet)
        XFreeFontSet(style.display, trash->userFontSet);
    if (trash->sysFontStruct)
        XFreeFont(style.display, trash->sysFontStruct);
    if (trash->userFontStruct)
        XFreeFont(style.display, trash->userFontStruct);
    memset(trash, 0, sizeof(*trash));
    trash->pending = False;
}

static Boolean
FontDebugEnabled(void)
{
    static int cached = -1;
    if (cached >= 0)
        return cached ? True : False;
    const char *env = getenv("DTSTYLE_FONT_DEBUG");
    cached = (env && *env && strcmp(env, "0") != 0) ? 1 : 0;
    return cached ? True : False;
}

static void
FontDebugLog(const char *fmt, ...)
{
    if (!FontDebugEnabled())
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "dtstyle: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static void
ComputeScreenDpi(
        int *outDpiX,
        int *outDpiY )
{
    if (outDpiX)
        *outDpiX = 96;
    if (outDpiY)
        *outDpiY = 96;

    if (!style.display)
        return;

    int screen = style.screenNum;
    int widthPx = DisplayWidth(style.display, screen);
    int heightPx = DisplayHeight(style.display, screen);
    int widthMm = DisplayWidthMM(style.display, screen);
    int heightMm = DisplayHeightMM(style.display, screen);

    if (widthPx > 0 && widthMm > 0) {
        double dpiX = (double)widthPx * 25.4 / (double)widthMm;
        if (dpiX > 1.0 && dpiX < 1000.0 && outDpiX)
            *outDpiX = (int)(dpiX + 0.5);
    }
    if (heightPx > 0 && heightMm > 0) {
        double dpiY = (double)heightPx * 25.4 / (double)heightMm;
        if (dpiY > 1.0 && dpiY < 1000.0 && outDpiY)
            *outDpiY = (int)(dpiY + 0.5);
    }
}

static int
PointToPixel(
        int pointSize,
        int dpi )
{
    if (pointSize <= 0 || dpi <= 0)
        return 0;
    double px = (double)pointSize * (double)dpi / 72.0;
    int rounded = (int)(px + 0.5);
    if (rounded < 1)
        rounded = 1;
    return rounded;
}


/*+++++++++++++++++++++++++++++++++++++++*/
/* popup_fontBB                          */
/*+++++++++++++++++++++++++++++++++++++++*/
void 
popup_fontBB(
        Widget shell )
{
  if (style.fontDialog == NULL) {
    _DtTurnOnHourGlass(shell);  
    CreateFontDlg(shell); 
    XtManageChild(style.fontDialog);
    _DtTurnOffHourGlass(shell);  
  } else { 
    XtManageChild(style.fontDialog);
    raiseWindow(XtWindow(XtParent(style.fontDialog)));
  }

  RefreshFontDialogState();
}

/*+++++++++++++++++++++++++++++++++++++++*/
/* CreateFontDlg                         */
/*+++++++++++++++++++++++++++++++++++++++*/
static void 
CreateFontDlg(
        Widget parent )
{

    int     n;
    int              i;
    Arg              args[MAX_ARGS];
    Widget           appTBox;
    Widget           sizeMenuPlDn;
    XmString         button_string[NUM_LABELS];
    XmString         string;
    int              count = 0;
    Widget           widget_list[6];
    XmString         *sizeItems;
    XmStringTable    selectedSize;
    char             sizeStr[111];
    Dimension        fontheight;


    font.selectedFontStr = style.xrdb.systemFontStr;

    /* Assume nothing is selected */
    font.selectedFontIndex = -1;
    font.selectedFamilyIndex = -1;
    font.selectedVariantIndex = -1;

    /* 
     * The following flag is used to determine if the user has 
     * entered anything into the sample user font field.  If 
     * he does, than when the font selection is changed, the 
     * default message "aAbBcC..." won't be displayed overwriting
     * the user's text, only the fontlist will be changed. 
     * This flag will be set in the valueChanged callback for the
     * font.sizeList widget.
     */
    font.userTextChanged = FALSE;
    ClearDescriptorBackup();

    /* 
     * Look for the selectedFont in the fontChoice array and set 
     * selectedFontIndex to that entry
     */
    /* Prefer the current *systemFont value even if it does not match the
     * limited application-default presets.  We store a working descriptor in
     * fontChoice[0] so the rest of the dialog can preview and apply it. */
    if (font.selectedFontStr && *font.selectedFontStr) {
        FontDescriptor *parsed = ParseXLFD(font.selectedFontStr);
        if (parsed) {
            font.selectedFontIndex = 0;
            if (style.xrdb.fontChoice[0].descriptor)
                FreeFontDescriptor(style.xrdb.fontChoice[0].descriptor);
            style.xrdb.fontChoice[0].descriptor = parsed;
            RebuildFontListsForIndex(0);
        }
    }
    if (font.selectedFontIndex < 0) {
        for (i=0; i<style.xrdb.numFonts; i++) {
            const char *candidate = GetFontChoiceString(i);
            if (candidate && font.selectedFontStr &&
                strcmp(font.selectedFontStr, candidate) == 0)
            {
                font.selectedFontIndex = i;
                if (!style.xrdb.fontChoice[i].userFont)
                    GetUserFontResource(i);
                if (!style.xrdb.fontChoice[i].sysFont)
                    GetSysFontResource(i);
                break;
            }
        }
    }
    if (font.selectedFontIndex < 0 && style.xrdb.numFonts > 0) {
        font.selectedFontIndex = 0;
        RebuildFontListsForIndex(0);
    }

    /* 
     * Save the index of the originally selected font.  If no
     * font is selected, this value will remain -1.
     */
    font.originalFontIndex = font.selectedFontIndex;
    font.dirty = FALSE;

    /* Set up button labels. */
    button_string[0] = CMPSTR((String) _DtOkString);
    button_string[1] = CMPSTR((String) _DtCancelString);
    button_string[2] = CMPSTR((String) _DtHelpString);

    /* Create toplevel DialogBox */
    /* saveRestore
     * Note that save.poscnt has been initialized elsewhere.  
     * save.posArgs may contain information from restoreFont().
     */

    XtSetArg(save.posArgs[save.poscnt], XmNallowOverlap, False); save.poscnt++;
    XtSetArg(save.posArgs[save.poscnt], XmNdefaultPosition, False); 
    save.poscnt++;
    XtSetArg(save.posArgs[save.poscnt], XmNbuttonCount, NUM_LABELS);  
    save.poscnt++;
    XtSetArg(save.posArgs[save.poscnt], XmNbuttonLabelStrings, button_string); 
    save.poscnt++;
    style.fontDialog = 
        __DtCreateDialogBoxDialog(parent, "Fonts", save.posArgs, save.poscnt);
    XtAddCallback(style.fontDialog, XmNcallback, ButtonCB, NULL);
    XtAddCallback(style.fontDialog, XmNmapCallback, _DtmapCB_fontBB, 
                            (XtPointer)parent);
    XtAddCallback(style.fontDialog, XmNhelpCallback,
            (XtCallbackProc)HelpRequestCB, (XtPointer)HELP_FONT_DIALOG);

    XmStringFree(button_string[0]);
    XmStringFree(button_string[1]);
    XmStringFree(button_string[2]);

    widget_list[0] = _DtDialogBoxGetButton(style.fontDialog,2);
    n=0;
    XtSetArg(args[n], XmNautoUnmanage, False); n++;
    XtSetArg(args[n], XmNcancelButton, widget_list[0]); n++;
    XtSetValues (style.fontDialog, args, n);

    n=0;
    XtSetArg(args[n], XmNtitle, FONT_MSG); n++;
    XtSetArg (args[n], XmNuseAsyncGeometry, True); n++;
    XtSetArg(args[n], XmNmwmFunctions, DIALOG_MWM_FUNC); n++;
    XtSetValues (XtParent(style.fontDialog), args, n);

    n = 0;
    XtSetArg (args[n], XmNchildType, XmWORK_AREA);  n++;
    XtSetArg(args[n], XmNhorizontalSpacing, style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNverticalSpacing, style.verticalSpacing); n++;
    XtSetArg(args[n], XmNallowOverlap, False); n++;
    font.fontWkarea = XmCreateForm(style.fontDialog, "fontWorkArea", args, n);

    n = 0;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM);  n++;
    XtSetArg(args[n], XmNtopOffset, style.verticalSpacing);  n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM);  n++;

    XtSetArg(args[n], XmNfillMode, XmFILL_SELF); n++;
    XtSetArg(args[n], XmNbehavior, XmICON_LABEL); n++;
    XtSetArg(args[n], XmNpixmapForeground, style.secBSCol); n++;
    XtSetArg(args[n], XmNpixmapBackground, style.secTSCol); n++;
    XtSetArg(args[n], XmNstring, NULL); n++;  
    XtSetArg(args[n], XmNshadowThickness, 0); n++;  
    XtSetArg(args[n], XmNimageName, FONT_ICON); n++;  
    XtSetArg(args[n], XmNtraversalOn, False); n++;  
    widget_list[count++] = font.fontpictLabel = 
        _DtCreateIcon(font.fontWkarea, "fontpictLabel", args, n);

    /* Create a TitleBox to host the font catalog. */

    n = 0;
    string = CMPSTR(FONT_SELECTION);
    XtSetArg(args[n], XmNtitleString, string);  n++;
    XtSetArg(args[n], XmNtopAttachment,      XmATTACH_WIDGET);     n++;
    XtSetArg(args[n], XmNtopWidget,          font.fontpictLabel);  n++;
    XtSetArg(args[n], XmNtopOffset,          style.verticalSpacing+5);  n++;
    XtSetArg(args[n], XmNleftAttachment,     XmATTACH_FORM);       n++;
    XtSetArg(args[n], XmNleftOffset,         style.horizontalSpacing);  n++;
    XtSetArg(args[n], XmNbottomAttachment,   XmATTACH_FORM);       n++;
    XtSetArg(args[n], XmNbottomOffset,       style.verticalSpacing);    n++;
    widget_list[count++] = font.sizeTB =
        _DtCreateTitleBox(font.fontWkarea, "sizeTB", args, n); 
    XmStringFree(string);

    /* Build the catalog selection UI inside the TitleBox work area. */
    Widget catalogWorkArea = _DtTitleBoxGetWorkArea(font.sizeTB);
    if (!catalogWorkArea) {
        int waArgs = 0;
        XtSetArg(args[waArgs], XmNchildType, XmWORK_AREA);  waArgs++;
        catalogWorkArea = XmCreateForm(font.sizeTB, "catalogWorkArea", args, waArgs);
        XtManageChild(catalogWorkArea);
    }

    n = 0;
    XtSetArg(args[n], XmNfractionBase, 100); n++;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
    font.catalogForm = XmCreateForm(catalogWorkArea, "catalogForm", args, n);
    XtManageChild(font.catalogForm);

    n = 0;
    XtSetArg(args[n], XmNselectionPolicy, XmBROWSE_SELECT); n++;
    XtSetArg(args[n], XmNautomaticSelection, True); n++;
    XtSetArg(args[n], XmNvisibleItemCount, 12); n++;
    font.familyList = XmCreateScrolledList(font.catalogForm, "familyList", args, n);
    XtVaSetValues(XtParent(font.familyList),
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_POSITION,
                  XmNrightPosition, 25,
                  NULL);
    XtAddCallback(font.familyList, XmNbrowseSelectionCallback,
                  fontFamilySelectedCB, NULL);
    XtManageChild(font.familyList);

    n = 0;
    XtSetArg(args[n], XmNselectionPolicy, XmBROWSE_SELECT); n++;
    XtSetArg(args[n], XmNautomaticSelection, True); n++;
    XtSetArg(args[n], XmNvisibleItemCount, 12); n++;
    font.variantList = XmCreateScrolledList(font.catalogForm, "variantList", args, n);
    XtVaSetValues(XtParent(font.variantList),
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_POSITION,
                  XmNleftPosition, 25,
                  XmNrightAttachment, XmATTACH_POSITION,
                  XmNrightPosition, 50,
                  NULL);
    XtAddCallback(font.variantList, XmNbrowseSelectionCallback,
                  fontVariantSelectedCB, NULL);
    XtManageChild(font.variantList);

    n = 0;
    XtSetArg(args[n], XmNselectionPolicy, XmBROWSE_SELECT); n++;
    XtSetArg(args[n], XmNautomaticSelection, True); n++;
    XtSetArg(args[n], XmNvisibleItemCount, 12); n++;
    font.charsetList = XmCreateScrolledList(font.catalogForm, "charsetList", args, n);
    XtVaSetValues(XtParent(font.charsetList),
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_POSITION,
                  XmNleftPosition, 50,
                  XmNrightAttachment, XmATTACH_POSITION,
                  XmNrightPosition, 75,
                  NULL);
    XtAddCallback(font.charsetList, XmNbrowseSelectionCallback,
                  fontCharsetSelectedCB, NULL);
    XtManageChild(font.charsetList);

    n = 0;
    XtSetArg(args[n], XmNselectionPolicy, XmBROWSE_SELECT); n++;
    XtSetArg(args[n], XmNautomaticSelection, True); n++;
    XtSetArg(args[n], XmNvisibleItemCount, 12); n++;
    font.sizeList = XmCreateScrolledList(font.catalogForm, "sizeList", args, n);
    XtVaSetValues(XtParent(font.sizeList),
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_POSITION,
                  XmNleftPosition, 75,
                  XmNrightAttachment, XmATTACH_FORM,
                  NULL);
    XtAddCallback(font.sizeList, XmNbrowseSelectionCallback,
                  fontSizeSelectedCB, NULL);
    XtManageChild(font.sizeList);

    /* preview TitleBox */
    n = 0;
    XtSetArg(args[n], XmNtopAttachment,      XmATTACH_WIDGET);     n++;
    XtSetArg(args[n], XmNtopWidget,          font.fontpictLabel);  n++;
    XtSetArg(args[n], XmNtopOffset,          style.verticalSpacing+5);  n++;
    XtSetArg(args[n], XmNleftAttachment,     XmATTACH_WIDGET);     n++; 
    XtSetArg(args[n], XmNleftWidget,         font.sizeTB);         n++;
    XtSetArg(args[n], XmNrightAttachment,    XmATTACH_FORM);       n++;
    XtSetArg(args[n], XmNrightOffset,        style.horizontalSpacing);  n++;
    XtSetArg(args[n], XmNbottomAttachment,   XmATTACH_FORM);       n++;
    XtSetArg(args[n], XmNbottomOffset,       style.verticalSpacing);    n++;
    string = CMPSTR(PREVIEW); 
    XtSetArg(args[n], XmNtitleString, string); n++;
    widget_list[count++] = font.previewTB =
        _DtCreateTitleBox(font.fontWkarea, "systemSample", args, n);
    XmStringFree(string);
  
    /*form to contain preview font area*/
    n = 0;
    font.previewForm = 
        XmCreateForm(font.previewTB, "previewForm", args, n);

    /* sample system font */
    n = 0;

    /* 
     * If a font match was found and selected, then set the fontlist
     * and the sample string.  Otherwise, output a blank message.
     */
    if (font.selectedFontIndex >=0) {
      XtSetArg (args[n], XmNfontList, 
        style.xrdb.fontChoice[font.selectedFontIndex].sysFont); n++; 
      string = CMPSTR(SYSTEM_MSG); 
    } else {
      string = CMPSTR(BLANK_MSG);
    }
    XtSetArg (args[n], XmNlabelString, string);  n++;
    XtSetArg (args[n], XmNalignment, XmALIGNMENT_BEGINNING);  n++;
    XtSetArg (args[n], XmNtopAttachment, XmATTACH_FORM);  n++;
    XtSetArg (args[n], XmNtopOffset, 2 * style.verticalSpacing);  n++;    
    XtSetArg (args[n], XmNleftAttachment, XmATTACH_FORM);  n++;
    XtSetArg (args[n], XmNrightAttachment, XmATTACH_FORM);  n++;
    XtSetArg (args[n], XmNbottomAttachment, XmATTACH_NONE);  n++;
    font.systemLabel = 
        XmCreateLabelGadget(font.previewForm, "systemSample", args, n);
    XmStringFree(string);

    /* sample user font */
    n = 0;
    /* 
     * If a font match was found and selected, then set the fontlist
     * and the sample string.   Otherwise output a blank message.
     */
    if (font.selectedFontIndex >=0) {
      XtSetArg (args[n], XmNfontList, 
	      style.xrdb.fontChoice[font.selectedFontIndex].userFont); n++;
      XtSetArg (args[n], XmNvalue, USER_MSG);  n++;
    } else {
      XtSetArg (args[n], XmNvalue, NULL);  n++;
    }

    XtSetArg (args[n], XmNtopAttachment, XmATTACH_WIDGET);  n++;
    XtSetArg (args[n], XmNtopWidget, font.systemLabel);  n++;
    XtSetArg (args[n], XmNtopOffset, 2 * style.verticalSpacing);  n++;    
    XtSetArg (args[n], XmNleftAttachment, XmATTACH_OPPOSITE_WIDGET);  n++;
    XtSetArg (args[n], XmNleftWidget, font.systemLabel);  n++;
    XtSetArg (args[n], XmNbottomAttachment, XmATTACH_NONE);  n++;
    XtSetArg (args[n], XmNrightAttachment, XmATTACH_OPPOSITE_WIDGET);  n++;
    XtSetArg (args[n], XmNrightWidget, font.systemLabel);  n++;
    font.userText = 
        XmCreateText(font.previewForm, "userText", args, n);
    /* Add callback to determine if user changes text in sample field */
    XtAddCallback(font.userText, XmNvalueChangedCallback, valueChangedCB, NULL);

    n = 0;
    string = CMPSTR("Type:");
    XtSetArg(args[n], XmNlabelString, string);  n++;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING);  n++;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET);  n++;
    XtSetArg(args[n], XmNtopWidget, font.userText);  n++;
    XtSetArg(args[n], XmNtopOffset, style.verticalSpacing);  n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM);  n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM);  n++;
    font.fontTypeLabel =
        XmCreateLabelGadget(font.previewForm, "fontTypeLabel", args, n);
    XmStringFree(string);

    n = 0;
    string = XmStringCreateLocalized("Apply to Window Manager titles");
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING); n++;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNtopWidget, font.fontTypeLabel); n++;
    XtSetArg(args[n], XmNtopOffset, style.verticalSpacing); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
    font.dtwmApplyTG =
        XmCreateToggleButtonGadget(font.previewForm, "dtwmApplyTG", args, n);
    XmStringFree(string);
    XtAddCallback(font.dtwmApplyTG, XmNvalueChangedCallback, dtwmApplyToggleCB, NULL);
    XmToggleButtonGadgetSetState(font.dtwmApplyTG, True, False);

    n = 0;
    string = XmStringCreateLocalized("Apply to Window Manager icon labels");
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING); n++;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNtopWidget, font.dtwmApplyTG); n++;
    XtSetArg(args[n], XmNtopOffset, 0); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
    font.dtwmIconTG =
        XmCreateToggleButtonGadget(font.previewForm, "dtwmIconTG", args, n);
    XmStringFree(string);
    XmToggleButtonGadgetSetState(font.dtwmIconTG, False, False);

    n = 0;
    string = XmStringCreateLocalized("Apply as Window Manager default font");
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING); n++;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNtopWidget, font.dtwmIconTG); n++;
    XtSetArg(args[n], XmNtopOffset, 0); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
    font.dtwmGlobalTG =
        XmCreateToggleButtonGadget(font.previewForm, "dtwmGlobalTG", args, n);
    XmStringFree(string);
    XmToggleButtonGadgetSetState(font.dtwmGlobalTG, False, False);

    n = 0;
    string = XmStringCreateLocalized("Apply to Front Panel");
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING); n++;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNtopWidget, font.dtwmGlobalTG); n++;
    XtSetArg(args[n], XmNtopOffset, 0); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
    font.dtwmPanelTG =
        XmCreateToggleButtonGadget(font.previewForm, "dtwmPanelTG", args, n);
    XmStringFree(string);
    XmToggleButtonGadgetSetState(font.dtwmPanelTG, False, False);

    n = 0;
    string = XmStringCreateLocalized("Restart Window Manager now (recommended)");
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING); n++;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNtopWidget, font.dtwmPanelTG); n++;
    XtSetArg(args[n], XmNtopOffset, 0); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
    font.dtwmRestartTG =
        XmCreateToggleButtonGadget(font.previewForm, "dtwmRestartTG", args, n);
    XmStringFree(string);
    XmToggleButtonGadgetSetState(font.dtwmRestartTG, True, False);

    n = 0;
    string = XmStringCreateLocalized("Override Help document fonts");
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_BEGINNING); n++;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNtopWidget, font.dtwmRestartTG); n++;
    XtSetArg(args[n], XmNtopOffset, 0); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
    font.dtHelpApplyTG =
        XmCreateToggleButtonGadget(font.previewForm, "dtHelpApplyTG", args, n);
    XmStringFree(string);
    XmToggleButtonGadgetSetState(font.dtHelpApplyTG, True, False);

    XtManageChild(font.systemLabel);
    XtManageChild(font.userText);
    XtManageChild(font.fontTypeLabel);
    XtManageChild(font.dtwmApplyTG);
    XtManageChild(font.dtwmIconTG);
    XtManageChild(font.dtwmGlobalTG);
    XtManageChild(font.dtwmPanelTG);
    XtManageChild(font.dtwmRestartTG);
    XtManageChild(font.dtHelpApplyTG);
    XtManageChild(font.previewForm);

    XtManageChildren(widget_list,count);
    XtManageChild(font.fontWkarea);

    UpdateDtwmToggleSensitivity();
}

static void
RefreshFontDialogState( void )
{
    if (style.xrdb.numFonts <= 0)
        return;

    font.selectedFontIndex = 0;
    font.selectedFontStr = style.xrdb.systemFontStr;

    if (font.selectedFontStr && *font.selectedFontStr) {
        FontDescriptor *parsed = ParseXLFD(font.selectedFontStr);
        if (parsed) {
            if (style.xrdb.fontChoice[0].descriptor)
                FreeFontDescriptor(style.xrdb.fontChoice[0].descriptor);
            style.xrdb.fontChoice[0].descriptor = parsed;
            RebuildFontListsForIndex(0);
        }
    }

    if (!style.xrdb.fontChoice[0].sysFont || !style.xrdb.fontChoice[0].userFont)
        RebuildFontListsForIndex(0);

    UpdatePreviewFonts(0);
    UpdateFontTypeLabel(0);

    FontDescriptor *desc = style.xrdb.fontChoice[0].descriptor;
    FontFaceCatalog *catalog = EnsureFaceCatalog();
    PopulateFamilyList();

    int familyIndex = -1;
    int variantIndex = -1;
    int charsetIndex = -1;
    if (catalog && desc)
        FontFaceCatalogFindVariantForDescriptor(catalog, desc,
                                               &familyIndex,
                                               &variantIndex,
                                               &charsetIndex);

    if (familyIndex < 0 && catalog)
        familyIndex = FontFaceCatalogFamilyCount(catalog) > 0 ? 0 : -1;

    if (familyIndex >= 0) {
        font.selectedFamilyIndex = familyIndex;
        if (font.familyList)
            XmListSelectPos(font.familyList, familyIndex + 1, False);
        PopulateVariantList(familyIndex);
    } else {
        ClearSizeList();
    }

    if (variantIndex < 0 && catalog && familyIndex >= 0)
        variantIndex = FontFaceCatalogVariantCount(catalog, familyIndex) > 0 ? 0 : -1;

    if (variantIndex >= 0) {
        font.selectedVariantIndex = variantIndex;
        if (font.variantList)
            XmListSelectPos(font.variantList, variantIndex + 1, False);
        PopulateCharsetList(familyIndex, variantIndex);
        int charsetCount =
            FontFaceCatalogVariantCharsetCount(catalog, familyIndex, variantIndex);
        if (charsetIndex < 0 && charsetCount > 0)
            charsetIndex = 0;
        if (charsetIndex >= 0 && charsetCount > 0) {
            font.selectedCharsetIndex = charsetIndex;
            if (font.charsetList)
                XmListSelectPos(font.charsetList, charsetIndex + 1, False);
            int currentSize = 0;
            if (desc) {
                if (desc->scalable && desc->pointSize > 0)
                    currentSize = desc->pointSize / 10;
                else if (!desc->scalable)
                    currentSize = desc->pixelSize;
            }
            PopulateVariantSizeList(familyIndex, variantIndex, charsetIndex, currentSize);
        } else {
            font.selectedCharsetIndex = -1;
            ClearSizeList();
        }
    } else {
        ClearSizeList();
    }

    ClearDescriptorBackup();
    if (desc) {
        font.descriptorBackup = DuplicateFontDescriptor(desc);
        font.backupIndex = 0;
    }

    font.originalFontIndex = 0;
    font.dirty = FALSE;
}


/*+++++++++++++++++++++++++++++++++++++++*/
/* _DtmapCB_fontBB                          */
/*+++++++++++++++++++++++++++++++++++++++*/
static void 
_DtmapCB_fontBB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
   
    DtWsmRemoveWorkspaceFunctions(style.display, XtWindow(XtParent(w)));

    if (!save.restoreFlag)
        putDialog ((Widget)client_data, w);
      
    XmTextShowPosition(font.userText, 0);    

    XtRemoveCallback(style.fontDialog, XmNmapCallback, _DtmapCB_fontBB, NULL);

}

    
/*+++++++++++++++++++++++++++++++++++++++++++++++++*/
/* ButtonCB                                        */
/* Process callback from PushButtons in DialogBox. */
/*+++++++++++++++++++++++++++++++++++++++++++++++++*/
static void 
ButtonCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
  DtDialogBoxCallbackStruct *cb     
           = (DtDialogBoxCallbackStruct *) call_data;
  int      n, len;
  char	   *str, *fntstr, *fntsetstr;
  Arg      args[MAX_ARGS];
  char     fontres[8192];

  switch (cb->button_position)
    {
        /* Set the xrdb or pass to dtsession and close the window */
      case OK_BUTTON:
         /*   Post an info dialog explaining when the new fonts will appear */
         if (font.dirty && (font.selectedFontIndex >= 0))
         {
            XtUnmanageChild(style.fontDialog);  

            if(style.xrdb.writeXrdbImmediate) 
            { 
              InfoDialog(IMMEDIATE, style.shell, False); 
            }
            else 
            {
              InfoDialog(LATER, style.shell, False); 
            }

            const char *chosenStr = GetFontChoiceString(font.selectedFontIndex);
            if (!chosenStr)
                chosenStr = style.xrdb.fontChoice[font.selectedFontIndex].userStr;

            /* 
               for *FontSet resource: find first font entry delimited by a ":" 
               or an "=". 
            */ 
            len = strcspn(chosenStr, ":=");
            fntsetstr = (char *) XtCalloc(1, len + 1);
            memcpy(fntsetstr, chosenStr, len);

            /* 
	       Since the *Font and *FontSet resources may be used by old
	       X applications, a fontlist of multiple fonts must be converted 
               to Xt font set format (';'s converted to ','s since many old X 
               apps don't understand ';' syntax.)
	    */
            str = strstr(fntsetstr,";");
            while (str) {
   		*str = ',';
 		str = strstr(str,";");
	    }

	    /* 
	       for *Font resource: find first font entry delimited by a comma, 
               a colon or an = 
	    */
            len = strcspn(fntsetstr,",:=");
            fntstr = (char *) XtCalloc(1, len + 1);
            memcpy(fntstr, 
		   chosenStr,
			   len);

	    /*
	      for *FontSet resource: if we got a font (instead of a font set)
	      from the first entry, then wildcard its charset fields
	     */
	    len = strlen(fntsetstr);
	    if (style.xrdb.fontChoice[font.selectedFontIndex].userStr[len] 
		!= ':') {
		str = strchr(fntsetstr, '-');
		for (n = 1; n < 13 && str; n++)
		    str = strchr(str + 1, '-');
		if (str)
		    strcpy(str + 1, "*-*");
	    }

	           /* create the font resource specs with the selected font for xrdb */
	            const char *sysStr = GetFontChoiceString(font.selectedFontIndex);
	            if (!sysStr)
	                sysStr = style.xrdb.fontChoice[font.selectedFontIndex].sysStr;
	            const char *userStr = chosenStr;
	            if (!userStr)
	                userStr = style.xrdb.fontChoice[font.selectedFontIndex].userStr;
	
            int resLen = snprintf(fontres, sizeof(fontres),
                "*systemFont: %s\n*userFont: %s\n*FontList: %s\n*buttonFontList: %s\n*labelFontList: %s\n*textFontList: %s\n*XmText*FontList: %s\n*XmTextField*FontList: %s\n*DtEditor*textFontList: %s\n*Font: %s\n*FontSet: %s\n",
                 sysStr,
                 userStr,
                 sysStr,
                 sysStr,
                 sysStr,
                 userStr,
                 userStr,
                 userStr,
	         userStr,
                 fntstr, fntsetstr);
            if (resLen < 0 || resLen >= (int)sizeof(fontres))
                fontres[sizeof(fontres) - 1] = '\0';

	            if (!font.dtHelpApplyTG ||
	                XmToggleButtonGadgetGetState(font.dtHelpApplyTG))
	            {
	                /*
	                 * Ensure the Help display area inherits the Style Manager font
	                 * by writing both the generic fontList resources and the
	                 * specific menu buttons that back the topic/display area.
	                 */
                const char *helpFont = (userStr && *userStr) ? userStr : sysStr;
                const char *helpFontList = (userStr && *userStr) ? userStr : helpFont;
                AppendResourceLine(fontres, sizeof(fontres),
                                   "DtHelpDialog*fontList: %s\n"
                                   "DtHelpQuickDialog*fontList: %s\n"
                                   "DtHelpDialog*homeTopic.fontList: %s\n"
                                   "DtHelpDialog*navigateMenu.homeTopic.fontList: %s\n"
                                   "DtHelpDialog*menuBar.navigateMenu.homeTopic.fontList: %s\n"
                                   "DtHelpQuickDialog*closeButton.fontList: %s\n"
                                   "DtHelpDialog*DisplayArea.userFont: %s\n"
                                   "DtHelpDialog*DisplayArea.fontList: %s\n"
                                   "DtHelpDialog*volumeLabel.fontList: %s\n"
                                   "DtHelpDialog*pathLabel.fontList: %s\n"
                                   "DtHelpDialog*overrideFontList: True\n",
                                   helpFont, helpFont,
                                   helpFont, helpFont, helpFont, helpFont,
                                   helpFont, helpFontList, helpFontList, helpFontList);
	            }

	            if (font.dtwmApplyTG && XmToggleButtonGadgetGetState(font.dtwmApplyTG))
	            {
	                AppendResourceLine(fontres, sizeof(fontres),
	                                   "Dtwm*client*fontList: %s\n"
	                                   "Dtwm*feedback*fontList: %s\n",
	                                   sysStr, sysStr);
	                if (font.dtwmIconTG && XmToggleButtonGadgetGetState(font.dtwmIconTG))
	                    AppendResourceLine(fontres, sizeof(fontres),
	                                       "Dtwm*icon*fontList: %s\n", sysStr);
	                if (font.dtwmGlobalTG && XmToggleButtonGadgetGetState(font.dtwmGlobalTG))
	                    AppendResourceLine(fontres, sizeof(fontres),
	                                       "Dtwm*fontList: %s\n", sysStr);
	                if (font.dtwmPanelTG && XmToggleButtonGadgetGetState(font.dtwmPanelTG))
	                    AppendResourceLine(fontres, sizeof(fontres),
	                                       "Dtwm*FrontPanel*highResFontList: %s\n"
	                                       "Dtwm*FrontPanel*mediumResFontList: %s\n"
	                                       "Dtwm*FrontPanel*lowResFontList: %s\n",
	                                       sysStr, sysStr, sysStr);
	            }
	
		    XtFree(fntstr);
		    XtFree(fntsetstr);
	
	            /* if writeXrdbImmediate true write to Xrdb else send to session mgr */
	    	    if(style.xrdb.writeXrdbImmediate)
		        _DtAddToResource(style.display,fontres);

	            SmNewFontSettings(fontres);

	            if (style.xrdb.writeXrdbImmediate &&
	                font.dtwmApplyTG && XmToggleButtonGadgetGetState(font.dtwmApplyTG) &&
	                font.dtwmRestartTG && XmToggleButtonGadgetGetState(font.dtwmRestartTG))
	            {
	                _DtWmRestartNoConfirm(style.display, style.root);
	            }
	
	            font.originalFontIndex = font.selectedFontIndex;
	            style.xrdb.systemFontStr = font.selectedFontStr;
	            ClearDescriptorBackup();
	            font.dirty = FALSE;
         }

         else 
           	 XtUnmanageChild(style.fontDialog);  
                 
         break;

    case CANCEL_BUTTON:

      /* reset preview area fonts to original and close the window*/

      XtUnmanageChild(style.fontDialog);

      RestoreDescriptorBackup();

      if (font.originalFontIndex >= 0) {
        font.selectedFontIndex = font.originalFontIndex;
        RebuildFontListsForIndex(font.originalFontIndex);
        UpdatePreviewFonts(font.originalFontIndex);
        UpdateFontTypeLabel(font.originalFontIndex);
      } else { 
	/* 
	 * if no font was originally selected, need to undo any results
	 * from selections that were made by user before pressing Cancel.
	 */
        XtVaSetValues (font.userText, 
		       XmNvalue, BLANK_MSG, 
		       XmNfontList, style.xrdb.userFont,
		       NULL);
	XtVaSetValues (font.systemLabel, 
		       XmNlabelString, CMPSTR(BLANK_MSG), 
		       XmNfontList, style.xrdb.systemFont,
		       NULL);
	font.userTextChanged = FALSE;
	font.selectedFontIndex = -1;
        UpdateFontTypeLabel(-1);
      }
      font.dirty = FALSE;
      break;

    case HELP_BUTTON:
      XtCallCallbacks(style.fontDialog, XmNhelpCallback, (XtPointer)NULL);
      break;

    default:
      break;
    }
}


/*+++++++++++++++++++++++++++++++++++++++*/
/* valueChangedCB                        */
/*  Set flag indicating that the user    */
/*  text field has been modified.        */
/*+++++++++++++++++++++++++++++++++++++++*/
static void 
valueChangedCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )

{ 
  font.userTextChanged = TRUE; 
}

static void 
UpdateFontTypeLabel(
        int index )
{
    char labelText[96];
    FontDescriptor *desc = NULL;
    Arg args[1];

    if (index >= 0 && index < style.xrdb.numFonts)
        desc = style.xrdb.fontChoice[index].descriptor;

    if (index >= 0)
        font.selectedFontStr = (String)GetFontChoiceString(index);

    if (desc) {
        if (desc->scalable) {
            snprintf(labelText, sizeof(labelText), "Type: scalable (pt sizes)");
        } else {
            snprintf(labelText, sizeof(labelText), "Type: bitmap (px sizes)");
        }
    } else {
        snprintf(labelText, sizeof(labelText), "Type: (none)");
    }

    XmString label = CMPSTR(labelText);
    XtSetArg(args[0], XmNlabelString, label);
    XtSetValues(font.fontTypeLabel, args, 1);
    XmStringFree(label);
}

static void
ClearSizeList( void )
{
    if (!font.sizeList)
        return;
    XmListDeleteAllItems(font.sizeList);
}

static void
PopulateFamilyList( void )
{
    if (!font.familyList)
        return;
    XmListDeleteAllItems(font.familyList);
    FontFaceCatalog *catalog = EnsureFaceCatalog();
    if (!catalog)
        return;
    int familyCount = FontFaceCatalogFamilyCount(catalog);
    for (int i = 0; i < familyCount; i++)
        XmListAddItem(font.familyList, FontFaceCatalogFamilyLabel(catalog, i), 0);
}

static void
PopulateVariantList(
        int familyIndex )
{
    if (!font.variantList)
        return;
    XmListDeleteAllItems(font.variantList);
    FontFaceCatalog *catalog = EnsureFaceCatalog();
    if (!catalog || familyIndex < 0)
        return;
    int variantCount = FontFaceCatalogVariantCount(catalog, familyIndex);
    for (int i = 0; i < variantCount; i++)
        XmListAddItem(font.variantList,
                      FontFaceCatalogVariantLabel(catalog, familyIndex, i),
                      0);
}

static void
PopulateCharsetList(
        int familyIndex,
        int variantIndex )
{
    if (!font.charsetList)
        return;
    XmListDeleteAllItems(font.charsetList);
    FontFaceCatalog *catalog = EnsureFaceCatalog();
    if (!catalog || familyIndex < 0 || variantIndex < 0)
        return;
    int charsetCount =
        FontFaceCatalogVariantCharsetCount(catalog, familyIndex, variantIndex);
    for (int i = 0; i < charsetCount; i++)
        XmListAddItem(font.charsetList,
                      FontFaceCatalogVariantCharsetLabel(catalog, familyIndex, variantIndex, i),
                      0);
}

static void
PopulateVariantSizeList(
        int familyIndex,
        int variantIndex,
        int charsetIndex,
        int selectedSize )
{
    if (!font.sizeList)
        return;
    XmListDeleteAllItems(font.sizeList);
    FontFaceCatalog *catalog = EnsureFaceCatalog();
    if (!catalog || familyIndex < 0 || variantIndex < 0 || charsetIndex < 0)
        return;

    Boolean scalable =
        FontFaceCatalogVariantIsScalable(catalog, familyIndex, variantIndex);
    FontDebugLog("size list: family=%d variant=%d charset=%d scalable=%d selected=%d",
                 familyIndex, variantIndex, charsetIndex, scalable ? 1 : 0, selectedSize);
    if (scalable) {
        static const int common_pt_sizes[] = {
            6, 7, 8, 9, 10, 11, 12, 14, 16, 18, 20, 24, 28, 32, 36, 48, 72
        };
        const int count = (int)(sizeof(common_pt_sizes) / sizeof(common_pt_sizes[0]));
        for (int i = 0; i < count; i++) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", common_pt_sizes[i]);
            XmString item = CMPSTR(buf);
            XmListAddItem(font.sizeList, item, 0);
            XmStringFree(item);
        }
        if (selectedSize > 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", selectedSize);
            XmString select = CMPSTR(buf);
            XmListSelectItem(font.sizeList, select, False);
            XmStringFree(select);
        } else {
            XmListSelectPos(font.sizeList, 1, False);
        }
        return;
    }

    int sizeCount =
        FontFaceCatalogVariantCharsetSizeCount(catalog, familyIndex, variantIndex, charsetIndex);
    FontDebugLog("size list: bitmap sizes=%d", sizeCount);
    for (int i = 0; i < sizeCount; i++) {
        int size = FontFaceCatalogVariantCharsetSizeAt(
                catalog, familyIndex, variantIndex, charsetIndex, i);
        if (size <= 0)
            continue;
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", size);
        XmString item = CMPSTR(buf);
        XmListAddItem(font.sizeList, item, 0);
        XmStringFree(item);
    }

    if (sizeCount <= 0)
        return;

    if (selectedSize > 0) {
        int nearest = FontFaceCatalogVariantCharsetNearestSize(
                catalog, familyIndex, variantIndex, charsetIndex, selectedSize);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", nearest);
        XmString select = CMPSTR(buf);
        XmListSelectItem(font.sizeList, select, False);
        XmStringFree(select);
    } else {
        XmListSelectPos(font.sizeList, 1, False);
    }
}

static void
ApplyVariantAndCharset(
        int familyIndex,
        int variantIndex,
        int charsetIndex,
        int desiredSize )
{
    if (familyIndex < 0 || variantIndex < 0 || charsetIndex < 0 ||
        font.selectedFontIndex < 0)
        return;

    FontFaceCatalog *catalog = EnsureFaceCatalog();
    if (!catalog)
        return;

    FontDescriptor *current =
        style.xrdb.fontChoice[font.selectedFontIndex].descriptor;
    int savedSize = desiredSize;
    if (savedSize <= 0 && current) {
        if (current->scalable && current->pointSize > 0)
            savedSize = current->pointSize / 10;
        else if (!current->scalable)
            savedSize = current->pixelSize;
    }
    if (savedSize <= 0)
        savedSize = 12;

    FontDebugLog("apply: family=%d variant=%d charset=%d desired=%d resolved=%d",
                 familyIndex, variantIndex, charsetIndex, desiredSize, savedSize);

    FontDescriptor *newDesc =
        FontFaceCatalogCreateDescriptorForVariant(
                catalog,
                familyIndex,
                variantIndex,
                charsetIndex,
                savedSize);
    if (!newDesc)
        return;

    if (newDesc->scalable) {
        int dpiX = 96;
        int dpiY = 96;
        ComputeScreenDpi(&dpiX, &dpiY);
        FontDescriptorSetResolution(newDesc, dpiX, dpiY);
        int pixelSize = PointToPixel(savedSize, dpiY);
        if (pixelSize > 0)
            FontDescriptorSetPixelSize(newDesc, pixelSize);
        FontDebugLog("apply: scalable dpi=%d/%d pt=%d px=%d raw=%s",
                     dpiX, dpiY, savedSize, pixelSize,
                     newDesc->raw ? newDesc->raw : "(null)");
    } else {
        FontDebugLog("apply: bitmap px=%d raw=%s",
                     newDesc->pixelSize,
                     newDesc->raw ? newDesc->raw : "(null)");
    }

    FreeFontDescriptor(current);
    style.xrdb.fontChoice[font.selectedFontIndex].descriptor = newDesc;
    font.selectedFontStr = newDesc->raw;
    font.selectedFamilyIndex = familyIndex;
    font.selectedVariantIndex = variantIndex;
    font.selectedCharsetIndex = charsetIndex;
    RebuildFontListsForIndex(font.selectedFontIndex);
    UpdatePreviewFonts(font.selectedFontIndex);
    UpdateFontTypeLabel(font.selectedFontIndex);
    int newSize = newDesc->scalable ? (newDesc->pointSize / 10) : newDesc->pixelSize;
    PopulateVariantSizeList(familyIndex, variantIndex, charsetIndex, newSize);
    font.dirty = TRUE;
}

static void
fontFamilySelectedCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
    XmListCallbackStruct *cb = (XmListCallbackStruct *)call_data;
    if (!cb || font.selectedFontIndex < 0)
        return;

    int familyIndex = cb->item_position - 1;
    FontFaceCatalog *catalog = EnsureFaceCatalog();
    if (!catalog || familyIndex < 0 ||
        familyIndex >= FontFaceCatalogFamilyCount(catalog))
    {
        return;
    }

    font.selectedFamilyIndex = familyIndex;
    PopulateVariantList(familyIndex);
    int variantCount = FontFaceCatalogVariantCount(catalog, familyIndex);
    if (variantCount > 0) {
        XmListSelectPos(font.variantList, 1, False);
        XmListCallbackStruct variantCB = {
            .reason = XmCR_BROWSE_SELECT,
            .item_position = 1
        };
        fontVariantSelectedCB(font.variantList, NULL, &variantCB);
    } else {
        font.selectedVariantIndex = -1;
        font.selectedCharsetIndex = -1;
        ClearSizeList();
    }
}

static void
fontVariantSelectedCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
    XmListCallbackStruct *cb = (XmListCallbackStruct *)call_data;
    if (!cb || font.selectedFontIndex < 0 || font.selectedFamilyIndex < 0)
        return;

    int variantIndex = cb->item_position - 1;
    FontFaceCatalog *catalog = EnsureFaceCatalog();
    if (!catalog)
        return;
    int variantCount = FontFaceCatalogVariantCount(catalog, font.selectedFamilyIndex);
    if (variantIndex < 0 || variantIndex >= variantCount)
        return;

    font.selectedVariantIndex = variantIndex;
    PopulateCharsetList(font.selectedFamilyIndex, variantIndex);
    int charsetCount = FontFaceCatalogVariantCharsetCount(
            catalog, font.selectedFamilyIndex, variantIndex);
    if (charsetCount > 0) {
        XmListSelectPos(font.charsetList, 1, False);
        XmListCallbackStruct charsetCB = {
            .reason = XmCR_BROWSE_SELECT,
            .item_position = 1
        };
        fontCharsetSelectedCB(font.charsetList, NULL, &charsetCB);
    } else {
        font.selectedCharsetIndex = -1;
        ClearSizeList();
    }
}

static void
fontCharsetSelectedCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
    XmListCallbackStruct *cb = (XmListCallbackStruct *)call_data;
    if (!cb || font.selectedFontIndex < 0 ||
        font.selectedFamilyIndex < 0 || font.selectedVariantIndex < 0)
        return;

    int charsetIndex = cb->item_position - 1;
    FontFaceCatalog *catalog = EnsureFaceCatalog();
    if (!catalog)
        return;
    int charsetCount = FontFaceCatalogVariantCharsetCount(
            catalog, font.selectedFamilyIndex, font.selectedVariantIndex);
    if (charsetIndex < 0 || charsetIndex >= charsetCount)
        return;

    ApplyVariantAndCharset(font.selectedFamilyIndex,
                           font.selectedVariantIndex,
                           charsetIndex,
                           0);
}

static void
fontSizeSelectedCB(
        Widget w,
        XtPointer client_data,
        XtPointer call_data )
{
    XmListCallbackStruct *cb = (XmListCallbackStruct *)call_data;
    if (!cb || font.selectedFontIndex < 0 ||
        font.selectedFamilyIndex < 0 || font.selectedVariantIndex < 0)
        return;

    char *text = NULL;
    if (!XmStringGetLtoR(cb->item, XmFONTLIST_DEFAULT_TAG, &text) || !text)
        return;

    int size = atoi(text);
    XtFree(text);
    if (size <= 0)
        return;

    ApplyVariantAndCharset(font.selectedFamilyIndex,
                           font.selectedVariantIndex,
                           font.selectedCharsetIndex,
                           size);
}

static FontFaceCatalog *
EnsureFaceCatalog( void )
{
    if (!faceCatalog)
        faceCatalog = FontFaceCatalogLoad(style.display);
    return faceCatalog;
}

static XmFontList
BuildFontListFromDescriptor(
        const FontDescriptor *descriptor,
        XFontSet *outFontSet,
        XFontStruct **outFontStruct )
{
    if (outFontSet)
        *outFontSet = NULL;
    if (outFontStruct)
        *outFontStruct = NULL;
    if (!descriptor || !descriptor->raw)
        return NULL;

    const char *raw = descriptor->raw;
    FontDebugLog("preview: raw=%s scalable=%d px=%d pt=%d resx=%s resy=%s",
                 raw,
                 descriptor->scalable ? 1 : 0,
                 descriptor->pixelSize,
                 descriptor->pointSize,
                 descriptor->fields[8] ? descriptor->fields[8] : "?",
                 descriptor->fields[9] ? descriptor->fields[9] : "?");
    Boolean looksLikeFontSet = (strchr(raw, ';') != NULL) || (strchr(raw, ':') != NULL);
    if (looksLikeFontSet) {
        char **missing_list = NULL;
        int missing_count = 0;
        char *def_string = NULL;
        XFontSet font_set = XCreateFontSet(style.display, raw,
                                           &missing_list, &missing_count, &def_string);
        if (FontDebugEnabled() && missing_count > 0 && missing_list) {
            FontDebugLog("preview: fontset missing %d charset(s) for %s",
                         missing_count, raw);
        }
        if (missing_list)
            XFreeStringList(missing_list);
        if (!font_set) {
            FontDebugLog("preview: failed to create fontset for %s", raw);
            return NULL;
        }
        if (outFontSet)
            *outFontSet = font_set;
        XmFontListEntry entry =
            XmFontListEntryCreate(FONT_LIST_TAG, XmFONT_IS_FONTSET, (XtPointer)font_set);
        if (!entry)
            return NULL;
        XmFontList fontList = XmFontListAppendEntry(NULL, entry);
        XmFontListEntryFree(&entry);
        return fontList;
    }

    XFontStruct *font = XLoadQueryFont(style.display, raw);
    if (font) {
        if (outFontStruct)
            *outFontStruct = font;
        XmFontListEntry entry =
            XmFontListEntryCreate(FONT_LIST_TAG, XmFONT_IS_FONT, (XtPointer)font);
        if (!entry)
            return NULL;
        XmFontList fontList = XmFontListAppendEntry(NULL, entry);
        XmFontListEntryFree(&entry);
        return fontList;
    }

    char **missing_list = NULL;
    int missing_count = 0;
    char *def_string = NULL;
    XFontSet font_set = XCreateFontSet(style.display, raw,
                                       &missing_list, &missing_count, &def_string);
    if (FontDebugEnabled() && missing_count > 0 && missing_list) {
        FontDebugLog("preview: fontset missing %d charset(s) for %s",
                     missing_count, raw);
    }
    if (missing_list)
        XFreeStringList(missing_list);
    if (!font_set) {
        FontDebugLog("preview: failed to create fontset for %s", raw);
        return NULL;
    }
    if (outFontSet)
        *outFontSet = font_set;
    XmFontListEntry entry =
        XmFontListEntryCreate(FONT_LIST_TAG, XmFONT_IS_FONTSET, (XtPointer)font_set);
    if (!entry)
        return NULL;

    XmFontList fontList = XmFontListAppendEntry(NULL, entry);
    XmFontListEntryFree(&entry);
    return fontList;
}

static void
UpdatePreviewFonts(
        int index )
{
    if (index < 0 || index >= style.xrdb.numFonts)
        return;

    Arg args[MAX_ARGS];
    int n = 0;

    if (style.xrdb.fontChoice[index].sysFont)
        XtSetArg(args[n], XmNfontList, style.xrdb.fontChoice[index].sysFont), n++;
    XmString systemSample = CMPSTR(SYSTEM_MSG);
    XtSetArg(args[n], XmNlabelString, systemSample); n++;
    XtSetValues(font.systemLabel, args, n);
    XmStringFree(systemSample);

    n = 0;
    if (!font.userTextChanged)
        XtSetArg(args[n], XmNvalue, USER_MSG), n++;
    if (style.xrdb.fontChoice[index].userFont)
        XtSetArg(args[n], XmNfontList, style.xrdb.fontChoice[index].userFont), n++;
    XtSetValues(font.userText, args, n);
    XmTextShowPosition(font.userText, 0);
}

static void
RebuildFontListsForIndex(
        int index )
{
    if (index < 0 || index >= style.xrdb.numFonts)
        return;

    FontDescriptor *desc = style.xrdb.fontChoice[index].descriptor;
    if (!desc)
        return;

    XFontSet newSysFontSet = NULL;
    XFontStruct *newSysFontStruct = NULL;
    XmFontList newSysFont = BuildFontListFromDescriptor(desc, &newSysFontSet, &newSysFontStruct);
    if (newSysFont) {
        fontTrash[index].sysFont = style.xrdb.fontChoice[index].sysFont;
        fontTrash[index].sysFontSet = style.xrdb.fontChoice[index].sysFontSet;
        fontTrash[index].sysFontStruct = style.xrdb.fontChoice[index].sysFontStruct;
        style.xrdb.fontChoice[index].sysFont = newSysFont;
        style.xrdb.fontChoice[index].sysFontSet = newSysFontSet;
        style.xrdb.fontChoice[index].sysFontStruct = newSysFontStruct;
    }

    XFontSet newUserFontSet = NULL;
    XFontStruct *newUserFontStruct = NULL;
    XmFontList newUserFont = BuildFontListFromDescriptor(desc, &newUserFontSet, &newUserFontStruct);
    if (newUserFont) {
        fontTrash[index].userFont = style.xrdb.fontChoice[index].userFont;
        fontTrash[index].userFontSet = style.xrdb.fontChoice[index].userFontSet;
        fontTrash[index].userFontStruct = style.xrdb.fontChoice[index].userFontStruct;
        style.xrdb.fontChoice[index].userFont = newUserFont;
        style.xrdb.fontChoice[index].userFontSet = newUserFontSet;
        style.xrdb.fontChoice[index].userFontStruct = newUserFontStruct;
    }

    if ((fontTrash[index].sysFont || fontTrash[index].userFont ||
         fontTrash[index].sysFontSet || fontTrash[index].userFontSet ||
         fontTrash[index].sysFontStruct || fontTrash[index].userFontStruct) &&
        !fontTrash[index].pending)
    {
        XtAppAddTimeOut(XtDisplayToApplicationContext(style.display),
                        0, FontResourceFreeTimeout, &fontTrash[index]);
        fontTrash[index].pending = True;
    }
}

/************************************************************************
 * restoreFonts()
 *
 * restore any state information saved with saveFonts.
 * This is called from restoreSession with the application
 * shell and the special xrm database retrieved for restore.
 ************************************************************************/
void 
restoreFonts(
        Widget shell,
        XrmDatabase db )
{
    XrmName xrm_name[5];
    XrmRepresentation rep_type;
    XrmValue value;

    xrm_name [0] = XrmStringToQuark ("Fonts");
    xrm_name [2] = 0;

    /* get x position */
    xrm_name [1] = XrmStringToQuark ("x");
    if (XrmQGetResource (db, xrm_name, xrm_name, &rep_type, &value)){
      XtSetArg (save.posArgs[save.poscnt], XmNx, atoi((char *)value.addr)); 
      save.poscnt++;
      save.restoreFlag = True;
    }

    /* get y position */
    xrm_name [1] = XrmStringToQuark ("y");
    if (XrmQGetResource (db, xrm_name, xrm_name, &rep_type, &value)){
      XtSetArg (save.posArgs[save.poscnt], XmNy, atoi((char *)value.addr)); 
      save.poscnt++;
    }

    xrm_name [1] = XrmStringToQuark ("ismapped");
    XrmQGetResource (db, xrm_name, xrm_name, &rep_type, &value);
    /* Are we supposed to be mapped? */
    if (strcmp(value.addr, "True") == 0) 
      popup_fontBB(shell);
}

/************************************************************************
 * saveFonts()
 *
 * This routine will write out to the passed file descriptor any state
 * information this dialog needs.  It is called from saveSessionCB with the
 * file already opened.
 * All information is saved in xrm format.  There is no restriction
 * on what can be saved.  It doesn't have to be defined or be part of any
 * widget or Xt definition.  Just name and save it here and recover it in
 * restoreFonts.  The suggested minimum is whether you are mapped, and your
 * location.
 ************************************************************************/
void
saveFonts(
        int fd )
{
    Position x,y;
    char bufr[1024];     /* size=[1024], make bigger if needed */
    XmVendorShellExtObject  vendorExt;
    XmWidgetExtData         extData;

    if (style.fontDialog != NULL) {
        if (XtIsManaged(style.fontDialog))
          sprintf(bufr, "*Fonts.ismapped: True\n");
        else
          sprintf(bufr, "*Fonts.ismapped: False\n");

        WRITE_STR2FD(fd, bufr);

        /* Get and write out the geometry info for our Window */

        x = XtX(XtParent(style.fontDialog));
        y = XtY(XtParent(style.fontDialog));

        /* Modify x & y to take into account window mgr frames
         * This is pretty bogus, but I don't know a better way to do it.
         */
        extData = _XmGetWidgetExtData(style.shell, XmSHELL_EXTENSION);
        vendorExt = (XmVendorShellExtObject)extData->widget;
        x -= vendorExt->vendor.xOffset;
        y -= vendorExt->vendor.yOffset;

        snprintf(bufr, sizeof(style.tmpBigStr), "*Fonts.x: %d\n", x);
        WRITE_STR2FD(fd, bufr);
        snprintf(bufr, sizeof(style.tmpBigStr), "*Fonts.y: %d\n", y);
        WRITE_STR2FD(fd, bufr);
    }
}
