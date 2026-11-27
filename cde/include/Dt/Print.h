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
/* $XConsortium: Print.h /main/13 1996/10/31 02:09:08 cde-hp $ */
/*
 * DtPrint/Print.h
 */
/* (c) Copyright 1990, 1991, 1992, 1993, 1994 OPEN SOFTWARE FOUNDATION, INC.
 *     ALL RIGHTS RESERVED
 * (c) Copyright 1989, 1996 DIGITAL EQUIPMENT CORPORATION, MAYNARD, MASS.
 * (c) Copyright 1987, 1988, 1989, 1990, 1991, 1992, 1995, 1996,
 *     HEWLETT-PACKARD COMPANY
 * (c) Copyright 1996 International Business Machines Corp.
 * (c) Copyright 1996 Sun Microsystems, Inc.
 * (c) Copyright 1996 Novell, Inc. 
 * (c) Copyright 1996 FUJITSU LIMITED.
 * (c) Copyright 1996 Hitachi.
 */
#ifndef _DtPrint_h
#define _DtPrint_h

#include <Xm/Xm.h>
#if 0 && defined(PRINTING_SUPPORTED)
#include <X11/extensions/Print.h> 
#endif /* PRINTING_SUPPORTED */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Class record constants 
 */
externalref WidgetClass dtPrintSetupBoxWidgetClass;

typedef struct _DtPrintSetupBoxClassRec * DtPrintSetupBoxWidgetClass;
typedef struct _DtPrintSetupBoxRec      * DtPrintSetupBoxWidget;


#ifndef DtIsPrintSetupBox
#define DtIsPrintSetupBox(w)  (XtIsSubclass (w, dtPrintSetupBoxWidgetClass))
#endif

/*
 * Resource Names
 */
#define DtNcancelCallback "cancelCallback"
#define DtNclosePrintDisplayCallback "closePrintDisplayCallback"
#define DtNcopies "copies"
#define DtNdescription "description"
#define DtNdestroyContextCallback "destroyContextCallback"
#define DtNfileName "fileName"
#ifndef DtNminimizeButtons
# define DtNminimizeButtons "minimizeButtons"
#endif
#define DtNoptionCount "optionCount"
#define DtNoptions "options"
#define DtNprintCallback "printCallback"
#define DtNprintDestination "printDestination"
#define DtNprinterInfoProc "printerInfoProc"
#define DtNprinterName "printerName"
#define DtNprintSetupMode "printSetupMode"
#define DtNselectFileProc "selectFileProc"
#define DtNselectPrinterProc "selectPrinterProc"
#define DtNsetupCallback "setupCallback"
#define DtNverifyPrinterProc "verifyPrinterProc"
#define DtNworkAreaLocation "workAreaLocation"

#define DtCCancelCallback "CancelCallback"
#define DtCClosePrintDisplayCallback "ClosePrintDisplayCallback"
#define DtCCopies "Copies"
#define DtCDescription "Description"
#define DtCDestroyContextCallback "DestroyContextCallback"
#define DtCFileName "FileName"
#ifndef DtCMinimizeButtons
# define DtCMinimizeButtons "MinimizeButtons"
#endif
#define DtCOptionCount "OptionCount"
#define DtCOptions "Options"
#define DtCPrintCallback "PrintCallback"
#define DtCPrintDestination "PrintDestination"
#define DtCPrinterInfoProc "PrinterInfoProc"
#define DtCPrinterName "PrinterName"
#define DtCPrintSetupMode "PrintSetupMode"
#define DtCSelectFileProc "SelectFileProc"
#define DtCSelectPrinterProc "SelectPrinterProc"
#define DtCSetupCallback "SetupCallback"
#define DtCVerifyPrinterProc "VerifyPrinterProc"
#define DtCWorkAreaLocation "WorkAreaLocation"

#define DtRPrintSetupProc "PrintSetupProc"

/*
 * DtNsetupMode Resource Values
 */
enum {
    DtPRINT_SETUP_PLAIN,
    DtPRINT_SETUP_XP
};

/*
 * DtNworkAreaLocation Resource Values
 */
enum {
    DtWORK_AREA_NONE,
    DtWORK_AREA_TOP,
    DtWORK_AREA_TOP_AND_BOTTOM,
    DtWORK_AREA_BOTTOM
};

/*
 * DtNprintDestination Resource Values
 */
enum { DtPRINT_NO_DESTINATION, DtPRINT_TO_PRINTER, DtPRINT_TO_FILE };

/*
 * DtNprinterNameMode Resource Values
 */
enum { DtSHORT_NAME, DtMEDIUM_NAME, DtLONG_NAME };

/*
 * Mode values for DtPrintSetupBoxResetConnection()
 */
typedef enum {
    DtPRINT_CLOSE_CONNECTION,
    DtPRINT_RELEASE_CONNECTION
} DtPrintResetConnectionMode;

/*
 * Callback Reasons
 */
enum {
    DtPRINT_CR_NONE,
    DtPRINT_CR_CANCEL,
    DtPRINT_CR_CLOSE_PRINT_DISPLAY,
    DtPRINT_CR_PRINT,
    DtPRINT_CR_SETUP
};

/*
 * DtPrint proecdure return values
 */
enum {
    DtPRINT_SUCCESS,
    DtPRINT_BAD_PARM,
    DtPRINT_FAILURE,
    DtPRINT_INVALID_DISPLAY,
    DtPRINT_NOT_XP_DISPLAY,
    DtPRINT_NO_CONNECTION,
    DtPRINT_NO_DEFAULT,
    DtPRINT_NO_DEFAULT_DISPLAY,
    DtPRINT_NO_PRINTER,
    DtPRINT_PRINTER_MISSING
};

enum {
    DtPRINT_HINT_MESSAGES_OK,
    DtPRINT_HINT_NO_MESSAGES
};

/*
 * PrintSetupBox Callback Structure Definition
 */
typedef struct _DtPrintSetupData
{
    String printer_name;
    Display *print_display;
#ifdef PRINTING_SUPPORT
    XPContext print_context;
#endif /* PRINTING_SUPPORTED */
    XtEnum destination;
    String dest_info;
    XtEnum messages_hint;
} DtPrintSetupData;

typedef struct _DtPrintSetupCallbackStruct
{
    int reason;
    XEvent *event;
    DtPrintSetupData *print_data;
} DtPrintSetupCallbackStruct;

/*
 * PrintSetupBox Procedure Resource Type Definition
 */
typedef XtEnum (*DtPrintSetupProc)(Widget, DtPrintSetupData*);

/*
 * Public Function Declarations
 */
extern Widget DtCreatePrintSetupBox(
				    Widget p,
				    String name,
				    ArgList args,
				    Cardinal n) ;
extern Widget DtCreatePrintSetupDialog(
				       Widget ds_p,
				       String name,
				       ArgList sb_args,
				       Cardinal sb_n) ;
extern DtPrintSetupData* DtPrintCopySetupData(
					      DtPrintSetupData* target,
					      const DtPrintSetupData* source);
extern XtEnum DtPrintFillSetupData(
				   Widget psub,
				   DtPrintSetupData* print_data);
extern void DtPrintFreeSetupData(
				 DtPrintSetupData* target);
extern XtEnum DtPrintResetConnection(
				     Widget psub,
				     DtPrintResetConnectionMode m);

/*
 * Modern Vector Cairo/Pango PDF & Direct CUPS/IPP Pipeline (ABI-compatible additions)
 */
typedef enum {
    DtPRINT_PAPER_LETTER = 0,
    DtPRINT_PAPER_LEGAL = 1,
    DtPRINT_PAPER_EXECUTIVE = 2,
    DtPRINT_PAPER_A4 = 3,
    DtPRINT_PAPER_B5 = 4
} DtPrintPaperSize;

typedef enum {
    DtPRINT_DUPLEX_NONE = 0,
    DtPRINT_DUPLEX_LONG_EDGE = 1,
    DtPRINT_DUPLEX_SHORT_EDGE = 2
} DtPrintDuplexMode;

typedef struct _DtPrintJobOptions {
    DtPrintPaperSize paper_size;
    DtPrintDuplexMode duplex;
    int copies;
    const char *page_range;     /* NULL/"all" or e.g. "1-3,5" */
    double margin_top_pt;       /* points (72 pt = 1 in); <= 0 -> 54 pt */
    double margin_right_pt;
    double margin_bottom_pt;
    double margin_left_pt;
    Boolean word_wrap;
    const char *font_desc;      /* Pango font description, e.g. "Monospace 10" */
    const char *header_left;
    const char *header_right;
    const char *footer_left;
    const char *footer_right;
    const char *job_title;
} DtPrintJobOptions;

typedef struct _DtPrintCupsQueue {
    char name[128];
    char device_uri[256];
    char make_model[256];
    char location[128];
    char state[32];
    Boolean is_default;
    Boolean accepting;
} DtPrintCupsQueue;

typedef void (*DtPrintPageDrawProc)(
    void *cairo_ctx,
    double page_w_pt,
    double page_h_pt,
    int page_1based,
    int total_pages,
    void *user_data
);

extern void DtPrintInitJobOptions(DtPrintJobOptions *opts);
extern void DtPrintReadWidgetJobOptions(Widget psub, DtPrintJobOptions *opts);
extern void DtPrintSetWidgetJobOptions(
    Widget psub,
    DtPrintPaperSize paper_size,
    DtPrintDuplexMode duplex,
    const char *page_range
);
extern void DtPrintPopupJobSetupDialog(Widget parent, Widget psub);
extern double DtPrintParseMarginPoints(const char *spec, double fallback_pt);
extern void DtPrintPaperDimensionsPt(DtPrintPaperSize paper, double *out_w_pt, double *out_h_pt);
extern const char *DtPrintPaperIppMedia(DtPrintPaperSize paper);
extern const char *DtPrintDuplexIppSides(DtPrintDuplexMode duplex);
extern Boolean DtPrintPageInRange(const char *range_spec, int page_1based);

extern int DtPrintDiscoverCupsQueues(DtPrintCupsQueue *out_queues, int max_queues);
extern Boolean DtPrintFindCupsQueue(const char *name, DtPrintCupsQueue *out_queue);
extern Boolean DtPrintDefaultCupsQueueName(char *out_name, int out_cap);

extern XtEnum DtPrintRenderTextToPdf(
    const char *utf8_text,
    const DtPrintJobOptions *opts,
    const char *output_pdf_path,
    int *out_pages_written,
    char *err_buf,
    int err_cap
);

extern XtEnum DtPrintRenderCustomPagesToPdf(
    int total_pages,
    DtPrintPageDrawProc draw_proc,
    void *user_data,
    const DtPrintJobOptions *opts,
    const char *output_pdf_path,
    int *out_pages_written,
    char *err_buf,
    int err_cap
);

extern XtEnum DtPrintSubmitCupsPdfJob(
    const char *printer_name,
    const char *pdf_path,
    const DtPrintJobOptions *opts,
    int *out_job_id,
    char *err_buf,
    int err_cap
);

extern XtEnum DtPrintDeliverPdfJob(
    const DtPrintSetupData *psd,
    const DtPrintJobOptions *opts,
    const char *rendered_pdf_path,
    int *out_job_id,
    char *err_buf,
    int err_cap
);

#ifdef __cplusplus
}  /* Close scope of 'extern "C"' declaration which encloses file. */
#endif

#endif /* _DtPrint_h */
/* DON'T ADD ANYTHING AFTER THIS #endif */
