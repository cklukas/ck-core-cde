#ifndef DTMAIL_FOLDER_SIDEBAR_H
#define DTMAIL_FOLDER_SIDEBAR_H

#include <Xm/Xm.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*DtMailFolderSelectCB)(const char *mbox_path, void *userdata);
typedef void (*DtMailSidebarActionCB)(void *userdata);

typedef struct DtMailFolderSidebar DtMailFolderSidebar;

DtMailFolderSidebar *dtmail_folder_sidebar_create(
    Widget parent_form,
    Widget right_pane,
    DtMailFolderSelectCB on_select,
    DtMailSidebarActionCB on_sync,
    DtMailSidebarActionCB on_accounts,
    void *userdata);

void dtmail_folder_sidebar_destroy(DtMailFolderSidebar *sb);
void dtmail_folder_sidebar_refresh(DtMailFolderSidebar *sb);
void dtmail_folder_sidebar_set_visible(DtMailFolderSidebar *sb, int visible);
int  dtmail_folder_sidebar_is_visible(const DtMailFolderSidebar *sb);
Widget dtmail_folder_sidebar_widget(const DtMailFolderSidebar *sb);

#ifdef __cplusplus
}
#endif

#endif /* DTMAIL_FOLDER_SIDEBAR_H */
