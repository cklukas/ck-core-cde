/*
 * CDE - Common Desktop Environment
 *
 * Inline Quick-Filter bar and Mail Rules editor dialog for dtmail.
 */

#include "MailQuickFilter.h"

#include <Xm/DialogS.h>
#include <Xm/Form.h>
#include <Xm/Label.h>
#include <Xm/List.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
#include <Xm/TextF.h>
#include <Xm/ToggleB.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct DtMailQuickFilterBar {
    Widget form;
    Widget below_widget;
    Widget query_field;
    Widget chip_all;
    Widget chip_unread;
    Widget chip_starred;
    Widget chip_attach;
    Widget status_lbl;
    DtMailQuickFilterCB on_change;
    void *userdata;
    DtMailFilterMode mode;
    int visible;
};

static void emit_filter_change(DtMailQuickFilterBar *bar)
{
    if (!bar || !bar->on_change) return;
    char *txt = bar->query_field ? XmTextFieldGetString(bar->query_field) : NULL;
    bar->on_change(txt ? txt : "", bar->mode, bar->userdata);
    if (txt) XtFree(txt);
}

static void on_query_changed(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    emit_filter_change((DtMailQuickFilterBar *)client_data);
}

static void update_chip_states(DtMailQuickFilterBar *bar)
{
    if (!bar) return;
    XmToggleButtonSetState(bar->chip_all,     bar->mode == DTMAIL_FILTER_ALL, False);
    XmToggleButtonSetState(bar->chip_unread,  bar->mode == DTMAIL_FILTER_UNREAD, False);
    XmToggleButtonSetState(bar->chip_starred, bar->mode == DTMAIL_FILTER_STARRED, False);
    XmToggleButtonSetState(bar->chip_attach,  bar->mode == DTMAIL_FILTER_HAS_ATTACH, False);
}

static void on_chip_toggled(Widget w, XtPointer client_data, XtPointer call_data)
{
    DtMailQuickFilterBar *bar = (DtMailQuickFilterBar *)client_data;
    XmToggleButtonCallbackStruct *cbs = (XmToggleButtonCallbackStruct *)call_data;
    if (!bar || !cbs || !cbs->set) return;

    if (w == bar->chip_all)          bar->mode = DTMAIL_FILTER_ALL;
    else if (w == bar->chip_unread)  bar->mode = DTMAIL_FILTER_UNREAD;
    else if (w == bar->chip_starred) bar->mode = DTMAIL_FILTER_STARRED;
    else if (w == bar->chip_attach)  bar->mode = DTMAIL_FILTER_HAS_ATTACH;
    update_chip_states(bar);
    emit_filter_change(bar);
}

static void on_clear_click(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    dtmail_quick_filter_clear((DtMailQuickFilterBar *)client_data);
}

void dtmail_quick_filter_clear(DtMailQuickFilterBar *bar)
{
    if (!bar) return;
    bar->mode = DTMAIL_FILTER_ALL;
    update_chip_states(bar);
    if (bar->query_field) {
        XmTextFieldSetString(bar->query_field, (char *)"");
    }
    emit_filter_change(bar);
}

void dtmail_quick_filter_set_visible(DtMailQuickFilterBar *bar, int visible)
{
    if (!bar || !bar->form) return;
    bar->visible = visible ? 1 : 0;
    if (bar->visible) {
        XtManageChild(bar->form);
        if (bar->below_widget) {
            XtVaSetValues(bar->below_widget,
                XmNtopAttachment, XmATTACH_WIDGET,
                XmNtopWidget, bar->form,
                XmNtopOffset, 3,
                NULL);
        }
    } else {
        if (bar->below_widget) {
            XtVaSetValues(bar->below_widget,
                XmNtopAttachment, XmATTACH_FORM,
                XmNtopOffset, 5,
                NULL);
        }
        XtUnmanageChild(bar->form);
    }
}

int dtmail_quick_filter_is_visible(const DtMailQuickFilterBar *bar)
{
    return bar ? bar->visible : 0;
}

void dtmail_quick_filter_focus(DtMailQuickFilterBar *bar)
{
    if (!bar) return;
    if (!bar->visible) {
        dtmail_quick_filter_set_visible(bar, 1);
    }
    if (bar->query_field) {
        XmProcessTraversal(bar->query_field, XmTRAVERSE_CURRENT);
    }
}

void dtmail_quick_filter_set_status(DtMailQuickFilterBar *bar, const char *text)
{
    if (!bar || !bar->status_lbl) return;
    XmString s = XmStringCreateLocalized((char *)(text ? text : ""));
    XtVaSetValues(bar->status_lbl, XmNlabelString, s, NULL);
    XmStringFree(s);
}

Widget dtmail_quick_filter_widget(const DtMailQuickFilterBar *bar)
{
    return bar ? bar->form : NULL;
}

DtMailQuickFilterBar *dtmail_quick_filter_create(
    Widget parent_form,
    Widget below_widget,
    DtMailQuickFilterCB on_change,
    void *userdata)
{
    if (!parent_form) return NULL;
    DtMailQuickFilterBar *bar = (DtMailQuickFilterBar *)calloc(1, sizeof(DtMailQuickFilterBar));
    if (!bar) return NULL;

    bar->below_widget = below_widget;
    bar->on_change = on_change;
    bar->userdata = userdata;
    bar->mode = DTMAIL_FILTER_ALL;
    bar->visible = 1;

    bar->form = XtVaCreateManagedWidget("QuickFilterBar", xmFormWidgetClass, parent_form,
        XmNtopAttachment, XmATTACH_FORM,
        XmNtopOffset, 2,
        XmNleftAttachment, XmATTACH_FORM,
        XmNleftOffset, 2,
        XmNrightAttachment, XmATTACH_FORM,
        XmNrightOffset, 2,
        XmNshadowThickness, 1,
        XmNshadowType, XmSHADOW_ETCHED_IN,
        NULL);

    XmString lbl_s = XmStringCreateLocalized((char *)"Filter:");
    Widget lbl = XtVaCreateManagedWidget("QuickFilterLabel", xmLabelWidgetClass, bar->form,
        XmNlabelString, lbl_s,
        XmNtopAttachment, XmATTACH_FORM,
        XmNtopOffset, 5,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNbottomOffset, 4,
        XmNleftAttachment, XmATTACH_FORM,
        XmNleftOffset, 6,
        NULL);
    XmStringFree(lbl_s);

    XmString clr_s = XmStringCreateLocalized((char *)"Clear");
    Widget clr_btn = XtVaCreateManagedWidget("QuickFilterClear", xmPushButtonWidgetClass, bar->form,
        XmNlabelString, clr_s,
        XmNtopAttachment, XmATTACH_FORM,
        XmNtopOffset, 2,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNbottomOffset, 2,
        XmNrightAttachment, XmATTACH_FORM,
        XmNrightOffset, 4,
        NULL);
    XmStringFree(clr_s);
    XtAddCallback(clr_btn, XmNactivateCallback, on_clear_click, bar);

    XmString st_s = XmStringCreateLocalized((char *)"");
    bar->status_lbl = XtVaCreateManagedWidget("QuickFilterStatus", xmLabelWidgetClass, bar->form,
        XmNlabelString, st_s,
        XmNtopAttachment, XmATTACH_FORM,
        XmNtopOffset, 5,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNbottomOffset, 4,
        XmNrightAttachment, XmATTACH_WIDGET,
        XmNrightWidget, clr_btn,
        XmNrightOffset, 6,
        NULL);
    XmStringFree(st_s);

    Widget chips = XtVaCreateManagedWidget("QuickFilterChips", xmRowColumnWidgetClass, bar->form,
        XmNorientation, XmHORIZONTAL,
        XmNradioBehavior, True,
        XmNpacking, XmPACK_TIGHT,
        XmNmarginHeight, 1,
        XmNmarginWidth, 2,
        XmNspacing, 2,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_WIDGET,
        XmNrightWidget, bar->status_lbl,
        XmNrightOffset, 4,
        NULL);

    XmString s_all = XmStringCreateLocalized((char *)"All");
    bar->chip_all = XtVaCreateManagedWidget("ChipAll", xmToggleButtonWidgetClass, chips,
        XmNlabelString, s_all, XmNset, True, XmNindicatorOn, False, XmNshadowThickness, 2, NULL);
    XmStringFree(s_all);
    XtAddCallback(bar->chip_all, XmNvalueChangedCallback, on_chip_toggled, bar);

    XmString s_unr = XmStringCreateLocalized((char *)"Unread");
    bar->chip_unread = XtVaCreateManagedWidget("ChipUnread", xmToggleButtonWidgetClass, chips,
        XmNlabelString, s_unr, XmNset, False, XmNindicatorOn, False, XmNshadowThickness, 2, NULL);
    XmStringFree(s_unr);
    XtAddCallback(bar->chip_unread, XmNvalueChangedCallback, on_chip_toggled, bar);

    XmString s_str = XmStringCreateLocalized((char *)"Starred");
    bar->chip_starred = XtVaCreateManagedWidget("ChipStarred", xmToggleButtonWidgetClass, chips,
        XmNlabelString, s_str, XmNset, False, XmNindicatorOn, False, XmNshadowThickness, 2, NULL);
    XmStringFree(s_str);
    XtAddCallback(bar->chip_starred, XmNvalueChangedCallback, on_chip_toggled, bar);

    XmString s_att = XmStringCreateLocalized((char *)"Attach");
    bar->chip_attach = XtVaCreateManagedWidget("ChipAttach", xmToggleButtonWidgetClass, chips,
        XmNlabelString, s_att, XmNset, False, XmNindicatorOn, False, XmNshadowThickness, 2, NULL);
    XmStringFree(s_att);
    XtAddCallback(bar->chip_attach, XmNvalueChangedCallback, on_chip_toggled, bar);

    bar->query_field = XtVaCreateManagedWidget("QuickFilterField", xmTextFieldWidgetClass, bar->form,
        XmNcolumns, 22,
        XmNtopAttachment, XmATTACH_FORM,
        XmNtopOffset, 2,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNbottomOffset, 2,
        XmNleftAttachment, XmATTACH_WIDGET,
        XmNleftWidget, lbl,
        XmNleftOffset, 4,
        XmNrightAttachment, XmATTACH_WIDGET,
        XmNrightWidget, chips,
        XmNrightOffset, 6,
        NULL);
    XtAddCallback(bar->query_field, XmNvalueChangedCallback, on_query_changed, bar);

    dtmail_quick_filter_set_visible(bar, 1);
    return bar;
}

void dtmail_quick_filter_destroy(DtMailQuickFilterBar *bar)
{
    if (!bar) return;
    free(bar);
}

/* -------------------------------------------------------------------------
 * Mail Rules Editor Dialog (~/.dt/dtmail_rules)
 * ------------------------------------------------------------------------- */

typedef struct {
    Widget shell;
    Widget list_w;
    Widget name_tf;
    Widget pattern_tf;
    Widget folder_tf;
    int    selected_idx;
} RulesDialogState;

static RulesDialogState *g_rules_dlg = NULL;

static void rules_refresh_list(RulesDialogState *st)
{
    if (!st || !st->list_w) return;
    XmListDeleteAllItems(st->list_w);
    int n = dtmail_rules_count();
    for (int i = 0; i < n; i++) {
        const DtMailRule *r = dtmail_rules_get(i);
        if (!r) continue;
        const char *fld = (r->field == DTMAIL_RULE_FIELD_TO) ? "To" :
                          (r->field == DTMAIL_RULE_FIELD_SUBJECT) ? "Subject" : "From";
        const char *act = (r->action == DTMAIL_RULE_ACT_MARK_READ) ? "Mark Read" :
                          (r->action == DTMAIL_RULE_ACT_STAR) ? "Star" :
                          (r->action == DTMAIL_RULE_ACT_JUNK) ? "Junk" : "Move";
        char line[512];
        snprintf(line, sizeof(line), "%s: if %s contains \"%s\" -> %s %s",
                 r->name, fld, r->pattern, act,
                 (r->action == DTMAIL_RULE_ACT_MOVE) ? r->target_folder : "");
        XmString s = XmStringCreateLocalized(line);
        XmListAddItemUnselected(st->list_w, s, 0);
        XmStringFree(s);
    }
}

static void on_rules_select(Widget w, XtPointer cd, XtPointer cbd)
{
    (void)w;
    RulesDialogState *st = (RulesDialogState *)cd;
    XmListCallbackStruct *cbs = (XmListCallbackStruct *)cbd;
    if (st && cbs) st->selected_idx = cbs->item_position - 1;
}

static void on_rules_add(Widget w, XtPointer cd, XtPointer cbd)
{
    (void)w; (void)cbd;
    RulesDialogState *st = (RulesDialogState *)cd;
    if (!st) return;

    char *nm = XmTextFieldGetString(st->name_tf);
    char *pat = XmTextFieldGetString(st->pattern_tf);
    char *fld = XmTextFieldGetString(st->folder_tf);

    if (pat && pat[0]) {
        DtMailRule r;
        memset(&r, 0, sizeof(r));
        snprintf(r.name, sizeof(r.name), "%s", (nm && nm[0]) ? nm : "New Rule");
        r.field = DTMAIL_RULE_FIELD_FROM;
        snprintf(r.pattern, sizeof(r.pattern), "%s", pat);
        if (fld && strcasecmp(fld, "Junk") == 0) {
            r.action = DTMAIL_RULE_ACT_JUNK;
        } else {
            r.action = DTMAIL_RULE_ACT_MOVE;
            snprintf(r.target_folder, sizeof(r.target_folder), "%s",
                     (fld && fld[0]) ? fld : "Archive");
        }
        r.enabled = 1;
        if (dtmail_rules_add(&r) == 0) {
            char path[1024];
            if (dtmail_rules_default_path(path, sizeof(path)) == 0) {
                (void)dtmail_rules_save_file(path);
            }
            rules_refresh_list(st);
            XmTextFieldSetString(st->name_tf, (char *)"");
            XmTextFieldSetString(st->pattern_tf, (char *)"");
        }
    }

    if (nm) XtFree(nm);
    if (pat) XtFree(pat);
    if (fld) XtFree(fld);
}

static void on_rules_delete(Widget w, XtPointer cd, XtPointer cbd)
{
    (void)w; (void)cbd;
    RulesDialogState *st = (RulesDialogState *)cd;
    if (!st || st->selected_idx < 0) return;
    if (dtmail_rules_remove(st->selected_idx) == 0) {
        char path[1024];
        if (dtmail_rules_default_path(path, sizeof(path)) == 0) {
            (void)dtmail_rules_save_file(path);
        }
        st->selected_idx = -1;
        rules_refresh_list(st);
    }
}

static void on_rules_close(Widget w, XtPointer cd, XtPointer cbd)
{
    (void)w; (void)cbd;
    RulesDialogState *st = (RulesDialogState *)cd;
    if (st && st->shell) XtPopdown(st->shell);
}

void dtmail_rules_dialog_open(Widget parent)
{
    dtmail_rules_init();
    if (g_rules_dlg && g_rules_dlg->shell) {
        rules_refresh_list(g_rules_dlg);
        XtPopup(g_rules_dlg->shell, XtGrabNone);
        return;
    }

    RulesDialogState *st = (RulesDialogState *)calloc(1, sizeof(RulesDialogState));
    if (!st) return;
    st->selected_idx = -1;
    g_rules_dlg = st;

    st->shell = XmCreateDialogShell(parent, (char *)"MailRulesDialog", NULL, 0);
    XtVaSetValues(st->shell, XmNtitle, "Mail Rules & Junk Filters", NULL);

    Widget form = XtVaCreateWidget("RulesForm", xmFormWidgetClass, st->shell,
        XmNmarginWidth, 10,
        XmNmarginHeight, 10,
        XmNwidth, 500,
        XmNheight, 360,
        NULL);

    Arg largs[4];
    int n = 0;
    XtSetArg(largs[n], XmNvisibleItemCount, 8); n++;
    XtSetArg(largs[n], XmNselectionPolicy, XmBROWSE_SELECT); n++;
    st->list_w = XmCreateScrolledList(form, (char *)"RulesList", largs, n);
    XtManageChild(st->list_w);
    XtAddCallback(st->list_w, XmNbrowseSelectionCallback, on_rules_select, st);
    XtVaSetValues(XtParent(st->list_w),
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL);

    XmString nl = XmStringCreateLocalized((char *)"Rule Name:");
    Widget l1 = XtVaCreateManagedWidget("RuleNameLbl", xmLabelWidgetClass, form,
        XmNlabelString, nl,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, XtParent(st->list_w),
        XmNtopOffset, 8,
        XmNleftAttachment, XmATTACH_FORM,
        NULL);
    XmStringFree(nl);

    st->name_tf = XtVaCreateManagedWidget("RuleNameTF", xmTextFieldWidgetClass, form,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, XtParent(st->list_w),
        XmNtopOffset, 4,
        XmNleftAttachment, XmATTACH_WIDGET,
        XmNleftWidget, l1,
        XmNleftOffset, 8,
        XmNrightAttachment, XmATTACH_FORM,
        NULL);

    XmString pl = XmStringCreateLocalized((char *)"From Contains:");
    Widget l2 = XtVaCreateManagedWidget("RulePatLbl", xmLabelWidgetClass, form,
        XmNlabelString, pl,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, st->name_tf,
        XmNtopOffset, 8,
        XmNleftAttachment, XmATTACH_FORM,
        NULL);
    XmStringFree(pl);

    st->pattern_tf = XtVaCreateManagedWidget("RulePatTF", xmTextFieldWidgetClass, form,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, st->name_tf,
        XmNtopOffset, 4,
        XmNleftAttachment, XmATTACH_WIDGET,
        XmNleftWidget, l2,
        XmNleftOffset, 8,
        XmNrightAttachment, XmATTACH_FORM,
        NULL);

    XmString fl = XmStringCreateLocalized((char *)"Target Folder:");
    Widget l3 = XtVaCreateManagedWidget("RuleFldLbl", xmLabelWidgetClass, form,
        XmNlabelString, fl,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, st->pattern_tf,
        XmNtopOffset, 8,
        XmNleftAttachment, XmATTACH_FORM,
        NULL);
    XmStringFree(fl);

    st->folder_tf = XtVaCreateManagedWidget("RuleFldTF", xmTextFieldWidgetClass, form,
        XmNvalue, "Archive",
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, st->pattern_tf,
        XmNtopOffset, 4,
        XmNleftAttachment, XmATTACH_WIDGET,
        XmNleftWidget, l3,
        XmNleftOffset, 8,
        XmNrightAttachment, XmATTACH_FORM,
        NULL);

    XmString add_s = XmStringCreateLocalized((char *)"Add Rule");
    Widget add_btn = XtVaCreateManagedWidget("RuleAddBtn", xmPushButtonWidgetClass, form,
        XmNlabelString, add_s,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        NULL);
    XmStringFree(add_s);
    XtAddCallback(add_btn, XmNactivateCallback, on_rules_add, st);

    XmString del_s = XmStringCreateLocalized((char *)"Delete Selected");
    Widget del_btn = XtVaCreateManagedWidget("RuleDelBtn", xmPushButtonWidgetClass, form,
        XmNlabelString, del_s,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_WIDGET,
        XmNleftWidget, add_btn,
        XmNleftOffset, 8,
        NULL);
    XmStringFree(del_s);
    XtAddCallback(del_btn, XmNactivateCallback, on_rules_delete, st);

    XmString cls_s = XmStringCreateLocalized((char *)"Close");
    Widget cls_btn = XtVaCreateManagedWidget("RuleCloseBtn", xmPushButtonWidgetClass, form,
        XmNlabelString, cls_s,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL);
    XmStringFree(cls_s);
    XtAddCallback(cls_btn, XmNactivateCallback, on_rules_close, st);

    XtManageChild(form);
    rules_refresh_list(st);
    XtPopup(st->shell, XtGrabNone);
}
