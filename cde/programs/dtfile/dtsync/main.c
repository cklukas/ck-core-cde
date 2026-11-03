/* SPDX-License-Identifier: LGPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include "SyncJob.h"
#include "SyncDates.h"
#include "SyncProfiles.h"
#include <Xm/Xm.h>
#include <Xm/Form.h>
#include <Xm/RowColumn.h>
#include <Xm/Label.h>
#include <Xm/PushB.h>
#include <Xm/ToggleB.h>
#include <Xm/TextF.h>
#include <Xm/Text.h>
#include <Xm/List.h>
#include <Xm/Notebook.h>
#include <Xm/ScrolledW.h>
#include <Xm/FileSB.h>
#include <Xm/MessageB.h>
#include <Xm/Protocols.h>
#include <nl_types.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

static nl_catd catalog=(nl_catd)-1;
#define M(id,text) catgets(catalog,1,id,text)
/* Enable only after native execution and consumer fault gates qualify the pin. */
#ifndef DTFILE_SYNC_MUTATION_QUALIFIED
#define DTFILE_SYNC_MUTATION_QUALIFIED 0
#endif
typedef struct {
    XtAppContext context;
    Widget shell, notebook, setup, source, destination, list, details, status, rules_list;
    Widget compare, review, execute, pause, cancel, previous, next, remove_rule;
    Widget new_name, zero, retain, size_min, size_max, date_after, date_before;
    Widget choice_menu[23], class_menu[15];
    unsigned profile_index, choice, scope, kind, fallback, metadata, name_side;
    SyncJob *job;
    SyncView *view;
    SyncRule rules[SYNC_MAX_RULES];
    unsigned rule_count;
    uint64_t revision, rendered_sequence, rendered_offset;
    unsigned have_rendered, closing, recompare, pause_requested, cancel_requested, dirty, setup_dirty, review_pending, execute_pending;
    uint64_t previous_receipt;
    uint64_t review_serial, execute_serial;
    uint64_t local_rules_revision, reviewed_rules_revision;
    uint64_t rename_row;
    unsigned rename_selection_count;
    Widget rule_page;
    char last_error[512], command_notice[512];
    uint64_t command_notice_generation;
    uint64_t feedback_selection[SYNC_PAGE_SIZE];
    unsigned feedback_selection_count;
    uint64_t displayed_ids[SYNC_PAGE_SIZE];
    unsigned displayed_count;
    unsigned rendered_names_valid;
    SyncProfiles *preferences;
    SyncProfilesSnapshot preferences_view;
    Widget profile_list, profile_name, profile_status, profile_load, profile_save, profile_delete;
    Widget operation_option, metadata_option, operation_explanation, setup_notice, setup_viewport;
    Widget class_option, name_side_option;
    Widget compared_roots;
    Widget page_info;
    Widget plan_summary;
    char *native_source, *native_destination;
    unsigned setting_paths;
    XmFontList mono;
    XFontStruct *font;
} SyncWindow;
static const uint32_t profiles[]={CKFSP_COMPARE,CKFSP_COPY,CKFSP_UPDATE,CKFSP_MISSING_ONLY,CKFSP_SYNC,CKFSP_MIRROR};
static unsigned selected_rows(SyncWindow *,uint64_t [SYNC_PAGE_SIZE],int *);
static void render_rules(SyncWindow *);
static const char *choice_label(unsigned n)
{
    static const char *const names[]={"Ask / defer","Use source / replace target","Keep target / skip existing",
        "Use newest","Strict source-newer","Source-newer or size-different at equal time","Replace if size differs",
        "Prefer larger","Prefer smaller","Keep both: rename incoming","Keep both: rename existing","Rename incoming manually",
        "Compare content","Merge folders","Retry matching failures","Cancel","Skip matching",
        "Keep Left","Keep Right","Keep both","Restore deleted version","Keep edited version","Delete edited version"};
    return n<23 ? catgets(catalog,1,100+(int)n,names[n]) : M(1,"Unknown");
}
static const char *kind_label(unsigned n)
{
    static const char *const names[]={"Existing regular files","Both edited","Deletion versus edit","Folder deletion versus edited child",
        "Different object types","Different link types","Name collision","Required metadata differs","Unavailable object",
        "Stale object","Ambiguous publication","Existing folders","Hard-link effects","Access failure","Space or quota failure"};
    return n<15 ? catgets(catalog,1,130+(int)n,names[n]) : M(1,"Unknown");
}
static const char *state_label(unsigned n)
{
    static const char *const names[]={"Scanning folders...","Review proposed changes","Revalidating reviewed objects...",
        "Applying reviewed changes...","Completed","Failed","Cancelled","Partially completed","Recovery required"};
    return n<9 ? catgets(catalog,1,150+(int)n,names[n]) : M(1,"Unknown");
}
static const char *action_label(unsigned n)
{
    static const char *const names[]={"Review","Skip","Copy","Create folder","Merge folder","Replace","Remove file","Remove folder"};
    return n<8 ? catgets(catalog,1,170+(int)n,names[n]) : M(1,"Unknown");
}
static const char *item_state_label(unsigned n)
{
    static const char *const names[]={"Pending","Skipped","Completed","Failed","Cancelled","Recovery required"};
    return n<6 ? catgets(catalog,1,237+(int)n,names[n]) : M(1,"Unknown");
}
static const char *reason_label(unsigned n)
{
    static const char *const names[]={"Unknown object","Incomplete inventory","Identity unavailable","Size unavailable",
        "Timestamp unavailable","Timestamp precision unavailable","Digest unavailable","Digest incomplete","Content changed during read",
        "Digest identity mismatch","Source absent","Destination absent","Same size; content not verified",
        "Same size and time; content not verified","Verified same content","Source newer","Destination newer",
        "Different size","Different content","Different type","Unsupported object type","Folder pair","Excluded",
        "Filtered by policy","Filter cannot be evaluated","Protected descendant","Name collision","Existing destination",
        "Equal-time conflict","Ancestor conflict","Same object"};
    return n<sizeof(names)/sizeof(names[0]) ? catgets(catalog,1,190+(int)n,names[n]) : M(2,"See backend assessment");
}
static void label(Widget widget, const char *text)
{
    XmString value=XmStringCreateLocalized((char *)text);
    XtVaSetValues(widget,XmNlabelString,value,NULL); XmStringFree(value);
}
static void notify(SyncWindow *w, const char *text) { label(w->status,text); }
/* Local feedback may replace ordinary idle status, not an in-flight command,
 * cancellation, close, failed/recovery view or unfinished native cleanup. */
static int command_notice_ready(const SyncWindow *w)
{
    if (w->closing) return 0;
    if (!w->job) return 1;
    const SyncView *v=w->view;
    if (w->review_pending || w->execute_pending || v->settling || v->snapshot.recovery_required) return 0;
    if (v->finished || v->stopped) {
        return (v->snapshot.state==CKFET_COMPLETE || v->snapshot.state==CKFET_CANCELLED) &&
            (v->drained || v->stopped) && !v->error[0];
    }
    return !w->cancel_requested && v->ready && v->snapshot.state==CKFET_AWAITING_REVIEW &&
        v->snapshot.command_state!=CKFET_COMMAND_PENDING && SyncJobViewCoherent(v) &&
        v->summary_valid && v->summary_generation==v->snapshot.plan_generation &&
        v->summary_receipt==v->snapshot.receipt_id;
}
static void clear_command_notice(SyncWindow *w)
{
    int visible=w->command_notice[0]!=0;
    w->command_notice[0]=0; w->command_notice_generation=0;
    if (visible && !w->job && !w->closing && w->status)
        notify(w,M(78,"Choose folders and an operation, then compare. Review matching rules before applying changes."));
}
static void command_notice(SyncWindow *w,const char *text)
{
    snprintf(w->command_notice,sizeof(w->command_notice),"%s",text);
    w->command_notice_generation=w->view->snapshot.plan_generation;
    int first;
    w->feedback_selection_count=w->list?selected_rows(w,w->feedback_selection,&first):0;
    if (command_notice_ready(w)) notify(w,w->command_notice);
}
static void progress_notice(SyncWindow *w,const char *message)
{
    char text[1024];
    const char *recovery=w->job && w->view->snapshot.recovery_required?M(242,"Recovery required"):"";
    snprintf(text,sizeof(text),"%s%s%s",message,*recovery?"\n":"",recovery); notify(w,text);
}
static void rule_input_changed(Widget widget,XtPointer data,XtPointer call)
{
    (void)widget; (void)call; clear_command_notice((SyncWindow *)data);
}
static void setup_changed(Widget widget,XtPointer data,XtPointer call)
{
    (void)widget;(void)call; SyncWindow *w=data; clear_command_notice(w); w->setup_dirty=1;
    if (w->execute) XtSetSensitive(w->execute,False);
    if (w->review) XtSetSensitive(w->review,False);
    if (w->rule_page) XtSetSensitive(w->rule_page,False);
    if (w->job && w->view->snapshot.plan_generation) {
        const char *message=M(296,"These results use earlier folder options.\nCompare again before reviewing rules.");
        if (w->setup_notice) label(w->setup_notice,message);
        if (command_notice_ready(w)) notify(w,M(261,"Folder options changed. Compare again before reviewing or applying changes."));
    }
}
static void operation_explanation(SyncWindow *w)
{
    const char *text;
    switch (w->profile_index) {
    case 0: text=M(288,"Compare only: inspect differences in file contents and metadata.\nNo file changes are proposed or applied. This is not two-way synchronization."); break;
    case 1: text=M(289,"Copy preview: propose adding missing source objects to the destination.\nExisting destination objects need conflict-policy review.\nNewer targets are not implicitly protected. Preview only; Apply is unavailable.\nThis is not two-way synchronization."); break;
    case 2: text=M(290,"Update preview: add missing objects and consider source-newer files.\nRounded timestamps determine order; destination-newer files are skipped.\nEqual-time differences need review. Preview only; Apply is unavailable.\nThis is not two-way synchronization."); break;
    case 3: text=M(291,"Copy missing only preview: propose adding objects absent from the destination.\nExisting destination objects are kept; inspect conflicts and unavailable assessments.\nPreview only: Apply is unavailable. This is not two-way synchronization."); break;
    case 4: text=M(292,"Synchronize one way preview: make included destination files match the source.\nMay propose replacing destination-newer files; destination-only objects remain.\nPreview only: Apply is unavailable. This is not two-way synchronization."); break;
    case 5: text=M(293,"Mirror preview: also propose removing destination-only objects in scope.\nWarning: the plan can contain deletions, which require separate review.\nPreview only: mirror execution and two-way synchronization are unavailable."); break;
    default: text=M(1,"Unknown"); break;
    }
    if (w->operation_explanation) label(w->operation_explanation,text);
}
static void operation_changed(Widget widget,XtPointer data,XtPointer call)
{
    SyncWindow *w=data; operation_explanation(w); setup_changed(widget,data,call);
}
static void swap_folders(Widget widget,XtPointer data,XtPointer call)
{
    (void)widget;(void)call; SyncWindow *w=data;
    char *source=XmTextFieldGetString(w->source),*destination=XmTextFieldGetString(w->destination);
    if (!source || !destination) {
        XtFree(source); XtFree(destination); command_notice(w,M(223,"Not enough memory.")); return;
    }
    /* Both display copies exist before either field changes. An edited field
     * has no retained native pointer: move that fact with its exact text too.
     * Never parse display escapes into a native filename. */
    w->setting_paths=1;
    char *native=w->native_source; w->native_source=w->native_destination; w->native_destination=native;
    XmTextFieldSetString(w->source,destination); XmTextFieldSetString(w->destination,source);
    w->setting_paths=0; XtFree(source); XtFree(destination); setup_changed(NULL,w,NULL);
}
static void clear_filters(Widget widget,XtPointer data,XtPointer call)
{
    (void)widget;(void)call; SyncWindow *w=data;
    XmTextFieldSetString(w->size_min,""); XmTextFieldSetString(w->size_max,"");
    XmTextFieldSetString(w->date_after,""); XmTextFieldSetString(w->date_before,"");
    setup_changed(NULL,w,NULL);
}
typedef struct { char *native, *display; } PreparedPath;
static void clear_prepared_path(PreparedPath *path)
{
    free(path->native); free(path->display); path->native=path->display=NULL;
}
static int prepare_native_path(SyncWindow *w,const char *path,PreparedPath *prepared)
{
    size_t n=strnlen(path,SYNC_PROFILE_PATH_MAX+1);
    if (n>SYNC_PROFILE_PATH_MAX) { command_notice(w,M(222,"Folder path is too long.")); return 0; }
    prepared->native=strdup(path); prepared->display=malloc(n*4+1);
    if (!prepared->native || !prepared->display) {
        clear_prepared_path(prepared); command_notice(w,M(223,"Not enough memory.")); return 0;
    }
    size_t j=0; static const char hex[]="0123456789abcdef";
    for (size_t i=0;i<n;++i) {
        unsigned char byte=(unsigned char)path[i];
        if (byte>=32 && byte<127 && byte!='\\') prepared->display[j++]=(char)byte;
        else { prepared->display[j++]='\\'; prepared->display[j++]='x'; prepared->display[j++]=hex[byte>>4]; prepared->display[j++]=hex[byte&15]; }
    }
    prepared->display[j]=0; return 1;
}
/* Commit consumes the prepared native allocation. Path callbacks stay silent;
 * the caller invalidates setup once the complete UI operation has committed. */
static void commit_native_path(SyncWindow *w,Widget field,PreparedPath *prepared)
{
    char **owned=field==w->source?&w->native_source:&w->native_destination;
    free(*owned); *owned=prepared->native; prepared->native=NULL;
    unsigned setting=w->setting_paths; w->setting_paths=1;
    XmTextFieldSetString(field,prepared->display); w->setting_paths=setting;
}
static int set_native_path(SyncWindow *w,Widget field,const char *path)
{
    PreparedPath prepared={0};
    if (!prepare_native_path(w,path,&prepared)) return 0;
    commit_native_path(w,field,&prepared); clear_prepared_path(&prepared);
    setup_changed(field,w,NULL); return 1;
}
static void path_edited(Widget field,XtPointer data,XtPointer call)
{
    (void)call; SyncWindow *w=data; if (w->setting_paths) return;
    char **owned=field==w->source?&w->native_source:&w->native_destination;
    free(*owned); *owned=NULL;
    setup_changed(field,w,NULL);
}
static void mark_dirty(SyncWindow *w)
{
    clear_command_notice(w); w->dirty=1;
    ++w->local_rules_revision;
    if (w->execute) XtSetSensitive(w->execute,False);
}
static void name_changed(Widget widget,XtPointer data,XtPointer call)
{
    (void)widget;(void)call; SyncWindow *w=data; uint64_t ids[SYNC_PAGE_SIZE]; int first;
    w->rename_selection_count=selected_rows(w,ids,&first);
    w->rename_row=w->rename_selection_count==1?ids[0]:0;
    mark_dirty(w); if (w->rules_list) render_rules(w);
}
static Widget button(Widget parent,const char *name,const char *text,XtCallbackProc callback,SyncWindow *w)
{
    Widget b=XtVaCreateManagedWidget(name,xmPushButtonWidgetClass,parent,NULL);
    label(b,text); if (callback) XtAddCallback(b,XmNactivateCallback,callback,w); return b;
}
static Widget field(Widget parent,const char *name,const char *title,const char *initial,int columns)
{
    Widget row=XtVaCreateManagedWidget(name,xmRowColumnWidgetClass,parent,XmNorientation,XmHORIZONTAL,NULL);
    Widget l=XtVaCreateManagedWidget("label",xmLabelWidgetClass,row,NULL); label(l,title);
    return XtVaCreateManagedWidget(name,xmTextFieldWidgetClass,row,XmNcolumns,columns,XmNvalue,initial,NULL);
}
typedef struct { SyncWindow *window; Widget target; } FolderChoice;
static void folder_choice_destroyed(Widget dialog,XtPointer data,XtPointer call)
{ (void)dialog;(void)call; free(data); }
static void folder_chosen(Widget dialog,XtPointer data,XtPointer call)
{
    FolderChoice *choice=data;
    XmFileSelectionBoxCallbackStruct *selection=call; char *path=NULL;
    if (XmStringGetLtoR(selection->value,XmFONTLIST_DEFAULT_TAG,&path) && path) {
        int accepted=set_native_path(choice->window,choice->target,path); XtFree(path);
        if (!accepted) return;
    } else {
        command_notice(choice->window,M(282,"Cannot read the selected folder name.")); return;
    }
    XtDestroyWidget(XtParent(dialog));
}
static void folder_cancelled(Widget dialog,XtPointer data,XtPointer call)
{ (void)data;(void)call; XtDestroyWidget(XtParent(dialog)); }
static void choose_folder(Widget button,XtPointer data,XtPointer call)
{
    (void)call; SyncWindow *w=data; Widget target=NULL;
    XtVaGetValues(button,XmNuserData,&target,NULL);
    FolderChoice *choice=malloc(sizeof(*choice)); if (!choice) { command_notice(w,M(223,"Not enough memory.")); return; }
    choice->window=w; choice->target=target;
    Widget dialog=XmCreateFileSelectionDialog(w->shell,"chooseFolder",NULL,0);
    XmString title=XmStringCreateLocalized((char *)M(96,"Choose a folder"));
    XtVaSetValues(dialog,XmNdialogTitle,title,XmNfileTypeMask,XmFILE_DIRECTORY,XmNautoUnmanage,False,
        XmNdialogStyle,XmDIALOG_FULL_APPLICATION_MODAL,NULL);
    XmStringFree(title);
    XtAddCallback(dialog,XmNokCallback,folder_chosen,choice);
    XtAddCallback(dialog,XmNdestroyCallback,folder_choice_destroyed,choice);
    XtAddCallback(dialog,XmNcancelCallback,folder_cancelled,NULL);
    XtUnmanageChild(XmFileSelectionBoxGetChild(dialog,XmDIALOG_HELP_BUTTON));
    XtManageChild(dialog);
}
static Widget folder_field(SyncWindow *w,const char *name,const char *title,const char *initial)
{
    Widget text=field(w->setup,name,title,initial,45);
    Widget browse=button(XtParent(text),"browse",M(97,"Browse..."),choose_folder,w);
    XtVaSetValues(browse,XmNuserData,text,NULL); return text;
}
static void option_callback(Widget widget,XtPointer data,XtPointer call)
{
    (void)call; unsigned *value=data; XtPointer index=NULL;
    XtVaGetValues(widget,XmNuserData,&index,NULL); *value=(unsigned)(uintptr_t)index;
}
static Widget option(Widget parent,const char *name,const char *title,const char *const *names,
                      unsigned n,unsigned *selected,Widget *items)
{
    Widget pulldown=XmCreatePulldownMenu(parent,(char *)name,NULL,0);
    for (unsigned i=0;i<n;++i) {
        Widget item=XtVaCreateManagedWidget(names[i],xmPushButtonWidgetClass,pulldown,XmNuserData,(XtPointer)(uintptr_t)i,NULL);
        label(item,names[i]); XtAddCallback(item,XmNactivateCallback,option_callback,selected);
        if (items) items[i]=item;
    }
    XmString text=XmStringCreateLocalized((char *)title);
    Widget menu=XtVaCreateManagedWidget(name,xmRowColumnWidgetClass,parent,XmNrowColumnType,XmMENU_OPTION,
        XmNsubMenuId,pulldown,XmNlabelString,text,NULL);
    XmStringFree(text); return menu;
}
static void option_selected(Widget menu,unsigned index)
{
    Widget pane=NULL; WidgetList children=NULL; Cardinal count=0;
    XtVaGetValues(menu,XmNsubMenuId,&pane,NULL);
    XtVaGetValues(pane,XmNchildren,&children,XmNnumChildren,&count,NULL);
    if (index<count) XtVaSetValues(menu,XmNmenuHistory,children[index],NULL);
}
static void observe_setup_option(Widget menu,SyncWindow *w)
{
    Widget pane=NULL; WidgetList children=NULL; Cardinal count=0;
    XtVaGetValues(menu,XmNsubMenuId,&pane,NULL); XtVaGetValues(pane,XmNchildren,&children,XmNnumChildren,&count,NULL);
    for (Cardinal i=0;i<count;++i)
        XtAddCallback(children[i],XmNactivateCallback,menu==w->operation_option?operation_changed:setup_changed,w);
}
static unsigned selected_rows(SyncWindow *w,uint64_t ids[SYNC_PAGE_SIZE],int *first)
{
    int *positions=NULL,count=0; *first=-1;
    if (!XmListGetSelectedPos(w->list,&positions,&count)) return 0;
    unsigned n=0;
    for (int i=0;i<count && n<SYNC_PAGE_SIZE;++i) {
        int row=positions[i]-1;
        if (row>=0 && (unsigned)row<w->displayed_count) {
            uint64_t id=w->displayed_ids[row]; ids[n++]=id;
            for (unsigned j=0;j<w->view->row_count && *first<0;++j)
                if (w->view->rows[j].observed.row_id==id) *first=(int)j;
        }
    }
    XtFree((char *)positions); return n;
}
static void observed_text(const ckfet_observed_v1 *o,char *out,size_t cap)
{
    if (o->presence==CKFET_PRESENCE_ABSENT) { snprintf(out,cap,"%s",M(3,"Absent")); return; }
    if (o->presence!=CKFET_PRESENCE_PRESENT) { snprintf(out,cap,"%s",M(1,"Unknown")); return; }
    char size[48],date[64],precision[64];
    if (o->known_fields&CKFSP_FIELD_SIZE) snprintf(size,sizeof(size),"%" PRIu64,o->size);
    else snprintf(size,sizeof(size),"%s",M(1,"Unknown"));
    if (o->known_fields&CKFSP_FIELD_MODIFIED) {
        time_t seconds=(time_t)o->modified.seconds; struct tm tm;
        if ((int64_t)seconds==o->modified.seconds && gmtime_r(&seconds,&tm)) {
            char base[32]; strftime(base,sizeof(base),"%Y-%m-%d %H:%M:%S",&tm);
            snprintf(date,sizeof(date),"%s.%09u UTC",base,o->modified.nanoseconds);
        } else snprintf(date,sizeof(date),"%" PRId64 ".%09u UTC",o->modified.seconds,o->modified.nanoseconds);
    } else snprintf(date,sizeof(date),"%s",M(1,"Unknown"));
    const char *metadata=o->metadata_assessment==CKFE_OBSERVATION_AVAILABLE?M(91,"qualified"):
        o->metadata_assessment==CKFE_OBSERVATION_UNSUPPORTED?M(92,"unsupported"):M(93,"not yet qualified");
    if (o->timestamp_quantum_ns) snprintf(precision,sizeof(precision),"%s %" PRIu64 " ns",M(94,"precision"),o->timestamp_quantum_ns);
    else snprintf(precision,sizeof(precision),"%s",M(262,"precision unavailable"));
    static const char *const kinds[]={"File","Folder","Symbolic link","Junction","Reparse point","FIFO","Socket","Character device","Block device","Other object"};
    const char *kind=(o->known_fields&CKFSP_FIELD_KIND) && o->kind<10 ? catgets(catalog,1,263+(int)o->kind,kinds[o->kind]) : M(1,"Unknown");
    snprintf(out,cap,"%s; %s %s; %s; %s; %s %s",kind,size,M(4,"bytes"),date,precision,M(95,"metadata"),metadata);
}
static void detail_line(char *text,size_t capacity,const char *title,const char *value)
{
    size_t used=strlen(text);
    if (used<capacity) snprintf(text+used,capacity-used,"%s: %s\n",title,value);
}
static void selection_changed(Widget widget,XtPointer data,XtPointer call)
{
    (void)widget; SyncWindow *w=data; uint64_t ids[SYNC_PAGE_SIZE]; int first;
    unsigned n=selected_rows(w,ids,&first);
    uint64_t rename_row=n==1?ids[0]:0;
    int rename_selection_changed=n!=w->rename_selection_count || rename_row!=w->rename_row;
    w->rename_selection_count=n; w->rename_row=rename_row;
    int feedback_changed=n!=w->feedback_selection_count;
    for (unsigned i=0;i<n && !feedback_changed;++i) {
        unsigned j=0;
        while (j<w->feedback_selection_count && ids[i]!=w->feedback_selection[j]) ++j;
        if (j==w->feedback_selection_count) feedback_changed=1;
    }
    memcpy(w->feedback_selection,ids,n*sizeof(*ids)); w->feedback_selection_count=n;
    if (feedback_changed) clear_command_notice(w);
    if (rename_selection_changed && w->new_name && XmTextFieldGetLastPosition(w->new_name)>0) {
        mark_dirty(w); if (w->rules_list) render_rules(w);
    }
    if (call && first>=0 && w->view->rows[first].observed.conflict_class<15) {
        w->kind=w->view->rows[first].observed.conflict_class; option_selected(w->class_option,w->kind);
    }
    uint64_t allowed=w->kind<15?w->view->class_choices[w->kind]:0;
    if (w->scope==1 || w->scope==2) {
        allowed=0; unsigned matched=0;
        for (unsigned i=0;i<w->view->row_count;++i) {
            const ckfet_row_v1 *r=&w->view->rows[i].observed;
            if (r->conflict_class!=w->kind) continue;
            for (unsigned j=0;j<n;++j) if (ids[j]==r->row_id) {
                allowed=matched?allowed&r->allowed_choices:r->allowed_choices; ++matched;
            }
        }
    }
    for (unsigned i=0;i<15;++i) XtSetSensitive(w->class_menu[i],w->view->class_rows[i]!=0);
    for (unsigned i=0;i<23;++i) XtSetSensitive(w->choice_menu[i],(allowed&(UINT64_C(1)<<i))!=0);
    if (first<0) { XmTextSetString(w->details,(char *)M(5,"Select a row to inspect both versions, proposed names and recovery information.")); return; }
    SyncRow *row=&w->view->rows[first]; const ckfet_row_v1 *r=&row->observed;
    char source[256],target[256],text[16384],journal_id[33],retention_id[33];
    for (unsigned i=0;i<16;++i) { snprintf(journal_id+i*2,3,"%02x",r->journal_id[i]); snprintf(retention_id+i*2,3,"%02x",r->retention_journal_id[i]); }
    observed_text(&r->source,source,sizeof(source)); observed_text(&r->destination,target,sizeof(target));
    snprintf(text,sizeof(text),"%s\n",row->path);
    detail_line(text,sizeof(text),M(6,"Source"),source);
    detail_line(text,sizeof(text),M(7,"Destination"),target);
    detail_line(text,sizeof(text),M(8,"Proposed action"),action_label(r->action));
    detail_line(text,sizeof(text),M(9,"Reason"),reason_label(r->reason));
    if (row->proposed[0] || (row->name_failures&(1u<<CKFET_NATIVE_PROPOSED_NAME)))
        detail_line(text,sizeof(text),M(10,"Proposed name"),row->proposed[0]?row->proposed:M(276,"Unavailable"));
    if (row->retained[0] || (row->name_failures&(1u<<CKFET_NATIVE_RETAINED_NAME)))
        detail_line(text,sizeof(text),M(12,"Retained name"),row->retained[0]?row->retained:M(276,"Unavailable"));
    if (r->state) detail_line(text,sizeof(text),M(243,"Item result"),item_state_label(r->state));
    if (r->failure_domain || r->failure_code) {
        char failure[128]; snprintf(failure,sizeof(failure),"%u/%u/%lld",r->failure_state,r->failure_domain,(long long)r->failure_code);
        detail_line(text,sizeof(text),M(13,"Failure state/domain/code"),failure);
    }
    if (r->recovery_required) detail_line(text,sizeof(text),M(242,"Recovery required"),M(248,"Yes"));
    if (r->has_journal || r->has_retention_journal || row->journal_name[0] || row->staging_name[0] || r->recovery_required) {
        detail_line(text,sizeof(text),M(14,"Journal directory"),w->view->journal);
        if (r->has_journal) detail_line(text,sizeof(text),M(246,"Operation journal identifier"),journal_id);
        if (r->has_retention_journal) detail_line(text,sizeof(text),M(247,"Retained-target journal identifier"),retention_id);
    }
    if (row->journal_name[0] || (row->name_failures&(1u<<CKFET_NATIVE_JOURNAL_NAME)))
        detail_line(text,sizeof(text),M(244,"Journal name"),row->journal_name[0]?row->journal_name:M(276,"Unavailable"));
    if (row->staging_name[0] || (row->name_failures&(1u<<CKFET_NATIVE_STAGING_NAME)))
        detail_line(text,sizeof(text),M(245,"Staging name"),row->staging_name[0]?row->staging_name:M(276,"Unavailable"));
    XmTextSetString(w->details,text);
}
static void choice_context_changed(Widget widget,XtPointer data,XtPointer call)
{ (void)call; clear_command_notice((SyncWindow *)data); selection_changed(widget,data,NULL); }
static void observe_choice_option(Widget menu,SyncWindow *w)
{
    Widget pane=NULL; WidgetList children=NULL; Cardinal count=0;
    XtVaGetValues(menu,XmNsubMenuId,&pane,NULL); XtVaGetValues(pane,XmNchildren,&children,XmNnumChildren,&count,NULL);
    for (Cardinal i=0;i<count;++i) XtAddCallback(children[i],XmNactivateCallback,choice_context_changed,w);
}
static void render_rows(SyncWindow *w)
{
    uint64_t selected[SYNC_PAGE_SIZE]; int first; unsigned n=selected_rows(w,selected,&first);
    XmListDeleteAllItems(w->list);
    w->displayed_count=w->view->row_count;
    for (unsigned i=0;i<w->view->row_count;++i) {
        const SyncRow *r=&w->view->rows[i]; char text[1024];
        w->displayed_ids[i]=r->observed.row_id;
        snprintf(text,sizeof(text),"%-32.32s  %-15s  %-11s  %s",r->path,
            action_label(r->observed.action),item_state_label(r->observed.state),
            reason_label(r->observed.reason));
        XmString item=XmStringCreateLocalized(text); XmListAddItemUnselected(w->list,item,0); XmStringFree(item);
        for (unsigned j=0;j<n;++j) if (selected[j]==r->observed.row_id) XmListSelectPos(w->list,(int)i+1,False);
    }
    selection_changed(NULL,w,NULL);
    char range[128]; snprintf(range,sizeof(range),M(259,"Objects %llu-%llu of %llu"),
        (unsigned long long)(w->view->row_count?w->view->offset+1:0),
        (unsigned long long)(w->view->offset+w->view->row_count),(unsigned long long)w->view->total_rows);
    label(w->page_info,range);
    char summary[512];
    if (w->view->summary_valid) snprintf(summary,sizeof(summary),M(274,"Plan: %llu copies, %llu new folders, %llu replacements, %llu removals\n%llu skipped/protected, %llu to review/report; %llu bytes%s"),
        (unsigned long long)w->view->planned_copies,(unsigned long long)w->view->planned_folders,
        (unsigned long long)w->view->planned_replacements,(unsigned long long)w->view->planned_removals,
        (unsigned long long)w->view->planned_skips,(unsigned long long)w->view->planned_reports,
        (unsigned long long)w->view->planned_bytes,w->view->planned_bytes_known?"":M(41," (totals incomplete)"));
    else snprintf(summary,sizeof(summary),"%s",M(275,"Plan totals are not yet available."));
    label(w->plan_summary,summary);
}
static void render_rules(SyncWindow *w)
{
    XmListDeleteAllItems(w->rules_list);
    for (unsigned i=0;i<w->rule_count;++i) {
        char text[512]; const ckfet_rule_counts_v1 *c=&w->view->counts[i];
        const char *scope=w->rules[i].scope==CKFET_SCOPE_JOB?M(19,"job"):
            w->rules[i].scope==CKFET_SCOPE_ITEM?M(83,"item"):
            w->rules[i].scope==CKFET_SCOPE_SUBTREE?M(84,"subtree"):M(20,"selected rows");
        if (w->dirty) snprintf(text,sizeof(text),"%u. %s: %s [%s] - %s",i+1,
            kind_label(w->rules[i].kind),choice_label(w->rules[i].choice),scope,M(221,"Not reviewed"));
        else snprintf(text,sizeof(text),"%u. %s: %s [%s] - %" PRIu64 " %s, %" PRIu64 " %s, %" PRIu64 " %s",
            i+1,kind_label(w->rules[i].kind),choice_label(w->rules[i].choice),
            scope,
            c->matched_rows,M(21,"matched"),c->applied_rows,M(22,"resolved"),c->unresolved_rows,M(23,"unresolved"));
        XmString item=XmStringCreateLocalized(text); XmListAddItemUnselected(w->rules_list,item,0); XmStringFree(item);
    }
}
static int numeric(Widget field,uint64_t *out,int *present)
{
    char *text=XmTextFieldGetString(field); *present=0; *out=0;
    if (!text) return -1;
    *present=*text!=0; uint64_t value=0;
    for (const unsigned char *p=(const unsigned char *)text;*p;++p) {
        if (*p<'0' || *p>'9' || value>(UINT64_MAX-(uint64_t)(*p-'0'))/10) { XtFree(text); return 0; }
        value=value*10+(uint64_t)(*p-'0');
    }
    *out=value; XtFree(text); return 1;
}
static int date_bound(Widget field,ckfsp_timestamp *out,int *present)
{
    char *text=XmTextFieldGetString(field);
    if (!text) return -1;
    int valid=SyncParseDate(text,out,present);
    XtFree(text); return valid;
}
static int setup_error(SyncWindow *w,Widget field,const char *message)
{
    command_notice(w,message); XtVaSetValues(w->notebook,XmNcurrentPageNumber,1,NULL);
    XmScrollVisible(w->setup_viewport,field,8,8);
    XmProcessTraversal(field,XmTRAVERSE_CURRENT);
    XmTextFieldSetInsertionPosition(field,0);
    XmTextFieldSetSelection(field,0,XmTextFieldGetLastPosition(field),CurrentTime);
    return 0;
}
static int setup_fields(SyncWindow *w,SyncSetup *setup,char **source,char **destination)
{
    *source=NULL; *destination=NULL; memset(setup,0,sizeof(*setup));
    *source=w->native_source?XtNewString(w->native_source):XmTextFieldGetString(w->source);
    if (!*source) return setup_error(w,w->source,M(223,"Not enough memory."));
    if (!**source) return setup_error(w,w->source,M(297,"Choose a source folder."));
    if (strnlen(*source,SYNC_PROFILE_PATH_MAX+1)>SYNC_PROFILE_PATH_MAX)
        return setup_error(w,w->source,M(301,"The source folder path is too long."));
    if (**source!='/') return setup_error(w,w->source,M(299,"Enter an absolute source folder path, starting with /."));
    *destination=w->native_destination?XtNewString(w->native_destination):XmTextFieldGetString(w->destination);
    if (!*destination) return setup_error(w,w->destination,M(223,"Not enough memory."));
    if (!**destination) return setup_error(w,w->destination,M(298,"Choose a destination folder."));
    if (strnlen(*destination,SYNC_PROFILE_PATH_MAX+1)>SYNC_PROFILE_PATH_MAX)
        return setup_error(w,w->destination,M(302,"The destination folder path is too long."));
    if (**destination!='/') return setup_error(w,w->destination,M(300,"Enter an absolute destination folder path, starting with /."));
    setup->source=*source; setup->destination=*destination;
    setup->profile=profiles[w->profile_index]; setup->metadata_scope=w->metadata;
    setup->filters.allowed_kind_mask=CKFSP_ALL_KINDS; setup->filters.maximum_depth=1024;
    int min=0,max=0,after=0,before=0;
    int result=numeric(w->size_min,&setup->filters.minimum_size,&min);
    if (result!=1) return setup_error(w,w->size_min,result<0?M(223,"Not enough memory."):
        M(303,"Minimum bytes must be empty or digits from 0 through 18446744073709551615.\nSigns and spaces are not allowed."));
    result=numeric(w->size_max,&setup->filters.maximum_size,&max);
    if (result!=1) return setup_error(w,w->size_max,result<0?M(223,"Not enough memory."):
        M(304,"Maximum bytes must be empty or digits from 0 through 18446744073709551615.\nSigns and spaces are not allowed."));
    if (min && max && setup->filters.minimum_size>setup->filters.maximum_size)
        return setup_error(w,w->size_max,M(305,"Maximum bytes must be at least minimum bytes.\nBoth bounds are inclusive for regular files."));
    result=date_bound(w->date_after,&setup->filters.modified_not_before,&after);
    if (result!=1) return setup_error(w,w->date_after,result<0?M(223,"Not enough memory."):
        M(306,"Modified on/after: enter a valid local YYYY-MM-DD date or ISO UTC timestamp,\nor leave empty. This bound is inclusive for regular files."));
    result=date_bound(w->date_before,&setup->filters.modified_before,&before);
    if (result!=1) return setup_error(w,w->date_before,result<0?M(223,"Not enough memory."):
        M(307,"Modified before: enter a valid local YYYY-MM-DD date or ISO UTC timestamp,\nor leave empty. This bound is exclusive for regular files."));
    if (after && before && (setup->filters.modified_not_before.seconds>setup->filters.modified_before.seconds ||
        (setup->filters.modified_not_before.seconds==setup->filters.modified_before.seconds &&
         setup->filters.modified_not_before.nanoseconds>=setup->filters.modified_before.nanoseconds)))
        return setup_error(w,w->date_before,M(308,"Modified before must be later than modified on/after.\nThe lower bound is inclusive and the upper bound exclusive."));
    setup->filters.has_minimum_size=min; setup->filters.has_maximum_size=max;
    setup->filters.has_modified_not_before=after; setup->filters.has_modified_before=before;
    return 1;
}
static void reset_result_view(SyncWindow *w)
{
    clear_command_notice(w);
    /* Retain only reusable job preferences. Row-scoped rules belong to the old
     * generation and cannot be rebound by list position to a new inventory. */
    unsigned reusable=0;
    for (unsigned i=0;i<w->rule_count;++i) {
        if (w->rules[i].scope==CKFET_SCOPE_JOB) w->rules[reusable++]=w->rules[i];
        else free(w->rules[i].subtree);
    }
    w->rule_count=reusable; w->have_rendered=0; w->displayed_count=0; w->pause_requested=0; w->cancel_requested=0; w->last_error[0]=0;
    w->rename_selection_count=0; w->rename_row=0; w->feedback_selection_count=0;
    w->dirty=1; w->setup_dirty=0; w->review_pending=0; w->execute_pending=0;
    memset(w->view,0,sizeof(*w->view));
    XmListDeleteAllItems(w->rules_list); XmListDeleteAllItems(w->list);
    label(w->plan_summary,M(275,"Plan totals are not yet available."));
    label(w->page_info,M(286,"Comparison results are not yet available."));
    XmTextSetString(w->details,(char *)M(5,"Select a row to inspect both versions, proposed names and recovery information."));
    Widget actions[]={w->list,w->previous,w->next,w->review,w->execute,w->pause,w->cancel,w->rule_page};
    for (unsigned i=0;i<XtNumber(actions);++i) XtSetSensitive(actions[i],False);
    label(w->compared_roots,M(287,"No comparison has started."));
    label(w->setup_notice,"");
    XtSetSensitive(w->setup,True); XtSetSensitive(w->compare,True);
}
static void compare(Widget widget,XtPointer data,XtPointer call)
{
    (void)widget;(void)call; SyncWindow *w=data;
    if (w->closing) return;
    if (w->job) {
        if (!SyncJobDestroy(w->job)) {
            if (w->view->finished || (w->view->snapshot.state==CKFET_AWAITING_REVIEW && !w->review_pending && !w->execute_pending)) {
                w->recompare=1; SyncJobStop(w->job); XtSetSensitive(w->setup,False); XtSetSensitive(w->compare,False);
                progress_notice(w,M(260,"Preparing a fresh comparison..."));
            }
            else command_notice(w,M(24,"Cancel or finish the current operation before comparing again."));
            return;
        }
        w->job=NULL;
    }
    reset_result_view(w);
    char *source=NULL,*destination=NULL; SyncSetup setup;
    if (!setup_fields(w,&setup,&source,&destination)) {
        XtFree(source); XtFree(destination); return;
    }
    char error[512]={0}; w->job=SyncJobStart(&setup,error,sizeof(error));
    XtFree(source); XtFree(destination);
    if (!w->job) { command_notice(w,error[0]?error:M(26,"Cannot start operation")); return; }
    char *shown_source=XmTextFieldGetString(w->source),*shown_target=XmTextFieldGetString(w->destination);
    char roots[2048]; snprintf(roots,sizeof(roots),"%s: %.900s\n%s: %.900s",M(6,"Source"),shown_source,M(7,"Destination"),shown_target);
    label(w->compared_roots,roots); XtFree(shown_source); XtFree(shown_target);
    notify(w,state_label(CKFET_INVENTORY));
    XtSetSensitive(w->setup,False);
    XtVaSetValues(w->notebook,XmNcurrentPageNumber,2,NULL);
}
static void rule_name_side(SyncWindow *w,uint32_t choice)
{
    if (choice==CKFET_KEEP_BOTH_TARGET) w->name_side=1;
    else if (choice==CKFET_KEEP_BOTH_INCOMING || choice==CKFET_RENAME) w->name_side=0;
    option_selected(w->name_side_option,w->name_side);
}
static void add_rule(Widget widget,XtPointer data,XtPointer call)
{
    (void)widget;(void)call;
    SyncWindow *w=data;
    if (w->closing || w->cancel_requested) return;
    if (w->view->snapshot.recovery_required || w->view->finished || w->view->stopped) return;
    if (!w->job || w->view->snapshot.state!=CKFET_AWAITING_REVIEW) return;
    clear_command_notice(w);
    const uint32_t scopes[]={CKFET_SCOPE_JOB,CKFET_SCOPE_SELECTED,CKFET_SCOPE_ITEM,CKFET_SCOPE_SUBTREE};
    if (w->scope>=XtNumber(scopes)) {
        command_notice(w,M(312,"Choose a matching-rule scope.")); return;
    }
    SyncRule r; memset(&r,0,sizeof(r)); r.scope=scopes[w->scope];
    uint64_t ids[SYNC_PAGE_SIZE]; int first;
    unsigned n=selected_rows(w,ids,&first);
    if (r.scope!=CKFET_SCOPE_JOB) {
        if (!n) { command_notice(w,M(27,"Select matching rows first.")); return; }
        if ((r.scope==CKFET_SCOPE_ITEM || r.scope==CKFET_SCOPE_SUBTREE) && n!=1) {
            command_notice(w,M(85,"Select exactly one item or subtree root.")); return;
        }
        memcpy(r.selected,ids,n*sizeof(*ids)); r.selected_count=n;
    }
    if (w->choice>=23 || !XtIsSensitive(w->choice_menu[w->choice])) {
        command_notice(w,M(279,"Choose an action available for this matching class and scope.")); return;
    }
    r.choice=w->choice; r.kind=w->kind;
    r.tie=w->fallback==0?CKFET_ASK:w->fallback==1?CKFET_KEEP_TARGET:CKFET_KEEP_BOTH_INCOMING;
    r.protect_nonempty=XmToggleButtonGetState(w->zero); r.retain_replaced=XmToggleButtonGetState(w->retain);
    /* Existing rules can be replaced even when every slot is occupied. Only a
     * successful edit may adjust the rename side or invalidate its receipt. */
    for (unsigned i=0;i<w->rule_count;++i) {
        if (w->rules[i].kind==r.kind && w->rules[i].scope==r.scope && w->rules[i].selected_count==r.selected_count &&
            !memcmp(w->rules[i].selected,r.selected,r.selected_count*sizeof(uint64_t))) {
            free(w->rules[i].subtree); w->rules[i]=r; rule_name_side(w,r.choice);
            mark_dirty(w); render_rules(w);
            command_notice(w,M(28,"Rule updated. Review rules before applying the changed plan.")); return;
        }
    }
    if (w->rule_count==SYNC_MAX_RULES) {
        command_notice(w,M(310,"Matching rule limit reached. Remove a rule before adding another.")); return;
    }
    w->rules[w->rule_count++]=r; rule_name_side(w,r.choice); mark_dirty(w); render_rules(w);
    command_notice(w,M(29,"Rule added. Review rules to see matched counts and consequences."));
}
static void remove_rule(Widget widget,XtPointer data,XtPointer call)
{
    (void)widget;(void)call;
    SyncWindow *w=data;
    if (w->closing || w->cancel_requested) return;
    if (w->view->snapshot.recovery_required || w->view->finished || w->view->stopped) return;
    int *positions=NULL,n=0;
    clear_command_notice(w);
    if (!XmListGetSelectedPos(w->rules_list,&positions,&n) || n!=1) {
        XtFree((char *)positions); command_notice(w,M(311,"Select one rule to remove.")); return;
    }
    unsigned i=(unsigned)(positions[0]-1); XtFree((char *)positions);
    if (i>=w->rule_count) { command_notice(w,M(311,"Select one rule to remove.")); return; }
    free(w->rules[i].subtree); memmove(w->rules+i,w->rules+i+1,(w->rule_count-i-1)*sizeof(SyncRule)); --w->rule_count;
    mark_dirty(w); render_rules(w); command_notice(w,M(30,"Rule removed. Review the changed rule list before execution."));
}
/* List positions address this window's owned snapshot, never the editable
 * Save name. Copy the identity before callbacks or a new snapshot replace it. */
static int selected_profile(SyncWindow *w,char name[SYNC_PROFILE_NAME_MAX+1])
{
    int *positions=NULL,count=0,found=0;
    if (w->profile_list && XmListGetSelectedPos(w->profile_list,&positions,&count) && count==1 &&
        positions[0]>0 && (unsigned)positions[0]<=w->preferences_view.count) {
        memcpy(name,w->preferences_view.names[positions[0]-1],SYNC_PROFILE_NAME_MAX+1);
        found=1;
    }
    XtFree((char *)positions); return found;
}
static int preferences_ready(const SyncWindow *w)
{
    return w->preferences && !w->closing &&
        (w->preferences_view.state==SYNC_PROFILES_READY || w->preferences_view.state==SYNC_PROFILES_ERROR) &&
        w->preferences_view.completed==w->preferences_view.submitted;
}
static int profile_setup_ready(const SyncWindow *w)
{
    if (!preferences_ready(w) || w->recompare || w->review_pending || w->execute_pending) return 0;
    if (!w->job) return 1;
    const SyncView *v=w->view;
    if (v->settling || v->snapshot.recovery_required) return 0;
    if (v->finished || v->stopped) return v->drained || v->stopped;
    return !w->cancel_requested && v->ready && v->snapshot.state==CKFET_AWAITING_REVIEW &&
        v->snapshot.command_state!=CKFET_COMMAND_PENDING && SyncJobViewCoherent(v);
}
static void profile_controls(SyncWindow *w)
{
    char name[SYNC_PROFILE_NAME_MAX+1]; int selected=selected_profile(w,name);
    XtSetSensitive(w->profile_save,preferences_ready(w));
    XtSetSensitive(w->profile_load,selected && profile_setup_ready(w));
    XtSetSensitive(w->profile_delete,selected && preferences_ready(w));
}
static void profile_selected(Widget widget,XtPointer data,XtPointer call)
{
    (void)widget; (void)call; SyncWindow *w=data; char name[SYNC_PROFILE_NAME_MAX+1];
    clear_command_notice(w);
    if (selected_profile(w,name)) XmTextFieldSetString(w->profile_name,name);
    profile_controls(w);
}
static void save_profile(Widget widget,XtPointer data,XtPointer call)
{
    (void)widget;(void)call;
    SyncWindow *w=data;
    if (w->closing) return;
    clear_command_notice(w);
    if (!w->preferences) return;
    SyncProfile *profile=calloc(1,sizeof(*profile)); if (!profile) { command_notice(w,M(223,"Not enough memory.")); return; }
    char *name=XmTextFieldGetString(w->profile_name),*source=NULL,*destination=NULL; SyncSetup setup;
    if (!name || !*name || strlen(name)>SYNC_PROFILE_NAME_MAX) {
        label(w->profile_status,M(224,"Enter a profile name (1-64 bytes).")); goto done;
    }
    if (!setup_fields(w,&setup,&source,&destination)) goto done;
    if (!source || !destination || strlen(source)>SYNC_PROFILE_PATH_MAX || strlen(destination)>SYNC_PROFILE_PATH_MAX) {
        label(w->profile_status,M(222,"Folder path is too long.")); goto done;
    }
    profile->version=SYNC_PROFILES_VERSION; strcpy(profile->name,name); strcpy(profile->source,source); strcpy(profile->destination,destination);
    profile->profile=setup.profile; profile->metadata_scope=setup.metadata_scope; profile->filters=setup.filters;
    for (unsigned i=0;i<w->rule_count;++i) {
        if (w->rules[i].scope!=CKFET_SCOPE_JOB) {
            label(w->profile_status,M(225,"Profiles save job-wide rules. Item, selection and subtree decisions stay with this comparison.")); goto done;
        }
        const SyncRule *r=&w->rules[i]; SyncProfileRule *out=&profile->rules[profile->rule_count++];
        *out=(SyncProfileRule){r->kind,r->choice,r->tie,r->protect_nonempty,r->retain_replaced};
    }
    SyncProfilesResult result=SyncProfilesSave(w->preferences,profile);
    label(w->profile_status,result==SYNC_PROFILES_OK?M(226,"Saving profile preferences..."):M(227,"Profile save was not queued; check the name or wait for the current save."));
done:
    XtFree(name); XtFree(source); XtFree(destination); free(profile);
}
static void load_profile(Widget widget,XtPointer data,XtPointer call)
{
    (void)widget;(void)call;
    SyncWindow *w=data;
    if (w->closing) return;
    clear_command_notice(w);
    if (!profile_setup_ready(w)) return;
    char name[SYNC_PROFILE_NAME_MAX+1];
    if (!selected_profile(w,name)) { label(w->profile_status,M(228,"Choose an available saved profile.")); return; }
    SyncProfile *profile=calloc(1,sizeof(*profile));
    if (!profile) { command_notice(w,M(223,"Not enough memory.")); return; }
    SyncProfilesResult result=SyncProfilesGetProfile(w->preferences,name,profile);
    if (result!=SYNC_PROFILES_OK) { free(profile); label(w->profile_status,M(228,"Choose an available saved profile.")); return; }
    char after[40]="",before[40]=""; unsigned mode=0;
    for (;mode<sizeof(profiles)/sizeof(profiles[0]);++mode) if (profiles[mode]==profile->profile) break;
    if (mode==sizeof(profiles)/sizeof(profiles[0]) || profile->rule_count>SYNC_MAX_RULES || profile->metadata_scope>1 ||
        profile->filters.allowed_kind_mask!=CKFSP_ALL_KINDS || profile->filters.maximum_depth!=1024 ||
        (profile->filters.has_modified_not_before && !SyncFormatDate(&profile->filters.modified_not_before,after)) ||
        (profile->filters.has_modified_before && !SyncFormatDate(&profile->filters.modified_before,before))) {
        free(profile); label(w->profile_status,M(229,"This profile has options or dates this window cannot represent.")); return;
    }
    PreparedPath source={0},destination={0};
    if (!prepare_native_path(w,profile->source,&source) || !prepare_native_path(w,profile->destination,&destination)) {
        clear_prepared_path(&source); clear_prepared_path(&destination); free(profile); return;
    }
    /* No application allocation or option/date validation remains after both
     * paths are ready. Toolkit setters copy their supplied display strings. */
    commit_native_path(w,w->source,&source); commit_native_path(w,w->destination,&destination);
    clear_prepared_path(&source); clear_prepared_path(&destination);
    w->profile_index=mode; w->metadata=profile->metadata_scope;
    option_selected(w->operation_option,mode); option_selected(w->metadata_option,w->metadata);
    operation_explanation(w);
    char value[32];
    if (profile->filters.has_minimum_size) snprintf(value,sizeof(value),"%" PRIu64,profile->filters.minimum_size); else value[0]=0;
    XmTextFieldSetString(w->size_min,value);
    if (profile->filters.has_maximum_size) snprintf(value,sizeof(value),"%" PRIu64,profile->filters.maximum_size); else value[0]=0;
    XmTextFieldSetString(w->size_max,value); XmTextFieldSetString(w->date_after,after); XmTextFieldSetString(w->date_before,before);
    for (unsigned i=0;i<w->rule_count;++i) free(w->rules[i].subtree);
    w->rule_count=profile->rule_count; memset(w->rules,0,sizeof(w->rules));
    for (unsigned i=0;i<w->rule_count;++i) {
        const SyncProfileRule *in=&profile->rules[i]; SyncRule *out=&w->rules[i];
        out->kind=in->kind; out->choice=in->choice; out->tie=in->tie; out->protect_nonempty=in->protect_nonempty;
        out->retain_replaced=in->retain_replaced; out->scope=CKFET_SCOPE_JOB;
    }
    mark_dirty(w); setup_changed(NULL,w,NULL); render_rules(w); free(profile);
    label(w->profile_status,M(230,"Profile loaded. Compare and review again before applying changes."));
}
typedef struct { SyncWindow *window; char *name; } ProfileDeletion;
static void profile_deletion_free(Widget widget,XtPointer data,XtPointer call)
{
    (void)widget;(void)call; ProfileDeletion *choice=data; free(choice->name); free(choice);
}
static void profile_deletion_ok(Widget dialog,XtPointer data,XtPointer call)
{
    (void)call; ProfileDeletion *choice=data; SyncWindow *w=choice->window;
    if (!w->closing && w->preferences) {
        SyncProfilesResult result=SyncProfilesDelete(w->preferences,choice->name);
        label(w->profile_status,result==SYNC_PROFILES_OK?M(250,"Deleting saved preferences..."):M(251,"Profile deletion was not queued; wait for the current preferences operation."));
    }
    XtDestroyWidget(XtParent(dialog));
}
static void delete_profile(Widget widget,XtPointer data,XtPointer call)
{
    (void)widget;(void)call;
    SyncWindow *w=data;
    if (w->closing) return;
    clear_command_notice(w);
    if (!w->preferences) { command_notice(w,M(236,"Saved preferences are unavailable.")); return; }
    if (!preferences_ready(w)) return;
    char name[SYNC_PROFILE_NAME_MAX+1];
    if (!selected_profile(w,name)) { label(w->profile_status,M(228,"Choose an available saved profile.")); return; }
    ProfileDeletion *choice=calloc(1,sizeof(*choice));
    if (choice) choice->name=strdup(name);
    if (!choice || !choice->name) { if (choice) free(choice); command_notice(w,M(223,"Not enough memory.")); return; }
    choice->window=w;
    char question[256]; snprintf(question,sizeof(question),M(252,"Delete saved profile '%s'?\nThis removes preferences only."),choice->name);
    Widget dialog=XmCreateQuestionDialog(w->shell,"deleteProfile",NULL,0);
    XmString text=XmStringCreateLocalized(question),ok=XmStringCreateLocalized((char *)M(253,"Delete profile"));
    XtVaSetValues(dialog,XmNdialogTitle,ok,XmNmessageString,text,XmNokLabelString,ok,XmNdialogStyle,XmDIALOG_FULL_APPLICATION_MODAL,
        XmNdefaultButtonType,XmDIALOG_CANCEL_BUTTON,XmNautoUnmanage,False,NULL);
    XmStringFree(text); XmStringFree(ok);
    XtUnmanageChild(XmMessageBoxGetChild(dialog,XmDIALOG_HELP_BUTTON));
    XtAddCallback(dialog,XmNokCallback,profile_deletion_ok,choice);
    XtAddCallback(dialog,XmNcancelCallback,folder_cancelled,NULL);
    XtAddCallback(dialog,XmNdestroyCallback,profile_deletion_free,choice);
    XtManageChild(dialog);
}
static void review(Widget widget,XtPointer data,XtPointer call)
{
    (void)widget;(void)call;
    SyncWindow *w=data;
    if (w->closing || w->cancel_requested) return;
    if (w->view->snapshot.recovery_required || w->view->finished || w->view->stopped) return;
    clear_command_notice(w);
    if (!w->job || w->review_pending || w->execute_pending) return;
    if (w->setup_dirty) { command_notice(w,M(261,"Folder options changed. Compare again before reviewing or applying changes.")); return; }
    uint64_t ids[SYNC_PAGE_SIZE]; int first; unsigned n=selected_rows(w,ids,&first);
    char *name=XmTextFieldGetString(w->new_name);
    if (!name) { command_notice(w,M(223,"Not enough memory.")); return; }
    if (*name && (n!=1 || strchr(name,'/') || !strcmp(name,".") || !strcmp(name,".."))) {
        command_notice(w,M(31,"For a reviewed rename, select one row and enter one basename.")); XtFree(name); return;
    }
    w->rename_selection_count=n; w->rename_row=n==1?ids[0]:0;
    uint64_t serial=SyncJobReview(w->job,w->view->snapshot.plan_generation,w->rules,w->rule_count,NULL,0,
        n==1?ids[0]:0,*name?name:NULL,w->name_side?CKFET_NAME_TARGET:CKFET_NAME_INCOMING);
    XtFree(name); w->dirty=1; w->review_pending=serial!=0; w->review_serial=serial;
    w->previous_receipt=w->view->snapshot.receipt_id;
    w->reviewed_rules_revision=w->local_rules_revision;
    XtSetSensitive(w->execute,False);
    if (serial) { XtSetSensitive(w->review,False); XtSetSensitive(w->rule_page,False); XtSetSensitive(w->setup,False); XtSetSensitive(w->compare,False); }
    if (serial) notify(w,M(32,"Evaluating rules; no files have been changed."));
    else command_notice(w,M(33,"Worker is busy; try the command again."));
}
static void execute(Widget widget,XtPointer data,XtPointer call)
{
    (void)widget;(void)call;
    SyncWindow *w=data;
    if (w->closing || w->cancel_requested) return;
    if (w->view->snapshot.recovery_required || w->view->finished || w->view->stopped) return;
    clear_command_notice(w);
    if (!w->job || w->review_pending || w->execute_pending) return;
    if (!DTFILE_SYNC_MUTATION_QUALIFIED) {
        command_notice(w,M(258,"Applying changes is unavailable until native execution is qualified.")); return;
    }
    if (w->dirty || w->setup_dirty || !SyncJobViewCoherent(w->view) || w->view->api!=CKFE_API_OK ||
        w->view->snapshot.state!=CKFET_AWAITING_REVIEW || w->view->snapshot.unresolved_rows ||
        !w->view->snapshot.receipt_id || w->view->snapshot.rule_count!=w->rule_count ||
        profiles[w->profile_index]==CKFSP_COMPARE || profiles[w->profile_index]==CKFSP_MIRROR) {
        command_notice(w,M(273,"Compare and review the complete current plan before applying changes.")); return;
    }
    w->execute_serial=SyncJobExecute(w->job,w->view->snapshot.plan_generation,w->view->snapshot.receipt_id);
    if (!w->execute_serial) command_notice(w,M(33,"Worker is busy; try the command again."));
    else {
        w->execute_pending=1; XtSetSensitive(w->execute,False); XtSetSensitive(w->review,False); XtSetSensitive(w->rule_page,False);
        XtSetSensitive(w->setup,False); XtSetSensitive(w->compare,False);
        notify(w,M(34,"Revalidating and applying the reviewed plan..."));
    }
}
static void pause_job(Widget widget,XtPointer data,XtPointer call)
{
    (void)widget;(void)call; SyncWindow *w=data; if (!w->job || w->closing || w->cancel_requested) return;
    if (w->view->settling || w->view->finished || w->view->stopped || w->view->snapshot.recovery_required) return;
    w->pause_requested=!w->pause_requested; SyncJobPause(w->job,w->pause_requested);
}
static void cancel_job(Widget widget,XtPointer data,XtPointer call)
{
    (void)widget;(void)call; SyncWindow *w=data; if (!w->job || w->closing || w->cancel_requested) return;
    clear_command_notice(w); w->cancel_requested=1; SyncJobCancel(w->job);
    Widget controls[]={w->setup,w->compare,w->review,w->execute,w->rule_page,w->pause,w->cancel,w->previous,w->next};
    for (unsigned i=0;i<XtNumber(controls);++i) XtSetSensitive(controls[i],False);
    progress_notice(w,M(35,"Cancelling - finishing safe cleanup..."));
}
static void help(Widget widget,XtPointer data,XtPointer call)
{
    (void)widget;(void)call; SyncWindow *w=data;
    Widget dialog=XmCreateInformationDialog(w->shell,"folderHelp",NULL,0);
    XmString title=XmStringCreateLocalized((char *)M(281,"Help"));
    XtVaSetValues(dialog,XmNdialogTitle,title,NULL); XmStringFree(title);
    const char *body=M(280,"Choose two folders and an operation, then Compare. Inspect the plan totals\nand select rows to see both versions and the proposed names.\n\nUse matching rules for one item, selected rows, a subtree or the whole job.\nReview rules after every edit. Saved profiles store preferences only;\nloading one always requires a fresh comparison and review.\n\nDisplayed \\xNN escapes retain original folder bytes until the field is edited.\nDate-only bounds use local midnight; UTC timestamps preserve exact bounds.\nPause and Cancel are requests. Closing waits for safe worker shutdown.");
    XmString text=XmStringCreateLocalized((char *)body);
    XtVaSetValues(dialog,XmNmessageString,text,XmNautoUnmanage,False,NULL); XmStringFree(text);
    XtUnmanageChild(XmMessageBoxGetChild(dialog,XmDIALOG_HELP_BUTTON));
    XtUnmanageChild(XmMessageBoxGetChild(dialog,XmDIALOG_CANCEL_BUTTON));
    XtAddCallback(dialog,XmNokCallback,folder_cancelled,NULL); XtManageChild(dialog);
}
static void close_window(Widget widget,XtPointer data,XtPointer call)
{
    (void)widget;(void)call; SyncWindow *w=data;
    w->closing=1; clear_command_notice(w); w->recompare=0;
    if (w->job) SyncJobStop(w->job);
    if (w->preferences) SyncProfilesStop(w->preferences);
    Widget controls[]={w->setup,w->compare,w->review,w->execute,w->rule_page,w->pause,w->cancel,
        w->previous,w->next,w->profile_save,w->profile_load,w->profile_delete};
    for (unsigned i=0;i<XtNumber(controls);++i) XtSetSensitive(controls[i],False);
    progress_notice(w,M(313,"Closing - finishing safe cleanup..."));
}
static void previous(Widget widget,XtPointer data,XtPointer call)
{
    (void)widget;(void)call; SyncWindow *w=data;
    if (w->job && !w->closing && !w->cancel_requested) SyncJobPage(w->job,w->view->offset>=SYNC_PAGE_SIZE?w->view->offset-SYNC_PAGE_SIZE:0);
}
static void next(Widget widget,XtPointer data,XtPointer call)
{
    (void)widget;(void)call; SyncWindow *w=data; if (w->job && !w->closing && !w->cancel_requested && w->view->has_next) SyncJobPage(w->job,w->view->next_offset);
}
static void tick(XtPointer data,XtIntervalId *id)
{
    (void)id; SyncWindow *w=data;
    if (w->preferences) {
        SyncProfilesSnapshot snapshot;
        if (SyncProfilesGetSnapshot(w->preferences,&snapshot)==SYNC_PROFILES_OK && snapshot.revision!=w->preferences_view.revision) {
            char selected[SYNC_PROFILE_NAME_MAX+1]; int have_selected=selected_profile(w,selected);
            w->preferences_view=snapshot; XmListDeleteAllItems(w->profile_list);
            for (unsigned i=0;i<snapshot.count;++i) {
                XmString item=XmStringCreateLocalized(snapshot.names[i]); XmListAddItemUnselected(w->profile_list,item,0); XmStringFree(item);
                if (have_selected && !strcmp(selected,snapshot.names[i]))
                    XmListSelectPos(w->profile_list,(int)i+1,False);
            }
            /* Preserve by exact name, not old index. A removed profile leaves
             * no selection; refreshing never overwrites a typed Save name. */
            if (snapshot.error[0]) label(w->profile_status,snapshot.error);
            else if (snapshot.state==SYNC_PROFILES_SAVING) label(w->profile_status,M(226,"Saving profile preferences..."));
            else if (snapshot.state==SYNC_PROFILES_READY) label(w->profile_status,M(231,"Profile preferences ready."));
        }
    }
    if (w->job && SyncJobRead(w->job,w->view)) {
        const SyncView *v=w->view;
        if (w->command_notice[0] && w->command_notice_generation!=v->snapshot.plan_generation)
            clear_command_notice(w);
        int awaiting=v->ready && !v->settling && !v->snapshot.recovery_required &&
            v->snapshot.state==CKFET_AWAITING_REVIEW && !v->stopped &&
            v->snapshot.command_state!=CKFET_COMMAND_PENDING;
        int active=!v->finished && !v->stopped;
        int configure=(!active || (awaiting && !w->cancel_requested)) && !w->review_pending && !w->execute_pending && !w->recompare && !w->closing;
        XtSetSensitive(w->setup,configure); XtSetSensitive(w->compare,configure);
        int coherent=SyncJobViewCoherent(v) && v->summary_valid && v->summary_generation==v->snapshot.plan_generation &&
            v->summary_receipt==v->snapshot.receipt_id;
        if (w->review_pending && v->completed_submission==w->review_serial) {
            if (!v->command_accepted || v->command_api!=CKFE_API_OK) { w->dirty=1; w->review_pending=0; }
            else if (coherent && v->review_submission==w->review_serial && v->snapshot.receipt_id) {
                w->dirty=w->local_rules_revision!=w->reviewed_rules_revision; w->review_pending=0;
            }
        }
        if (w->execute_pending && v->completed_submission==w->execute_serial && !v->command_accepted) {
            w->dirty=1; w->execute_pending=0;
        }
        XtSetSensitive(w->review,awaiting && !w->cancel_requested && !w->closing && !w->setup_dirty && !w->review_pending && !w->execute_pending);
        XtSetSensitive(w->pause,!w->closing && !w->cancel_requested && !v->settling && !v->snapshot.recovery_required && active && (v->snapshot.state!=CKFET_AWAITING_REVIEW ||
            v->snapshot.pause_requested || v->snapshot.paused));
        XtSetSensitive(w->rule_page,awaiting && coherent && !w->cancel_requested && !w->closing && !w->setup_dirty && !w->review_pending && !w->execute_pending);
        XtSetSensitive(w->list,v->cache_valid);
        XtSetSensitive(w->cancel,!w->closing && !w->cancel_requested && active);
        XtSetSensitive(w->previous,!w->closing && !w->cancel_requested && v->offset>0);
        XtSetSensitive(w->next,!w->closing && !w->cancel_requested && v->has_next);
        /* A local rule edit must not reuse a receipt from the prior rule list. */
        int reviewed=!w->closing && !w->cancel_requested && !w->dirty && !w->setup_dirty && !w->review_pending && !w->execute_pending && v->snapshot.rule_count==w->rule_count;
        XtSetSensitive(w->execute,DTFILE_SYNC_MUTATION_QUALIFIED && awaiting && coherent && reviewed && v->api==CKFE_API_OK && v->snapshot.receipt_id && !v->snapshot.unresolved_rows &&
            profiles[w->profile_index]!=CKFSP_COMPARE && profiles[w->profile_index]!=CKFSP_MIRROR);
        label(w->pause,v->snapshot.pause_requested?M(36,"Resume"):M(37,"Pause"));
        if (v->cache_valid && (!w->have_rendered || v->snapshot.sequence!=w->rendered_sequence || v->offset!=w->rendered_offset ||
            w->rendered_names_valid!=v->names_valid)) {
            render_rows(w); render_rules(w); w->rendered_sequence=v->snapshot.sequence; w->rendered_offset=v->offset;
            w->rendered_names_valid=v->names_valid; w->have_rendered=1;
        }
        char text[1024],backend_error[1024]={0};
        if (v->error[0]) {
            snprintf(backend_error,sizeof(backend_error),"%s\n%s",
                M(98,"Operation needs attention. See details and recovery information."),v->error);
            if (strcmp(w->last_error,v->error)) {
                char details[4096];
                if (v->journal[0]) snprintf(details,sizeof(details),"%s\n\n%s: %s",v->error,M(14,"Journal directory"),v->journal);
                else snprintf(details,sizeof(details),"%s",v->error);
                XmTextSetString(w->details,details); snprintf(w->last_error,sizeof(w->last_error),"%s",v->error);
            }
        }
        const char *recovery=v->snapshot.recovery_required?M(242,"Recovery required"):"";
        if (w->closing)
            snprintf(text,sizeof(text),"%s%s%s",M(313,"Closing - finishing safe cleanup..."),*recovery?"\n":"",recovery);
        else if ((w->cancel_requested || v->settling) && active)
            snprintf(text,sizeof(text),"%s%s%s",M(35,"Cancelling - finishing safe cleanup..."),*recovery?"\n":"",recovery);
        else if (v->snapshot.state>=CKFET_COMPLETE && !v->drained && !v->stopped)
            snprintf(text,sizeof(text),"%s%s%s",M(257,"Finalizing the operation and releasing native resources..."),*recovery?"\n":"",recovery);
        else if (*recovery) snprintf(text,sizeof(text),"%s%s%s",recovery,v->error[0]?"\n":"",v->error);
        else if (v->error[0] && (!awaiting || !coherent)) snprintf(text,sizeof(text),"%s",backend_error);
        /* Local command feedback belongs to this coherent review generation.
         * It may replace an older command rejection, never recovery or drain. */
        else if (w->command_notice[0] && command_notice_ready(w))
            snprintf(text,sizeof(text),"%s",w->command_notice);
        else if (w->setup_dirty && awaiting) snprintf(text,sizeof(text),"%s",M(261,"Folder options changed. Compare again before reviewing or applying changes."));
        else if (v->error[0]) snprintf(text,sizeof(text),"%s",backend_error);
        else if (w->dirty && awaiting) snprintf(text,sizeof(text),"%s",M(221,"Not reviewed"));
        else snprintf(text,sizeof(text),"%s%s\n%" PRIu64 " %s, %" PRIu64 " %s, %" PRIu64 " %s; %" PRIu64 "/%" PRIu64 " %s%s",
            v->snapshot.paused?M(38,"Paused"):state_label(v->snapshot.state),
            v->snapshot.pause_requested && !v->snapshot.paused?M(39," (pause requested)"):"",
            v->snapshot.completed_items,M(40,"completed"),v->snapshot.skipped_items,M(16,"skipped"),
            v->snapshot.unresolved_rows,M(23,"unresolved"),v->snapshot.copied_bytes,v->snapshot.known_copy_bytes,M(4,"bytes"),
            v->snapshot.counts_complete?"":M(41," (totals incomplete)"));
        notify(w,text);
        if (w->recompare && !w->closing && v->stopped) { SyncJobDestroy(w->job); w->job=NULL; w->recompare=0; compare(NULL,w,NULL); }
    }
    profile_controls(w);
    if (w->closing) {
        if (w->job && SyncJobDestroy(w->job)) w->job=NULL;
        if (w->preferences && SyncProfilesDestroy(w->preferences)) w->preferences=NULL;
        if (!w->job && !w->preferences) { XtAppSetExitFlag(w->context); return; }
    }
    XtAppAddTimeOut(w->context,100,tick,w);
}
int main(int argc,char **argv)
{
    /* File Manager owns its cache and IPC descriptors. Do this after exec,
     * before opening our catalog, display or worker resources. */
    closefrom(3);
    setlocale(LC_ALL,""); XtSetLanguageProc(NULL,NULL,NULL); catalog=catopen("dtfile_sync",NL_CAT_LOCALE);
    SyncWindow w; memset(&w,0,sizeof(w)); w.view=calloc(1,sizeof(*w.view)); if (!w.view) return 1;
    const char *source=argc>1?argv[1]:"",*destination=argc>2?argv[2]:"";
    if (argc>3) { fprintf(stderr,"Usage: %s [source-folder [destination-folder]]\n",argv[0]); free(w.view); return 2; }
    int xt_argc=1; char *xt_argv[]={argv[0],NULL};
    w.shell=XtVaAppInitialize(&w.context,"DtfileSync",NULL,0,&xt_argc,xt_argv,NULL,XmNtitle,M(42,"File Manager - Synchronize Folders"),
        XmNwidth,1100,XmNheight,760,
        XmNdeleteResponse,XmDO_NOTHING,NULL);
    /* Keep the actions visible on smaller desktops. Page content scrolls
     * independently instead of forcing controls outside the screen. */
    Screen *screen=XtScreen(w.shell);
    int width=WidthOfScreen(screen)-40,height=HeightOfScreen(screen)-100;
    if (width>1100) width=1100;
    if (height>760) height=760;
    if (width<1) width=1;
    if (height<1) height=1;
    XtVaSetValues(w.shell,XmNwidth,(Dimension)width,XmNheight,(Dimension)height,NULL);
    Widget root=XtVaCreateManagedWidget("operations",xmFormWidgetClass,w.shell,XmNmarginWidth,10,XmNmarginHeight,10,NULL);
    Widget profile_bar=XtVaCreateManagedWidget("profileBar",xmRowColumnWidgetClass,root,
        XmNtopAttachment,XmATTACH_FORM,XmNleftAttachment,XmATTACH_FORM,XmNrightAttachment,XmATTACH_FORM,NULL);
    Widget profile_entry=XtVaCreateManagedWidget("profileEntry",xmRowColumnWidgetClass,profile_bar,XmNorientation,XmHORIZONTAL,NULL);
    w.profile_name=field(profile_entry,"profileName",M(314,"Save as profile:"),"",28);
    XtVaSetValues(w.profile_name,XmNmaxLength,SYNC_PROFILE_NAME_MAX,NULL);
    XtAddCallback(w.profile_name,XmNvalueChangedCallback,rule_input_changed,&w);
    w.profile_save=button(profile_entry,"saveProfile",M(233,"Save preferences"),save_profile,&w);
    w.profile_status=XtVaCreateManagedWidget("profileStatus",xmLabelWidgetClass,profile_bar,
        XmNalignment,XmALIGNMENT_BEGINNING,XmNrecomputeSize,False,XmNheight,24,XmNwidth,1000,NULL);
    label(w.profile_status,M(234,"Loading saved preferences...")); XtSetSensitive(w.profile_save,False);
    Widget actions=XtVaCreateManagedWidget("actions",xmRowColumnWidgetClass,root,XmNorientation,XmHORIZONTAL,
        XmNbottomAttachment,XmATTACH_FORM,XmNleftAttachment,XmATTACH_FORM,XmNrightAttachment,XmATTACH_FORM,NULL);
    w.status=XtVaCreateManagedWidget("status",xmLabelWidgetClass,root,XmNalignment,XmALIGNMENT_BEGINNING,
        XmNrecomputeSize,False,XmNheight,42,
        XmNbottomAttachment,XmATTACH_WIDGET,XmNbottomWidget,actions,XmNleftAttachment,XmATTACH_FORM,XmNrightAttachment,XmATTACH_FORM,NULL);
    w.notebook=XtVaCreateManagedWidget("workflow",xmNotebookWidgetClass,root,
        XmNtopAttachment,XmATTACH_WIDGET,XmNtopWidget,profile_bar,XmNleftAttachment,XmATTACH_FORM,XmNrightAttachment,XmATTACH_FORM,
        XmNbottomAttachment,XmATTACH_WIDGET,XmNbottomWidget,w.status,XmNbackPagePlacement,XmTOP_RIGHT,
        XmNorientation,XmVERTICAL,XmNbindingType,XmNONE,XmNcurrentPageNumber,1,NULL);
    const char *tabs[]={M(86,"Folders and options"),M(87,"Comparison and results"),M(88,"Matching rules")};
    Widget pages_widgets[3];
    for (int page=1;page<=3;++page) {
        Widget viewport=XtVaCreateManagedWidget("pageViewport",xmScrolledWindowWidgetClass,w.notebook,
            XmNnotebookChildType,XmPAGE,XmNpageNumber,page,XmNscrollingPolicy,XmAUTOMATIC,
            XmNvisualPolicy,XmCONSTANT,XmNscrollBarDisplayPolicy,XmAS_NEEDED,NULL);
        if (page==1) w.setup_viewport=viewport;
        pages_widgets[page-1]=XtVaCreateManagedWidget("page",xmRowColumnWidgetClass,viewport,
            XmNmarginWidth,8,XmNmarginHeight,8,NULL);
        Widget tab=XtVaCreateManagedWidget("tab",xmPushButtonWidgetClass,w.notebook,XmNnotebookChildType,XmMAJOR_TAB,XmNpageNumber,page,NULL);
        label(tab,tabs[page-1]);
    }
    w.setup=pages_widgets[0]; Widget comparison=pages_widgets[1],rule_page=pages_widgets[2]; w.rule_page=rule_page;
    Arg args[6]; Cardinal n=0; XtSetArg(args[n],XmNvisibleItemCount,2);++n;
    w.profile_list=XmCreateScrolledList(w.setup,"savedProfiles",args,n); XtManageChild(w.profile_list);
    XtAddCallback(w.profile_list,XmNbrowseSelectionCallback,profile_selected,&w);
    XtAddCallback(w.profile_list,XmNdefaultActionCallback,load_profile,&w);
    Widget profile_buttons=XtVaCreateManagedWidget("profileButtons",xmRowColumnWidgetClass,w.setup,XmNorientation,XmHORIZONTAL,NULL);
    w.profile_load=button(profile_buttons,"loadProfile",M(235,"Load selected preferences"),load_profile,&w); XtSetSensitive(w.profile_load,False);
    w.profile_delete=button(profile_buttons,"deleteProfile",M(315,"Delete selected profile"),delete_profile,&w);
    XtSetSensitive(w.profile_delete,False);
    w.source=folder_field(&w,"source",M(43,"Source folder:"),"");
    w.destination=folder_field(&w,"destination",M(44,"Destination folder:"),"");
    XtAddCallback(w.source,XmNvalueChangedCallback,path_edited,&w); XtAddCallback(w.destination,XmNvalueChangedCallback,path_edited,&w);
    button(w.setup,"swapFolders",M(294,"Swap folders"),swap_folders,&w);
    Widget modes=XtVaCreateManagedWidget("modes",xmRowColumnWidgetClass,w.setup,NULL);
    const char *mode_names[]={M(45,"Compare only"),M(46,"Copy"),M(47,"Update: protect destination-newer"),M(48,"Copy missing only"),M(49,"Synchronize one way"),M(50,"Mirror - deletion review required")};
    w.operation_option=option(modes,"operation",M(51,"Operation:"),mode_names,6,&w.profile_index,NULL);
    w.operation_explanation=XtVaCreateManagedWidget("operationExplanation",xmLabelWidgetClass,modes,
        XmNalignment,XmALIGNMENT_BEGINNING,NULL);
    operation_explanation(&w);
    observe_setup_option(w.operation_option,&w);
    const char *metadata[]={M(52,"Complete preservation"),M(53,"Ordinary mode/times; new caller ownership")};
    w.metadata_option=option(modes,"metadata",M(54,"Metadata:"),metadata,2,&w.metadata,NULL);
    observe_setup_option(w.metadata_option,&w);
    Widget warning=XtVaCreateManagedWidget("capabilities",xmLabelWidgetClass,w.setup,NULL);
    label(warning,M(55,"Compare and rule review are available. Applying changes awaits native backend qualification.\nTwo-way sync, move and mirror execution require further qualification."));
    Widget comparison_note=XtVaCreateManagedWidget("comparisonPolicy",xmLabelWidgetClass,w.setup,NULL);
    label(comparison_note,M(309,"Byte and date bounds filter regular files only; comparison reads contents.\nUnknown required size, time or precision stays unknown; it is not excluded."));
    Widget filters=XtVaCreateManagedWidget("filters",xmRowColumnWidgetClass,w.setup,NULL);
    w.size_min=field(filters,"minimumBytes",M(56,"Minimum bytes:"),"",9); w.size_max=field(filters,"maximumBytes",M(57,"Maximum bytes:"),"",9);
    w.date_after=field(filters,"modifiedAfter",M(58,"Modified on/after (local YYYY-MM-DD or UTC time):"),"",30);
    w.date_before=field(filters,"modifiedBefore",M(59,"Modified before (local YYYY-MM-DD or UTC time):"),"",30);
    XtAddCallback(w.size_min,XmNvalueChangedCallback,setup_changed,&w); XtAddCallback(w.size_max,XmNvalueChangedCallback,setup_changed,&w);
    XtAddCallback(w.date_after,XmNvalueChangedCallback,setup_changed,&w); XtAddCallback(w.date_before,XmNvalueChangedCallback,setup_changed,&w);
    button(filters,"clearFilters",M(295,"Clear filters"),clear_filters,&w);
    w.setup_notice=XtVaCreateManagedWidget("setupNotice",xmLabelWidgetClass,comparison,
        XmNalignment,XmALIGNMENT_BEGINNING,NULL); label(w.setup_notice,"");
    w.compared_roots=XtVaCreateManagedWidget("comparedFolders",xmLabelWidgetClass,comparison,
        XmNalignment,XmALIGNMENT_BEGINNING,XmNrecomputeSize,False,XmNheight,44,XmNwidth,970,NULL);
    label(w.compared_roots,"");
    w.plan_summary=XtVaCreateManagedWidget("planSummary",xmLabelWidgetClass,comparison,
        XmNalignment,XmALIGNMENT_BEGINNING,XmNrecomputeSize,False,XmNheight,42,XmNwidth,970,NULL);
    label(w.plan_summary,M(275,"Plan totals are not yet available."));
    Widget heading=XtVaCreateManagedWidget("columns",xmLabelWidgetClass,comparison,NULL);
    char headings[256]; snprintf(headings,sizeof(headings),"%-32s  %-15s  %-11s  %s",M(61,"Path"),M(277,"Action"),M(278,"Result"),M(9,"Reason"));
    label(heading,headings);
    w.font=XLoadQueryFont(XtDisplay(w.shell),"9x15");
    if (!w.font) w.font=XLoadQueryFont(XtDisplay(w.shell),"fixed");
    if (w.font) w.mono=XmFontListCreate(w.font,XmFONTLIST_DEFAULT_TAG);
    if (w.mono) XtVaSetValues(heading,XmNfontList,w.mono,XmNalignment,XmALIGNMENT_BEGINNING,NULL);
    n=0; XtSetArg(args[n],XmNvisibleItemCount,14);++n;
    XtSetArg(args[n],XmNselectionPolicy,XmEXTENDED_SELECT);++n;
    XtSetArg(args[n],XmNscrollBarDisplayPolicy,XmSTATIC);++n;
    XtSetArg(args[n],XmNlistSizePolicy,XmCONSTANT);++n;
    if (w.mono) { XtSetArg(args[n],XmNfontList,w.mono);++n; }
    w.list=XmCreateScrolledList(comparison,"comparison",args,n); XtManageChild(w.list);
    XtAddCallback(w.list,XmNextendedSelectionCallback,selection_changed,&w);
    Widget pages=XtVaCreateManagedWidget("pages",xmRowColumnWidgetClass,comparison,XmNorientation,XmHORIZONTAL,NULL);
    w.previous=button(pages,"previous",M(62,"Previous page"),previous,&w); w.next=button(pages,"next",M(63,"Next page"),next,&w);
    w.page_info=XtVaCreateManagedWidget("pageRange",xmLabelWidgetClass,pages,NULL); label(w.page_info,"");
    Widget editor=XtVaCreateManagedWidget("ruleEditor",xmRowColumnWidgetClass,rule_page,NULL);
    const char *classes[15],*choices[23]; for (unsigned i=0;i<15;++i) classes[i]=kind_label(i); for (unsigned i=0;i<23;++i) choices[i]=choice_label(i);
    w.class_option=option(editor,"conflictClass",M(64,"Matching class:"),classes,15,&w.kind,w.class_menu);
    observe_choice_option(w.class_option,&w);
    Widget choice_option=option(editor,"choice",M(65,"Action:"),choices,23,&w.choice,w.choice_menu);
    observe_choice_option(choice_option,&w);
    const char *scopes[]={M(66,"All matching in job"),M(67,"Selected matching rows"),M(89,"This item"),M(90,"Matching in selected subtree")};
    Widget scope_option=option(editor,"scope",M(68,"Scope:"),scopes,4,&w.scope,NULL); observe_choice_option(scope_option,&w);
    Widget policies=XtVaCreateManagedWidget("policies",xmRowColumnWidgetClass,rule_page,NULL);
    const char *fallbacks[]={M(69,"Ask"),M(70,"Keep target"),M(71,"Keep both incoming")};
    Widget fallback_option=option(policies,"fallback",M(72,"Tie / unknown fallback:"),fallbacks,3,&w.fallback,NULL);
    observe_choice_option(fallback_option,&w);
    w.zero=XtVaCreateManagedWidget("protectNonempty",xmToggleButtonWidgetClass,policies,XmNset,True,NULL);
    label(w.zero,M(73,"Do not replace nonempty with empty"));
    XtAddCallback(w.zero,XmNvalueChangedCallback,rule_input_changed,&w);
    w.retain=XtVaCreateManagedWidget("retainReplaced",xmToggleButtonWidgetClass,policies,XmNset,True,NULL); label(w.retain,M(74,"Preserve replaced versions"));
    XtAddCallback(w.retain,XmNvalueChangedCallback,rule_input_changed,&w);
    w.new_name=field(rule_page,"newName",M(75,"Exact reviewed basename (one row):"),"",40);
    XtAddCallback(w.new_name,XmNvalueChangedCallback,name_changed,&w);
    const char *name_sides[]={M(254,"Incoming object"),M(255,"Existing target")};
    w.name_side_option=option(rule_page,"nameSide",M(256,"Name applies to:"),name_sides,2,&w.name_side,NULL);
    Widget name_pane=NULL; WidgetList name_items=NULL; Cardinal name_count=0;
    XtVaGetValues(w.name_side_option,XmNsubMenuId,&name_pane,NULL);
    XtVaGetValues(name_pane,XmNchildren,&name_items,XmNnumChildren,&name_count,NULL);
    for (Cardinal i=0;i<name_count;++i) XtAddCallback(name_items[i],XmNactivateCallback,name_changed,&w);
    Widget rule_buttons=XtVaCreateManagedWidget("ruleButtons",xmRowColumnWidgetClass,rule_page,XmNorientation,XmHORIZONTAL,NULL);
    button(rule_buttons,"addRule",M(76,"Add / update matching rule"),add_rule,&w);
    w.remove_rule=button(rule_buttons,"removeRule",M(77,"Remove selected rule"),remove_rule,&w);
    n=0; XtSetArg(args[n],XmNvisibleItemCount,5);++n; w.rules_list=XmCreateScrolledList(rule_page,"activeRules",args,n); XtManageChild(w.rules_list);
    XtAddCallback(w.rules_list,XmNbrowseSelectionCallback,rule_input_changed,&w);
    n=0; XtSetArg(args[n],XmNrows,5);++n; XtSetArg(args[n],XmNeditable,False);++n;
    XtSetArg(args[n],XmNeditMode,XmMULTI_LINE_EDIT);++n;
    XtSetArg(args[n],XmNwordWrap,True);++n; XtSetArg(args[n],XmNscrollHorizontal,False);++n;
    w.details=XmCreateScrolledText(comparison,"details",args,n); XtManageChild(w.details);
    notify(&w,M(78,"Choose folders and an operation, then compare. Review matching rules before applying changes."));
    set_native_path(&w,w.source,source); set_native_path(&w,w.destination,destination);
    w.compare=button(actions,"compare",M(60,"Compare folders"),compare,&w);
    w.review=button(actions,"review",M(79,"Review rules"),review,&w); w.execute=button(actions,"apply",M(80,"Apply reviewed plan"),execute,&w);
    w.pause=button(actions,"pause",M(37,"Pause"),pause_job,&w); w.cancel=button(actions,"cancel",M(81,"Cancel operation"),cancel_job,&w);
    button(actions,"help",M(281,"Help"),help,&w);
    button(actions,"close",M(82,"Close"),close_window,&w);
    XtSetSensitive(w.review,False); XtSetSensitive(w.execute,False); XtSetSensitive(w.pause,False); XtSetSensitive(w.cancel,False);
    XtSetSensitive(w.rule_page,False);
    XtSetSensitive(w.previous,False); XtSetSensitive(w.next,False);
    Atom close=XmInternAtom(XtDisplay(w.shell),"WM_DELETE_WINDOW",False); XmAddWMProtocolCallback(w.shell,close,close_window,&w);
    char preferences_error[512]={0}; const char *home=getenv("HOME");
    if (home) w.preferences=SyncProfilesStart(home,preferences_error,sizeof(preferences_error));
    if (!w.preferences) label(w.profile_status,preferences_error[0]?preferences_error:M(236,"Saved preferences are unavailable."));
    XtRealizeWidget(w.shell); XtAppAddTimeOut(w.context,100,tick,&w); XtAppMainLoop(w.context);
    for (unsigned i=0;i<w.rule_count;++i) free(w.rules[i].subtree);
    Display *display=XtDisplay(w.shell); XtDestroyWidget(w.shell);
    if (w.mono) XmFontListFree(w.mono);
    if (w.font) XFreeFont(display,w.font);
    free(w.native_source); free(w.native_destination); free(w.view); catclose(catalog); return 0;
}
