#include "BodySearchDialog.h"

#if DTMAIL_BODY_INDEX_ENABLED
#include "MailBodyIndex.h"
#include "MailSession.hh"
#include "MsgScrollingList.hh"
#include "RoamApp.h"
#include "RoamMenuWindow.h"

#include <Xm/Form.h>
#include <Xm/Label.h>
#include <Xm/List.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
#include <Xm/TextF.h>

#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <optional>
#include <thread>

namespace {
struct SearchDialog {
    explicit SearchDialog(RoamMenuWindow *window) : window(window) {}

    RoamMenuWindow *window;
    Widget form = nullptr;
    Widget field = nullptr;
    Widget search = nullptr;
    Widget open = nullptr;
    Widget list = nullptr;
    Widget status = nullptr;
    XtIntervalId timer = 0;
    XtIntervalId open_timer = 0;
    std::optional<DtMailBodyHit> opening;
    std::chrono::steady_clock::time_point open_deadline;
    std::shared_ptr<DtMailBodyIndex> index = std::make_shared<DtMailBodyIndex>();
    std::future<DtMailBodySearch> pending;
    std::vector<DtMailBodyHit> hits;

    void setStatus(const std::string &text)
    {
        XmString label = XmStringCreateLocalized(const_cast<char *>(text.c_str()));
        XtVaSetValues(status, XmNlabelString, label, nullptr);
        XmStringFree(label);
    }

    void cancelOpen()
    {
        if (open_timer)
            XtRemoveTimeOut(open_timer);
        open_timer = 0;
        opening.reset();
    }
};

void finish(XtPointer closure, XtIntervalId *)
{
    auto &dialog = *static_cast<SearchDialog *>(closure);
    dialog.timer = 0;
    if (dialog.pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        dialog.timer = XtAppAddTimeOut(XtWidgetToApplicationContext(dialog.form), 80, finish, closure);
        return;
    }

    DtMailBodySearch result;
    try {
        result = dialog.pending.get();
    } catch (...) {
        result.ok = false;
    }
    XtSetSensitive(dialog.search, True);
    if (!result.ok) {
        dialog.setStatus("Could not search the mail index.");
        return;
    }
    dialog.hits = std::move(result.hits);
    for (const DtMailBodyHit &hit : dialog.hits) {
        std::string label = (hit.subject.empty() ? "(No subject)" : hit.subject)
                           + "  [" + hit.path + "]";
        XmString item = XmStringCreateLocalized(const_cast<char *>(label.c_str()));
        XmListAddItemUnselected(dialog.list, item, 0);
        XmStringFree(item);
    }
    XtSetSensitive(dialog.open, !dialog.hits.empty());
    std::string status = std::to_string(dialog.hits.size()) + " matching messages";
    if (result.incomplete)
        status += " (partial results)";
    dialog.setStatus(status);
}

void start(Widget, XtPointer closure, XtPointer)
{
    auto &dialog = *static_cast<SearchDialog *>(closure);
    if (dialog.pending.valid())
        return;
    char *text = XmTextFieldGetString(dialog.field);
    std::string query = text ? text : "";
    XtFree(text);
    if (query.find_first_not_of(" \t") == std::string::npos) {
        dialog.setStatus("Enter a phrase to search for.");
        return;
    }

    std::vector<std::string> files, roots;
    if (const char *current = dialog.window->mailbox_fullpath())
        files.emplace_back(current);
    RoamMenuWindow *inbox = theRoamApp.inboxWindow();
    if (inbox && inbox->mailbox_fullpath())
        files.emplace_back(inbox->mailbox_fullpath());

    if (const char *home = getenv("HOME")) {
        roots.emplace_back(std::string(home) + "/.dt/mail");
        roots.emplace_back(std::string(home) + "/Mail");
    }
    DtMailEnv error;
    DtMail::Session *session = theRoamApp.session()->session();
    const char *configured = nullptr;
    session->mailRc(error)->getValue(error, "folder", &configured);
    if (configured && *configured) {
        char *expanded = session->expandPath(error, "+");
        if (expanded && *expanded && error.isNotSet())
            roots.emplace_back(expanded);
        free(expanded);
    }
    free(const_cast<char *>(configured));

    dialog.hits.clear();
    dialog.cancelOpen();
    XmListDeleteAllItems(dialog.list);
    XtSetSensitive(dialog.open, False);
    try {
        std::packaged_task<DtMailBodySearch()> task(
            [index = dialog.index, files = std::move(files), roots = std::move(roots),
             query = std::move(query)] {
                return index->search(files, roots, query);
            });
        auto completion = task.get_future();
        std::thread(std::move(task)).detach();
        dialog.pending = std::move(completion);
    } catch (...) {
        dialog.setStatus("Could not start mail search.");
        return;
    }
    dialog.setStatus("Searching mail bodies...");
    XtSetSensitive(dialog.search, False);
    dialog.timer = XtAppAddTimeOut(XtWidgetToApplicationContext(dialog.form), 80, finish, closure);
}

void select_message(XtPointer closure, XtIntervalId *)
{
    auto &dialog = *static_cast<SearchDialog *>(closure);
    dialog.open_timer = 0;
    if (!dialog.opening)
        return;
    const auto &hit = *dialog.opening;
    if (!hit.stillCurrent()) {
        dialog.cancelOpen();
        dialog.setStatus("This folder has changed. Search again to update the results.");
        return;
    }

    RoamMenuWindow *window = theRoamApp.session()->getRMW(hit.path.c_str());
    if (!window || !window->mailbox() || !window->list()) {
        // The new mailbox opens and loads in its MapNotify handler.
        if (std::chrono::steady_clock::now() < dialog.open_deadline) {
            dialog.open_timer = XtAppAddTimeOut(XtWidgetToApplicationContext(dialog.form),
                                                75, select_message, closure);
        } else {
            dialog.cancelOpen();
            dialog.setStatus("Could not open the message folder.");
        }
        return;
    }

    DtMailEnv error;
    char id_header[] = "Message-ID";
    char *headers[] = {id_header};
    const DtMailHeaderRequest request = {headers, 1};
    DtMailHeaderLine line = {};
    DtMail::MailBox *box = window->mailbox();
    DtMailMessageHandle handle = box->getFirstMessageSummary(error, request, line);
    for (size_t ordinal = 1; handle && ordinal < hit.ordinal; ++ordinal) {
        box->clearMessageSummary(line);
        handle = box->getNextMessageSummary(error, handle, request, line);
    }
    std::string id;
    if (handle && line.header_values && line.header_values[0].length()) {
        const char *raw_id = line.header_values[0][0]->raw();
        if (raw_id)
            id = raw_id;
    }
    size_t first = id.find_first_not_of(" \t\r\n");
    if (first != std::string::npos)
        id = id.substr(first, id.find_last_not_of(" \t\r\n") - first + 1);
    if (id.size() >= 2 && id.front() == '<' && id.back() == '>')
        id = id.substr(1, id.size() - 2);
    if (handle)
        box->clearMessageSummary(line);
    if (!handle || id != hit.message_id) {
        dialog.cancelOpen();
        dialog.setStatus("Could not match the message in this folder.");
        return;
    }
    dialog.cancelOpen();
    XmListDeselectAllItems(window->list()->get_scrolling_list());
    window->list()->display_and_select_message(error, handle);
    if (error.isNotSet())
        XtUnmanageChild(dialog.form);
}

void open_result(Widget, XtPointer closure, XtPointer)
{
    auto &dialog = *static_cast<SearchDialog *>(closure);
    int *positions = nullptr;
    int count = 0;
    if (!XmListGetSelectedPos(dialog.list, &positions, &count) || count == 0) {
        XtFree(reinterpret_cast<char *>(positions));
        return;
    }
    size_t offset = positions[0] - 1;
    XtFree(reinterpret_cast<char *>(positions));
    if (offset >= dialog.hits.size())
        return;
    dialog.cancelOpen();
    dialog.opening = dialog.hits[offset];
    if (!dialog.opening->stillCurrent()) {
        dialog.cancelOpen();
        dialog.setStatus("This folder has changed. Search again to update the results.");
        return;
    }
    std::string path = dialog.opening->path;
    dialog.open_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    dialog.window->view_mail_file(&path[0], DTM_FALSE);
    select_message(closure, nullptr);
}

void close(Widget, XtPointer closure, XtPointer)
{
    auto &dialog = *static_cast<SearchDialog *>(closure);
    dialog.cancelOpen();
    XtUnmanageChild(dialog.form);
}

void destroy(Widget, XtPointer closure, XtPointer)
{
    auto *dialog = static_cast<SearchDialog *>(closure);
    if (dialog->timer)
        XtRemoveTimeOut(dialog->timer);
    dialog->cancelOpen();
    delete dialog;
}
} // namespace

Widget dtmail_create_body_search(Widget parent, RoamMenuWindow *owner)
{
    auto *dialog = new SearchDialog(owner);
    Arg args[4];
    XmString title = XmStringCreateLocalized((char *)"Search Mail Bodies");
    XtSetArg(args[0], XmNdialogTitle, title);
    XtSetArg(args[1], XmNautoUnmanage, False);
    dialog->form = XmCreateFormDialog(parent, (char *)"mailBodySearch", args, 2);
    XtVaSetValues(XtParent(dialog->form), XmNdeleteResponse, XmUNMAP, NULL);
    XmStringFree(title);
    XtAddCallback(dialog->form, XmNdestroyCallback, destroy, dialog);

    Widget content = XtVaCreateManagedWidget("bodySearchContent", xmRowColumnWidgetClass,
        dialog->form, XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_FORM, NULL);
    XtVaCreateManagedWidget("Search phrase", xmLabelWidgetClass, content,
        XmNalignment, XmALIGNMENT_BEGINNING, NULL);
    dialog->field = XtVaCreateManagedWidget("phrase", xmTextFieldWidgetClass, content,
        XmNcolumns, 54, XmNmaxLength, 512, NULL);
    XtSetArg(args[0], XmNvisibleItemCount, 12);
    XtSetArg(args[1], XmNlistSizePolicy, XmCONSTANT);
    XtSetArg(args[2], XmNselectionPolicy, XmBROWSE_SELECT);
    dialog->list = XmCreateScrolledList(content, (char *)"matches", args, 3);
    XtManageChild(dialog->list);
    dialog->status = XtVaCreateManagedWidget("matchStatus", xmLabelWidgetClass, content,
        XmNalignment, XmALIGNMENT_BEGINNING, NULL);
    dialog->setStatus("");
    Widget controls = XtVaCreateManagedWidget("controls", xmRowColumnWidgetClass, content,
        XmNorientation, XmHORIZONTAL, NULL);
    dialog->search = XtVaCreateManagedWidget("Search", xmPushButtonWidgetClass, controls, NULL);
    dialog->open = XtVaCreateManagedWidget("Open Message", xmPushButtonWidgetClass, controls, NULL);
    Widget dismiss = XtVaCreateManagedWidget("Close", xmPushButtonWidgetClass, controls, NULL);
    XtSetSensitive(dialog->open, False);
    XtAddCallback(dialog->field, XmNactivateCallback, start, dialog);
    XtAddCallback(dialog->search, XmNactivateCallback, start, dialog);
    XtAddCallback(dialog->list, XmNdefaultActionCallback, open_result, dialog);
    XtAddCallback(dialog->open, XmNactivateCallback, open_result, dialog);
    XtAddCallback(dismiss, XmNactivateCallback, close, dialog);
    return dialog->form;
}

#else
Widget dtmail_create_body_search(Widget, RoamMenuWindow *) { return nullptr; }
#endif
