/* SPDX-License-Identifier: LGPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include "SyncJob.h"
#include "SyncPrivateStorage.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <limits.h>

enum CommandKind { NO_COMMAND, PAGE_COMMAND, REVIEW_COMMAND, EXECUTE_COMMAND };
typedef struct {
    enum CommandKind kind;
    uint64_t serial, offset, generation, receipt, rename_row;
    unsigned count, deletion_count;
    SyncRule rules[SYNC_MAX_RULES];
    uint64_t deletions[SYNC_PAGE_SIZE];
    char *rename;
    uint32_t rename_side;
} Command;
struct SyncJob {
    pthread_mutex_t mutex;
    atomic_int stop, cancel, pause;
    SyncView view;
    Command *command;
    uint64_t next_command;
    int action_busy;
    char *source, *destination, *home;
    uint32_t profile, metadata_scope;
    ckfsp_filters filters;
};
static void command_free(Command *c)
{
    if (!c) return;
    for (unsigned i=0; i<c->count; ++i) free(c->rules[i].subtree);
    free(c->rename); free(c);
}
static uint64_t submit(SyncJob *s, Command *c)
{
    if (!c) return 0;
    if (pthread_mutex_trylock(&s->mutex)) { command_free(c); return 0; }
    uint64_t accepted=0;
    if (!s->command && !s->view.stopped && !atomic_load(&s->stop) &&
        (c->kind==PAGE_COMMAND || !s->action_busy) && s->next_command<UINT64_MAX) {
        accepted=c->serial=++s->next_command; s->command=c;
        if (c->kind!=PAGE_COMMAND) s->action_busy=1;
    }
    pthread_mutex_unlock(&s->mutex);
    if (!accepted) command_free(c);
    return accepted;
}
static void publish(SyncJob *s, SyncView *v);
static void action_complete(SyncJob *s, SyncView *v, uint64_t serial, ckfe_api_status api, unsigned accepted)
{
    v->completed_submission=serial; v->command_api=api; v->command_accepted=accepted;
    publish(s,v);
    pthread_mutex_lock(&s->mutex); s->action_busy=0; pthread_mutex_unlock(&s->mutex);
}
static void publish(SyncJob *s, SyncView *v)
{
    pthread_mutex_lock(&s->mutex);
    v->revision=s->view.revision+1;
    s->view=*v;
    pthread_mutex_unlock(&s->mutex);
}
static int terminal(uint32_t state) { return state>=CKFET_COMPLETE; }
static void failure(SyncView *v, ckfe_api_status api, const ckfe_error *error)
{
    v->api=api;
    char native[128]="";
    if (error && error->domain==CKFE_ERROR_POSIX && error->code>0 && error->code<=INT_MAX &&
        strerror_r((int)error->code,native,sizeof(native))!=0) native[0]=0;
    snprintf(v->error,sizeof(v->error),"%s: %s%s%s (state %u, domain %u, code %lld)",
        ckfe_api_status_name(api),error && error->operation ? error->operation : "operation",
        native[0]?": ":"",native,
        error ? error->state : 0,error ? error->domain : 0,
        (long long)(error ? error->code : 0));
}
/* Escape non-ASCII/control bytes for display only. Native names stay in core. */
static void append_name(char *out, size_t cap, const unsigned char *p, size_t n)
{
    size_t used=strlen(out);
    static const char hex[]="0123456789abcdef";
    for (size_t i=0; i<n; ++i) {
        unsigned b=p[i]; size_t need=b>=32 && b<127 && b!='\\' ? 1 : 4;
        if (used+need+4>=cap) { if (used+4<cap) memcpy(out+used,"...",4); return; }
        if (need==1) out[used++]=(char)b;
        else { out[used++]='\\'; out[used++]='x'; out[used++]=hex[b>>4]; out[used++]=hex[b&15]; }
        out[used]=0;
    }
}
static ckfe_api_status native_name(ckfet_job *job, uint64_t gen, uint64_t id, uint32_t kind,
                        uint64_t index, char *out, size_t cap)
{
    uint64_t required=0; uint32_t encoding=0;
    ckfe_api_status api=ckfet_native_copy_v1(job,gen,id,kind,index,NULL,0,&required,&encoding);
    if (api!=CKFE_API_BUFFER_TOO_SMALL && api!=CKFE_API_OK) return api;
    if (required>65536 || encoding!=CKFET_POSIX_BYTES || (!required && kind==CKFET_NATIVE_COMPONENT)) return CKFE_API_INVALID_ARGUMENT;
    uint64_t capacity=required;
    unsigned char *bytes=malloc((size_t)required+1);
    if (!bytes) return CKFE_API_OUT_OF_MEMORY;
    api=ckfet_native_copy_v1(job,gen,id,kind,index,bytes,required,&required,&encoding);
    if (api==CKFE_API_OK && (required>capacity || encoding!=CKFET_POSIX_BYTES)) api=CKFE_API_INVALID_ARGUMENT;
    if (api==CKFE_API_OK && kind!=CKFET_NATIVE_COMPONENT) {
        size_t display=0;
        for (size_t i=0;i<(size_t)required;++i) display+=bytes[i]>=32 && bytes[i]<127 && bytes[i]!='\\'?1:4;
        if (display+5>=cap) api=CKFE_API_BUFFER_TOO_SMALL;
    }
    if (api==CKFE_API_OK) append_name(out,cap,bytes,(size_t)required);
    free(bytes); return api;
}
static ckfe_api_status read_page(ckfet_job *job, SyncView *v, uint64_t offset)
{
    ckfet_row_v1 rows[SYNC_PAGE_SIZE]; ckfet_page_v1 page;
    ckfe_api_status api=ckfet_page_v1_get(job,v->snapshot.plan_generation,offset,rows,SYNC_PAGE_SIZE,&page);
    if (api!=CKFE_API_OK) return api;
    if (page.plan_generation!=v->snapshot.plan_generation || page.row_count>SYNC_PAGE_SIZE ||
        offset>page.total_rows || page.row_count>page.total_rows-offset ||
        (page.has_next && (page.next_offset<=offset || page.next_offset>page.total_rows)))
        return CKFE_API_INVALID_ARGUMENT;
    SyncRow *fresh=calloc(SYNC_PAGE_SIZE,sizeof(*fresh));
    if (!fresh) return CKFE_API_OUT_OF_MEMORY;
    ckfe_api_status name_error=CKFE_API_OK;
    for (unsigned i=0; i<page.row_count; ++i) {
        SyncRow *r=&fresh[i]; r->observed=rows[i];
        for (uint64_t j=0; j<rows[i].component_count; ++j) {
            if (j && strlen(r->path)+1<sizeof(r->path)) strcat(r->path,"/");
            api=native_name(job,page.plan_generation,rows[i].row_id,CKFET_NATIVE_COMPONENT,j,r->path,sizeof(r->path));
            if (api!=CKFE_API_OK) { free(fresh); return api; }
            if (strlen(r->path)+8>=sizeof(r->path)) break;
        }
        const uint32_t kinds[]={CKFET_NATIVE_PROPOSED_NAME,CKFET_NATIVE_RETAINED_NAME,CKFET_NATIVE_JOURNAL_NAME,CKFET_NATIVE_STAGING_NAME};
        char *values[]={r->proposed,r->retained,r->journal_name,r->staging_name};
        const int expected[]={!!rows[i].has_name_side,!!rows[i].target_retained,!!rows[i].has_journal,0};
        for (unsigned field=0;field<4;++field) {
            api=native_name(job,page.plan_generation,rows[i].row_id,kinds[field],0,values[field],sizeof(r->proposed));
            if (api==CKFE_API_OPERATION_UNAVAILABLE && !expected[field]) continue;
            if (api!=CKFE_API_OK || (expected[field] && !values[field][0])) {
                r->name_failures|=UINT32_C(1)<<kinds[field];
                if (name_error==CKFE_API_OK) name_error=api==CKFE_API_OK?CKFE_API_OPERATION_UNAVAILABLE:api;
            }
        }
    }
    v->offset=offset; v->total_rows=page.total_rows; v->next_offset=page.next_offset;
    v->has_next=page.has_next; v->row_count=(unsigned)page.row_count;
    memcpy(v->rows,fresh,SYNC_PAGE_SIZE*sizeof(*fresh)); free(fresh);
    v->cache_generation=page.plan_generation; v->cache_receipt=v->snapshot.receipt_id; v->cache_valid=1;
    v->names_valid=name_error==CKFE_API_OK;
    return name_error;
}
/* Presentation totals from engine-selected actions, never policy evaluation. */
static ckfe_api_status summarize(SyncJob *owner,ckfet_job *job,SyncView *v)
{
    uint64_t copies=0,folders=0,replacements=0,removals=0,skips=0,reports=0,bytes=0,offset=0;
    uint64_t choices[15]={0},classes[15]={0};
    unsigned bytes_known=1;
    for (;;) {
        if (atomic_load(&owner->cancel) || atomic_load(&owner->stop)) {
            ckfet_request_cancel(job);
            return CKFE_API_OK; /* Leave totals unavailable while settling. */
        }
        ckfet_row_v1 rows[SYNC_PAGE_SIZE]; ckfet_page_v1 page;
        ckfe_api_status api=ckfet_page_v1_get(job,v->snapshot.plan_generation,offset,rows,SYNC_PAGE_SIZE,&page);
        if (api!=CKFE_API_OK) return api;
        if (page.plan_generation!=v->snapshot.plan_generation || page.row_count>SYNC_PAGE_SIZE || offset>page.total_rows ||
            page.row_count>page.total_rows-offset || (page.has_next && (page.next_offset<=offset || page.next_offset>page.total_rows)))
            return CKFE_API_INVALID_ARGUMENT;
        for (uint64_t i=0;i<page.row_count;++i) {
            const ckfet_row_v1 *r=&rows[i];
            if (r->conflict_class<15) { choices[r->conflict_class]|=r->allowed_choices; ++classes[r->conflict_class]; }
            switch (r->action) {
            case CKFET_COPY: ++copies; break;
            case CKFET_CREATE_DIRECTORY: ++folders; break;
            case CKFET_REPLACE: ++replacements; break;
            case CKFET_REMOVE_FILE: case CKFET_REMOVE_DIRECTORY: ++removals; break;
            case CKFET_SKIP: ++skips; break;
            case CKFET_REPORT: ++reports; break;
            default: break;
            }
            if (r->action==CKFET_COPY || r->action==CKFET_REPLACE) {
                if (!(r->source.known_fields&CKFSP_FIELD_SIZE) || bytes>UINT64_MAX-r->source.size) bytes_known=0;
                else if (bytes_known) bytes+=r->source.size;
            }
        }
        if (!page.has_next) break;
        offset=page.next_offset;
    }
    v->planned_copies=copies; v->planned_folders=folders; v->planned_replacements=replacements;
    v->planned_removals=removals; v->planned_skips=skips; v->planned_reports=reports;
    v->planned_bytes=bytes; v->planned_bytes_known=bytes_known;
    v->summary_generation=v->snapshot.plan_generation; v->summary_receipt=v->snapshot.receipt_id; v->summary_valid=1;
    memcpy(v->class_choices,choices,sizeof(choices)); memcpy(v->class_rows,classes,sizeof(classes));
    return CKFE_API_OK;
}
static ckfe_api_status open_root(ckfe_engine *engine, const char *path,
                                 ckfe_entry **out, ckfe_error *error)
{
    char *copy=strdup(path); ckfe_directory *parent=NULL;
    if (!copy) return CKFE_API_OUT_OF_MEMORY;
    size_t length=strlen(copy);
    while (length>1 && copy[length-1]=='/') copy[--length]=0;
    char *slash=strrchr(copy,'/');
    if (!slash || !slash[1]) { free(copy); return CKFE_API_INVALID_ARGUMENT; }
    char *name=strdup(slash+1);
    if (!name) { free(copy); return CKFE_API_OUT_OF_MEMORY; }
    if (slash==copy) slash[1]=0; else *slash=0;
    ckfe_api_status api=ckfe_directory_open_utf8(engine,copy,0,&parent,error);
    if (api==CKFE_API_OK && parent) api=ckfe_entry_open_utf8(parent,name,out,error);
    else if (api==CKFE_API_OK) api=CKFE_API_OPERATION_UNAVAILABLE;
    ckfe_directory_destroy(parent); free(copy); free(name);
    if (api==CKFE_API_OK && !*out) api=CKFE_API_OPERATION_UNAVAILABLE;
    return api;
}
static int private_journal(SyncJob *s, SyncView *v, unsigned char id[16])
{
    if (!s->home || s->home[0]!='/') return 0;
    char journal[sizeof(v->journal)];
    int length=snprintf(journal,sizeof(journal),"%s/.dtfile-operations",s->home);
    if (length<0 || (size_t)length>=sizeof(journal)) {
        v->journal[0]=0;
        snprintf(v->error,sizeof(v->error),"Operation journal directory path is too long.");
        return 0;
    }
    int random=open("/dev/urandom",O_RDONLY|O_CLOEXEC);
    if (random<0) return 0;
    size_t got=0;
    while (got<16) {
        ssize_t n=read(random,id+got,16-got);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) break;
        got+=(size_t)n;
    }
    close(random); if (got!=16) return 0;
    int home=-1;
    if (SyncStorageOpenHome(s->home,&home,v->error,sizeof(v->error))!=SYNC_STORAGE_OK) return 0;
    int made=mkdirat(home,".dtfile-operations",0700);
    if (made<0 && errno!=EEXIST) { close(home); return 0; }
    int state=openat(home,".dtfile-operations",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (state<0) { close(home); return 0; }
    int safe=SyncStorageCheck(state,SYNC_STORAGE_DIRECTORY,v->error,sizeof(v->error))==SYNC_STORAGE_OK;
    /* Persist the container itself before any recovery journal relies on it. */
    if (safe && (fsync(state)!=0 || fsync(home)!=0)) safe=0;
    if (close(state)!=0) safe=0;
    if (close(home)!=0) safe=0;
    if (!safe) return 0;
    memcpy(v->journal,journal,(size_t)length+1);
    return 1;
}
static void *worker(void *arg)
{
    SyncJob *s=arg; SyncView *v=calloc(1,sizeof(*v));
    ckfe_engine *engine=NULL; ckfe_entry *source=NULL,*destination=NULL;
    ckfet_job *job=NULL; ckfe_error error; ckfe_error_init(&error);
    if (!v) {
        pthread_mutex_lock(&s->mutex); s->view.stopped=1; s->view.api=CKFE_API_OUT_OF_MEMORY;
        strcpy(s->view.error,"Cannot allocate operation view"); ++s->view.revision;
        pthread_mutex_unlock(&s->mutex); return NULL;
    }
    v->snapshot.state=CKFET_INVENTORY;
    if (ckfe_abi_version_major()!=0 || ckfe_abi_version_minor()<3) {
        v->snapshot.state=CKFET_FAILED; v->api=CKFE_API_OPERATION_UNAVAILABLE;
        strcpy(v->error,"Installed filesystem library does not provide the required ABI 0.3");
        goto done;
    }
    ckfe_api_status api=ckfe_engine_create(&engine);
    if (api==CKFE_API_OK && !engine) api=CKFE_API_OPERATION_UNAVAILABLE;
    if (api!=CKFE_API_OK || !engine) goto failed;
    if (atomic_load(&s->stop) || atomic_load(&s->cancel)) goto cancelled;
    api=open_root(engine,s->source,&source,&error); if (api!=CKFE_API_OK) goto failed;
    api=open_root(engine,s->destination,&destination,&error); if (api!=CKFE_API_OK) goto failed;
    ckfet_options_v1 options; ckfet_options_init_v1(&options);
    options.profile=s->profile; options.metadata_scope=s->metadata_scope;
    options.metadata_ownership=s->metadata_scope==CKFE_METADATA_ORDINARY_CALLER_VISIBLE_V1 ?
        CKFE_OWNERSHIP_CURRENT_CALLER : CKFE_OWNERSHIP_ORIGINAL;
    options.filters=s->filters; options.limits.max_page_rows=SYNC_PAGE_SIZE;
    if (s->profile!=CKFSP_COMPARE) {
        if (!private_journal(s,v,options.operation_id)) {
            v->snapshot.state=CKFET_FAILED; v->api=CKFE_API_OPERATION_UNAVAILABLE;
            if (!v->error[0]) strcpy(v->error,"Cannot open private operation journal directory; check home ownership and permissions");
            goto done;
        }
        options.journal_root=(ckfet_native){v->journal,strlen(v->journal),CKFET_POSIX_BYTES,0};
    }
    api=ckfet_start_v1(source,destination,&options,&job,&error);
    if (api==CKFE_API_OK && !job) api=CKFE_API_OPERATION_UNAVAILABLE;
    if (api!=CKFE_API_OK || !job) goto failed;
    v->ready=1;
    uint64_t pending=0, pending_serial=0, offset=0, last_sequence=UINT64_MAX;
    enum CommandKind pending_kind=NO_COMMAND;
    int applied_pause=-1, error_is_cache=0, settling_error=0;
    for (;;) {
        if (atomic_load(&s->cancel) || atomic_load(&s->stop)) ckfet_request_cancel(job);
        int pause=atomic_load(&s->pause);
        if (pause!=applied_pause) { ckfet_request_pause(job,(uint32_t)pause); applied_pause=pause; }
        Command *command=NULL;
        pthread_mutex_lock(&s->mutex); command=s->command; s->command=NULL; pthread_mutex_unlock(&s->mutex);
        int refresh=0;
        if (command) {
            error_is_cache=0;
            v->api=CKFE_API_OK;
            if (settling_error && command->kind!=PAGE_COMMAND) v->api=CKFE_API_NOT_READY;
            else if (command->kind==PAGE_COMMAND) { offset=command->offset; refresh=1; }
            else if (pending) { v->api=CKFE_API_BUSY; strcpy(v->error,"Wait for the previous review command"); }
            else if (command->kind==EXECUTE_COMMAND)
                v->api=ckfet_execute_v1(job,command->generation,command->receipt,&pending);
            else if (command->kind==REVIEW_COMMAND) {
                ckfet_rule_v1 rules[SYNC_MAX_RULES]; memset(rules,0,sizeof(rules));
                for (unsigned i=0; i<command->count; ++i) {
                    SyncRule *in=&command->rules[i]; ckfet_rule_v1 *out=&rules[i];
                    if (in->scope==CKFET_SCOPE_SUBTREE && in->selected_count==1) {
                        uint64_t bytes=0;
                        ckfe_api_status key_api=ckfet_key_copy_v1(job,command->generation,in->selected[0],NULL,0,&bytes);
                        if ((key_api!=CKFE_API_BUFFER_TOO_SMALL && key_api!=CKFE_API_OK) || !bytes || bytes>65536) {
                            v->api=key_api==CKFE_API_OK ? CKFE_API_INVALID_ARGUMENT : key_api;
                            break;
                        }
                        in->subtree=malloc((size_t)bytes); in->subtree_size=(size_t)bytes;
                        if (!in->subtree) { v->api=CKFE_API_OUT_OF_MEMORY; break; }
                        key_api=ckfet_key_copy_v1(job,command->generation,in->selected[0],in->subtree,bytes,&bytes);
                        if (key_api!=CKFE_API_OK) { v->api=key_api; break; }
                        in->selected_count=0;
                    }
                    out->version=CKFET_VERSION; out->struct_size=sizeof(*out);
                    out->kind=in->kind; out->choice=in->choice; out->tie=in->tie;
                    out->scope=in->scope; out->direction=CKFET_SOURCE_TO_TARGET;
                    out->protect_nonempty=in->protect_nonempty; out->retain_replaced=in->retain_replaced;
                    out->plan_generation=command->generation;
                    out->selected_rows=in->selected; out->selected_count=in->selected_count;
                    out->subtree_key=(ckfsp_bytes){in->subtree,in->subtree_size};
                }
                ckfet_name_choice_v1 name; memset(&name,0,sizeof(name));
                name.row_id=command->rename_row; name.side=command->rename_side;
                if (command->rename) name.component=(ckfet_native){command->rename,strlen(command->rename),CKFET_POSIX_BYTES,0};
                if (v->api==CKFE_API_OK) v->api=ckfet_review_names_v1(job,command->generation,rules,command->count,
                        command->rename ? &name : NULL,command->rename ? 1 : 0,
                        command->deletions,command->deletion_count,&pending);
            }
            if (v->api!=CKFE_API_OK) snprintf(v->error,sizeof(v->error),"Command: %s",ckfe_api_status_name(v->api));
            else v->error[0]=0;
            if (command->kind!=PAGE_COMMAND) {
                v->cache_valid=0;
                if (pending) { pending_serial=command->serial; pending_kind=command->kind; }
                else action_complete(s,v,command->serial,v->api,0);
            }
            command_free(command);
        }
        ckfet_snapshot_v1 observed_snapshot;
        api=ckfet_poll_v1(job,&observed_snapshot);
        if (api!=CKFE_API_OK) {
            failure(v,api,NULL);
            v->snapshot.state=CKFET_NEEDS_RECONCILIATION; v->snapshot.recovery_required=1;
            v->cache_valid=0; v->finished=0; v->settling=1; settling_error=1;
            atomic_store(&s->cancel,1); atomic_store(&s->pause,0);
            uint32_t drained=0;
            if (ckfet_worker_drained_v1(job,&drained)==CKFE_API_OK && drained) v->finished=v->drained=1;
            publish(s,v);
            if (v->drained && atomic_load(&s->stop)) break;
            struct timespec delay={0,100000000}; nanosleep(&delay,NULL);
            continue; /* Never exit a helper while backend writes may continue. */
        }
        v->snapshot=observed_snapshot;
        if (pending && v->snapshot.command_id==pending && v->snapshot.command_state!=CKFET_COMMAND_PENDING) {
            if (v->snapshot.command_state==CKFET_COMMAND_REJECTED)
                snprintf(v->error,sizeof(v->error),"Review rejected (state %u, domain %u, code %lld); inspect unresolved rows",
                    v->snapshot.failure_state,v->snapshot.failure_domain,(long long)v->snapshot.failure_code);
            ckfe_api_status ack=ckfet_acknowledge_command_v1(job,pending);
            if (ack!=CKFE_API_OK) {
                failure(v,ack,NULL); v->settling=1; settling_error=1; atomic_store(&s->cancel,1); atomic_store(&s->pause,0);
            }
            if (ack==CKFE_API_OK && pending_kind==REVIEW_COMMAND && v->snapshot.command_state==CKFET_COMMAND_ACCEPTED)
                v->review_submission=pending_serial;
            action_complete(s,v,pending_serial,ack!=CKFE_API_OK ? ack : v->snapshot.command_state==CKFET_COMMAND_ACCEPTED ?
                CKFE_API_OK : CKFE_API_OPERATION_UNAVAILABLE,ack==CKFE_API_OK && v->snapshot.command_state==CKFET_COMMAND_ACCEPTED);
            pending=0; pending_serial=0; pending_kind=NO_COMMAND; refresh=1;
        }
        if (v->snapshot.failure_domain!=CKFE_ERROR_NONE && !v->error[0]) {
            ckfe_error observed={v->snapshot.failure_state,v->snapshot.failure_domain,v->snapshot.failure_code,NULL};
            failure(v,CKFE_API_OPERATION_UNAVAILABLE,&observed);
            error_is_cache=0;
        }
        if (v->snapshot.plan_generation && (refresh || !v->cache_valid || !v->names_valid || v->snapshot.sequence!=last_sequence)) {
            v->cache_valid=0;
            api=read_page(job,v,offset);
            if (api!=CKFE_API_OK) { failure(v,api,NULL); error_is_cache=1; }
            else if (error_is_cache) { v->api=CKFE_API_OK; v->error[0]=0; error_is_cache=0; }
            memset(v->counts,0,sizeof(v->counts));
            ckfe_api_status counts_api=v->snapshot.rule_count>SYNC_MAX_RULES ? CKFE_API_INVALID_ARGUMENT : CKFE_API_OK;
            for (uint64_t i=0; counts_api==CKFE_API_OK && i<v->snapshot.rule_count; ++i)
                counts_api=ckfet_rule_counts_get_v1(job,i,&v->counts[i]);
            if (counts_api!=CKFE_API_OK) {
                failure(v,counts_api,NULL); v->cache_valid=0; error_is_cache=1;
            }
        }
        if (v->snapshot.state==CKFET_AWAITING_REVIEW && (!v->summary_valid ||
            v->summary_generation!=v->snapshot.plan_generation || v->summary_receipt!=v->snapshot.receipt_id)) {
            v->summary_valid=0;
            api=summarize(s,job,v);
            if (api!=CKFE_API_OK) { failure(v,api,NULL); v->cache_valid=0; error_is_cache=1; }
        }
        if (v->cache_valid && v->names_valid) last_sequence=v->snapshot.sequence;
        if (terminal(v->snapshot.state)) {
            uint32_t drained=0;
            ckfe_api_status drain_api=ckfet_worker_drained_v1(job,&drained);
            if (drain_api==CKFE_API_OK) v->drained=!!drained;
            else { failure(v,drain_api,NULL); v->settling=1; settling_error=1; }
        }
        v->finished=terminal(v->snapshot.state) && v->drained;
        publish(s,v);
        /* Keep terminal jobs queryable until the UI releases the result view. */
        if (v->finished && atomic_load(&s->stop)) break;
        struct timespec delay={0,100000000}; nanosleep(&delay,NULL);
    }
    goto done;
cancelled:
    v->snapshot.state=CKFET_CANCELLED; goto done;
failed:
    v->snapshot.state=CKFET_FAILED; failure(v,api,&error);
done:
    if (job) ckfet_release(job);
    ckfe_entry_destroy(destination); ckfe_entry_destroy(source); ckfe_engine_destroy(engine); ckfe_error_free(&error);
    v->finished=1; v->stopped=1; publish(s,v); free(v); return NULL;
}
SyncJob *SyncJobStart(const SyncSetup *setup, char *error, size_t capacity)
{
    if (!setup || !setup->source || !setup->destination || setup->source[0]!='/' || setup->destination[0]!='/' ||
        strnlen(setup->source,65537)>65536 || strnlen(setup->destination,65537)>65536) {
        snprintf(error,capacity,"Choose two absolute folder paths"); return NULL;
    }
    SyncJob *s=calloc(1,sizeof(*s)); if (!s) return NULL;
    s->source=strdup(setup->source); s->destination=strdup(setup->destination);
    const char *home=getenv("HOME"); s->home=home ? strdup(home) : NULL;
    s->profile=setup->profile; s->metadata_scope=setup->metadata_scope; s->filters=setup->filters;
    atomic_init(&s->stop,0); atomic_init(&s->cancel,0); atomic_init(&s->pause,0);
    if (!s->source || !s->destination || (!s->home && setup->profile!=CKFSP_COMPARE)) goto fail_alloc;
    if (pthread_mutex_init(&s->mutex,NULL)) goto fail_alloc;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr)) { pthread_mutex_destroy(&s->mutex); goto fail_alloc; }
    pthread_t thread;
    int rc=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
    if (!rc) rc=pthread_create(&thread,&attr,worker,s);
    pthread_attr_destroy(&attr);
    if (rc) { pthread_mutex_destroy(&s->mutex); goto fail_alloc; }
    return s;
fail_alloc:
    free(s->source); free(s->destination); free(s->home); free(s);
    snprintf(error,capacity,"Cannot start filesystem worker"); return NULL;
}
int SyncJobRead(SyncJob *s, SyncView *view)
{
    if (pthread_mutex_trylock(&s->mutex)) return 0;
    *view=s->view; pthread_mutex_unlock(&s->mutex); return 1;
}
int SyncJobPage(SyncJob *s, uint64_t offset)
{
    Command *c=calloc(1,sizeof(*c)); if (c) { c->kind=PAGE_COMMAND; c->offset=offset; }
    return submit(s,c)!=0;
}
uint64_t SyncJobReview(SyncJob *s, uint64_t generation, const SyncRule *rules, unsigned count,
                  const uint64_t *deletions, unsigned deletion_count, uint64_t rename_row,
                  const char *rename, uint32_t side)
{
    if (count>SYNC_MAX_RULES || deletion_count>SYNC_PAGE_SIZE || (count && !rules) || (deletion_count && !deletions)) return 0;
    Command *c=calloc(1,sizeof(*c)); if (!c) return 0;
    c->kind=REVIEW_COMMAND; c->generation=generation; c->count=count; c->deletion_count=deletion_count;
    for (unsigned i=0; i<count; ++i) {
        c->rules[i]=rules[i]; c->rules[i].subtree=NULL;
        if (rules[i].selected_count>SYNC_PAGE_SIZE || rules[i].subtree_size>65536 ||
            (rules[i].subtree_size && !rules[i].subtree)) { command_free(c); return 0; }
        if (rules[i].subtree_size) {
            c->rules[i].subtree=malloc(rules[i].subtree_size);
            if (!c->rules[i].subtree) { command_free(c); return 0; }
            memcpy(c->rules[i].subtree,rules[i].subtree,rules[i].subtree_size);
        }
    }
    if (deletion_count) memcpy(c->deletions,deletions,deletion_count*sizeof(*deletions));
    if (rename) { c->rename=strdup(rename); if (!c->rename) { command_free(c); return 0; } }
    c->rename_row=rename_row; c->rename_side=side; return submit(s,c);
}
uint64_t SyncJobExecute(SyncJob *s, uint64_t generation, uint64_t receipt)
{
    Command *c=calloc(1,sizeof(*c)); if (c) { c->kind=EXECUTE_COMMAND; c->generation=generation; c->receipt=receipt; }
    return submit(s,c);
}
void SyncJobPause(SyncJob *s, int paused) { atomic_store(&s->pause,!!paused); }
void SyncJobCancel(SyncJob *s) { atomic_store(&s->cancel,1); atomic_store(&s->pause,0); }
void SyncJobStop(SyncJob *s) { atomic_store(&s->stop,1); SyncJobCancel(s); }
int SyncJobDestroy(SyncJob *s)
{
    if (!s) return 1;
    if (pthread_mutex_trylock(&s->mutex)) return 0;
    int done=s->view.stopped;
    pthread_mutex_unlock(&s->mutex);
    if (!done) return 0;
    command_free(s->command); pthread_mutex_destroy(&s->mutex);
    free(s->source); free(s->destination); free(s->home); free(s); return 1;
}
