/*
 * CDE - Common Desktop Environment
 *
 * These libraries and programs are free software; you can redistribute them
 * and/or modify them under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * These libraries and programs are distributed without any warranty; without
 * even the implied warranty of merchantability or fitness for a particular
 * purpose. See the GNU Lesser General Public License for more details.
 */

/* The conflict view owns presentation and user choices, never filesystem I/O. */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <Xm/DialogS.h>
#include <Xm/Form.h>
#include <Xm/Label.h>
#include <Xm/List.h>
#include <Xm/MessageB.h>
#include <Xm/MwmUtil.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
#include <Xm/Text.h>
#include <Xm/TextF.h>
#include <Xm/ToggleB.h>
#include <Dt/SharedProcs.h>
#include "Encaps.h"
#include "FileMgr.h"
#include "Desktop.h"
#include "Main.h"
#include "Help.h"
#include "FileConflictDialog.h"
#include "FileConflictWire.h"

#define CONFLICT_PAGE_ITEMS 128u
#define CONFLICT_ACTIONS 6u

typedef struct {
    struct DtfileConflictDialog *dialog;
    DtfileConflictAction action;
} ConflictActionControl;

struct DtfileConflictDialog {
    unsigned int references;        /* Operation owner + live widget tree. */
    int reply_fd;
    int closing, updating, name_dirty, display_valid;
    DtfileConflictBatch batch;
    DtfileConflictDecision *decisions;
    char **names;                   /* Owns remembered per-row rename drafts. */
    size_t name_bytes, selected, page;
    Widget shell, box, help_shell, list, details, manual, feedback, caption;
    Widget previous, next, apply, toggles[CONFLICT_ACTIONS];
    ConflictActionControl controls[CONFLICT_ACTIONS];
};

static int ConflictRefresh(DtfileConflictDialog *dialog);

static int
ConflictManual(DtfileConflictAction action)
{
    return action == DTFILE_CONFLICT_RENAME_EXISTING || action == DTFILE_CONFLICT_RENAME_INCOMING;
}

static char *
ConflictActionLabel(DtfileConflictAction action)
{
    switch (action)
    {
        case DTFILE_CONFLICT_REPLACE: return GETMESSAGE(42, 3, "Replace destination");
        case DTFILE_CONFLICT_KEEP_BOTH: return GETMESSAGE(42, 4, "Keep both (add a number)");
        case DTFILE_CONFLICT_RENAME_EXISTING: return GETMESSAGE(42, 5, "Rename destination");
        case DTFILE_CONFLICT_RENAME_INCOMING: return GETMESSAGE(42, 6, "Rename incoming object");
        case DTFILE_CONFLICT_MERGE: return GETMESSAGE(42, 7, "Merge folder contents");
        case DTFILE_CONFLICT_SKIP: return GETMESSAGE(42, 8, "Skip this object");
    }
    return "";
}

static int
ConflictLabel(Widget widget, const char *text)
{
    XmString string = XmStringCreateLocalized((char *)text);
    if (string != NULL)
    {
        XtVaSetValues(widget, XmNlabelString, string, NULL);
        XmStringFree(string);
        return 1;
    }
    return 0;
}

static void
ConflictFeedback(DtfileConflictDialog *dialog, const char *text)
{
    if (dialog->feedback != NULL) ConflictLabel(dialog->feedback, text);
}

static void
ConflictRelease(DtfileConflictDialog *dialog)
{
    size_t i;
    if (--dialog->references != 0) return;
    for (i = 0; i < dialog->batch.count; ++i) free(dialog->names == NULL ? NULL : dialog->names[i]);
    free(dialog->names);
    /* Decision names borrow the remembered drafts; decoded worker decisions
     * instead own their strings and use DtfileConflictDecisionsFree. */
    free(dialog->decisions);
    DtfileConflictBatchFree(&dialog->batch);
    free(dialog);
}

static void
ConflictCancelEndpoint(DtfileConflictDialog *dialog)
{
    if (dialog->reply_fd >= 0)
    {
        (void)DtfileConflictWriteCancel(dialog->reply_fd);
        (void)close(dialog->reply_fd);
        dialog->reply_fd = -1;
    }
}

static void
ConflictDestroyed(Widget widget, XtPointer data, XtPointer call)
{
    DtfileConflictDialog *dialog = data;
    (void)widget; (void)call;
    dialog->closing = 1;
    dialog->box = dialog->shell = NULL;
    ConflictCancelEndpoint(dialog);
    ConflictRelease(dialog);
}

int
DtfileConflictDialogPending(const DtfileConflictDialog *dialog)
{
    return dialog != NULL && !dialog->closing && dialog->reply_fd >= 0;
}

void
DtfileConflictDialogFinish(DtfileConflictDialog *dialog)
{
    if (dialog == NULL) return;
    dialog->closing = 1;
    ConflictCancelEndpoint(dialog);
    if (dialog->shell != NULL) XtDestroyWidget(dialog->shell);
    ConflictRelease(dialog);        /* Widget destruction can be deferred. */
}

static int
ConflictStoreName(DtfileConflictDialog *dialog)
{
    size_t index = dialog->selected, length, old_length;
    char *text, *copy;
    if (!ConflictManual(dialog->decisions[index].action) || !dialog->name_dirty) return 1;
    text = XmTextFieldGetString(dialog->manual);
    if (text == NULL)
    { ConflictFeedback(dialog, GETMESSAGE(42, 26, "Not enough memory to keep the new name. Try again.")); return 0; }
    length = strnlen(text, DTFILE_CONFLICT_PATH_LIMIT + 1);
    old_length = dialog->names[index] == NULL ? 0 : strlen(dialog->names[index]);
    if (length > DTFILE_CONFLICT_PATH_LIMIT ||
        length > DTFILE_CONFLICT_BYTES_LIMIT - (dialog->name_bytes - old_length))
    {
        XtFree(text);
        ConflictFeedback(dialog, GETMESSAGE(42, 10, "The new name is too long."));
        return 0;
    }
    copy = malloc(length + 1);
    if (copy == NULL)
    {
        XtFree(text);
        ConflictFeedback(dialog, GETMESSAGE(42, 26, "Not enough memory to keep the new name. Try again."));
        return 0;
    }
    memcpy(copy, text, length + 1);
    XtFree(text);
    free(dialog->names[index]);
    dialog->names[index] = copy;
    dialog->decisions[index].name = copy;
    dialog->name_bytes = dialog->name_bytes - old_length + length;
    dialog->name_dirty = 0;
    return 1;
}

static XmString
ConflictRow(DtfileConflictDialog *dialog, size_t index)
{
    const char *leaf = strrchr(dialog->batch.items[index].target, '/');
    char *display = DtfileConflictDisplayPath(leaf == NULL || leaf[1] == 0 ?
                                              dialog->batch.items[index].target : leaf + 1);
    char *label;
    char *action = ConflictActionLabel(dialog->decisions[index].action);
    size_t used = 0, length;
    XmString result;
    if (display == NULL) return NULL;
    /* A bounded row preview; full escaped paths remain in the details view. */
    while (display[used] != 0)
    {
        size_t next = display[used] == '\\' ? 4 : 1;
        if (used + next > 200) break;
        used += next;
    }
    length = strlen(action) + used + 48;
    label = malloc(length);
    if (label == NULL) { free(display); return NULL; }
    snprintf(label, length, "%lu. %s: %.*s%s",
             (unsigned long)dialog->batch.items[index].request_index + 1,
             action, (int)used, display, display[used] == 0 ? "" : "...");
    result = XmStringCreateLocalized(label);
    free(label); free(display);
    return result;
}

static int
ConflictRefreshPage(DtfileConflictDialog *dialog)
{
    XmString strings[CONFLICT_PAGE_ITEMS];
    size_t i, count = dialog->batch.count - dialog->page;
    char caption[160];
    if (count > CONFLICT_PAGE_ITEMS) count = CONFLICT_PAGE_ITEMS;
    memset(strings, 0, sizeof(strings));
    for (i = 0; i < count; ++i)
        if ((strings[i] = ConflictRow(dialog, dialog->page + i)) == NULL) break;
    if (i != count)
    {
        while (i != 0) XmStringFree(strings[--i]);
        return 0;
    }
    XtVaSetValues(dialog->list, XmNitems, strings, XmNitemCount, (int)count, NULL);
    for (i = 0; i < count; ++i) XmStringFree(strings[i]);
    XmListSelectPos(dialog->list, (int)(dialog->selected - dialog->page) + 1, False);
    snprintf(caption, sizeof(caption), GETMESSAGE(42, 9, "Conflicts %lu-%lu of %lu"),
             (unsigned long)dialog->page + 1, (unsigned long)(dialog->page + count),
             (unsigned long)dialog->batch.count);
    if (!ConflictLabel(dialog->caption, caption)) return 0;
    XtSetSensitive(dialog->previous, dialog->page != 0);
    XtSetSensitive(dialog->next, dialog->page + count < dialog->batch.count);
    return 1;
}

static int
ConflictRefresh(DtfileConflictDialog *dialog)
{
    size_t i = dialog->selected, action;
    DtfileConflictItem *item = &dialog->batch.items[i];
    char *source = item->source == NULL ? strdup(GETMESSAGE(42, 11, "Incoming buffer data")) :
                                        DtfileConflictDisplayPath(item->source);
    char *target = DtfileConflictDisplayPath(item->target);
    char *details = NULL;
    int details_ready = 0, page_ready;
    dialog->updating = 1;
    dialog->display_valid = 0;
    XtSetSensitive(XmMessageBoxGetChild(dialog->box, XmDIALOG_OK_BUTTON), False);
    if (source != NULL && target != NULL)
    {
        char *format = GETMESSAGE(42, 12, "Source: %s\nDestination: %s");
        size_t length = strlen(format) + strlen(source) + strlen(target) + 1;
        details = malloc(length);
        if (details != NULL)
        {
            snprintf(details, length, format, source, target);
            XmTextSetString(dialog->details, details);
            details_ready = 1;
        }
    }
    if (!details_ready) XmTextSetString(dialog->details, "");
    free(source); free(target); free(details);
    XmTextFieldSetString(dialog->manual, dialog->names[i] == NULL ? "" : dialog->names[i]);
    dialog->name_dirty = 0;
    XtSetSensitive(dialog->manual, ConflictManual(dialog->decisions[i].action));
    XtSetSensitive(dialog->apply, !ConflictManual(dialog->decisions[i].action));
    for (action = 0; action < CONFLICT_ACTIONS; ++action)
    {
        DtfileConflictAction value = dialog->controls[action].action;
        XmToggleButtonSetState(dialog->toggles[action], value == dialog->decisions[i].action, False);
        XtSetSensitive(dialog->toggles[action], DtfileConflictActionAllowed(&dialog->batch, i, value));
    }
    page_ready = ConflictRefreshPage(dialog);
    if (!details_ready || !page_ready)
        ConflictFeedback(dialog, GETMESSAGE(42, 13, "Not enough memory to update the conflict view."));
    dialog->display_valid = details_ready && page_ready;
    XtSetSensitive(XmMessageBoxGetChild(dialog->box, XmDIALOG_OK_BUTTON), dialog->display_valid);
    dialog->updating = 0;
    return dialog->display_valid;
}

static void
ConflictSelection(Widget widget, XtPointer data, XtPointer call)
{
    DtfileConflictDialog *dialog = data;
    XmListCallbackStruct *selected = call;
    (void)widget;
    if (dialog->updating || dialog->closing || selected == NULL ||
        selected->item_position < 1 || selected->item_position > (int)CONFLICT_PAGE_ITEMS ||
        dialog->page + (size_t)selected->item_position > dialog->batch.count) return;
    if (!ConflictStoreName(dialog))
    {
        XmListSelectPos(dialog->list, (int)(dialog->selected - dialog->page) + 1, False);
        return;
    }
    dialog->selected = dialog->page + (size_t)selected->item_position - 1;
    ConflictFeedback(dialog, "");
    ConflictRefresh(dialog);
}

static void
ConflictNameChanged(Widget widget, XtPointer data, XtPointer call)
{
    DtfileConflictDialog *dialog = data;
    (void)widget; (void)call;
    if (dialog->updating || dialog->closing) return;
    dialog->name_dirty = 1;
    ConflictFeedback(dialog, "");
}

static void
ConflictActionChanged(Widget widget, XtPointer data, XtPointer call)
{
    ConflictActionControl *control = data;
    DtfileConflictDialog *dialog = control->dialog;
    XmToggleButtonCallbackStruct *toggle = call;
    (void)widget;
    if (dialog->updating || dialog->closing || toggle == NULL || !toggle->set ||
        !DtfileConflictActionAllowed(&dialog->batch, dialog->selected, control->action)) return;
    if (!ConflictStoreName(dialog))
    {
        size_t i;
        for (i = 0; i < CONFLICT_ACTIONS; ++i)
            XmToggleButtonSetState(dialog->toggles[i],
                dialog->controls[i].action == dialog->decisions[dialog->selected].action, False);
        return;
    }
    dialog->decisions[dialog->selected].action = control->action;
    dialog->decisions[dialog->selected].name = ConflictManual(control->action) ?
                                             dialog->names[dialog->selected] : NULL;
    ConflictFeedback(dialog, "");
    ConflictRefresh(dialog);
}

static void
ConflictPage(Widget widget, XtPointer data, XtPointer call)
{
    DtfileConflictDialog *dialog = data;
    (void)call;
    if (dialog->closing || !ConflictStoreName(dialog)) return;
    if (widget == dialog->previous && dialog->page >= CONFLICT_PAGE_ITEMS)
        dialog->page -= CONFLICT_PAGE_ITEMS;
    else if (widget == dialog->next && dialog->page + CONFLICT_PAGE_ITEMS < dialog->batch.count)
        dialog->page += CONFLICT_PAGE_ITEMS;
    dialog->selected = dialog->page;
    ConflictRefresh(dialog);
}

static void
ConflictApply(Widget widget, XtPointer data, XtPointer call)
{
    DtfileConflictDialog *dialog = data;
    DtfileConflictAction action = dialog->decisions[dialog->selected].action;
    size_t i, changed = 0;
    char feedback[160];
    (void)widget; (void)call;
    if (dialog->closing || ConflictManual(action)) return;
    for (i = 0; i < dialog->batch.count; ++i)
        if (DtfileConflictActionAllowed(&dialog->batch, i, action))
        {
            dialog->decisions[i].action = action;
            dialog->decisions[i].name = NULL;
            ++changed;
        }
    ConflictRefresh(dialog);
    snprintf(feedback, sizeof(feedback), GETMESSAGE(42, 14, "Applied to %lu conflicts; %lu kept their other action."),
             (unsigned long)changed, (unsigned long)(dialog->batch.count - changed));
    ConflictFeedback(dialog, feedback);
}

static void
ConflictCancel(Widget widget, XtPointer data, XtPointer call)
{
    DtfileConflictDialog *dialog = data;
    (void)widget; (void)call;
    if (dialog->closing) return;
    dialog->closing = 1;
    ConflictCancelEndpoint(dialog);
    XtDestroyWidget(dialog->shell);
}

static void
ConflictAccept(Widget widget, XtPointer data, XtPointer call)
{
    DtfileConflictDialog *dialog = data;
    size_t i;
    (void)widget; (void)call;
    if (dialog->closing || !dialog->display_valid || dialog->reply_fd < 0 || !ConflictStoreName(dialog)) return;
    for (i = 0; i < dialog->batch.count; ++i)
    {
        DtfileConflictDecision *decision = &dialog->decisions[i];
        if (DtfileConflictDecisionValidate(&dialog->batch, i, decision) != 0)
        {
            dialog->selected = i; dialog->page = (i / CONFLICT_PAGE_ITEMS) * CONFLICT_PAGE_ITEMS;
            ConflictRefresh(dialog);
            ConflictFeedback(dialog, GETMESSAGE(42, 15, "Enter a new name for the selected conflict."));
            return;
        }
        /* These are cached application-view checks, not filesystem deletion or
         * permission work. Query the current UI state, not a worker's forked copy.
         */
        if ((DtfileConflictChangesExisting(&dialog->batch, i, decision->action) &&
             DirectoryBusy(dialog->batch.items[i].target)) ||
            (dialog->batch.operation == DTFILE_CONFLICT_MOVE && decision->action != DTFILE_CONFLICT_SKIP &&
             DirectoryBusy(dialog->batch.items[i].source)))
        {
            dialog->selected = i; dialog->page = (i / CONFLICT_PAGE_ITEMS) * CONFLICT_PAGE_ITEMS;
            ConflictRefresh(dialog);
            ConflictFeedback(dialog, GETMESSAGE(42, 16, "Close the open views for this folder, then try again."));
            return;
        }
    }
    if (DtfileConflictWriteDecisions(dialog->reply_fd, &dialog->batch, dialog->decisions) != 0)
        _DtMessage(toplevel, GETMESSAGE(42, 17, "File operation response"),
                   GETMESSAGE(42, 18, "The response could not be sent. Wait for the operation's result."),
                   NULL, HelpRequestCB);
    (void)close(dialog->reply_fd);
    dialog->reply_fd = -1;
    dialog->closing = 1;
    XtDestroyWidget(dialog->shell);
}

static Widget
ConflictButton(Widget parent, const char *name, const char *label,
               XtCallbackProc callback, DtfileConflictDialog *dialog)
{
    XmString string = XmStringCreateLocalized((char *)label);
    Widget button;
    if (string == NULL) return NULL;
    button = XtVaCreateManagedWidget(name, xmPushButtonWidgetClass, parent,
                                            XmNlabelString, string, NULL);
    XmStringFree(string);
    XtAddCallback(button, XmNactivateCallback, callback, dialog);
    return button;
}

static void
ConflictPosition(Widget shell, XtPointer data, XtPointer call)
{
    Widget parent = data;
    Dimension width = 0, height = 0;
    long x, y, screen_width = WidthOfScreen(XtScreen(shell)), screen_height = HeightOfScreen(XtScreen(shell));
    XWindowAttributes attributes;
    (void)call;
    XtVaGetValues(shell, XmNwidth, &width, XmNheight, &height, NULL);
    x = (screen_width - width) / 2; y = (screen_height - height) / 2;
    if (XtIsRealized(parent) && XGetWindowAttributes(XtDisplay(parent), XtWindow(parent), &attributes) &&
        attributes.map_state == IsViewable)
    {
        Window child;
        int parent_x, parent_y;
        if (XTranslateCoordinates(XtDisplay(parent), XtWindow(parent),
                                  RootWindowOfScreen(XtScreen(parent)), 0, 0, &parent_x, &parent_y, &child))
        { x = parent_x + (attributes.width - (long)width) / 2; y = parent_y + (attributes.height - (long)height) / 2; }
    }
    if (x > screen_width - width) x = screen_width - width;
    if (y > screen_height - height) y = screen_height - height;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > SHRT_MAX) x = SHRT_MAX;
    if (y > SHRT_MAX) y = SHRT_MAX;
    XtVaSetValues(shell, XmNx, (Position)x, XmNy, (Position)y, NULL);
}

static void
ConflictHelpDestroyed(Widget widget, XtPointer data, XtPointer call)
{
    DtfileConflictDialog *dialog = data;
    (void)call;
    if (dialog->help_shell == widget) dialog->help_shell = NULL;
    ConflictRelease(dialog);
}

static void
ConflictHelpClose(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget; (void)call;
    XtDestroyWidget((Widget)data);
}

static void
ConflictHelp(Widget widget, XtPointer data, XtPointer call)
{
    DtfileConflictDialog *dialog = data;
    Widget shell, box;
    XmString text;
    Arg args[8];
    Cardinal n;
    (void)widget; (void)call;
    if (dialog->closing) return;
    if (dialog->help_shell != NULL)
    {
        XtPopup(dialog->help_shell, XtGrabNone);
        return;
    }
    n = 0;
    XtSetArg(args[n], XmNtitle, GETMESSAGE(42, 27, "Conflict choices")); ++n;
    XtSetArg(args[n], XmNdeleteResponse, XmDESTROY); ++n;
    XtSetArg(args[n], XmNmwmFunctions, MWM_FUNC_MOVE | MWM_FUNC_CLOSE); ++n;
    XtSetArg(args[n], XmNmwmDecorations, MWM_DECOR_BORDER | MWM_DECOR_TITLE | MWM_DECOR_MENU); ++n;
    shell = XmCreateDialogShell(dialog->shell, "conflictHelp_popup", args, n);
    if (shell == NULL) return;
    dialog->help_shell = shell;
    ++dialog->references;
    XtAddCallback(shell, XmNdestroyCallback, ConflictHelpDestroyed, dialog);
    XtAddCallback(shell, XmNpopupCallback, ConflictPosition, dialog->shell);
    text = XmStringCreateLocalized(GETMESSAGE(42, 28,
        "Each row has its own action. Select a row to see its complete source\n"
        "and destination. Apply action changes only compatible conflicts.\n\n"
        "Replace keeps a temporary backup until the incoming operation succeeds.\n"
        "If automatic recovery cannot finish, File Manager reports the retained backup.\n"
        "Keep both adds a number to the incoming object's name and leaves\n"
        "the existing destination unchanged.\n"
        "Rename destination keeps it under the name you enter.\n"
        "Rename incoming uses your new name for the incoming object.\n"
        "Enter one filename without a slash. Type it literally, not as the\n"
        "displayed byte escapes.\n"
        "Merge combines folder contents. Skip leaves the object alone.\n\n"
        "Cancel or window Close stops pending conflict actions.\n"
        "Earlier completed work is kept; cancellation does not undo it."));
    if (text == NULL) { XtDestroyWidget(shell); return; }
    n = 0;
    XtSetArg(args[n], XmNdialogType, XmDIALOG_INFORMATION); ++n;
    XtSetArg(args[n], XmNmessageString, text); ++n;
    XtSetArg(args[n], XmNautoUnmanage, False); ++n;
    XtSetArg(args[n], XmNdefaultPosition, False); ++n;
    box = XmCreateMessageBox(shell, "conflictHelp", args, n);
    XmStringFree(text);
    if (box == NULL) { XtDestroyWidget(shell); return; }
    XtUnmanageChild(XmMessageBoxGetChild(box, XmDIALOG_HELP_BUTTON));
    XtUnmanageChild(XmMessageBoxGetChild(box, XmDIALOG_CANCEL_BUTTON));
    XtAddCallback(box, XmNokCallback, ConflictHelpClose, shell);
    XtManageChild(box);
}

DtfileConflictDialog *
DtfileConflictDialogCreate(Widget parent, DtfileConflictBatch *batch, int reply_fd)
{
    DtfileConflictDialog *dialog;
    Widget form, pager, controls, radio, tools, list_parent, detail_parent, label;
    Arg args[12];
    Cardinal n;
    size_t i;
    XmString string;
    char *title;
    if (reply_fd < 0 || DtfileConflictBatchValidate(batch) != 0) goto failed;
    dialog = calloc(1, sizeof(*dialog));
    if (dialog == NULL) goto failed;
    dialog->references = 1;
    dialog->reply_fd = reply_fd;
    dialog->batch = *batch; memset(batch, 0, sizeof(*batch));
    dialog->decisions = calloc(dialog->batch.count, sizeof(*dialog->decisions));
    dialog->names = calloc(dialog->batch.count, sizeof(*dialog->names));
    if (dialog->decisions == NULL || dialog->names == NULL) { DtfileConflictDialogFinish(dialog); return NULL; }
    for (i = 0; i < dialog->batch.count; ++i)
    {
        dialog->decisions[i].request_index = dialog->batch.items[i].request_index;
        dialog->decisions[i].action = DtfileConflictDefaultAction(&dialog->batch, i);
    }
    title = dialog->batch.operation == DTFILE_CONFLICT_MOVE ? GETMESSAGE(9, 34, "File Manager - Move Warning") :
            dialog->batch.operation == DTFILE_CONFLICT_LINK ? GETMESSAGE(9, 36, "File Manager - Link Warning") :
            GETMESSAGE(9, 35, "File Manager - Copy Warning");
    n = 0;
    XtSetArg(args[n], XmNtitle, title); ++n;
    XtSetArg(args[n], XmNallowShellResize, True); ++n;
    XtSetArg(args[n], XmNmwmFunctions, MWM_FUNC_MOVE | MWM_FUNC_RESIZE | MWM_FUNC_CLOSE); ++n;
    XtSetArg(args[n], XmNmwmDecorations, MWM_DECOR_BORDER | MWM_DECOR_TITLE | MWM_DECOR_MENU | MWM_DECOR_RESIZEH); ++n;
    XtSetArg(args[n], XmNdeleteResponse, XmDESTROY); ++n;
    dialog->shell = XmCreateDialogShell(parent, "fileConflicts_popup", args, n);
    if (dialog->shell == NULL) { DtfileConflictDialogFinish(dialog); return NULL; }
    XtAddCallback(dialog->shell, XmNpopupCallback, ConflictPosition, parent);
    string = XmStringCreateLocalized(GETMESSAGE(42, 2,
        "Review the actions for pending conflicts.\nEarlier completed work is kept if you cancel."));
    if (string == NULL) { DtfileConflictDialogFinish(dialog); return NULL; }
    n = 0;
    XtSetArg(args[n], XmNdialogType, XmDIALOG_WARNING); ++n;
    XtSetArg(args[n], XmNmessageString, string); ++n;
    XtSetArg(args[n], XmNdialogStyle, XmDIALOG_MODELESS); ++n;
    XtSetArg(args[n], XmNautoUnmanage, False); ++n;
    XtSetArg(args[n], XmNdefaultPosition, False); ++n;
    XtSetArg(args[n], XmNdefaultButtonType, XmDIALOG_CANCEL_BUTTON); ++n;
    dialog->box = XmCreateMessageBox(dialog->shell, "fileConflicts", args, n);
    XmStringFree(string);
    if (dialog->box == NULL) { DtfileConflictDialogFinish(dialog); return NULL; }
    ++dialog->references;
    XtAddCallback(dialog->box, XmNdestroyCallback, ConflictDestroyed, dialog);
    XtAddCallback(dialog->box, XmNokCallback, ConflictAccept, dialog);
    XtAddCallback(dialog->box, XmNcancelCallback, ConflictCancel, dialog);
    XtAddCallback(dialog->box, XmNhelpCallback, ConflictHelp, dialog);
    ConflictLabel(XmMessageBoxGetChild(dialog->box, XmDIALOG_OK_BUTTON), GETMESSAGE(42, 19, "Continue operation"));
    ConflictLabel(XmMessageBoxGetChild(dialog->box, XmDIALOG_CANCEL_BUTTON), GETMESSAGE(42, 20, "Cancel operation"));
    dialog->updating = 1;
    form = XtVaCreateManagedWidget("review", xmFormWidgetClass, dialog->box,
                                  XmNwidth, 700, XmNmarginWidth, 8, XmNmarginHeight, 8, NULL);
    pager = XtVaCreateManagedWidget("pages", xmRowColumnWidgetClass, form,
                                   XmNorientation, XmHORIZONTAL, XmNpacking, XmPACK_TIGHT,
                                   XmNtopAttachment, XmATTACH_FORM, XmNleftAttachment, XmATTACH_FORM, NULL);
    dialog->previous = ConflictButton(pager, "previous", GETMESSAGE(42, 21, "Previous"), ConflictPage, dialog);
    dialog->caption = XtVaCreateManagedWidget("count", xmLabelWidgetClass, pager, NULL);
    dialog->next = ConflictButton(pager, "next", GETMESSAGE(42, 22, "Next"), ConflictPage, dialog);
    if (dialog->previous == NULL || dialog->next == NULL)
    { DtfileConflictDialogFinish(dialog); return NULL; }
    dialog->list = XmCreateScrolledList(form, "conflictList", NULL, 0);
    list_parent = XtParent(dialog->list);
    XtVaSetValues(dialog->list, XmNvisibleItemCount, 5, XmNselectionPolicy, XmBROWSE_SELECT, NULL);
    XtVaSetValues(list_parent, XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, pager,
                  XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_FORM, NULL);
    XtAddCallback(dialog->list, XmNbrowseSelectionCallback, ConflictSelection, dialog);
    XtManageChild(dialog->list);
    dialog->details = XmCreateScrolledText(form, "paths", NULL, 0);
    detail_parent = XtParent(dialog->details);
    XtVaSetValues(dialog->details, XmNrows, 4, XmNeditMode, XmMULTI_LINE_EDIT, XmNeditable, False,
                  XmNcursorPositionVisible, False, XmNwordWrap, True, XmNscrollHorizontal, False, NULL);
    XtVaSetValues(detail_parent, XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, list_parent,
                  XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_FORM, NULL);
    XtManageChild(dialog->details);
    controls = XtVaCreateManagedWidget("choices", xmRowColumnWidgetClass, form,
                                      XmNorientation, XmHORIZONTAL, XmNpacking, XmPACK_TIGHT,
                                      XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, detail_parent,
                                      XmNleftAttachment, XmATTACH_FORM, NULL);
    radio = XmCreateRadioBox(controls, "action", NULL, 0);
    for (i = 0; i < CONFLICT_ACTIONS; ++i)
    {
        dialog->controls[i].dialog = dialog;
        dialog->controls[i].action = (DtfileConflictAction)(i + DTFILE_CONFLICT_REPLACE);
        string = XmStringCreateLocalized(ConflictActionLabel(dialog->controls[i].action));
        if (string == NULL) { DtfileConflictDialogFinish(dialog); return NULL; }
        dialog->toggles[i] = XtVaCreateManagedWidget("choice", xmToggleButtonWidgetClass, radio,
                                                     XmNlabelString, string, NULL);
        XmStringFree(string);
        XtAddCallback(dialog->toggles[i], XmNvalueChangedCallback, ConflictActionChanged, &dialog->controls[i]);
    }
    XtManageChild(radio);
    tools = XtVaCreateManagedWidget("rename", xmRowColumnWidgetClass, controls, XmNorientation, XmVERTICAL, NULL);
    label = XtVaCreateManagedWidget("newNameLabel", xmLabelWidgetClass, tools, NULL);
    if (!ConflictLabel(label, GETMESSAGE(42, 23, "New name (typed literally):")))
    { DtfileConflictDialogFinish(dialog); return NULL; }
    dialog->manual = XtVaCreateManagedWidget("newName", xmTextFieldWidgetClass, tools,
                                            XmNcolumns, 28, XmNmaxLength, (int)DTFILE_CONFLICT_PATH_LIMIT, NULL);
    XtAddCallback(dialog->manual, XmNvalueChangedCallback, ConflictNameChanged, dialog);
    dialog->apply = ConflictButton(tools, "apply", GETMESSAGE(42, 24, "Apply action to compatible conflicts"), ConflictApply, dialog);
    if (dialog->apply == NULL) { DtfileConflictDialogFinish(dialog); return NULL; }
    label = XtVaCreateManagedWidget("pathNote", xmLabelWidgetClass, tools, NULL);
    if (!ConflictLabel(label, GETMESSAGE(42, 25, "Paths display unusual bytes as \\xHH.")))
    { DtfileConflictDialogFinish(dialog); return NULL; }
    dialog->feedback = XtVaCreateManagedWidget("feedback", xmLabelWidgetClass, form,
                                              XmNalignment, XmALIGNMENT_BEGINNING,
                                              XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, controls,
                                              XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_FORM,
                                              XmNbottomAttachment, XmATTACH_FORM, NULL);
    ConflictFeedback(dialog, "");
    if (!ConflictRefresh(dialog)) { DtfileConflictDialogFinish(dialog); return NULL; }
    XtManageChild(dialog->box);
    return dialog;
failed:
    if (reply_fd >= 0) { (void)DtfileConflictWriteCancel(reply_fd); (void)close(reply_fd); }
    DtfileConflictBatchFree(batch);
    return NULL;
}
