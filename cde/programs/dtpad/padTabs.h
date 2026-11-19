#ifndef DTPAD_PAD_TABS_H
#define DTPAD_PAD_TABS_H

#include "dtpad.h"

Widget DtpadCreateWorkArea(Editor *pPad);
void DtpadPopulateFileMenuExtras(Editor *pPad, Widget file_pane);
void DtpadPopulateOptionsMenuExtras(Editor *pPad, Widget options_pane);
void DtpadRefreshRecentMenu(Editor *pPad);

void DtpadNotifyFileLoaded(Editor *pPad);
void DtpadNotifyFileSaved(Editor *pPad, const char *saved_path);
void DtpadNotifyPadClosing(Editor *pPad, Boolean discard_autosave);
void DtpadSyncActiveTabTitle(Editor *pPad);
void DtpadPublishState(Editor *pPad);

void DtpadNewTab(Editor *pPad, const char *initial_path);
void DtpadSelectTab(Editor *pPad, int index);
void DtpadCloseActiveTab(Editor *pPad);
void DtpadSetLineNumbers(Editor *pPad, Boolean enabled);
void DtpadFlushAutosaveNow(Editor *pPad);

#endif /* DTPAD_PAD_TABS_H */
