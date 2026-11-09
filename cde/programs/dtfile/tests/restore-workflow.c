/* The established fixture supplies complete production FileManip/FileOp
 * functions and syscall seams; no transfer result is replaced by an adapter. */
#define main transfer_fixture_main
#define DtEliminateDots TransferFixtureEliminateDots
#include "transfer-base.c"
#undef main
#undef DtEliminateDots
/* These inputs are canonical; preserve the actual pointer-returning API. */
static char *DtEliminateDots(char *path) { return path; }
#include "TrashMetadata.c"
#include <sys/wait.h>

static int erase_fault;
static int RestoreEraseUnlink(const char *path)
{ if (erase_fault) { errno=EACCES;return -1; } return unlink(path); }
/* Actual bounded production erasure, with reported unlink faults only. */
#define CopyDir RestoreFsCopyDir
#define CopyFile RestoreFsCopyFile
#define CopyLink RestoreFsCopyLink
#define CopyObject RestoreFsCopyObject
#define EmptyDir RestoreFsEmptyDir
#define EraseObject RestoreFsEraseObject
#define unlink RestoreEraseUnlink
#include "dtcopy/fsrtns.c"
#undef CopyDir
#undef CopyFile
#undef CopyLink
#undef CopyObject
#undef EmptyDir
#undef EraseObject
#undef unlink

typedef void *XtPointer;
typedef unsigned long XtInputId;
typedef struct _Tt_message_handle *Tt_message;
#include "restore-definitions.inc"
static TrashEntry *trashCan;
static int numTrashItems;
static char *TrashInfoFileName = "ledger";
static Widget toplevel = (Widget)1;
static struct { char *host, *current_directory; } trash_view = {"local", "."}, *trashFileMgrData = &trash_view;
static int notifications, confirmations, input_removals, restore_strdup_calls, restore_strdup_fail;
static int pipe_write_fd = -1, pipe_write_calls, permission_checks;
static char permission_parent[PATH_MAX];
static int resolver_null;
static int TrashInitialized=True;
#undef XtNewString
#define XtNewString(s) ((s)!=NULL?strdup(s):NULL)
#define DBGFORK(args) ((void)0)
#define XtCalloc(n,size) calloc(n,size)
#define TT_DESKTOP_ENOSPC 1
#define XtInputReadMask ((uintptr_t)1)
static int tttk_message_fail(Tt_message m,int code,int unused,int destroy)
{ (void)m;(void)code;(void)unused;(void)destroy;return 0; }

#ifndef RESTORE_BASELINE
typedef void (*FixtureCallback)(Widget,XtPointer,XtPointer);
typedef struct FixtureWidget {
    struct FixtureWidget *parent, *child;
    XtPointer data, destroy_data;
    FixtureCallback destroy;
    int delete_response;
    unsigned functions, decorations;
    int managed, auto_unmanage, warning, help_hidden;
    FixtureCallback ok, cancel;
    XtPointer ok_data, cancel_data;
} FixtureWidget;
typedef uintptr_t XtArgVal;
typedef unsigned int Cardinal;
typedef struct { const char *name; XtArgVal value; } Arg;
typedef char *XmString;
#define XtSetArg(arg,key,val) ((arg).name=(key),(arg).value=(XtArgVal)(val))
#define Dt11GETMESSAGE(set,id,text) (text)
#define XmNtitle "title"
#define XmNallowShellResize "allowShellResize"
#define XmNmwmFunctions "mwmFunctions"
#define XmNmwmDecorations "mwmDecorations"
#define XmNdialogType "dialogType"
#define XmNmessageString "messageString"
#define XmNokLabelString "okLabelString"
#define XmNcancelLabelString "cancelLabelString"
#define XmNautoUnmanage "autoUnmanage"
#define XmNuseAsyncGeometry "useAsyncGeometry"
#define XmNokCallback "okCallback"
#define XmNcancelCallback "cancelCallback"
#define XmDIALOG_WARNING 1
#define XmDIALOG_HELP_BUTTON 2
#define MWM_FUNC_MOVE (1U<<2)
#define MWM_FUNC_CLOSE (1U<<5)
#define MWM_DECOR_BORDER (1U<<1)
#define MWM_DECOR_TITLE (1U<<3)
#define MWM_DECOR_MENU (1U<<4)
#define XmNuserData "userData"
#define XmNdeleteResponse "deleteResponse"
#define XmNdestroyCallback "destroyCallback"
#define XmDESTROY 3
#define WARNING_DIALOG 1
static FixtureWidget *dialog_box;
static int destroyed_dialogs, input_active, admitted_fd=-1, spawn_fault;
static RestoreFromTrashCBData *admitted_request;
static void (*admitted_callback)(XtPointer,int *,XtInputId *);
static char updated_directory[PATH_MAX];
static int staged_pipe[2]={-1,-1};
static Widget XtParent(Widget widget) { return ((FixtureWidget *)widget)->parent; }
static void XtAddCallback(Widget raw,const char *name,FixtureCallback callback,XtPointer data)
{
    FixtureWidget *widget=raw;CHECK(!widget->managed);
    if (!strcmp(name,XmNdestroyCallback)) { widget->destroy=callback;widget->destroy_data=data; }
    else if (!strcmp(name,XmNokCallback)) { widget->ok=callback;widget->ok_data=data; }
    else { CHECK(!strcmp(name,XmNcancelCallback));widget->cancel=callback;widget->cancel_data=data; }
}
static void XtVaSetValues(Widget raw,...)
{
    FixtureWidget *widget=raw;va_list args;va_start(args,raw);const char *name;
    while ((name=va_arg(args,char *))!=NULL) {
        if (!strcmp(name,XmNuserData)) widget->data=va_arg(args,RestoreConflict *);
        else { CHECK(!strcmp(name,XmNdeleteResponse));widget->delete_response=va_arg(args,int); }
    }
    va_end(args);
}
static void XtVaGetValues(Widget raw,...)
{
    FixtureWidget *widget=raw;va_list args;va_start(args,raw);
    CHECK(!strcmp(va_arg(args,char *),XmNuserData));
    *va_arg(args,RestoreConflict **)=widget->data;
    CHECK(va_arg(args,char *)==NULL);va_end(args);
}
static void XtDestroyWidget(Widget raw)
{
    FixtureWidget *widget=raw;
    if (widget->child) { XtDestroyWidget(widget->child);widget->child=NULL; }
    if (widget->destroy) widget->destroy(widget,widget->destroy_data,NULL);
    if (widget==dialog_box) { dialog_box=NULL;++destroyed_dialogs; }
    free(widget);
}
static XmString XmStringCreateLocalized(char *text) { return strdup(text); }
static void XmStringFree(XmString text) { free(text); }
static Widget XmCreateDialogShell(Widget parent,char *name,Arg *args,Cardinal count)
{
    (void)parent;CHECK(!strcmp(name,"restoreConflict_popup"));
    FixtureWidget *shell=calloc(1,sizeof(*shell));fixture(shell!=NULL);
    for (Cardinal i=0;i<count;++i) {
        if (!strcmp(args[i].name,XmNmwmFunctions)) shell->functions=(unsigned)args[i].value;
        else if (!strcmp(args[i].name,XmNmwmDecorations)) shell->decorations=(unsigned)args[i].value;
        else if (!strcmp(args[i].name,XmNdeleteResponse)) shell->delete_response=(int)args[i].value;
        else if (!strcmp(args[i].name,XmNtitle)) CHECK(!strcmp((char *)args[i].value,"Put Back Warning"));
    }
    CHECK(shell->functions==(MWM_FUNC_MOVE|MWM_FUNC_CLOSE));
    CHECK(shell->decorations==(MWM_DECOR_BORDER|MWM_DECOR_TITLE|MWM_DECOR_MENU));
    CHECK(shell->delete_response==XmDESTROY);
    return shell;
}
static Widget XmCreateMessageBox(Widget parent,char *name,Arg *args,Cardinal count)
{
    CHECK(!strcmp(name,"restoreConflict"));FixtureWidget *shell=parent;
    dialog_box=calloc(1,sizeof(*dialog_box));fixture(dialog_box!=NULL);
    shell->child=dialog_box;dialog_box->parent=shell;dialog_box->auto_unmanage=True;++confirmations;
    for (Cardinal i=0;i<count;++i) {
        if (!strcmp(args[i].name,XmNuserData)) dialog_box->data=(void *)args[i].value;
        else if (!strcmp(args[i].name,XmNautoUnmanage)) dialog_box->auto_unmanage=(int)args[i].value;
        else if (!strcmp(args[i].name,XmNdialogType)) dialog_box->warning=args[i].value==XmDIALOG_WARNING;
        else if (!strcmp(args[i].name,XmNmessageString)) CHECK(strstr((char *)args[i].value,"There is already an object named")!=NULL);
        else if (!strcmp(args[i].name,XmNokLabelString)) CHECK(!strcmp((char *)args[i].value,"OK"));
        else if (!strcmp(args[i].name,XmNcancelLabelString)) CHECK(!strcmp((char *)args[i].value,"Cancel"));
    }
    return dialog_box;
}
static Widget XmMessageBoxGetChild(Widget box,int child) { CHECK(child==XmDIALOG_HELP_BUTTON);return box; }
static void XtUnmanageChild(Widget raw) { ((FixtureWidget *)raw)->help_hidden=True; }
static void XtManageChild(Widget raw)
{
    FixtureWidget *box=raw;
    CHECK(!box->auto_unmanage && box->warning && box->help_hidden && box->data && box->destroy);
    CHECK(box->ok && box->cancel && box->ok_data==box && box->cancel_data==box);
    box->managed=True;
}
static void *XtWidgetToApplicationContext(Widget widget) { (void)widget;return NULL; }
static XtInputId XtAppAddInput(void *context,int fd,XtPointer mask,void (*callback)(XtPointer,int *,XtInputId *),XtPointer data)
{
    (void)context;(void)mask;CHECK(!input_active);input_active=1;admitted_fd=fd;
    admitted_callback=callback;admitted_request=data;return 1;
}
static int RestorePipe(int fds[2])
{
    if (spawn_fault==1) { errno=EMFILE;return -1; }
    int result=pipe(fds);if (!result) { staged_pipe[0]=fds[0];staged_pipe[1]=fds[1]; }return result;
}
static pid_t RestoreFork(void)
{ if (spawn_fault==2) { errno=EAGAIN;return -1; } return fork(); }
static int RestoreDirectoryBusy(char *path) { (void)path;return False; } /* UI cache adapter. */
static int append_allocations, append_fail_at, tt_allocations, tt_frees, tt_argc;
static const char *tt_directory, *tt_values[3];
static char *tt_owned[8];
static char *RestoreRealloc(char *old,size_t bytes)
{
    if (++append_allocations==append_fail_at) { errno=ENOMEM;return NULL; }
    return realloc(old,bytes);
}
/* Mock only the ToolTalk value provider/allocator. Production constructs and
 * cleans its own filename arrays. Every tt_free must name a provider allocation. */
static char *tt_value_copy(const char *value)
{
    if (!value) return NULL;
    char *copy=strdup(value);fixture(copy!=NULL);
    for (unsigned i=0;i<sizeof(tt_owned)/sizeof(tt_owned[0]);++i)
        if (!tt_owned[i]) { tt_owned[i]=copy;++tt_allocations;return copy; }
    fixture(0);return NULL;
}
static int tt_error_case;
static int tt_message_args_count(Tt_message message) { (void)message;return tt_error_case==1?-1:tt_argc; }
static char *tt_message_file(Tt_message message) { (void)message;return tt_error_case==2?(char *)(uintptr_t)1:tt_value_copy(tt_directory); }
static char *tt_message_arg_val(Tt_message message,int index)
{ (void)message;fixture(index>=0 && index<tt_argc);return tt_value_copy(tt_values[index]); }
static int tt_int_error(int value) { return value<0; }
static int tt_ptr_error(char *value) { return value==(char *)(uintptr_t)1; }
static int tt_is_err(int value) { return value!=0; }
static void tt_free(char *value)
{
    for (unsigned i=0;i<sizeof(tt_owned)/sizeof(tt_owned[0]);++i)
        if (value && tt_owned[i]==value) { tt_owned[i]=NULL;++tt_frees;free(value);return; }
    CHECK(0); /* Includes attempting to free a fallback literal or NULL. */
}
/* ASCII delimiter adapter for these grammar fixtures; not a locale/tokenizer qualification. */
static char *DtStrtok(char *text,const char *delimiters)
{ static char *state;return strtok_r(text,delimiters,&state); }
#endif

static void HelpRequestCB(void) {}
static void _DtMessage(Widget w,char *title,char *text,void *unused,void (*help)(void))
{ (void)w;(void)title;(void)unused;(void)help;CHECK(text && *text);++notifications; }
static void XtRemoveInput(XtInputId id)
{
    (void)id;++input_removals;
#ifndef RESTORE_BASELINE
    input_active=0;
#endif
}
static void UpdateDirectoryOf(char *path) { CHECK(path && *path=='/'); }
static void UpdateDirectory(void *unused,char *host,char *directory)
{
    (void)unused;CHECK(host && directory);
#ifndef RESTORE_BASELINE
    if (directory && directory[0]=='/') { fixture(strlen(directory)<sizeof(updated_directory));strcpy(updated_directory,directory); }
#endif
}
static void TrashUpdateWindowIcon(void) {}
static void tt_message_reply(Tt_message m) { CHECK(m!=0); }
static void tttk_message_destroy(Tt_message m) { CHECK(m!=0); }
static void FileOperationError(Widget w,char *text,char *path) { Error(w,text,path); }
#ifdef RESTORE_BASELINE
static void CreateRestoreDialog(char *source,char *target)
{ CHECK(source && *source=='/' && target && *target=='/');++confirmations; }
#else
static void CreateRestoreDialog(RestoreConflict *);
#endif
static void AddString(String *out,int *capacity,char *text,char *header)
{
    size_t used=*out?strlen(*out):0, length=strlen(text)+strlen(header)+2;
    char *next=realloc(*out,used+length+1);fixture(next!=NULL);
    if (!used) next[0]=0;
    strcat(next,header);strcat(next,text);strcat(next,"\n");
    *out=next;*capacity=(int)(used+length+1);
}
/* Permission service adapter uses real access checks. Recursive directory
 * deletion permissions are a separate workflow and are not qualified here. */
static int CheckDeletePermission(char *parent,char *target)
{
    (void)target;fixture(strlen(parent)<sizeof(permission_parent));
    strcpy(permission_parent,parent);++permission_checks;
    return access(parent,W_OK|X_OK);
}
static char *RestoreStrdup(const char *text)
{
    if (++restore_strdup_calls==restore_strdup_fail) { errno=ENOMEM;return NULL; }
    return strdup(text);
}
static char *RestoreResolve(char *host,char *directory,char *file,char *local,Tt_status *status)
{
    if (resolver_null) { *status=TT_OK;return NULL; }
    return ResolveLocalPathName(host,directory,file,local,status);
}
static ssize_t RestoreWrite(int fd,const void *data,size_t length)
{
    if (fd==pipe_write_fd) {
        ++pipe_write_calls;
        if (!strcmp(case_name,"write-eintr") && pipe_write_calls==1) { errno=EINTR;return -1; }
        if (!strcmp(case_name,"write-zero") && pipe_write_calls>1) return 0;
        if (!strcmp(case_name,"write-error") && pipe_write_calls>1) { errno=EIO;return -1; }
        if (!strcmp(case_name,"write-short") && length>1) length=1;
    }
    return write(fd,data,length);
}
static int EnsureParentDirs(const char *);
#ifdef RESTORE_BASELINE
static int RestoreObject(Widget,int,char *,char *,Boolean,FileOperationErrorProc,Boolean,int,Boolean);
#else
static int RestoreObject(Widget,int,char *,char *,Boolean,FileOperationErrorProc,Boolean,int,RestoreReplacePolicy);
static int RestoreDirectoryContains(const struct stat *,const char *);
static int RestoreReadConflict(int,RestoreFromTrashCBData *);
static void RestoreUpdateDestination(const char *);
static Boolean RestoreFromTrash(char **,int,char *,char *,Tt_message,RestoreTargetIntent *);
static void RestoreVerifyOk(Widget,XtPointer,XtPointer);
static void RestoreVerifyCancel(Widget,XtPointer,XtPointer);
#endif
static int WriteEntry(FILE *,String,String,String,unsigned long long,Boolean);
static Boolean WriteTrashEntries(void);
#define strdup RestoreStrdup
#define ResolveLocalPathName RestoreResolve
#define write RestoreWrite
#ifndef RESTORE_BASELINE
#define pipe RestorePipe
#define fork RestoreFork
#define DirectoryBusy RestoreDirectoryBusy
#undef XtRealloc
#define XtRealloc(old,bytes) RestoreRealloc(old,bytes)
#endif
#include "restore-production.inc"
#undef strdup
#undef ResolveLocalPathName
#undef write
#undef pipe
#undef fork
#undef DirectoryBusy

static char *absolute(const char *tail)
{
    char cwd[PATH_MAX];fixture(getcwd(cwd,sizeof(cwd))!=NULL);
    size_t n=strlen(cwd)+strlen(tail)+2;char *p=malloc(n);fixture(p!=NULL);
    snprintf(p,n,"%s/%s",cwd,tail);return p;
}
static void setup_rows(void)
{
    fixture(mkdir("trash",0700)==0 && mkdir("original",0700)==0 && mkdir("destination",0700)==0);
    trashCan=calloc(2,sizeof(*trashCan));fixture(trashCan!=NULL);numTrashItems=2;
    for (int i=0;i<2;++i) {
        TrashEntry *entry=&trashCan[i];const char *name=i?"second":"first";
        entry->filename=strdup(name);entry->intNew=absolute(i?"trash/second":"trash/first");
        entry->intOrig=absolute(i?"original/second":"original/first");entry->external=strdup(entry->intOrig);
        fixture(entry->filename && entry->external);
        put(entry->intNew,payload,sizeof(payload));
    }
    fixture(WriteTrashEntries());
}
static void cleanup_rows(void)
{
    for (int i=0;i<numTrashItems;++i) {
        free(trashCan[i].intNew);free(trashCan[i].intOrig);free(trashCan[i].external);
        free(trashCan[i].filename);free(trashCan[i].deletionDate);
    }
    free(trashCan);
}
static void check_ledger(void)
{
    FILE *stream=fopen(TrashInfoFileName,"r");fixture(stream!=NULL);
    for (int i=0;i<numTrashItems;++i) {
        DtfileTrashMetadata row={0};
        CHECK(DtfileReadTrashMetadata(stream,&row)==DTFILE_TRASH_RECORD);
        CHECK(row.filename && !strcmp(row.filename,trashCan[i].filename));
        CHECK(row.external && !strcmp(row.external,trashCan[i].external));
        DtfileFreeTrashMetadata(&row);
    }
    DtfileTrashMetadata row={0};CHECK(DtfileReadTrashMetadata(stream,&row)==DTFILE_TRASH_END);
    DtfileFreeTrashMetadata(&row);fixture(fclose(stream)==0);
}
#ifndef RESTORE_BASELINE
static void process_case(void)
{
    setup_rows();
    int channel[2], results[2]={0,0}, wire[2]={99,99};fixture(pipe(channel)==0);
    char *directory=absolute("destination");
    char *names[2]={"first","second"};
    int success=0;
    if (!strcmp(case_name,"resolve-error")) resolve_target_error=1;
    else if (!strcmp(case_name,"resolve-null")) resolver_null=1;
    else if (!strcmp(case_name,"restore-parents")) {
        free(trashCan[0].intOrig);trashCan[0].intOrig=absolute("missing/a/b/restored");
        free(trashCan[0].external);trashCan[0].external=strdup(trashCan[0].intOrig);
        free(directory);directory=NULL;success=1;
    } else if (!strcmp(case_name,"restore-relative") || !strcmp(case_name,"restore-host")) {
        free(trashCan[0].intOrig);
        trashCan[0].intOrig=strdup(strstr(case_name,"host")?"legacy-host:/old/first":"old/first");
        fixture(trashCan[0].intOrig!=NULL);free(directory);directory=NULL;
    } else if (!strcmp(case_name,"known-dnd") || !strcmp(case_name,"known-dnd-trailing") ||
               !strcmp(case_name,"known-dnd-relative") || !strcmp(case_name,"known-dnd-host")) {
        if (strstr(case_name,"trailing")) {
            free(trashCan[0].intOrig);trashCan[0].intOrig=absolute("original/native-\\\n-\303\251///");
        } else if (strstr(case_name,"relative") || strstr(case_name,"host")) {
            free(trashCan[0].intOrig);
            trashCan[0].intOrig=strdup(strstr(case_name,"host")?
                "legacy-host:/old/native-\\\n\t-\303\251":"old/native-\\\n\t-\303\251");
            fixture(trashCan[0].intOrig!=NULL);
            free(trashCan[0].external);trashCan[0].external=strdup(trashCan[0].intOrig);
            fixture(trashCan[0].external!=NULL);
        }
        success=1;
    } else if (!strcmp(case_name,"known-dnd-occupied")) {
        put("destination/first","keep destination",16);
        put("original/first","keep original",13);
    } else if (!strncmp(case_name,"orphan-",7)) {
        put("orphan",payload,sizeof(payload));
        names[0]=!strcmp(case_name,"orphan-relative")?strdup("orphan"):absolute("orphan");
        if (!strcmp(case_name,"orphan-occupied")) put("destination/orphan","keep",4);
        success=!strcmp(case_name,"orphan-absolute");
    }
    int count=(!strcmp(case_name,"resolve-error") || !strcmp(case_name,"resolve-null"))?2:1;
    RestoreProcess(channel[1],names,count,"local",directory,results,NULL);
    fixture(close(channel[1])==0);
    short tag=-1;fixture(PipeRead(channel[0],&tag,sizeof(tag))==sizeof(tag));
    int errors=0;
    RestoreFromTrashCBData frames={0};frames.file_count=count;
    frames.conflicts=calloc((size_t)count,sizeof(*frames.conflicts));fixture(frames.conflicts!=NULL);
    while (tag==PIPEMSG_FILEOP_ERROR || tag==PIPEMSG_RESTORE_CONFLICT) {
        if (tag==PIPEMSG_RESTORE_CONFLICT) {
            fixture(RestoreReadConflict(channel[0],&frames)==0);
            fixture(PipeRead(channel[0],&tag,sizeof(tag))==sizeof(tag));continue;
        }
        int error;fixture(PipeRead(channel[0],&error,sizeof(error))==sizeof(error));
        char *text=PipeReadString(channel[0]), *arg=PipeReadString(channel[0]);
        CHECK(error<0 && text && *text);free(text);free(arg);++errors;
        fixture(PipeRead(channel[0],&tag,sizeof(tag))==sizeof(tag));
    }
    fixture(PipeRead(channel[0],wire,count*(int)sizeof(int))==count*(int)sizeof(int));
    CHECK(tag==PIPEMSG_DONE && !memcmp(wire,results,(size_t)count*sizeof(int)));
    int conflict=!strcmp(case_name,"known-dnd-occupied") || !strcmp(case_name,"orphan-occupied");
    CHECK(results[0]==(success?0:conflict?SKIP_FILE:-1));if (count==2) CHECK(results[1]==-1);
    if (conflict) {
        const char *expected_source=!strncmp(case_name,"orphan-",7)?names[0]:trashCan[0].intNew;
        char *expected_target=RestoreDestination(directory,!strncmp(case_name,"orphan-",7)?names[0]:trashCan[0].intOrig);
        CHECK(frames.conflicts[0] && !strcmp(frames.conflicts[0]->source,expected_source) &&
              !strcmp(frames.conflicts[0]->target,expected_target));free(expected_target);
    }
    for (int i=0;i<count;++i) RestoreFreeConflict(frames.conflicts[i]);free(frames.conflicts);
    unsigned char end;CHECK(read(channel[0],&end,1)==0);fixture(close(channel[0])==0);
    if (success) {
        char *target=directory?RestoreDestination(directory,
            !strncmp(case_name,"orphan-",7)?names[0]:trashCan[0].intOrig):strdup(trashCan[0].intOrig);
        CHECK(target && exact(target,payload,sizeof(payload)));
        CHECK(absent(!strncmp(case_name,"orphan-",7)?names[0]:trashCan[0].intNew));free(target);
    } else {
        CHECK(exact(trashCan[0].intNew,payload,sizeof(payload)));
        if (!strncmp(case_name,"orphan-",7)) CHECK(exact("orphan",payload,sizeof(payload)));
        if (!strcmp(case_name,"orphan-occupied")) CHECK(exact("destination/orphan","keep",4));
        if (!strcmp(case_name,"known-dnd-occupied")) {
            CHECK(exact("destination/first","keep destination",16));
            CHECK(exact("original/first","keep original",13));
        }
    }
    CHECK(exact(trashCan[1].intNew,payload,sizeof(payload)));
    if (!strcmp(case_name,"restore-relative") || !strcmp(case_name,"restore-host")) CHECK(errors>0);
    if (!strcmp(case_name,"orphan-relative")) CHECK(link_calls==0);
    if (!strncmp(case_name,"orphan-",7)) free(names[0]);
    free(directory);cleanup_rows();
}
static void path_case(void)
{
    if (!strcmp(case_name,"invalid-leaf")) {
        const char *bad[]={"", "/", "///", "/a/.", "/a/..///", ".", ".."};
        for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
            char *target=RestoreDestination("/destination",bad[i]);CHECK(target==NULL);free(target);
        }
        return;
    }
    if (!strcmp(case_name,"long-destination")) {
        char *directory=malloc(20001);fixture(directory!=NULL);
        memset(directory,'a',20000);directory[0]='/';directory[20000]=0;
        char *target=RestoreDestination(directory,"/original/leaf///");
        CHECK(target && strlen(target)==20005 && !memcmp(target,directory,20000) && !strcmp(target+20000,"/leaf"));
        free(target);free(directory);return;
    }
    if (!strcmp(case_name,"parent-root")) {
        setup_rows();
        /* The existing /bin name prevents execution of FileManip. Only the
         * derived parent and the non-mutating permission check are observed. */
        int result=RestoreObject(NULL,MOVE_FILE,trashCan[0].intNew,"/bin",True,Error,False,NOT_DESKTOP,False);
        CHECK(result==False || result==SKIP_FILE);
        CHECK(permission_checks==1 && !strcmp(permission_parent,"/"));
        CHECK(exact(trashCan[0].intNew,payload,sizeof(payload)));
        cleanup_rows();return;
    }
    setup_rows();
    char *target=absolute("new/a/b/restored");int success=0, conflict=0;
    if (!strcmp(case_name,"object-oom")) restore_strdup_fail=1;
    else if (!strcmp(case_name,"parent-oom")) restore_strdup_fail=2;
    else if (!strcmp(case_name,"parent-file")) { put("new","block",5); }
    else if (!strcmp(case_name,"checked-restore")) {
        free(target);target=absolute("destination/restored");success=1;
    } else if (!strcmp(case_name,"dangling-conflict")) {
        free(target);target=absolute("destination/restored");fixture(symlink("absent",target)==0);conflict=1;
    }
    int result=RestoreObject(NULL,MOVE_FILE,trashCan[0].intNew,target,False,Error,False,NOT_DESKTOP,
                             success?RESTORE_REPLACE_APPROVED:RESTORE_NO_REPLACE);
    CHECK(result==(success?True:conflict?SKIP_FILE:False));
    if (success) CHECK(absent(trashCan[0].intNew) && exact(target,payload,sizeof(payload)));
    else CHECK(exact(trashCan[0].intNew,payload,sizeof(payload)));
    if (conflict) {
        char *parent=absolute("destination");CHECK(permission_checks==1 && !strcmp(parent,permission_parent));free(parent);
        struct stat info;CHECK(lstat(target,&info)==0 && S_ISLNK(info.st_mode));
    }
    free(target);cleanup_rows();
}
static void write_case(void)
{
    int channel[2], results[]={-1,SKIP_FILE};fixture(pipe(channel)==0);pipe_write_fd=channel[1];
    struct sigaction before,after;fixture(sigaction(SIGPIPE,NULL,&before)==0);
    if (!strcmp(case_name,"write-closed")) fixture(close(channel[0])==0);
    int result=RestoreWriteResults(channel[1],results,2),saved=errno;
    fixture(sigaction(SIGPIPE,NULL,&after)==0);CHECK(before.sa_handler==after.sa_handler);
    int success=!strcmp(case_name,"write-short") || !strcmp(case_name,"write-eintr");
    CHECK(result==(success?0:-1));
    if (!success) CHECK(saved==(!strcmp(case_name,"write-closed")?EPIPE:EIO));
    fixture(close(channel[1])==0);pipe_write_fd=-1;
    if (strcmp(case_name,"write-closed")) {
        unsigned char wire[64];size_t n=drain(channel[0],wire,sizeof(wire));
        if (success) {
            short tag;int received[2];CHECK(n==sizeof(tag)+sizeof(received));
            if (n==sizeof(tag)+sizeof(received)) {
                memcpy(&tag,wire,sizeof(tag));memcpy(received,wire+sizeof(tag),sizeof(received));
                CHECK(tag==PIPEMSG_DONE && !memcmp(results,received,sizeof(results)));
            }
        }
    }
}
static void callback_case(void)
{
    setup_rows();
    RestoreFromTrashCBData *data=calloc(1,sizeof(*data));fixture(data!=NULL);
    data->file_count=2;data->file_list=calloc(2,sizeof(char *));data->rc=calloc(2,sizeof(int));
    data->conflicts=calloc(2,sizeof(*data->conflicts));fixture(data->conflicts!=NULL);
    fixture(data->file_list && data->rc);
    data->file_list[0]=strdup("first");data->file_list[1]=strdup("second");
    int results[2]={-1,-1};
    int completed_first=!strcmp(case_name,"callback-mixed") || !strcmp(case_name,"callback-read-eintr");
    if (completed_first) {
        results[0]=RestoreObject(NULL,MOVE_FILE,trashCan[0].intNew,trashCan[0].intOrig,True,Error,False,NOT_DESKTOP,False)==True?0:-1;
        fixture(results[0]==0);
    } else if (!strcmp(case_name,"callback-known-skip")) {
        put(trashCan[0].intOrig,"keep",4);
        results[0]=RestoreObject(NULL,MOVE_FILE,trashCan[0].intNew,trashCan[0].intOrig,True,Error,False,NOT_DESKTOP,False);
        fixture(results[0]==SKIP_FILE);
    } else if (!strcmp(case_name,"callback-orphan-skip")) {
        free(data->file_list[0]);data->file_list[0]=absolute("orphan");put("orphan",payload,sizeof(payload));
        put("destination/orphan","keep",4);char *target=absolute("destination/orphan");
        results[0]=RestoreObject(NULL,MOVE_FILE,data->file_list[0],target,False,Error,False,NOT_DESKTOP,RESTORE_NO_REPLACE);
        fixture(results[0]==SKIP_FILE);free(target);
    }
    int channel[2];fixture(pipe(channel)==0);short tag=PIPEMSG_DONE;
    if (results[0]==SKIP_FILE) {
        char *source=!strcmp(case_name,"callback-orphan-skip")?absolute("orphan"):strdup(trashCan[0].intNew);
        char *target=!strcmp(case_name,"callback-orphan-skip")?absolute("destination/orphan"):strdup(trashCan[0].intOrig);
        fixture(RestoreWriteConflict(channel[1],0,source,target)==0);free(source);free(target);
    }
    if (!strcmp(case_name,"callback-short-tag")) fixture(write(channel[1],&tag,1)==1);
    else {
        if (!strcmp(case_name,"callback-unknown-tag")) tag=123;
        if (!strcmp(case_name,"callback-error-then-done")) FileOpError((Widget)(intptr_t)channel[1],"reported error","first");
        fixture(write(channel[1],&tag,sizeof(tag))==sizeof(tag));
        if (!strcmp(case_name,"callback-short-results")) { int zero=0;fixture(write(channel[1],&zero,sizeof(zero))==sizeof(zero)); }
        else {
            if (!strcmp(case_name,"callback-invalid-result")) { results[0]=0;results[1]=123; }
            fixture(write(channel[1],results,sizeof(results))==sizeof(results));
        }
    }
    fixture(close(channel[1])==0);
    const char *saved_name=case_name;
    read_calls=0;
    if (!strcmp(case_name,"callback-read-eintr")) { source_fd=channel[0];case_name="copy-read-eintr"; }
    if (!strcmp(case_name,"callback-read-error")) { source_fd=channel[0];case_name="move-read-error"; }
    XtInputId input=1;
    for (unsigned iteration=0;input_removals==0 && iteration<4;++iteration) {
        RestorePipeCB(data,&channel[0],&input);
        if (!input_removals) CHECK(confirmations==0 && numTrashItems==2);
    }
    case_name=saved_name;
    if (!strcmp(case_name,"callback-read-eintr") || !strcmp(case_name,"callback-read-error")) CHECK(injected>0);
    CHECK(input_removals==1);
    CHECK(numTrashItems==(completed_first?1:2));
    CHECK(confirmations==((!strcmp(case_name,"callback-known-skip") || !strcmp(case_name,"callback-orphan-skip"))?1:0));
    if (!strcmp(case_name,"callback-known-skip")) CHECK(exact("original/first","keep",4));
    if (!strcmp(case_name,"callback-orphan-skip")) CHECK(exact("orphan",payload,sizeof(payload)));
    if (!completed_first) CHECK(exact("trash/first",payload,sizeof(payload)));
    else CHECK(exact("original/first",payload,sizeof(payload)) && absent("trash/first"));
    CHECK(exact("trash/second",payload,sizeof(payload)));
    if (dialog_box) RestoreVerifyCancel(NULL,dialog_box,NULL);
    check_ledger();cleanup_rows();
}
static int ignore_erase_error(char *path,int error) { (void)path;(void)error;return 0; }
static void replacement_case(void)
{
    setup_rows();char *source=strdup(trashCan[0].intNew),*target=absolute("destination/first");
    fixture(source && target);put(target,"old target",10);
    int success=!strcmp(case_name,"replace-exact") || !strcmp(case_name,"replace-target-gone");
    if (!strcmp(case_name,"replace-missing-source")) fixture(unlink(source)==0);
    if (!strcmp(case_name,"replace-target-gone")) fixture(unlink(target)==0);
    if (!strcmp(case_name,"replace-hardlink")) { fixture(unlink(target)==0);fixture(link(source,target)==0); }
    if (!strcmp(case_name,"replace-ancestor")) { free(target);target=absolute("trash"); }
    if (!strcmp(case_name,"replace-descendant")) {
        fixture(unlink(source)==0 && mkdir(source,0700)==0);
        size_t n=strlen(source)+16;free(target);target=malloc(n);fixture(target!=NULL);
        snprintf(target,n,"%s/child",source);put(target,"keep child",10);
    }
    if (!strcmp(case_name,"replace-erase-error") || !strcmp(case_name,"replace-ignored-erase")) erase_fault=1;
    if (!strcmp(case_name,"replace-ignored-erase")) errorCallback=ignore_erase_error;
    int result=RestoreObject(NULL,MOVE_FILE,source,target,False,Error,False,NOT_DESKTOP,RESTORE_REPLACE_APPROVED);
    CHECK(result==(success?True:False));
    if (success) { CHECK(absent(source));CHECK(exact(target,payload,sizeof(payload))); }
    else if (!strcmp(case_name,"replace-descendant")) CHECK(exact(target,"keep child",10));
    else {
        if (strcmp(case_name,"replace-missing-source")) CHECK(exact(source,payload,sizeof(payload)));
        if (!strcmp(case_name,"replace-hardlink")) CHECK(exact(target,payload,sizeof(payload)));
        else if (strcmp(case_name,"replace-ancestor")) CHECK(exact(target,"old target",10));
        CHECK(exact("trash/second",payload,sizeof(payload)));
    }
    errorCallback=NULL;free(source);free(target);cleanup_rows();
}
static RestoreFromTrashCBData *callback_request(void)
{
    RestoreFromTrashCBData *request=calloc(1,sizeof(*request));fixture(request!=NULL);
    request->file_count=2;request->file_list=calloc(2,sizeof(char *));request->rc=calloc(2,sizeof(int));
    request->conflicts=calloc(2,sizeof(*request->conflicts));fixture(request->file_list && request->rc && request->conflicts);
    request->file_list[0]=strdup("first");request->file_list[1]=strdup("second");return request;
}
static void conflict_case(void)
{
    setup_rows();put("original/first","keep",4);
    RestoreFromTrashCBData *request=callback_request();int channel[2];fixture(pipe(channel)==0);
    short tag=PIPEMSG_RESTORE_CONFLICT;uint32_t fields[3]={0,0,0};
    const char *source=trashCan[0].intNew,*target=trashCan[0].intOrig;
    int results[2]={SKIP_FILE,-1};
    if (!strcmp(case_name,"conflict-missing")) { results[0]=SKIP_FILE; }
    else if (!strcmp(case_name,"conflict-duplicate") || !strcmp(case_name,"conflict-extra")) {
        fixture(RestoreWriteConflict(channel[1],0,source,target)==0);
        if (!strcmp(case_name,"conflict-duplicate")) fixture(RestoreWriteConflict(channel[1],0,source,target)==0);
        else results[0]=0; /* Adversarial success: must be rejected, never executed. */
    } else {
        fields[1]=(uint32_t)strlen(source);fields[2]=(uint32_t)strlen(target);
        if (!strcmp(case_name,"conflict-index")) fields[0]=2;
        if (!strcmp(case_name,"conflict-oversize")) fields[1]=RESTORE_PATH_LIMIT+1;
        fixture(write(channel[1],&tag,sizeof(tag))==sizeof(tag));
        fixture(write(channel[1],fields,sizeof(fields))==sizeof(fields));
        if (strcmp(case_name,"conflict-short") && fields[1]<=RESTORE_PATH_LIMIT) {
            char *sent=strdup(source);fixture(sent!=NULL);
            if (!strcmp(case_name,"conflict-nul")) sent[1]=0;
            if (!strcmp(case_name,"conflict-relative")) sent[0]='x';
            fixture(write(channel[1],sent,fields[1])==(ssize_t)fields[1]);
            fixture(write(channel[1],target,fields[2])==(ssize_t)fields[2]);free(sent);
        }
    }
    if (strcmp(case_name,"conflict-short")) fixture(RestoreWriteResults(channel[1],results,2)==0);
    fixture(close(channel[1])==0);XtInputId input=1;
    for (unsigned i=0;input_removals==0 && i<4;++i) RestorePipeCB(request,&channel[0],&input);
    CHECK(input_removals==1 && confirmations==0 && numTrashItems==2);
    CHECK(exact("trash/first",payload,sizeof(payload)) && exact("trash/second",payload,sizeof(payload)));
    CHECK(exact("original/first","keep",4));check_ledger();cleanup_rows();
}
static void pump_admission(void)
{
    fixture(input_active && admitted_request!=NULL);
    pid_t child=admitted_request->child;
    XtInputId input=1;
    while (input_active) admitted_callback(admitted_request,&admitted_fd,&input);
    int status;fixture(waitpid(child,&status,0)==child);CHECK(WIFEXITED(status) && WEXITSTATUS(status)==0);
    admitted_request=NULL;admitted_fd=-1;
}
static void dialog_case(void)
{
    setup_rows();put("destination/reviewed","old target",10);
    RestoreConflict *conflict=calloc(1,sizeof(*conflict));fixture(conflict!=NULL);
    conflict->source=strdup(trashCan[0].intNew);conflict->target=absolute("destination/reviewed");
    char *expected=strdup(conflict->target);fixture(conflict->source && expected);
    CreateRestoreDialog(conflict);fixture(dialog_box!=NULL);
    CHECK(dialog_box->parent->delete_response==XmDESTROY && dialog_box->data==conflict);
    CHECK(dialog_box->managed && !dialog_box->auto_unmanage && dialog_box->warning && dialog_box->help_hidden);
    CHECK(dialog_box->parent->functions==(MWM_FUNC_MOVE|MWM_FUNC_CLOSE) &&
          dialog_box->parent->decorations==(MWM_DECOR_BORDER|MWM_DECOR_TITLE|MWM_DECOR_MENU));
    if (!strcmp(case_name,"dialog-cancel")) RestoreVerifyCancel(NULL,dialog_box,NULL);
    else if (!strcmp(case_name,"dialog-wm-close")) XtDestroyWidget(dialog_box->parent);
    else {
        /* Confirmation must not recompute a target from changed metadata. */
        free(trashCan[0].intOrig);trashCan[0].intOrig=absolute("original/changed-after-review");
        if (!strcmp(case_name,"dialog-admission-failure") || !strcmp(case_name,"dialog-retry")) TrashInitialized=False;
        if (!strcmp(case_name,"dialog-pipe-failure")) spawn_fault=1;
        if (!strcmp(case_name,"dialog-fork-failure")) spawn_fault=2;
        if (!strcmp(case_name,"dialog-stage-oom")) { restore_strdup_calls=0;restore_strdup_fail=1; }
        if (!strcmp(case_name,"dialog-source-gone")) fixture(unlink(conflict->source)==0);
        if (!strcmp(case_name,"dialog-erase-error")) erase_fault=1;
        RestoreVerifyOk(NULL,dialog_box,NULL);
        if (!strcmp(case_name,"dialog-confirm") || !strcmp(case_name,"dialog-source-gone") ||
            !strcmp(case_name,"dialog-erase-error")) {
            CHECK(dialog_box==NULL && destroyed_dialogs==1 && input_active);
            pump_admission();
            if (!strcmp(case_name,"dialog-confirm")) {
                CHECK(numTrashItems==1 && exact(expected,payload,sizeof(payload)));
                CHECK(absent("trash/first") && absent("original/changed-after-review"));
                char *parent=absolute("destination");CHECK(!strcmp(parent,updated_directory));free(parent);
            } else CHECK(numTrashItems==2 && exact(expected,"old target",10));
        } else {
            CHECK(dialog_box && dialog_box->managed && destroyed_dialogs==0 && !input_active);
            CHECK(conflict==dialog_box->data && !strcmp(conflict->target,expected));
            CHECK(exact(expected,"old target",10) && exact("trash/first",payload,sizeof(payload)));
            if (spawn_fault==2) {
                CHECK(fcntl(staged_pipe[0],F_GETFD)==-1 && errno==EBADF);
                CHECK(fcntl(staged_pipe[1],F_GETFD)==-1 && errno==EBADF);
            }
            if (!strcmp(case_name,"dialog-retry")) {
                TrashInitialized=True;RestoreVerifyOk(NULL,dialog_box,NULL);
                CHECK(dialog_box==NULL && input_active && destroyed_dialogs==1);pump_admission();
                CHECK(numTrashItems==1 && exact(expected,payload,sizeof(payload)) && absent("trash/first"));
            } else RestoreVerifyCancel(NULL,dialog_box,NULL);
        }
    }
    CHECK(dialog_box==NULL && destroyed_dialogs==1);
    if (strcmp(case_name,"dialog-confirm") && strcmp(case_name,"dialog-retry")) {
        CHECK(numTrashItems==2);
        CHECK(exact(expected,"old target",10));
        CHECK(!strcmp(case_name,"dialog-source-gone")?absent("trash/first"):exact("trash/first",payload,sizeof(payload)));
    }
    check_ledger();
    free(expected);cleanup_rows();
}
static void invalid_count_case(void)
{
    /* Deliberately provide exactly one valid owned slot. Walking a claimed
     * larger count crosses the allocation; freeing even slot zero violates
     * the rejection contract and is caught by the checks/caller cleanup. */
    char **files=(char **)XtCalloc(1,sizeof(*files));
    RestoreTargetIntent *intents=calloc(1,sizeof(*intents));
    char *host=XtNewString("caller-owned-host");
    fixture(files && intents && host);
    files[0]=XtNewString("/caller-owned-source");
    intents[0].target=strdup("/caller-owned-target");
    intents[0].replacement=RESTORE_REPLACE_APPROVED;
    fixture(files[0] && intents[0].target);
    char *source=files[0],*target=intents[0].target;
    int count=!strcmp(case_name,"admission-count-zero")?0:
              !strcmp(case_name,"admission-count-negative")?-1:INT_MAX;
    errno=0;
    Boolean accepted=RestoreFromTrash(files,count,host,NULL,NULL,intents);
    int saved=errno;
    CHECK(!accepted && saved==EINVAL && !input_active && staged_pipe[0]==-1 && staged_pipe[1]==-1);
    CHECK(files[0]==source && intents[0].target==target && intents[0].replacement==RESTORE_REPLACE_APPROVED);
    CHECK(!strcmp(source,"/caller-owned-source") && !strcmp(target,"/caller-owned-target"));
    CHECK(!strcmp(host,"caller-owned-host"));
    /* Cleanup uses the actual capacity one, never the rejected count. */
    XtFree(files[0]);XtFree((char *)files);XtFree(host);
    free(intents[0].target);free(intents);
}
static void append_case(void)
{
    char **files=NULL,*file=XtNewString("new caller-owned entry");fixture(file!=NULL);
    int count=0;
    if (!strcmp(case_name,"append-empty-success")) {
        CHECK(TrashAppendMessageFile(&files,&count,file));
        CHECK(count==1 && files && files[0]==file && append_allocations==1);
        XtFree(files[0]);XtFree((char *)files);return;
    }
    files=(char **)XtCalloc(1,sizeof(*files));fixture(files!=NULL);
    files[0]=XtNewString("existing entry");fixture(files[0]!=NULL);
    char **original=files;char *existing=files[0];
    count=!strcmp(case_name,"append-count-negative")?-1:
          !strcmp(case_name,"append-count-overflow")?INT_MAX:1;
    int before=count;
    if (!strcmp(case_name,"append-allocation-failure")) append_fail_at=1;
    errno=0;Boolean appended=TrashAppendMessageFile(&files,&count,file);int saved=errno;
    CHECK(!appended && saved==(append_fail_at?ENOMEM:EINVAL));
    CHECK(files==original && count==before && files[0]==existing);
    CHECK(!strcmp(existing,"existing entry") && !strcmp(file,"new caller-owned entry"));
    CHECK(append_allocations==(append_fail_at?1:0));
    /* Failure does not consume the candidate file or the existing array. */
    XtFree(file);XtFree(files[0]);XtFree((char *)files);
}
static void batch_message_case(void)
{
    char **files=NULL; int count=99;
    Tt_message messages[2]={(Tt_message)(uintptr_t)1,(Tt_message)(uintptr_t)2};
    tt_directory="/source/file";tt_argc=0;
    if (!strcmp(case_name,"batch-message-failure")) append_fail_at=4;
    Boolean parsed=MessagesToFileList(messages,
        !strcmp(case_name,"batch-message-invalid")?-1:2,&files,&count);
    if (!strcmp(case_name,"batch-message-files")) {
        CHECK(parsed && count==2 && files);
        if (files && count==2) CHECK(!strcmp(files[0],tt_directory) && !strcmp(files[1],tt_directory));
    } else CHECK(!parsed && files==NULL && count==0);
    for (unsigned i=0;i<sizeof(tt_owned)/sizeof(tt_owned[0]);++i) CHECK(tt_owned[i]==NULL);
    for (int i=0;i<count;++i) XtFree(files[i]);XtFree((char *)files);
}
static void message_case(void)
{
    char **files=NULL;int count=99;
    if (!strcmp(case_name,"message-missing-directory")) {
        tt_directory=NULL;tt_argc=2;tt_values[0]="first second";tt_values[1]="third with space";
    } else if (!strcmp(case_name,"message-append-failure")) {
        tt_directory="/source";tt_argc=1;tt_values[0]="first second";append_fail_at=2;
    }
    if (!strcmp(case_name,"message-count-error")) tt_error_case=1;
    if (!strcmp(case_name,"message-file-error")) tt_error_case=2;
    Boolean parsed=MessageToFileList((Tt_message)(uintptr_t)1,&files,&count);
    CHECK(parsed==(!append_fail_at && !tt_error_case));
    if (!strcmp(case_name,"message-missing-directory")) {
        CHECK(count==3 && files);
        if (files && count==3) {
            CHECK(!strcmp(files[0],"first") && !strcmp(files[1],"second") && !strcmp(files[2],"third with space"));
        }
        CHECK(tt_allocations==2 && tt_frees==2);
    } else {
        CHECK(count==0 && files==NULL);
        if (append_fail_at) CHECK(append_allocations==2 && tt_allocations==2 && tt_frees==2);
        else CHECK(append_allocations==0 && tt_allocations==0 && tt_frees==0);
    }
    for (unsigned i=0;i<sizeof(tt_owned)/sizeof(tt_owned[0]);++i) CHECK(tt_owned[i]==NULL);
    for (int i=0;i<count;++i) XtFree(files[i]);XtFree((char *)files);
}
#else
/* ABI adapter for the unchanged e021 bodies. These focused controls execute
 * old behavior; they neither introduce missing helpers nor change a result. */
static void baseline_case(void)
{
    setup_rows();int channel[2];fixture(pipe(channel)==0);int results[2]={0,0};
    if (!strcmp(case_name,"baseline-short-done")) {
        RestoreFromTrashCBData *request=calloc(1,sizeof(*request));fixture(request!=NULL);
        request->file_count=2;request->file_list=calloc(2,sizeof(char *));request->rc=calloc(2,sizeof(int));
        fixture(request->file_list && request->rc);request->file_list[0]=strdup("first");request->file_list[1]=strdup("second");
        short tag=PIPEMSG_DONE;int zero=0;fixture(write(channel[1],&tag,sizeof(tag))==sizeof(tag));
        fixture(write(channel[1],&zero,sizeof(zero))==sizeof(zero));fixture(close(channel[1])==0);
        XtInputId input=1;RestorePipeCB(request,&channel[0],&input);
        CHECK(numTrashItems==2);CHECK(exact("trash/first",payload,sizeof(payload)));
    } else {
        char *directory=absolute("destination"),*names[]={"first","second"};int count=1;
        if (!strcmp(case_name,"baseline-resolve-array")) { resolve_target_error=1;count=2; }
        if (!strcmp(case_name,"baseline-missing-parent")) {
            free(directory);directory=NULL;free(trashCan[0].intOrig);trashCan[0].intOrig=absolute("missing/a/b/restored");
        }
        RestoreProcess(channel[1],names,count,"local",directory,results,False);fixture(close(channel[1])==0);
        short tag;int received[2]={99,99};CHECK(PipeRead(channel[0],&tag,sizeof(tag))==sizeof(tag));
        CHECK(PipeRead(channel[0],received,count*(int)sizeof(int))==count*(int)sizeof(int));
        CHECK(tag==PIPEMSG_DONE);
        if (resolve_target_error) CHECK(results[0]==-1 && results[1]==-1 && received[0]==-1 && received[1]==-1);
        else {
            CHECK(results[0]==0 && absent(trashCan[0].intNew));
            char *target=directory?absolute("destination/first"):strdup(trashCan[0].intOrig);
            CHECK(exact(target,payload,sizeof(payload)));free(target);
        }
        fixture(close(channel[0])==0);free(directory);
    }
    cleanup_rows();
}
#endif
int main(int argc,char **argv)
{
    fixture(argc==3);case_name=argv[1];fixture(chdir(argv[2])==0);
    for (size_t i=0;i<sizeof(payload);++i) payload[i]=(unsigned char)(i%251);
#ifdef RESTORE_BASELINE
    baseline_case();
#else
    if (!strncmp(case_name,"append-",7)) append_case();
    else if (!strncmp(case_name,"batch-message-",14)) batch_message_case();
    else if (!strncmp(case_name,"message-",8)) message_case();
    else if (!strncmp(case_name,"admission-count-",16)) invalid_count_case();
    else if (!strncmp(case_name,"replace-",8)) replacement_case();
    else if (!strncmp(case_name,"conflict-",9)) conflict_case();
    else if (!strncmp(case_name,"dialog-",7)) dialog_case();
    else if (!strncmp(case_name,"callback-",9)) callback_case();
    else if (!strncmp(case_name,"write-",6)) write_case();
    else if (!strncmp(case_name,"resolve-",8) || !strncmp(case_name,"restore-",8) ||
             !strncmp(case_name,"known-",6) || !strncmp(case_name,"orphan-",7)) process_case();
    else path_case();
#endif
    if (!failed) printf("PASS %s\n",case_name);
    return failed?1:0;
}
