/*
 * CDE - Common Desktop Environment
 *
 * PrintPdfCups.c: Modern Unicode-clean Cairo/Pango vector PDF generator,
 * per-PrintSetupBox IPP job options dialog (Paper Size, Duplex/Sides, Page
 * Range), CUPS / IPP Everywhere queue discovery, and direct libcups / RFC 8011
 * IPP/1.1 job submission for libDtPrint.
 */

#ifdef HAVE_CONFIG_H
#include "cde_config.h"
#endif

#include <Dt/Print.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <netdb.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <Xm/DialogS.h>
#include <Xm/Form.h>
#include <Xm/LabelG.h>
#include <Xm/MessageB.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
#include <Xm/SeparatoG.h>
#include <Xm/TextF.h>

#if defined(HAVE_DTPRINT_PDF)
#include <cairo/cairo.h>
#include <cairo/cairo-pdf.h>
#include <pango/pangocairo.h>
#endif

/* -------------------------------------------------------------------------
 * 1. Small String & Option Helpers
 * ------------------------------------------------------------------------- */

static void safe_copy(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static DtPrintPaperSize parse_paper_env(const char *s) {
    if (!s || !s[0]) return DtPRINT_PAPER_LETTER;
    if (strcasecmp(s, "a4") == 0 || strcasestr(s, "iso_a4") != NULL)
        return DtPRINT_PAPER_A4;
    if (strcasecmp(s, "legal") == 0 || strcasestr(s, "na_legal") != NULL)
        return DtPRINT_PAPER_LEGAL;
    if (strcasecmp(s, "executive") == 0 || strcasestr(s, "executive") != NULL)
        return DtPRINT_PAPER_EXECUTIVE;
    if (strcasecmp(s, "b5") == 0 || strcasestr(s, "b5") != NULL)
        return DtPRINT_PAPER_B5;
    return DtPRINT_PAPER_LETTER;
}

static DtPrintDuplexMode parse_duplex_env(const char *s) {
    if (!s || !s[0]) return DtPRINT_DUPLEX_NONE;
    if (strcasecmp(s, "two-sided-long-edge") == 0 ||
        strcasecmp(s, "long-edge") == 0 ||
        strcasecmp(s, "duplex") == 0) {
        return DtPRINT_DUPLEX_LONG_EDGE;
    }
    if (strcasecmp(s, "two-sided-short-edge") == 0 ||
        strcasecmp(s, "short-edge") == 0 ||
        strcasecmp(s, "tumble") == 0) {
        return DtPRINT_DUPLEX_SHORT_EDGE;
    }
    return DtPRINT_DUPLEX_NONE;
}

void DtPrintPaperDimensionsPt(DtPrintPaperSize paper, double *out_w_pt, double *out_h_pt) {
    double w = 612.0, h = 792.0;
    switch (paper) {
    case DtPRINT_PAPER_LEGAL:
        w = 612.0; h = 1008.0;
        break;
    case DtPRINT_PAPER_EXECUTIVE:
        w = 522.0; h = 756.0;
        break;
    case DtPRINT_PAPER_A4:
        w = 595.27559; h = 841.88976;
        break;
    case DtPRINT_PAPER_B5:
        w = 515.90551; h = 728.50394;
        break;
    case DtPRINT_PAPER_LETTER:
    default:
        w = 612.0; h = 792.0;
        break;
    }
    if (out_w_pt) *out_w_pt = w;
    if (out_h_pt) *out_h_pt = h;
}

const char *DtPrintPaperIppMedia(DtPrintPaperSize paper) {
    switch (paper) {
    case DtPRINT_PAPER_LEGAL:     return "na_legal_8.5x14in";
    case DtPRINT_PAPER_EXECUTIVE: return "na_executive_7.25x10.5in";
    case DtPRINT_PAPER_A4:        return "iso_a4_210x297mm";
    case DtPRINT_PAPER_B5:        return "jis_b5_182x257mm";
    case DtPRINT_PAPER_LETTER:
    default:                      return "na_letter_8.5x11in";
    }
}

const char *DtPrintDuplexIppSides(DtPrintDuplexMode duplex) {
    switch (duplex) {
    case DtPRINT_DUPLEX_LONG_EDGE:  return "two-sided-long-edge";
    case DtPRINT_DUPLEX_SHORT_EDGE: return "two-sided-short-edge";
    case DtPRINT_DUPLEX_NONE:
    default:                        return "one-sided";
    }
}

double DtPrintParseMarginPoints(const char *spec, double fallback_pt) {
    if (!spec) return fallback_pt;
    while (*spec && isspace((unsigned char)*spec)) spec++;
    if (!*spec) return fallback_pt;

    char *endp = NULL;
    double val = strtod(spec, &endp);
    if (endp == spec || val < 0.0 || val > 500.0) return fallback_pt;
    while (endp && *endp && isspace((unsigned char)*endp)) endp++;

    if (!endp || !*endp || strncasecmp(endp, "in", 2) == 0) {
        return val * 72.0;
    }
    if (strncasecmp(endp, "mm", 2) == 0) {
        return val * (72.0 / 25.4);
    }
    if (strncasecmp(endp, "cm", 2) == 0) {
        return val * (72.0 / 2.54);
    }
    if (strncasecmp(endp, "pt", 2) == 0 || strncasecmp(endp, "pi", 2) == 0) {
        return strncasecmp(endp, "pi", 2) == 0 ? val * 12.0 : val;
    }
    return val * 72.0;
}

Boolean DtPrintPageInRange(const char *range_spec, int page_1based) {
    if (page_1based < 1) return False;
    if (!range_spec) return True;
    while (*range_spec && isspace((unsigned char)*range_spec)) range_spec++;
    if (!*range_spec || strcasecmp(range_spec, "all") == 0 || strcmp(range_spec, "*") == 0) {
        return True;
    }

    const char *p = range_spec;
    Boolean any_token = False;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',' || *p == ';')) p++;
        if (!*p) break;
        any_token = True;

        int lo = 1, hi = INT_MAX;
        if (*p == '-') {
            p++;
            char *ep = NULL;
            long v = strtol(p, &ep, 10);
            if (ep == p) return True;
            hi = (int)v;
            p = ep;
        } else {
            char *ep = NULL;
            long v1 = strtol(p, &ep, 10);
            if (ep == p) {
                while (*p && *p != ',' && *p != ';') p++;
                continue;
            }
            lo = (int)v1;
            p = ep;
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p == '-') {
                p++;
                while (*p && isspace((unsigned char)*p)) p++;
                if (isdigit((unsigned char)*p)) {
                    long v2 = strtol(p, &ep, 10);
                    hi = (int)v2;
                    p = ep;
                } else {
                    hi = INT_MAX;
                }
            } else {
                hi = lo;
            }
        }
        if (lo > hi) {
            int t = lo; lo = hi; hi = t;
        }
        if (page_1based >= lo && page_1based <= hi) return True;
        while (*p && *p != ',' && *p != ';') p++;
    }
    return any_token ? False : True;
}

void DtPrintInitJobOptions(DtPrintJobOptions *opts) {
    if (!opts) return;
    memset(opts, 0, sizeof(*opts));
    opts->paper_size = parse_paper_env(getenv("DTPRINT_PAPER_SIZE"));
    opts->duplex = parse_duplex_env(getenv("DTPRINT_DUPLEX"));
    const char *cenv = getenv("DTPRINT_COPIES");
    opts->copies = (cenv && atoi(cenv) > 0) ? atoi(cenv) : 1;
    opts->page_range = getenv("DTPRINT_PAGE_RANGE");
    opts->margin_top_pt = 54.0;
    opts->margin_right_pt = 54.0;
    opts->margin_bottom_pt = 54.0;
    opts->margin_left_pt = 54.0;
    opts->word_wrap = True;
    opts->font_desc = "Monospace 10";
    opts->job_title = "CDE Print Job";
}

/* -------------------------------------------------------------------------
 * 2. Per-PrintSetupBox State & Motif "Page & IPP Job Setup" Dialog
 * ------------------------------------------------------------------------- */

typedef struct _PsubJobSlot {
    Boolean in_use;
    Widget psub;
    DtPrintPaperSize paper_size;
    DtPrintDuplexMode duplex;
    char page_range[128];
    Widget dialog;
    Widget paper_opt;
    Widget paper_btns[5];
    Widget duplex_opt;
    Widget duplex_btns[3];
    Widget range_tf;
} PsubJobSlot;

#define MAX_PSUB_SLOTS 32
static PsubJobSlot g_psub_slots[MAX_PSUB_SLOTS];

static void on_psub_destroyed(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    PsubJobSlot *slot = (PsubJobSlot *)client_data;
    if (slot && slot->in_use) {
        memset(slot, 0, sizeof(*slot));
    }
}

static PsubJobSlot *get_or_create_psub_slot(Widget psub) {
    if (!psub) return NULL;
    for (int i = 0; i < MAX_PSUB_SLOTS; ++i) {
        if (g_psub_slots[i].in_use && g_psub_slots[i].psub == psub) {
            return &g_psub_slots[i];
        }
    }
    for (int i = 0; i < MAX_PSUB_SLOTS; ++i) {
        if (!g_psub_slots[i].in_use) {
            PsubJobSlot *s = &g_psub_slots[i];
            memset(s, 0, sizeof(*s));
            s->in_use = True;
            s->psub = psub;
            s->paper_size = parse_paper_env(getenv("DTPRINT_PAPER_SIZE"));
            s->duplex = parse_duplex_env(getenv("DTPRINT_DUPLEX"));
            const char *pr = getenv("DTPRINT_PAGE_RANGE");
            safe_copy(s->page_range, sizeof(s->page_range), (pr && pr[0]) ? pr : "all");
            XtAddCallback(psub, XmNdestroyCallback, on_psub_destroyed, (XtPointer)s);
            return s;
        }
    }
    return NULL;
}

void DtPrintSetWidgetJobOptions(
    Widget psub,
    DtPrintPaperSize paper_size,
    DtPrintDuplexMode duplex,
    const char *page_range
) {
    PsubJobSlot *s = get_or_create_psub_slot(psub);
    if (!s) return;
    s->paper_size = paper_size;
    s->duplex = duplex;
    safe_copy(s->page_range, sizeof(s->page_range),
              (page_range && page_range[0]) ? page_range : "all");
}

void DtPrintReadWidgetJobOptions(Widget psub, DtPrintJobOptions *opts) {
    if (!opts) return;
    if (!psub) return;
    int copies = opts->copies > 0 ? opts->copies : 1;
    if (DtIsPrintSetupBox(psub)) {
        XtVaGetValues(psub, DtNcopies, &copies, NULL);
        if (copies > 0) opts->copies = copies;
    }
    PsubJobSlot *s = get_or_create_psub_slot(psub);
    if (s) {
        opts->paper_size = s->paper_size;
        opts->duplex = s->duplex;
        opts->page_range = s->page_range;
    }
}

static void on_job_setup_ok(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    PsubJobSlot *s = (PsubJobSlot *)client_data;
    if (!s || !s->in_use) return;

    if (s->paper_opt) {
        Widget hist = NULL;
        XtVaGetValues(s->paper_opt, XmNmenuHistory, &hist, NULL);
        for (int i = 0; i < 5; ++i) {
            if (hist == s->paper_btns[i]) {
                s->paper_size = (DtPrintPaperSize)i;
                break;
            }
        }
    }
    if (s->duplex_opt) {
        Widget hist = NULL;
        XtVaGetValues(s->duplex_opt, XmNmenuHistory, &hist, NULL);
        for (int i = 0; i < 3; ++i) {
            if (hist == s->duplex_btns[i]) {
                s->duplex = (DtPrintDuplexMode)i;
                break;
            }
        }
    }
    if (s->range_tf) {
        char *txt = XmTextFieldGetString(s->range_tf);
        if (txt) {
            char *p = txt;
            while (*p && isspace((unsigned char)*p)) p++;
            safe_copy(s->page_range, sizeof(s->page_range), *p ? p : "all");
            XtFree(txt);
        }
    }
    if (s->dialog) XtUnmanageChild(s->dialog);
}

static void on_job_setup_cancel(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)call_data;
    PsubJobSlot *s = (PsubJobSlot *)client_data;
    if (s && s->dialog) XtUnmanageChild(s->dialog);
}

void DtPrintPopupJobSetupDialog(Widget parent, Widget psub) {
    PsubJobSlot *s = get_or_create_psub_slot(psub ? psub : parent);
    if (!s) return;
    Widget anchor = parent ? parent : psub;
    while (anchor && !XtIsShell(anchor)) anchor = XtParent(anchor);
    if (!anchor) anchor = psub;

    if (!s->dialog) {
        XmString xtitle = XmStringCreateLocalized("Page & IPP Job Setup");
        XmString xok = XmStringCreateLocalized("OK");
        Arg args[6];
        Cardinal n = 0;
        XtSetArg(args[n], XmNdialogTitle, xtitle); n++;
        XtSetArg(args[n], XmNokLabelString, xok); n++;
        XtSetArg(args[n], XmNautoUnmanage, False); n++;
        s->dialog = XmCreateTemplateDialog(anchor, "dtPrintIppSetupDialog", args, n);
        XmStringFree(xtitle);
        XmStringFree(xok);
        Widget hbtn = XmMessageBoxGetChild(s->dialog, XmDIALOG_HELP_BUTTON);
        if (hbtn) XtUnmanageChild(hbtn);

        Widget rc = XtVaCreateManagedWidget(
            "ippSetupRC", xmRowColumnWidgetClass, s->dialog,
            XmNorientation, XmVERTICAL,
            XmNspacing, 8,
            NULL
        );

        /* Paper Size option menu */
        Widget paper_pane = XmCreatePulldownMenu(rc, "paperPane", NULL, 0);
        static const char * const paper_labels[5] = {
            "US Letter (8.5 x 11 in)",
            "US Legal (8.5 x 14 in)",
            "US Executive (7.25 x 10.5 in)",
            "ISO A4 (210 x 297 mm)",
            "JIS B5 (182 x 257 mm)"
        };
        for (int i = 0; i < 5; ++i) {
            XmString xl = XmStringCreateLocalized((char *)paper_labels[i]);
            char bname[32];
            snprintf(bname, sizeof(bname), "paperBtn%d", i);
            s->paper_btns[i] = XtVaCreateManagedWidget(
                bname, xmPushButtonWidgetClass, paper_pane, XmNlabelString, xl, NULL
            );
            XmStringFree(xl);
        }
        XmString xpl = XmStringCreateLocalized("Paper Size:");
        Arg pargs[4];
        Cardinal pn = 0;
        XtSetArg(pargs[pn], XmNlabelString, xpl); pn++;
        XtSetArg(pargs[pn], XmNsubMenuId, paper_pane); pn++;
        s->paper_opt = XmCreateOptionMenu(rc, "paperOptMenu", pargs, pn);
        XtManageChild(s->paper_opt);
        XmStringFree(xpl);

        /* Duplex / Sides option menu */
        Widget dup_pane = XmCreatePulldownMenu(rc, "duplexPane", NULL, 0);
        static const char * const dup_labels[3] = {
            "One-Sided",
            "Two-Sided (Long-Edge / Portrait)",
            "Two-Sided (Short-Edge / Landscape)"
        };
        for (int i = 0; i < 3; ++i) {
            XmString xl = XmStringCreateLocalized((char *)dup_labels[i]);
            char bname[32];
            snprintf(bname, sizeof(bname), "duplexBtn%d", i);
            s->duplex_btns[i] = XtVaCreateManagedWidget(
                bname, xmPushButtonWidgetClass, dup_pane, XmNlabelString, xl, NULL
            );
            XmStringFree(xl);
        }
        XmString xdl = XmStringCreateLocalized("Two-Sided (Duplex):");
        Arg dargs[4];
        Cardinal dn = 0;
        XtSetArg(dargs[dn], XmNlabelString, xdl); dn++;
        XtSetArg(dargs[dn], XmNsubMenuId, dup_pane); dn++;
        s->duplex_opt = XmCreateOptionMenu(rc, "duplexOptMenu", dargs, dn);
        XtManageChild(s->duplex_opt);
        XmStringFree(xdl);

        /* Page Range row */
        Widget rrow = XtVaCreateManagedWidget(
            "rangeRow", xmRowColumnWidgetClass, rc,
            XmNorientation, XmHORIZONTAL,
            XmNspacing, 6,
            NULL
        );
        XmString xrl = XmStringCreateLocalized("Page Range (all or e.g. 1-3,5):");
        XtVaCreateManagedWidget("rangeLbl", xmLabelGadgetClass, rrow, XmNlabelString, xrl, NULL);
        XmStringFree(xrl);
        s->range_tf = XtVaCreateManagedWidget(
            "rangeText", xmTextFieldWidgetClass, rrow, XmNcolumns, 16, NULL
        );

        XtAddCallback(s->dialog, XmNokCallback, on_job_setup_ok, (XtPointer)s);
        XtAddCallback(s->dialog, XmNcancelCallback, on_job_setup_cancel, (XtPointer)s);
    }

    int pidx = (int)s->paper_size;
    if (pidx >= 0 && pidx < 5 && s->paper_opt && s->paper_btns[pidx]) {
        XtVaSetValues(s->paper_opt, XmNmenuHistory, s->paper_btns[pidx], NULL);
    }
    int didx = (int)s->duplex;
    if (didx >= 0 && didx < 3 && s->duplex_opt && s->duplex_btns[didx]) {
        XtVaSetValues(s->duplex_opt, XmNmenuHistory, s->duplex_btns[didx], NULL);
    }
    if (s->range_tf) {
        XmTextFieldSetString(s->range_tf, s->page_range[0] ? s->page_range : "all");
    }
    XtManageChild(s->dialog);
}

/* -------------------------------------------------------------------------
 * 3. CUPS / IPP Everywhere Queue Discovery (libcups.so.2 + Mock + Env)
 * ------------------------------------------------------------------------- */

typedef struct {
    char *name;
    char *value;
} DlCupsOption;

typedef struct {
    char *name;
    char *instance;
    int   is_default;
    int   num_options;
    DlCupsOption *options;
} DlCupsDest;

static int load_mock_cups_queues(const char *path, DtPrintCupsQueue *out_q, int max_q) {
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    int count = 0;
    char line[1024];
    while (count < max_q && fgets(line, sizeof(line), fp)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (strncmp(p, "PRINTER|", 8) != 0) continue;

        char *save = NULL;
        (void)strtok_r(p, "|", &save);
        const char *name = strtok_r(NULL, "|", &save);
        const char *uri = strtok_r(NULL, "|", &save);
        const char *model = strtok_r(NULL, "|", &save);
        const char *loc = strtok_r(NULL, "|", &save);
        const char *state = strtok_r(NULL, "|", &save);
        const char *acc = strtok_r(NULL, "|", &save);
        const char *def = strtok_r(NULL, "|", &save);
        if (!name || !name[0]) continue;

        DtPrintCupsQueue *q = &out_q[count++];
        memset(q, 0, sizeof(*q));
        safe_copy(q->name, sizeof(q->name), name);
        safe_copy(q->device_uri, sizeof(q->device_uri), uri ? uri : "ipp://localhost/printers");
        safe_copy(q->make_model, sizeof(q->make_model), model ? model : "IPP Everywhere Printer");
        safe_copy(q->location, sizeof(q->location), loc ? loc : "");
        safe_copy(q->state, sizeof(q->state), state ? state : "idle");
        q->accepting = (!acc || atoi(acc) != 0) ? True : False;
        q->is_default = (def && atoi(def) != 0) ? True : False;
    }
    fclose(fp);
    return count;
}

int DtPrintDiscoverCupsQueues(DtPrintCupsQueue *out_queues, int max_queues) {
    if (!out_queues || max_queues <= 0) return 0;
    int count = 0;

    /* 1. Explicit $CK_CUPS_MOCK_FILE override (shared contract with dtprintinfo) */
    const char *mock = getenv("CK_CUPS_MOCK_FILE");
    if (mock && mock[0]) {
        int mc = load_mock_cups_queues(mock, out_queues, max_queues);
        if (mc >= 0) count = mc;
    }

    /* 2. Explicit $DTPRINT_IPP_URI queue injection (e.g. ipp://127.0.0.1:18631/printers/OfficeLaser) */
    const char *ipp_env = getenv("DTPRINT_IPP_URI");
    if (ipp_env && ipp_env[0] && count < max_queues) {
        const char *slash = strrchr(ipp_env, '/');
        const char *qname = (slash && slash[1]) ? slash + 1 : "IPP_Printer";
        Boolean exists = False;
        for (int i = 0; i < count; ++i) {
            if (strcmp(out_queues[i].name, qname) == 0) {
                safe_copy(out_queues[i].device_uri, sizeof(out_queues[i].device_uri), ipp_env);
                exists = True;
                break;
            }
        }
        if (!exists) {
            DtPrintCupsQueue *q = &out_queues[count++];
            memset(q, 0, sizeof(*q));
            safe_copy(q->name, sizeof(q->name), qname);
            safe_copy(q->device_uri, sizeof(q->device_uri), ipp_env);
            safe_copy(q->make_model, sizeof(q->make_model), "IPP Everywhere Direct Queue");
            safe_copy(q->state, sizeof(q->state), "idle");
            q->accepting = True;
            q->is_default = (count == 1) ? True : False;
        }
    }

    /* 3. Live libcups.so.2 discovery when mock file was not used */
    if (!(mock && mock[0]) && count < max_queues) {
        void *lib = dlopen("libcups.so.2", RTLD_LAZY | RTLD_LOCAL);
        if (!lib) lib = dlopen("libcups.so", RTLD_LAZY | RTLD_LOCAL);
        if (lib) {
            int (*p_get_dests2)(void *, DlCupsDest **) =
                (int (*)(void *, DlCupsDest **))dlsym(lib, "cupsGetDests2");
            void (*p_free_dests)(int, DlCupsDest *) =
                (void (*)(int, DlCupsDest *))dlsym(lib, "cupsFreeDests");
            const char *(*p_get_opt)(const char *, int, DlCupsOption *) =
                (const char *(*)(const char *, int, DlCupsOption *))dlsym(lib, "cupsGetOption");
            if (p_get_dests2 && p_free_dests && p_get_opt) {
                DlCupsDest *dests = NULL;
                int nd = p_get_dests2(NULL, &dests);
                for (int i = 0; i < nd && count < max_queues; ++i) {
                    if (!dests[i].name || !dests[i].name[0]) continue;
                    DtPrintCupsQueue *q = &out_queues[count++];
                    memset(q, 0, sizeof(*q));
                    if (dests[i].instance && dests[i].instance[0]) {
                        snprintf(q->name, sizeof(q->name), "%s/%s", dests[i].name, dests[i].instance);
                    } else {
                        safe_copy(q->name, sizeof(q->name), dests[i].name);
                    }
                    const char *uri = p_get_opt("device-uri", dests[i].num_options, dests[i].options);
                    const char *model = p_get_opt("printer-make-and-model", dests[i].num_options, dests[i].options);
                    const char *loc = p_get_opt("printer-location", dests[i].num_options, dests[i].options);
                    safe_copy(q->device_uri, sizeof(q->device_uri), uri ? uri : "ipp://localhost/printers");
                    safe_copy(q->make_model, sizeof(q->make_model), model ? model : "CUPS IPP Queue");
                    safe_copy(q->location, sizeof(q->location), loc ? loc : "");
                    safe_copy(q->state, sizeof(q->state), "idle");
                    q->accepting = True;
                    q->is_default = dests[i].is_default ? True : False;
                }
                if (dests && nd > 0) p_free_dests(nd, dests);
            }
            dlclose(lib);
        }
    }

    /* 4. $LPDEST / $PRINTER fallback + guaranteed built-in Print_to_PDF queue */
    const char *env_pr = getenv("LPDEST");
    if (!env_pr || !env_pr[0]) env_pr = getenv("PRINTER");
    if (env_pr && env_pr[0] && count < max_queues) {
        Boolean exists = False;
        for (int i = 0; i < count; ++i) {
            if (strcmp(out_queues[i].name, env_pr) == 0) {
                exists = True;
                break;
            }
        }
        if (!exists) {
            DtPrintCupsQueue *q = &out_queues[count++];
            memset(q, 0, sizeof(*q));
            safe_copy(q->name, sizeof(q->name), env_pr);
            snprintf(q->device_uri, sizeof(q->device_uri), "ipp://localhost/printers/%s", env_pr);
            safe_copy(q->make_model, sizeof(q->make_model), "Configured Default Printer");
            safe_copy(q->state, sizeof(q->state), "idle");
            q->accepting = True;
            q->is_default = (count == 1) ? True : False;
        }
    }

    if (count < max_queues) {
        Boolean has_pdf = False;
        for (int i = 0; i < count; ++i) {
            if (strcmp(out_queues[i].name, "Print_to_PDF") == 0) {
                has_pdf = True;
                break;
            }
        }
        if (!has_pdf) {
            DtPrintCupsQueue *q = &out_queues[count++];
            memset(q, 0, sizeof(*q));
            safe_copy(q->name, sizeof(q->name), "Print_to_PDF");
            safe_copy(q->device_uri, sizeof(q->device_uri), "file:~/PrintOutput.pdf");
            safe_copy(q->make_model, sizeof(q->make_model), "CDE Vector Cairo PDF Generator");
            safe_copy(q->location, sizeof(q->location), "Local Home Folder");
            safe_copy(q->state, sizeof(q->state), "idle");
            q->accepting = True;
            q->is_default = (count == 1) ? True : False;
        }
    }
    return count;
}

Boolean DtPrintFindCupsQueue(const char *name, DtPrintCupsQueue *out_queue) {
    if (!name || !name[0]) return False;
    /* Strip optional legacy Xprint "@display" suffix if present */
    char clean[128];
    safe_copy(clean, sizeof(clean), name);
    char *at = strchr(clean, '@');
    if (at) *at = '\0';

    DtPrintCupsQueue list[64];
    int n = DtPrintDiscoverCupsQueues(list, 64);
    for (int i = 0; i < n; ++i) {
        if (strcmp(list[i].name, clean) == 0 || strcasecmp(list[i].name, clean) == 0) {
            if (out_queue) *out_queue = list[i];
            return True;
        }
    }
    return False;
}

Boolean DtPrintDefaultCupsQueueName(char *out_name, int out_cap) {
    if (!out_name || out_cap <= 0) return False;
    DtPrintCupsQueue list[64];
    int n = DtPrintDiscoverCupsQueues(list, 64);
    if (n <= 0) return False;
    for (int i = 0; i < n; ++i) {
        if (list[i].is_default) {
            safe_copy(out_name, (size_t)out_cap, list[i].name);
            return True;
        }
    }
    safe_copy(out_name, (size_t)out_cap, list[0].name);
    return True;
}

/* -------------------------------------------------------------------------
 * 4. Unicode-Clean Vector Cairo/Pango PDF Generation
 * ------------------------------------------------------------------------- */

static void expand_page_tokens(
    const char *tmpl,
    int page_num,
    int total_pages,
    char *out_buf,
    size_t out_cap
) {
    if (!out_buf || out_cap == 0) return;
    out_buf[0] = '\0';
    if (!tmpl || !tmpl[0]) return;

    /* Support "%d ... %d" or "{page}" / "{pages}" or pre-expanded strings */
    const char *p1 = strstr(tmpl, "%d");
    if (p1) {
        const char *p2 = strstr(p1 + 2, "%d");
        if (p2) {
            snprintf(out_buf, out_cap, tmpl, page_num, total_pages);
            return;
        }
        snprintf(out_buf, out_cap, tmpl, page_num);
        return;
    }
    safe_copy(out_buf, out_cap, tmpl);
}

#if defined(HAVE_DTPRINT_PDF)

static void draw_header_footer_chrome(
    cairo_t *cr,
    PangoFontDescription *hdr_fdesc,
    const DtPrintJobOptions *opts,
    double page_w,
    double page_h,
    double ml,
    double mr,
    double mt,
    double mb,
    int logical_page,
    int total_pages
) {
    double content_w = page_w - ml - mr;
    if (content_w < 36.0) content_w = 36.0;

    Boolean has_hl = (opts->header_left && opts->header_left[0] && strcmp(opts->header_left, " ") != 0);
    Boolean has_hr = (opts->header_right && opts->header_right[0] && strcmp(opts->header_right, " ") != 0);
    Boolean has_fl = (opts->footer_left && opts->footer_left[0] && strcmp(opts->footer_left, " ") != 0);
    Boolean has_fr = (opts->footer_right && opts->footer_right[0] && strcmp(opts->footer_right, " ") != 0);

    cairo_save(cr);
    PangoLayout *pl = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(pl, hdr_fdesc);

    if (has_hl || has_hr) {
        double hy = fmax(14.0, mt - 20.0);
        cairo_set_source_rgb(cr, 0.20, 0.22, 0.26);
        if (has_hl) {
            char buf[512];
            expand_page_tokens(opts->header_left, logical_page, total_pages, buf, sizeof(buf));
            pango_layout_set_text(pl, buf, -1);
            cairo_move_to(cr, ml, hy);
            pango_cairo_show_layout(cr, pl);
        }
        if (has_hr) {
            char buf[512];
            expand_page_tokens(opts->header_right, logical_page, total_pages, buf, sizeof(buf));
            pango_layout_set_text(pl, buf, -1);
            int pw = 0, ph = 0;
            pango_layout_get_pixel_size(pl, &pw, &ph);
            cairo_move_to(cr, fmax(ml, page_w - mr - (double)pw), hy);
            pango_cairo_show_layout(cr, pl);
        }
        cairo_set_source_rgb(cr, 0.65, 0.68, 0.72);
        cairo_set_line_width(cr, 0.5);
        cairo_move_to(cr, ml, mt - 6.0);
        cairo_line_to(cr, page_w - mr, mt - 6.0);
        cairo_stroke(cr);
    }

    if (has_fl || has_fr) {
        double fy = fmin(page_h - 18.0, page_h - mb + 8.0);
        cairo_set_source_rgb(cr, 0.65, 0.68, 0.72);
        cairo_set_line_width(cr, 0.5);
        cairo_move_to(cr, ml, page_h - mb + 4.0);
        cairo_line_to(cr, page_w - mr, page_h - mb + 4.0);
        cairo_stroke(cr);

        cairo_set_source_rgb(cr, 0.20, 0.22, 0.26);
        if (has_fl) {
            char buf[512];
            expand_page_tokens(opts->footer_left, logical_page, total_pages, buf, sizeof(buf));
            pango_layout_set_text(pl, buf, -1);
            cairo_move_to(cr, ml, fy);
            pango_cairo_show_layout(cr, pl);
        }
        if (has_fr) {
            char buf[512];
            expand_page_tokens(opts->footer_right, logical_page, total_pages, buf, sizeof(buf));
            pango_layout_set_text(pl, buf, -1);
            int pw = 0, ph = 0;
            pango_layout_get_pixel_size(pl, &pw, &ph);
            cairo_move_to(cr, fmax(ml, page_w - mr - (double)pw), fy);
            pango_cairo_show_layout(cr, pl);
        }
    }

    g_object_unref(pl);
    cairo_restore(cr);
}

/* Sanitize arbitrary input to valid UTF-8 so Pango never rejects Latin-1 bytes */
static char *sanitize_to_utf8(const char *raw) {
    if (!raw) return strdup("");
    size_t len = strlen(raw);
    char *out = (char *)malloc(len * 2 + 4);
    if (!out) return strdup("");
    size_t o = 0;
    for (size_t i = 0; i < len; ) {
        unsigned char c = (unsigned char)raw[i];
        if (c == '\r') {
            if (raw[i + 1] == '\n') { i++; continue; }
            out[o++] = '\n';
            i++;
        } else if (c < 0x80) {
            out[o++] = (char)c;
            i++;
        } else if ((c & 0xE0) == 0xC0 && (c & 0x1E) != 0 &&
                   ((unsigned char)raw[i + 1] & 0xC0) == 0x80) {
            out[o++] = raw[i]; out[o++] = raw[i + 1]; i += 2;
        } else if ((c & 0xF0) == 0xE0 &&
                   ((unsigned char)raw[i + 1] & 0xC0) == 0x80 &&
                   ((unsigned char)raw[i + 2] & 0xC0) == 0x80) {
            out[o++] = raw[i]; out[o++] = raw[i + 1]; out[o++] = raw[i + 2]; i += 3;
        } else if ((c & 0xF8) == 0xF0 &&
                   ((unsigned char)raw[i + 1] & 0xC0) == 0x80 &&
                   ((unsigned char)raw[i + 2] & 0xC0) == 0x80 &&
                   ((unsigned char)raw[i + 3] & 0xC0) == 0x80) {
            out[o++] = raw[i]; out[o++] = raw[i + 1];
            out[o++] = raw[i + 2]; out[o++] = raw[i + 3]; i += 4;
        } else {
            /* ISO-8859-1 -> UTF-8 */
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
            i++;
        }
    }
    out[o] = '\0';
    return out;
}

static int run_text_layout_pass(
    cairo_t *cr,
    const char *utf8_text,
    const DtPrintJobOptions *opts,
    double page_w,
    double page_h,
    Boolean emit_drawing,
    int known_total_pages,
    int *out_emitted_pages
) {
    double ml = opts->margin_left_pt > 0.0 ? opts->margin_left_pt : 54.0;
    double mr = opts->margin_right_pt > 0.0 ? opts->margin_right_pt : 54.0;
    double mt = opts->margin_top_pt > 0.0 ? opts->margin_top_pt : 54.0;
    double mb = opts->margin_bottom_pt > 0.0 ? opts->margin_bottom_pt : 54.0;
    double content_w = fmax(72.0, page_w - ml - mr);
    double content_h = fmax(72.0, page_h - mt - mb);

    PangoFontDescription *body_fdesc = pango_font_description_from_string(
        (opts->font_desc && opts->font_desc[0]) ? opts->font_desc : "Monospace 10"
    );
    PangoFontDescription *hdr_fdesc = pango_font_description_from_string("Sans 8.5");

    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, body_fdesc);
    if (opts->word_wrap) {
        pango_layout_set_width(layout, (int)(content_w * (double)PANGO_SCALE));
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    } else {
        pango_layout_set_width(layout, -1);
    }

    int logical_page = 1;
    int emitted = 0;
    double cur_y = 0.0;
    Boolean page_started_on_surface = False;

    const char *p = utf8_text;
    for (;;) {
        /* Find end of current paragraph ('\n', '\f', or '\0') */
        const char *seg_end = p;
        while (*seg_end && *seg_end != '\n' && *seg_end != '\f') seg_end++;
        size_t seg_len = (size_t)(seg_end - p);

        pango_layout_set_text(layout, seg_len > 0 ? p : " ", seg_len > 0 ? (int)seg_len : 1);
        PangoLayoutIter *iter = pango_layout_get_iter(layout);
        do {
            PangoLayoutLine *line = pango_layout_iter_get_line_readonly(iter);
            PangoRectangle ink_rect, log_rect;
            pango_layout_iter_get_line_extents(iter, &ink_rect, &log_rect);
            double line_h = fmax(11.0, ((double)log_rect.height / (double)PANGO_SCALE) * 1.15);

            if (cur_y + line_h > content_h && cur_y > 0.0) {
                if (emit_drawing && page_started_on_surface) {
                    cairo_show_page(cr);
                    page_started_on_surface = False;
                }
                logical_page++;
                cur_y = 0.0;
            }

            if (emit_drawing && DtPrintPageInRange(opts->page_range, logical_page)) {
                if (!page_started_on_surface) {
                    draw_header_footer_chrome(
                        cr, hdr_fdesc, opts, page_w, page_h, ml, mr, mt, mb,
                        logical_page, known_total_pages
                    );
                    page_started_on_surface = True;
                    emitted++;
                }
                if (seg_len > 0) {
                    int baseline_pango = pango_layout_iter_get_baseline(iter);
                    int line_top_pango = log_rect.y;
                    double baseline_offset =
                        (double)(baseline_pango - line_top_pango) / (double)PANGO_SCALE;
                    cairo_save(cr);
                    cairo_rectangle(cr, ml, mt, content_w, content_h);
                    cairo_clip(cr);
                    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
                    cairo_move_to(cr, ml, mt + cur_y + baseline_offset);
                    pango_cairo_show_layout_line(cr, line);
                    cairo_restore(cr);
                }
            }
            cur_y += line_h;
        } while (pango_layout_iter_next_line(iter));
        pango_layout_iter_free(iter);

        if (*seg_end == '\0') break;
        if (*seg_end == '\f') {
            p = seg_end + 1;
            if (*p == '\n') p++;
            if (*p != '\0') {
                if (emit_drawing && page_started_on_surface) {
                    cairo_show_page(cr);
                    page_started_on_surface = False;
                }
                logical_page++;
                cur_y = 0.0;
            } else {
                break;
            }
        } else {
            p = seg_end + 1;
            if (*p == '\0') break;
        }
    }

    if (emit_drawing && page_started_on_surface) {
        cairo_show_page(cr);
    }

    g_object_unref(layout);
    pango_font_description_free(body_fdesc);
    pango_font_description_free(hdr_fdesc);

    if (out_emitted_pages) *out_emitted_pages = emitted;
    return logical_page;
}

XtEnum DtPrintRenderTextToPdf(
    const char *utf8_text,
    const DtPrintJobOptions *in_opts,
    const char *output_pdf_path,
    int *out_pages_written,
    char *err_buf,
    int err_cap
) {
    if (out_pages_written) *out_pages_written = 0;
    if (!output_pdf_path || !output_pdf_path[0]) {
        safe_copy(err_buf, (size_t)(err_cap > 0 ? err_cap : 0), "Missing PDF output file path.");
        return DtPRINT_BAD_PARM;
    }

    DtPrintJobOptions def_opts;
    if (!in_opts) {
        DtPrintInitJobOptions(&def_opts);
        in_opts = &def_opts;
    }

    double page_w = 612.0, page_h = 792.0;
    DtPrintPaperDimensionsPt(in_opts->paper_size, &page_w, &page_h);

    char *clean_utf8 = sanitize_to_utf8(utf8_text ? utf8_text : "");

    char tmp_path[PATH_MAX];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", output_pdf_path, (int)getpid());
    unlink(tmp_path);

    /* Pass 1: measure exact total page count on a 1x1 recording/image surface */
    cairo_surface_t *meas_surf = cairo_pdf_surface_create_for_stream(NULL, NULL, page_w, page_h);
    cairo_t *meas_cr = cairo_create(meas_surf);
    int total_pages = run_text_layout_pass(
        meas_cr, clean_utf8, in_opts, page_w, page_h, False, 1, NULL
    );
    cairo_destroy(meas_cr);
    cairo_surface_destroy(meas_surf);
    if (total_pages < 1) total_pages = 1;

    /* Pass 2: emit crisp vector Cairo/Pango PDF */
    cairo_surface_t *pdf_surf = cairo_pdf_surface_create(tmp_path, page_w, page_h);
    if (!pdf_surf || cairo_surface_status(pdf_surf) != CAIRO_STATUS_SUCCESS) {
        if (pdf_surf) cairo_surface_destroy(pdf_surf);
        free(clean_utf8);
        safe_copy(err_buf, (size_t)(err_cap > 0 ? err_cap : 0), "Failed to create Cairo PDF surface.");
        return DtPRINT_FAILURE;
    }
    if (in_opts->job_title && in_opts->job_title[0]) {
        cairo_pdf_surface_set_metadata(pdf_surf, CAIRO_PDF_METADATA_TITLE, in_opts->job_title);
    }
    cairo_pdf_surface_set_metadata(pdf_surf, CAIRO_PDF_METADATA_CREATOR, "CK Core CDE libDtPrint");

    cairo_t *cr = cairo_create(pdf_surf);
    int emitted = 0;
    (void)run_text_layout_pass(cr, clean_utf8, in_opts, page_w, page_h, True, total_pages, &emitted);
    /* If page_range excluded every page, emit at least page 1 so PDF is never 0-page invalid */
    if (emitted == 0) {
        DtPrintJobOptions all_opts = *in_opts;
        all_opts.page_range = "1";
        (void)run_text_layout_pass(cr, clean_utf8, &all_opts, page_w, page_h, True, total_pages, &emitted);
    }
    cairo_destroy(cr);
    cairo_surface_finish(pdf_surf);
    cairo_status_t st = cairo_surface_status(pdf_surf);
    cairo_surface_destroy(pdf_surf);
    free(clean_utf8);

    if (st != CAIRO_STATUS_SUCCESS) {
        unlink(tmp_path);
        safe_copy(err_buf, (size_t)(err_cap > 0 ? err_cap : 0), cairo_status_to_string(st));
        return DtPRINT_FAILURE;
    }
    if (rename(tmp_path, output_pdf_path) != 0) {
        unlink(tmp_path);
        safe_copy(err_buf, (size_t)(err_cap > 0 ? err_cap : 0), strerror(errno));
        return DtPRINT_FAILURE;
    }
    if (out_pages_written) *out_pages_written = emitted;
    return DtPRINT_SUCCESS;
}

XtEnum DtPrintRenderCustomPagesToPdf(
    int total_pages,
    DtPrintPageDrawProc draw_proc,
    void *user_data,
    const DtPrintJobOptions *in_opts,
    const char *output_pdf_path,
    int *out_pages_written,
    char *err_buf,
    int err_cap
) {
    if (out_pages_written) *out_pages_written = 0;
    if (!draw_proc || !output_pdf_path || !output_pdf_path[0] || total_pages < 1) {
        safe_copy(err_buf, (size_t)(err_cap > 0 ? err_cap : 0), "Invalid custom PDF render parameters.");
        return DtPRINT_BAD_PARM;
    }

    DtPrintJobOptions def_opts;
    if (!in_opts) {
        DtPrintInitJobOptions(&def_opts);
        in_opts = &def_opts;
    }

    double page_w = 612.0, page_h = 792.0;
    DtPrintPaperDimensionsPt(in_opts->paper_size, &page_w, &page_h);

    char tmp_path[PATH_MAX];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", output_pdf_path, (int)getpid());
    unlink(tmp_path);

    cairo_surface_t *pdf_surf = cairo_pdf_surface_create(tmp_path, page_w, page_h);
    if (!pdf_surf || cairo_surface_status(pdf_surf) != CAIRO_STATUS_SUCCESS) {
        if (pdf_surf) cairo_surface_destroy(pdf_surf);
        safe_copy(err_buf, (size_t)(err_cap > 0 ? err_cap : 0), "Failed creating Cairo PDF surface.");
        return DtPRINT_FAILURE;
    }
    if (in_opts->job_title && in_opts->job_title[0]) {
        cairo_pdf_surface_set_metadata(pdf_surf, CAIRO_PDF_METADATA_TITLE, in_opts->job_title);
    }
    cairo_pdf_surface_set_metadata(pdf_surf, CAIRO_PDF_METADATA_CREATOR, "CK Core CDE libDtPrint");

    cairo_t *cr = cairo_create(pdf_surf);
    int emitted = 0;
    for (int pg = 1; pg <= total_pages; ++pg) {
        if (!DtPrintPageInRange(in_opts->page_range, pg)) continue;
        cairo_save(cr);
        draw_proc((void *)cr, page_w, page_h, pg, total_pages, user_data);
        cairo_restore(cr);
        cairo_show_page(cr);
        emitted++;
    }
    if (emitted == 0) {
        cairo_save(cr);
        draw_proc((void *)cr, page_w, page_h, 1, total_pages, user_data);
        cairo_restore(cr);
        cairo_show_page(cr);
        emitted = 1;
    }
    cairo_destroy(cr);
    cairo_surface_finish(pdf_surf);
    cairo_status_t st = cairo_surface_status(pdf_surf);
    cairo_surface_destroy(pdf_surf);

    if (st != CAIRO_STATUS_SUCCESS || rename(tmp_path, output_pdf_path) != 0) {
        unlink(tmp_path);
        safe_copy(err_buf, (size_t)(err_cap > 0 ? err_cap : 0), "Failed writing PDF file.");
        return DtPRINT_FAILURE;
    }
    if (out_pages_written) *out_pages_written = emitted;
    return DtPRINT_SUCCESS;
}

#else /* !HAVE_DTPRINT_PDF minimal PDF-1.4 fallback */

XtEnum DtPrintRenderTextToPdf(
    const char *utf8_text,
    const DtPrintJobOptions *in_opts,
    const char *output_pdf_path,
    int *out_pages_written,
    char *err_buf,
    int err_cap
) {
    (void)in_opts;
    if (out_pages_written) *out_pages_written = 0;
    if (!output_pdf_path || !output_pdf_path[0]) return DtPRINT_BAD_PARM;
    FILE *fp = fopen(output_pdf_path, "wb");
    if (!fp) {
        safe_copy(err_buf, (size_t)(err_cap > 0 ? err_cap : 0), strerror(errno));
        return DtPRINT_FAILURE;
    }
    const char *txt = utf8_text ? utf8_text : "";
    fprintf(fp, "%%PDF-1.4\n1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n");
    fprintf(fp, "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n");
    fprintf(fp, "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >> endobj\n");
    fprintf(fp, "4 0 obj << /Length %zu >>\nstream\nBT /F1 10 Tf 54 738 Td (", strlen(txt) + 32);
    for (const char *p = txt; *p && *p != '\n'; ++p) {
        if (*p == '(' || *p == ')' || *p == '\\') fputc('\\', fp);
        fputc(*p, fp);
    }
    fprintf(fp, ") Tj ET\nendstream\nendobj\n");
    fprintf(fp, "5 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Courier >> endobj\n");
    fprintf(fp, "trailer << /Root 1 0 R >>\n%%%%EOF\n");
    fclose(fp);
    if (out_pages_written) *out_pages_written = 1;
    return DtPRINT_SUCCESS;
}

XtEnum DtPrintRenderCustomPagesToPdf(
    int total_pages,
    DtPrintPageDrawProc draw_proc,
    void *user_data,
    const DtPrintJobOptions *in_opts,
    const char *output_pdf_path,
    int *out_pages_written,
    char *err_buf,
    int err_cap
) {
    (void)total_pages; (void)draw_proc; (void)user_data;
    return DtPrintRenderTextToPdf("CDE Print Output", in_opts, output_pdf_path, out_pages_written, err_buf, err_cap);
}

#endif /* HAVE_DTPRINT_PDF */

/* -------------------------------------------------------------------------
 * 5. Direct RFC 8011 IPP/1.1 Binary Wire Client + libcups.so.2 Submission
 * ------------------------------------------------------------------------- */

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} IppBuf;

static void ipp_put_bytes(IppBuf *b, const void *src, size_t n) {
    if (b->len + n > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 512;
        while (ncap < b->len + n) ncap *= 2;
        uint8_t *nd = (uint8_t *)realloc(b->data, ncap);
        if (!nd) return;
        b->data = nd;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
}

static void ipp_put_u8(IppBuf *b, uint8_t v) { ipp_put_bytes(b, &v, 1); }
static void ipp_put_u16_be(IppBuf *b, uint16_t v) {
    uint8_t be[2] = {(uint8_t)(v >> 8), (uint8_t)(v & 0xff)};
    ipp_put_bytes(b, be, 2);
}
static void ipp_put_i32_be(IppBuf *b, int32_t v) {
    uint32_t u = (uint32_t)v;
    uint8_t be[4] = {
        (uint8_t)(u >> 24), (uint8_t)((u >> 16) & 0xff),
        (uint8_t)((u >> 8) & 0xff), (uint8_t)(u & 0xff)
    };
    ipp_put_bytes(b, be, 4);
}

static void ipp_put_str_attr(IppBuf *b, uint8_t tag, const char *name, const char *val) {
    size_t nlen = name ? strlen(name) : 0;
    size_t vlen = val ? strlen(val) : 0;
    ipp_put_u8(b, tag);
    ipp_put_u16_be(b, (uint16_t)nlen);
    if (nlen) ipp_put_bytes(b, name, nlen);
    ipp_put_u16_be(b, (uint16_t)vlen);
    if (vlen) ipp_put_bytes(b, val, vlen);
}

static void ipp_put_int_attr(IppBuf *b, const char *name, int32_t val) {
    size_t nlen = strlen(name);
    ipp_put_u8(b, 0x21); /* integer */
    ipp_put_u16_be(b, (uint16_t)nlen);
    ipp_put_bytes(b, name, nlen);
    ipp_put_u16_be(b, 4);
    ipp_put_i32_be(b, val);
}

static void ipp_put_page_ranges_attr(IppBuf *b, const char *range_spec) {
    if (!range_spec) return;
    while (*range_spec && isspace((unsigned char)*range_spec)) range_spec++;
    if (!*range_spec || strcasecmp(range_spec, "all") == 0 || strcmp(range_spec, "*") == 0) {
        return;
    }

    Boolean first = True;
    const char *p = range_spec;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',' || *p == ';')) p++;
        if (!*p) break;
        char *ep = NULL;
        long lo = strtol(p, &ep, 10);
        if (ep == p) {
            while (*p && *p != ',' && *p != ';') p++;
            continue;
        }
        long hi = lo;
        p = ep;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '-') {
            p++;
            while (*p && isspace((unsigned char)*p)) p++;
            if (isdigit((unsigned char)*p)) {
                hi = strtol(p, &ep, 10);
                p = ep;
            } else {
                hi = 9999;
            }
        }
        if (lo < 1) lo = 1;
        if (hi < lo) hi = lo;

        ipp_put_u8(b, 0x33); /* rangeOfInteger */
        if (first) {
            const char *nm = "page-ranges";
            ipp_put_u16_be(b, (uint16_t)strlen(nm));
            ipp_put_bytes(b, nm, strlen(nm));
            first = False;
        } else {
            ipp_put_u16_be(b, 0); /* additional value in 1setOf rangeOfInteger */
        }
        ipp_put_u16_be(b, 8);
        ipp_put_i32_be(b, (int32_t)lo);
        ipp_put_i32_be(b, (int32_t)hi);
        while (*p && *p != ',' && *p != ';') p++;
    }
}

static Boolean parse_ipp_uri(
    const char *uri,
    char *out_host, size_t host_cap,
    int *out_port,
    char *out_path, size_t path_cap
) {
    if (!uri) return False;
    const char *rest = NULL;
    if (strncasecmp(uri, "ipp://", 6) == 0) rest = uri + 6;
    else if (strncasecmp(uri, "http://", 7) == 0) rest = uri + 7;
    else return False;

    const char *slash = strchr(rest, '/');
    size_t auth_len = slash ? (size_t)(slash - rest) : strlen(rest);
    if (auth_len == 0 || auth_len >= 256) return False;
    char auth[256];
    memcpy(auth, rest, auth_len);
    auth[auth_len] = '\0';

    char *colon = strrchr(auth, ':');
    if (colon) {
        *colon = '\0';
        *out_port = atoi(colon + 1);
        if (*out_port <= 0) *out_port = 631;
    } else {
        *out_port = 631;
    }
    safe_copy(out_host, host_cap, auth[0] ? auth : "127.0.0.1");
    safe_copy(out_path, path_cap, (slash && slash[0]) ? slash : "/ipp/print");
    return True;
}

static Boolean send_all_bytes(int fd, const uint8_t *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, 0);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return False;
        }
        off += (size_t)n;
    }
    return True;
}

static Boolean submit_direct_ipp_http(
    const char *ipp_uri,
    const uint8_t *pdf_bytes,
    size_t pdf_len,
    const DtPrintJobOptions *opts,
    int *out_job_id,
    char *err_buf,
    int err_cap
) {
    char host[128] = {0}, path[256] = {0};
    int port = 631;
    if (!parse_ipp_uri(ipp_uri, host, sizeof(host), &port, path, sizeof(path))) {
        return False;
    }

    struct passwd *pw = getpwuid(getuid());
    const char *user = (pw && pw->pw_name && pw->pw_name[0]) ? pw->pw_name : "cdeuser";
    const char *title = (opts->job_title && opts->job_title[0]) ? opts->job_title : "CDE Print Job";

    IppBuf ipp = {0};
    /* IPP/1.1 Print-Job (0x0002), request-id = 1 */
    ipp_put_u8(&ipp, 1);
    ipp_put_u8(&ipp, 1);
    ipp_put_u16_be(&ipp, 0x0002);
    ipp_put_i32_be(&ipp, 1);

    /* Operation attributes group (0x01) */
    ipp_put_u8(&ipp, 0x01);
    ipp_put_str_attr(&ipp, 0x47 /* charset */, "attributes-charset", "utf-8");
    ipp_put_str_attr(&ipp, 0x48 /* naturalLanguage */, "attributes-natural-language", "en");
    ipp_put_str_attr(&ipp, 0x45 /* uri */, "printer-uri", ipp_uri);
    ipp_put_str_attr(&ipp, 0x42 /* nameWithoutLanguage */, "requesting-user-name", user);
    ipp_put_str_attr(&ipp, 0x42 /* nameWithoutLanguage */, "job-name", title);
    ipp_put_str_attr(&ipp, 0x49 /* mimeMediaType */, "document-format", "application/pdf");

    /* Job-template attributes group (0x02) */
    ipp_put_u8(&ipp, 0x02);
    ipp_put_int_attr(&ipp, "copies", opts->copies > 0 ? opts->copies : 1);
    ipp_put_str_attr(&ipp, 0x44 /* keyword */, "sides", DtPrintDuplexIppSides(opts->duplex));
    ipp_put_str_attr(&ipp, 0x44 /* keyword */, "media", DtPrintPaperIppMedia(opts->paper_size));
    ipp_put_page_ranges_attr(&ipp, opts->page_range);

    /* End-of-attributes tag (0x03) */
    ipp_put_u8(&ipp, 0x03);

    char port_s[16];
    snprintf(port_s, sizeof(port_s), "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port_s, &hints, &res) != 0 || !res) {
        free(ipp.data);
        safe_copy(err_buf, (size_t)(err_cap > 0 ? err_cap : 0), "Cannot resolve IPP printer host.");
        return False;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        free(ipp.data);
        safe_copy(err_buf, (size_t)(err_cap > 0 ? err_cap : 0), "Cannot connect to IPP printer.");
        return False;
    }

    size_t body_len = ipp.len + pdf_len;
    char req_hdr[1024];
    int hlen = snprintf(
        req_hdr, sizeof(req_hdr),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/ipp\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        path, host, port, body_len
    );
    Boolean sent = send_all_bytes(fd, (const uint8_t *)req_hdr, (size_t)hlen) &&
                   send_all_bytes(fd, ipp.data, ipp.len) &&
                   send_all_bytes(fd, pdf_bytes, pdf_len);
    free(ipp.data);
    if (!sent) {
        close(fd);
        safe_copy(err_buf, (size_t)(err_cap > 0 ? err_cap : 0), "Failed sending IPP print payload.");
        return False;
    }

    uint8_t rx[8192];
    size_t rx_len = 0;
    while (rx_len + 1 < sizeof(rx)) {
        ssize_t n = recv(fd, rx + rx_len, sizeof(rx) - 1 - rx_len, 0);
        if (n <= 0) break;
        rx_len += (size_t)n;
    }
    close(fd);
    rx[rx_len] = '\0';

    /* Locate end of HTTP headers */
    uint8_t *ipp_resp = NULL;
    size_t ipp_resp_len = 0;
    for (size_t i = 0; i + 3 < rx_len; ++i) {
        if (rx[i] == '\r' && rx[i + 1] == '\n' && rx[i + 2] == '\r' && rx[i + 3] == '\n') {
            ipp_resp = rx + i + 4;
            ipp_resp_len = rx_len - (i + 4);
            break;
        }
    }
    if (!ipp_resp || ipp_resp_len < 8) {
        safe_copy(err_buf, (size_t)(err_cap > 0 ? err_cap : 0), "Invalid HTTP/IPP response from printer.");
        return False;
    }

    uint16_t status_code = (uint16_t)((ipp_resp[2] << 8) | ipp_resp[3]);
    if (status_code > 0x00FF) {
        char ebuf[128];
        snprintf(ebuf, sizeof(ebuf), "IPP printer rejected job (status 0x%04x).", status_code);
        safe_copy(err_buf, (size_t)(err_cap > 0 ? err_cap : 0), ebuf);
        return False;
    }

    /* Parse job-id integer attribute if present in response */
    int parsed_job_id = 1;
    size_t pos = 8;
    while (pos < ipp_resp_len) {
        uint8_t tag = ipp_resp[pos++];
        if (tag == 0x03) break;
        if (tag <= 0x0F) continue;
        if (pos + 2 > ipp_resp_len) break;
        uint16_t nl = (uint16_t)((ipp_resp[pos] << 8) | ipp_resp[pos + 1]);
        pos += 2;
        if (pos + nl + 2 > ipp_resp_len) break;
        const char *aname = (const char *)(ipp_resp + pos);
        pos += nl;
        uint16_t vl = (uint16_t)((ipp_resp[pos] << 8) | ipp_resp[pos + 1]);
        pos += 2;
        if (pos + vl > ipp_resp_len) break;
        if (tag == 0x21 && vl == 4 && nl == 6 && memcmp(aname, "job-id", 6) == 0) {
            parsed_job_id = (int)((ipp_resp[pos] << 24) | (ipp_resp[pos + 1] << 16) |
                                  (ipp_resp[pos + 2] << 8) | ipp_resp[pos + 3]);
        }
        pos += vl;
    }
    if (out_job_id) *out_job_id = parsed_job_id;
    return True;
}

XtEnum DtPrintSubmitCupsPdfJob(
    const char *printer_name,
    const char *pdf_path,
    const DtPrintJobOptions *in_opts,
    int *out_job_id,
    char *err_buf,
    int err_cap
) {
    if (out_job_id) *out_job_id = 0;
    if (!pdf_path || !pdf_path[0]) {
        safe_copy(err_buf, (size_t)(err_cap > 0 ? err_cap : 0), "Missing PDF file to print.");
        return DtPRINT_BAD_PARM;
    }

    DtPrintJobOptions def_opts;
    if (!in_opts) {
        DtPrintInitJobOptions(&def_opts);
        in_opts = &def_opts;
    }

    char clean_name[128] = {0};
    if (printer_name && printer_name[0]) {
        safe_copy(clean_name, sizeof(clean_name), printer_name);
        char *at = strchr(clean_name, '@');
        if (at) *at = '\0';
    } else if (!DtPrintDefaultCupsQueueName(clean_name, sizeof(clean_name))) {
        safe_copy(clean_name, sizeof(clean_name), "Print_to_PDF");
    }

    /* Built-in Print_to_PDF virtual queue writes ~/PrintOutput.pdf */
    if (strcasecmp(clean_name, "Print_to_PDF") == 0) {
        const char *home = getenv("HOME");
        char dest_pdf[PATH_MAX];
        snprintf(dest_pdf, sizeof(dest_pdf), "%s/PrintOutput.pdf",
                 (home && home[0]) ? home : "/var/tmp");
        DtPrintSetupData fdata;
        memset(&fdata, 0, sizeof(fdata));
        fdata.destination = DtPRINT_TO_FILE;
        fdata.dest_info = dest_pdf;
        return DtPrintDeliverPdfJob(&fdata, in_opts, pdf_path, out_job_id, err_buf, err_cap);
    }

    /* Read rendered PDF bytes into memory for IPP / CUPS streaming */
    FILE *fp = fopen(pdf_path, "rb");
    if (!fp) {
        safe_copy(err_buf, (size_t)(err_cap > 0 ? err_cap : 0), strerror(errno));
        return DtPRINT_FAILURE;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return DtPRINT_FAILURE;
    }
    long fsz = ftell(fp);
    rewind(fp);
    if (fsz <= 0 || fsz > 256L * 1024L * 1024L) {
        fclose(fp);
        safe_copy(err_buf, (size_t)(err_cap > 0 ? err_cap : 0), "Rendered PDF has invalid size.");
        return DtPRINT_FAILURE;
    }
    uint8_t *pdf_bytes = (uint8_t *)malloc((size_t)fsz);
    if (!pdf_bytes || fread(pdf_bytes, 1, (size_t)fsz, fp) != (size_t)fsz) {
        fclose(fp);
        free(pdf_bytes);
        return DtPRINT_FAILURE;
    }
    fclose(fp);

    /* 1. Direct IPP/1.1 URI submission when queue URI is explicit ($DTPRINT_IPP_URI,
     *    ipp:// printer_name, or $CK_CUPS_MOCK_FILE ipp://127.0.0.1:<port>/...) */
    char direct_uri[256] = {0};
    if (strncasecmp(clean_name, "ipp://", 6) == 0 || strncasecmp(clean_name, "http://", 7) == 0) {
        safe_copy(direct_uri, sizeof(direct_uri), clean_name);
    } else {
        const char *env_uri = getenv("DTPRINT_IPP_URI");
        if (env_uri && env_uri[0]) {
            safe_copy(direct_uri, sizeof(direct_uri), env_uri);
        } else if (getenv("CK_CUPS_MOCK_FILE")) {
            DtPrintCupsQueue q;
            if (DtPrintFindCupsQueue(clean_name, &q) &&
                (strncasecmp(q.device_uri, "ipp://", 6) == 0 ||
                 strncasecmp(q.device_uri, "http://", 7) == 0)) {
                safe_copy(direct_uri, sizeof(direct_uri), q.device_uri);
            }
        }
    }
    if (direct_uri[0]) {
        Boolean ok = submit_direct_ipp_http(
            direct_uri, pdf_bytes, (size_t)fsz, in_opts, out_job_id, err_buf, err_cap
        );
        free(pdf_bytes);
        return ok ? DtPRINT_SUCCESS : DtPRINT_FAILURE;
    }

    /* 2. Submit via libcups.so.2 (cupsCreateJob + cupsStartDocument + cupsWriteRequestData +
     *    cupsFinishDocument, with cupsPrintFile2 fallback) */
    void *lib = dlopen("libcups.so.2", RTLD_LAZY | RTLD_LOCAL);
    if (!lib) lib = dlopen("libcups.so", RTLD_LAZY | RTLD_LOCAL);
    if (lib) {
        int (*p_add_opt)(const char *, const char *, int, DlCupsOption **) =
            (int (*)(const char *, const char *, int, DlCupsOption **))dlsym(lib, "cupsAddOption");
        void (*p_free_opts)(int, DlCupsOption *) =
            (void (*)(int, DlCupsOption *))dlsym(lib, "cupsFreeOptions");
        int (*p_create_job)(void *, const char *, const char *, int, DlCupsOption *) =
            (int (*)(void *, const char *, const char *, int, DlCupsOption *))dlsym(lib, "cupsCreateJob");
        int (*p_start_doc)(void *, const char *, int, const char *, const char *, int) =
            (int (*)(void *, const char *, int, const char *, const char *, int))dlsym(lib, "cupsStartDocument");
        int (*p_write_data)(void *, const char *, size_t) =
            (int (*)(void *, const char *, size_t))dlsym(lib, "cupsWriteRequestData");
        int (*p_finish_doc)(void *, const char *) =
            (int (*)(void *, const char *))dlsym(lib, "cupsFinishDocument");
        int (*p_print_file2)(void *, const char *, const char *, const char *, int, DlCupsOption *) =
            (int (*)(void *, const char *, const char *, const char *, int, DlCupsOption *))dlsym(lib, "cupsPrintFile2");
        const char *(*p_last_err)(void) =
            (const char *(*)(void))dlsym(lib, "cupsLastErrorString");

        if (p_add_opt && p_free_opts) {
            DlCupsOption *copts = NULL;
            int nopts = 0;
            char copies_s[32];
            snprintf(copies_s, sizeof(copies_s), "%d", in_opts->copies > 0 ? in_opts->copies : 1);
            nopts = p_add_opt("copies", copies_s, nopts, &copts);
            nopts = p_add_opt("sides", DtPrintDuplexIppSides(in_opts->duplex), nopts, &copts);
            nopts = p_add_opt("media", DtPrintPaperIppMedia(in_opts->paper_size), nopts, &copts);
            if (in_opts->page_range && in_opts->page_range[0] &&
                strcasecmp(in_opts->page_range, "all") != 0 &&
                strcmp(in_opts->page_range, "*") != 0) {
                nopts = p_add_opt("page-ranges", in_opts->page_range, nopts, &copts);
            }

            const char *title = (in_opts->job_title && in_opts->job_title[0])
                                    ? in_opts->job_title : "CDE Print Job";
            int jid = 0;
            if (p_create_job && p_start_doc && p_write_data && p_finish_doc) {
                jid = p_create_job(NULL, clean_name, title, nopts, copts);
                if (jid > 0) {
                    /* HTTP_CONTINUE = 100 */
                    if (p_start_doc(NULL, clean_name, jid, title, "application/pdf", 1) == 100) {
                        (void)p_write_data(NULL, (const char *)pdf_bytes, (size_t)fsz);
                    }
                    int ipps = p_finish_doc(NULL, clean_name);
                    if (ipps > 0x00FF) jid = 0;
                }
            }
            if (jid <= 0 && p_print_file2) {
                jid = p_print_file2(NULL, clean_name, pdf_path, title, nopts, copts);
            }
            p_free_opts(nopts, copts);

            if (jid > 0) {
                if (out_job_id) *out_job_id = jid;
                dlclose(lib);
                free(pdf_bytes);
                return DtPRINT_SUCCESS;
            }
            if (p_last_err) {
                safe_copy(err_buf, (size_t)(err_cap > 0 ? err_cap : 0), p_last_err());
            }
        }
        dlclose(lib);
    }

    /* 3. Final direct IPP localhost:631 fallback if libcups.so.2 was absent */
    char local_ipp[256];
    snprintf(local_ipp, sizeof(local_ipp), "ipp://127.0.0.1:631/printers/%s", clean_name);
    Boolean ok = submit_direct_ipp_http(
        local_ipp, pdf_bytes, (size_t)fsz, in_opts, out_job_id, err_buf, err_cap
    );
    free(pdf_bytes);
    return ok ? DtPRINT_SUCCESS : DtPRINT_FAILURE;
}

XtEnum DtPrintDeliverPdfJob(
    const DtPrintSetupData *psd,
    const DtPrintJobOptions *opts,
    const char *rendered_pdf_path,
    int *out_job_id,
    char *err_buf,
    int err_cap
) {
    if (out_job_id) *out_job_id = 0;
    if (!psd || !rendered_pdf_path || !rendered_pdf_path[0]) {
        safe_copy(err_buf, (size_t)(err_cap > 0 ? err_cap : 0), "Invalid print setup parameters.");
        return DtPRINT_BAD_PARM;
    }

    if (psd->destination == DtPRINT_TO_FILE) {
        if (!psd->dest_info || !psd->dest_info[0]) {
            safe_copy(err_buf, (size_t)(err_cap > 0 ? err_cap : 0), "Missing output PDF file name.");
            return DtPRINT_BAD_PARM;
        }
        if (strcmp(rendered_pdf_path, psd->dest_info) == 0) {
            return DtPRINT_SUCCESS;
        }
        FILE *in = fopen(rendered_pdf_path, "rb");
        if (!in) {
            safe_copy(err_buf, (size_t)(err_cap > 0 ? err_cap : 0), strerror(errno));
            return DtPRINT_FAILURE;
        }
        char tmp_out[PATH_MAX];
        snprintf(tmp_out, sizeof(tmp_out), "%s.tmp.%d", psd->dest_info, (int)getpid());
        FILE *out = fopen(tmp_out, "wb");
        if (!out) {
            fclose(in);
            safe_copy(err_buf, (size_t)(err_cap > 0 ? err_cap : 0), strerror(errno));
            return DtPRINT_FAILURE;
        }
        uint8_t buf[16384];
        size_t n;
        Boolean write_ok = True;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
            if (fwrite(buf, 1, n, out) != n) {
                write_ok = False;
                break;
            }
        }
        fclose(in);
        if (fclose(out) != 0) write_ok = False;
        if (!write_ok || rename(tmp_out, psd->dest_info) != 0) {
            unlink(tmp_out);
            safe_copy(err_buf, (size_t)(err_cap > 0 ? err_cap : 0), strerror(errno));
            return DtPRINT_FAILURE;
        }
        return DtPRINT_SUCCESS;
    }

    return DtPrintSubmitCupsPdfJob(
        psd->printer_name, rendered_pdf_path, opts, out_job_id, err_buf, err_cap
    );
}
