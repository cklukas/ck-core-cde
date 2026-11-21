/*
 * CDE - Common Desktop Environment
 *
 * Persistent collapsible Accounts & Folders left sidebar for dtmail.
 */

#include "FolderSidebar.h"
#include "MailAccountStore.h"

#include <Xm/Form.h>
#include <Xm/Label.h>
#include <Xm/List.h>
#include <Xm/PushB.h>
#include <Xm/Separator.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_SIDEBAR_ITEMS 256

typedef struct {
    char label[256];
    char mbox_path[1024];
    int  is_header;
} SidebarEntry;

struct DtMailFolderSidebar {
    Widget form;
    Widget right_pane;
    Widget list_w;
    Widget sync_btn;
    Widget accts_btn;
    DtMailFolderSelectCB  on_select;
    DtMailSidebarActionCB on_sync;
    DtMailSidebarActionCB on_accounts;
    void *userdata;
    int visible;
    SidebarEntry entries[MAX_SIDEBAR_ITEMS];
    int nentries;
};

static int count_mbox_messages(const char *path)
{
    if (!path || !path[0]) return 0;
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char line[1024];
    int count = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "From ", 5) == 0) {
            count++;
        }
    }
    fclose(fp);
    return count;
}

static void add_entry(DtMailFolderSidebar *sb,
                      const char *label,
                      const char *mbox_path,
                      int is_header)
{
    if (!sb || sb->nentries >= MAX_SIDEBAR_ITEMS) return;
    SidebarEntry *e = &sb->entries[sb->nentries++];
    snprintf(e->label, sizeof(e->label), "%s", label ? label : "");
    snprintf(e->mbox_path, sizeof(e->mbox_path), "%s", mbox_path ? mbox_path : "");
    e->is_header = is_header;
}

static void on_list_select(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    DtMailFolderSidebar *sb = (DtMailFolderSidebar *)client_data;
    XmListCallbackStruct *cbs = (XmListCallbackStruct *)call_data;
    if (!sb || !cbs || cbs->item_position < 1 || cbs->item_position > sb->nentries) {
        return;
    }
    SidebarEntry *e = &sb->entries[cbs->item_position - 1];
    if (e->is_header || !e->mbox_path[0]) {
        return;
    }
    if (sb->on_select) {
        sb->on_select(e->mbox_path, sb->userdata);
    }
}

static void on_sync_click(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    DtMailFolderSidebar *sb = (DtMailFolderSidebar *)client_data;
    if (sb && sb->on_sync) {
        sb->on_sync(sb->userdata);
    }
}

static void on_accounts_click(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    DtMailFolderSidebar *sb = (DtMailFolderSidebar *)client_data;
    if (sb && sb->on_accounts) {
        sb->on_accounts(sb->userdata);
    }
}

void dtmail_folder_sidebar_refresh(DtMailFolderSidebar *sb)
{
    if (!sb || !sb->list_w) return;
    sb->nentries = 0;

    char inbox_path[1024] = "";
    const char *home = getenv("HOME");
    if (dtmail_ensure_local_fallback_inbox(inbox_path, sizeof(inbox_path)) == 0) {
        int n = count_mbox_messages(inbox_path);
        char lbl[256];
        if (n > 0) snprintf(lbl, sizeof(lbl), "Local Inbox (%d)", n);
        else snprintf(lbl, sizeof(lbl), "Local Inbox");
        add_entry(sb, lbl, inbox_path, 0);
    }

    if (home && home[0]) {
        char drafts_path[1024];
        snprintf(drafts_path, sizeof(drafts_path), "%s/.dt/mail/Drafts", home);
        int nd = count_mbox_messages(drafts_path);
        char lbl[256];
        if (nd > 0) snprintf(lbl, sizeof(lbl), "Drafts (%d)", nd);
        else snprintf(lbl, sizeof(lbl), "Drafts");
        add_entry(sb, lbl, drafts_path, 0);
    }

    static const char *std_folders[] = {
        "INBOX", "Drafts", "Sent", "Archive", "Trash", "Junk", NULL
    };

    int naccts = dtmail_accounts_count();
    for (int i = 0; i < naccts; i++) {
        const DtMailAccount *a = dtmail_accounts_get(i);
        if (!a || !a->enabled) continue;
        char hdr[256];
        snprintf(hdr, sizeof(hdr), "[%s]", a->name[0] ? a->name : a->email);
        add_entry(sb, hdr, "", 1);

        for (int f = 0; std_folders[f]; f++) {
            char fpath[1024];
            if (dtmail_account_folder_path(a->id, std_folders[f], fpath, sizeof(fpath)) == 0) {
                int cnt = count_mbox_messages(fpath);
                char flbl[256];
                if (cnt > 0) snprintf(flbl, sizeof(flbl), "  %s (%d)", std_folders[f], cnt);
                else snprintf(flbl, sizeof(flbl), "  %s", std_folders[f]);
                add_entry(sb, flbl, fpath, 0);
            }
        }
    }

    XmListDeleteAllItems(sb->list_w);
    if (sb->nentries > 0) {
        XmString *items = (XmString *)calloc((size_t)sb->nentries, sizeof(XmString));
        if (items) {
            for (int i = 0; i < sb->nentries; i++) {
                items[i] = XmStringCreateLocalized(sb->entries[i].label);
            }
            XmListAddItemsUnselected(sb->list_w, items, sb->nentries, 1);
            for (int i = 0; i < sb->nentries; i++) {
                XmStringFree(items[i]);
            }
            free(items);
        }
    }
}

void dtmail_folder_sidebar_set_visible(DtMailFolderSidebar *sb, int visible)
{
    if (!sb || !sb->form) return;
    sb->visible = visible ? 1 : 0;
    if (sb->visible) {
        XtManageChild(sb->form);
        if (sb->right_pane) {
            XtVaSetValues(sb->right_pane,
                XmNleftAttachment, XmATTACH_WIDGET,
                XmNleftWidget, sb->form,
                XmNleftOffset, 2,
                NULL);
        }
    } else {
        if (sb->right_pane) {
            XtVaSetValues(sb->right_pane,
                XmNleftAttachment, XmATTACH_FORM,
                XmNleftOffset, 0,
                NULL);
        }
        XtUnmanageChild(sb->form);
    }
}

int dtmail_folder_sidebar_is_visible(const DtMailFolderSidebar *sb)
{
    return sb ? sb->visible : 0;
}

Widget dtmail_folder_sidebar_widget(const DtMailFolderSidebar *sb)
{
    return sb ? sb->form : NULL;
}

DtMailFolderSidebar *dtmail_folder_sidebar_create(
    Widget parent_form,
    Widget right_pane,
    DtMailFolderSelectCB on_select,
    DtMailSidebarActionCB on_sync,
    DtMailSidebarActionCB on_accounts,
    void *userdata)
{
    if (!parent_form) return NULL;
    DtMailFolderSidebar *sb = (DtMailFolderSidebar *)calloc(1, sizeof(DtMailFolderSidebar));
    if (!sb) return NULL;

    sb->right_pane = right_pane;
    sb->on_select = on_select;
    sb->on_sync = on_sync;
    sb->on_accounts = on_accounts;
    sb->userdata = userdata;
    sb->visible = 1;

    sb->form = XtVaCreateManagedWidget("FolderSidebar", xmFormWidgetClass, parent_form,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNwidth, 205,
        XmNshadowThickness, 1,
        XmNshadowType, XmSHADOW_ETCHED_IN,
        NULL);

    XmString hdr_s = XmStringCreateLocalized((char *)"Accounts & Folders");
    Widget hdr_lbl = XtVaCreateManagedWidget("FolderSidebarHeader", xmLabelWidgetClass, sb->form,
        XmNlabelString, hdr_s,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNtopAttachment, XmATTACH_FORM,
        XmNtopOffset, 6,
        XmNleftAttachment, XmATTACH_FORM,
        XmNleftOffset, 6,
        XmNrightAttachment, XmATTACH_FORM,
        XmNrightOffset, 6,
        NULL);
    XmStringFree(hdr_s);

    Widget btn_form = XtVaCreateManagedWidget("FolderSidebarBtns", xmFormWidgetClass, sb->form,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNbottomOffset, 4,
        XmNleftAttachment, XmATTACH_FORM,
        XmNleftOffset, 4,
        XmNrightAttachment, XmATTACH_FORM,
        XmNrightOffset, 4,
        NULL);

    XmString sync_s = XmStringCreateLocalized((char *)"Sync");
    sb->sync_btn = XtVaCreateManagedWidget("SidebarSyncBtn", xmPushButtonWidgetClass, btn_form,
        XmNlabelString, sync_s,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_POSITION,
        XmNrightPosition, 48,
        NULL);
    XmStringFree(sync_s);
    XtAddCallback(sb->sync_btn, XmNactivateCallback, on_sync_click, sb);

    XmString accts_s = XmStringCreateLocalized((char *)"Accounts...");
    sb->accts_btn = XtVaCreateManagedWidget("SidebarAcctsBtn", xmPushButtonWidgetClass, btn_form,
        XmNlabelString, accts_s,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_POSITION,
        XmNleftPosition, 52,
        XmNrightAttachment, XmATTACH_FORM,
        NULL);
    XmStringFree(accts_s);
    XtAddCallback(sb->accts_btn, XmNactivateCallback, on_accounts_click, sb);

    Arg args[8];
    int n = 0;
    XtSetArg(args[n], XmNselectionPolicy, XmBROWSE_SELECT); n++;
    XtSetArg(args[n], XmNlistSizePolicy, XmCONSTANT); n++;
    sb->list_w = XmCreateScrolledList(sb->form, (char *)"FolderSidebarList", args, n);
    XtManageChild(sb->list_w);
    XtAddCallback(sb->list_w, XmNbrowseSelectionCallback, on_list_select, sb);
    XtAddCallback(sb->list_w, XmNdefaultActionCallback, on_list_select, sb);

    XtVaSetValues(XtParent(sb->list_w),
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, hdr_lbl,
        XmNtopOffset, 4,
        XmNbottomAttachment, XmATTACH_WIDGET,
        XmNbottomWidget, btn_form,
        XmNbottomOffset, 4,
        XmNleftAttachment, XmATTACH_FORM,
        XmNleftOffset, 4,
        XmNrightAttachment, XmATTACH_FORM,
        XmNrightOffset, 4,
        NULL);

    dtmail_folder_sidebar_refresh(sb);
    dtmail_folder_sidebar_set_visible(sb, 1);
    return sb;
}

void dtmail_folder_sidebar_destroy(DtMailFolderSidebar *sb)
{
    if (!sb) return;
    free(sb);
}
