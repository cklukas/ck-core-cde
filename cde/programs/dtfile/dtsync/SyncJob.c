/* SPDX-License-Identifier: LGPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include "SyncJob.h"
#include "SyncPrivateStorage.h"
#include <ck_fs_diagnostics.h>
#include <ck_fs_session.h>
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
    unsigned count, deletion_count, approve_all_deletions, approve_first_run;
    SyncRule rules[SYNC_MAX_RULES];
    uint64_t *deletions;
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
    free(c->deletions);
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
    ckfe_api_status api=ckfet_native_copy_v1(job,gen,id,kind,index,nullptr,0,&required,&encoding);
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
    char *copy=strdup(path); ckfe_directory *parent=nullptr;
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
static ckfe_api_status collect_all_deletions(SyncJob *owner, ckfet_job *job, uint64_t generation,
                                             uint64_t **out_ids, uint64_t *out_count)
{
    *out_ids=nullptr; *out_count=0;
    uint64_t cap=0, count=0, offset=0;
    uint64_t *ids=nullptr;
    for (;;) {
        if (atomic_load(&owner->cancel) || atomic_load(&owner->stop)) {
            free(ids); return CKFE_API_NOT_READY;
        }
        ckfet_row_v1 rows[SYNC_PAGE_SIZE]; ckfet_page_v1 page;
        ckfe_api_status api=ckfet_page_v1_get(job,generation,offset,rows,SYNC_PAGE_SIZE,&page);
        if (api!=CKFE_API_OK) { free(ids); return api; }
        for (uint64_t i=0; i<page.row_count; ++i) {
            if (rows[i].action==CKFET_REMOVE_FILE || rows[i].action==CKFET_REMOVE_DIRECTORY) {
                if (count==cap) {
                    uint64_t next_cap=cap ? cap*2 : 64;
                    uint64_t *grown=realloc(ids,(size_t)next_cap*sizeof(*ids));
                    if (!grown) { free(ids); return CKFE_API_OUT_OF_MEMORY; }
                    ids=grown; cap=next_cap;
                }
                ids[count++]=rows[i].row_id;
            }
        }
        if (!page.has_next) break;
        offset=page.next_offset;
    }
    *out_ids=ids; *out_count=count;
    return CKFE_API_OK;
}

static void tw_failure(SyncView *v, ckfss_status st, const ckfss_error *err)
{
    v->api = st==CKFSS_INVALID_ARGUMENT ? CKFE_API_INVALID_ARGUMENT :
             st==CKFSS_OUT_OF_MEMORY ? CKFE_API_OUT_OF_MEMORY :
             CKFE_API_OPERATION_UNAVAILABLE;
    char native[128]="";
    if (err && err->domain==CKFE_ERROR_POSIX && err->code>0 && err->code<=INT_MAX &&
        strerror_r((int)err->code,native,sizeof(native))!=0) native[0]=0;
    snprintf(v->error,sizeof(v->error),"Two-way sync (%u): %s%s%s (state %u, domain %u, code %lld)",
        st,err && err->operation[0] ? err->operation : "session",
        native[0]?": ":"",native,
        err ? err->observation : 0,err ? err->domain : 0,
        (long long)(err ? err->code : 0));
}

static uint32_t tw_effective_action(const ckfss_two_way_row *tr, const ckfss_two_way_rule_result *rr,
                                    uint32_t *out_reason, const char **out_dir_label)
{
    uint32_t lp = tr->left.presence == CKFSS_PRESENT;
    uint32_t rp = tr->right.presence == CKFSS_PRESENT;
    uint32_t ldir = tr->left.kind == CKFSS_DIRECTORY;
    uint32_t rdir = tr->right.kind == CKFSS_DIRECTORY;
    uint32_t reason = 15; /* different content default */
    if (tr->reason == CKFSS_TW_REASON_NONE && tr->proposed_action == CKFSS_TW_UNCHANGED) reason = 14; /* verified same */
    else if (!lp && rp) reason = 10; /* source/left absent */
    else if (lp && !rp) reason = 11; /* destination/right absent */
    else if (ldir && rdir) reason = 21; /* folder pair */
    else if (ldir != rdir) reason = 19; /* different type */
    else if (tr->reason == CKFSS_TW_REASON_METADATA_DIFFERENCE) reason = 18;
    if (out_reason) *out_reason = reason;
    if (out_dir_label) *out_dir_label = "";

    if (rr && rr->state == CKFSS_DECISION_APPLY) {
        switch (rr->choice) {
        case CKFSS_RULE_SKIP:
        case CKFSS_RULE_KEEP_TARGET:
            if (out_dir_label) *out_dir_label = "Skip (keep pending)";
            return CKFET_SKIP;
        case CKFSS_RULE_USE_SOURCE:
        case CKFSS_RULE_KEEP_LEFT:
            if (lp && rp && tr->left.kind == tr->right.kind && ldir) return CKFET_MERGE_DIRECTORY;
            if (lp) {
                if (out_dir_label) *out_dir_label = "Left -> Right";
                return rp ? CKFET_REPLACE : (ldir ? CKFET_CREATE_DIRECTORY : CKFET_COPY);
            }
            if (out_dir_label) *out_dir_label = "Remove on Right";
            return rdir ? CKFET_REMOVE_DIRECTORY : CKFET_REMOVE_FILE;
        case CKFSS_RULE_KEEP_RIGHT:
            if (lp && rp && tr->left.kind == tr->right.kind && rdir) return CKFET_MERGE_DIRECTORY;
            if (rp) {
                if (out_dir_label) *out_dir_label = "Right -> Left";
                return lp ? CKFET_REPLACE : (rdir ? CKFET_CREATE_DIRECTORY : CKFET_COPY);
            }
            if (out_dir_label) *out_dir_label = "Remove on Left";
            return ldir ? CKFET_REMOVE_DIRECTORY : CKFET_REMOVE_FILE;
        case CKFSS_RULE_RESTORE_DELETED:
        case CKFSS_RULE_KEEP_EDITED:
            if (lp) { if (out_dir_label) *out_dir_label = "Left -> Right"; return ldir ? CKFET_CREATE_DIRECTORY : CKFET_COPY; }
            if (out_dir_label) *out_dir_label = "Right -> Left";
            return rdir ? CKFET_CREATE_DIRECTORY : CKFET_COPY;
        case CKFSS_RULE_DELETE_EDITED:
            if (out_dir_label) *out_dir_label = lp ? "Remove on Left" : "Remove on Right";
            return (lp ? ldir : rdir) ? CKFET_REMOVE_DIRECTORY : CKFET_REMOVE_FILE;
        case CKFSS_RULE_MERGE_FOLDERS:
            return CKFET_MERGE_DIRECTORY;
        default:
            break;
        }
    }
    if (rr && (rr->state == CKFSS_DECISION_REVIEW || rr->state == CKFSS_DECISION_INVALID || rr->state == CKFSS_DECISION_STALE))
        return CKFET_REPORT;

    switch (tr->proposed_action) {
    case CKFSS_TW_UNCHANGED:
        if (lp && rp && ldir && rdir) return CKFET_MERGE_DIRECTORY;
        return CKFET_SKIP;
    case CKFSS_TW_COPY_LEFT_TO_RIGHT:
        if (out_dir_label) *out_dir_label = "Left -> Right";
        return rp ? CKFET_REPLACE : (ldir ? CKFET_CREATE_DIRECTORY : CKFET_COPY);
    case CKFSS_TW_COPY_RIGHT_TO_LEFT:
        if (out_dir_label) *out_dir_label = "Right -> Left";
        return lp ? CKFET_REPLACE : (rdir ? CKFET_CREATE_DIRECTORY : CKFET_COPY);
    case CKFSS_TW_DELETE_LEFT:
        if (out_dir_label) *out_dir_label = "Remove on Left";
        return ldir ? CKFET_REMOVE_DIRECTORY : CKFET_REMOVE_FILE;
    case CKFSS_TW_DELETE_RIGHT:
        if (out_dir_label) *out_dir_label = "Remove on Right";
        return rdir ? CKFET_REMOVE_DIRECTORY : CKFET_REMOVE_FILE;
    default:
        return CKFET_REPORT;
    }
}

static void tw_fill_side(const ckfss_two_way_observed *in, ckfet_observed_v1 *out)
{
    memset(out,0,sizeof(*out));
    if (in->status.presence == CKFSS_ABSENT) { out->presence = CKFET_PRESENCE_ABSENT; return; }
    if (in->status.presence != CKFSS_PRESENT) { out->presence = CKFET_PRESENCE_UNKNOWN; return; }
    out->presence = CKFET_PRESENCE_PRESENT;
    if (in->known_fields & CKFSS_FACT_KIND) { out->known_fields |= CKFSP_FIELD_KIND; out->kind = in->kind; }
    if (in->known_fields & CKFSS_FACT_SIZE) { out->known_fields |= CKFSP_FIELD_SIZE; out->size = in->size; }
    if (in->known_fields & CKFSS_FACT_MODIFIED) {
        out->known_fields |= CKFSP_FIELD_MODIFIED;
        out->modified.seconds = in->modified_seconds;
        out->modified.nanoseconds = in->modified_nanoseconds;
        out->timestamp_quantum_ns = 1;
    }
    out->metadata_assessment = (in->known_fields & CKFSS_FACT_REQUIRED_METADATA)
        ? CKFE_OBSERVATION_AVAILABLE : CKFE_OBSERVATION_UNKNOWN;
}

static ckfss_status tw_populate_view(ckfss_two_way *tw, const ckfss_two_way_review *rev,
                                     uint64_t total_rows, uint64_t offset, int completed_all,
                                     SyncView *v, ckfss_error *tw_err)
{
    if (offset > total_rows) return CKFSS_INVALID_ARGUMENT;
    uint64_t copies=0,folders=0,replacements=0,removals=0,skips=0,reports=0,bytes=0;
    uint64_t choices[15]={0},classes[15]={0};
    unsigned bytes_known=1;
    SyncRow *fresh=calloc(SYNC_PAGE_SIZE,sizeof(*fresh));
    if (!fresh) return CKFSS_OUT_OF_MEMORY;
    unsigned page_n=0;

    for (uint64_t idx=0; idx<total_rows; ++idx) {
        ckfss_two_way_row tr; memset(&tr,0,sizeof(tr));
        uint64_t key_req=0;
        ckfss_status st=ckfss_two_way_get_row(tw,idx,&tr,nullptr,0,&key_req);
        if (st!=CKFSS_OK && st!=CKFSS_BUFFER_TOO_SMALL) { free(fresh); return st; }
        ckfss_two_way_raw_facts facts; memset(&facts,0,sizeof(facts));
        st=ckfss_two_way_get_facts(tw,idx,&facts,tw_err);
        if (st!=CKFSS_OK) { free(fresh); return st; }
        ckfss_two_way_rule_result rr; memset(&rr,0,sizeof(rr));
        const ckfss_two_way_rule_result *rrp=nullptr;
        if (rev && ckfss_two_way_review_get_row(rev,idx,&rr)==CKFSS_OK) rrp=&rr;

        uint32_t reason=0; const char *dir_label="";
        uint32_t act=tw_effective_action(&tr,rrp,&reason,&dir_label);
        if (facts.conflict_class<15) {
            choices[facts.conflict_class]|=facts.allowed_choices;
            /* Two-way rules additionally accept Keep Left / Keep Right when present/retention-eligible. */
            if (facts.left.status.presence==CKFSS_PRESENT || facts.right_retention_available)
                choices[facts.conflict_class]|=(UINT64_C(1)<<CKFET_KEEP_LEFT);
            if (facts.right.status.presence==CKFSS_PRESENT || facts.left_retention_available)
                choices[facts.conflict_class]|=(UINT64_C(1)<<CKFET_KEEP_RIGHT);
            ++classes[facts.conflict_class];
        }
        switch (act) {
        case CKFET_COPY: ++copies; break;
        case CKFET_CREATE_DIRECTORY: ++folders; break;
        case CKFET_REPLACE: ++replacements; break;
        case CKFET_REMOVE_FILE: case CKFET_REMOVE_DIRECTORY: ++removals; break;
        case CKFET_SKIP: ++skips; break;
        case CKFET_REPORT: ++reports; break;
        default: break;
        }
        if (act==CKFET_COPY || act==CKFET_REPLACE) {
            const ckfss_two_way_observed *src_side =
                (dir_label && !strncmp(dir_label,"Right",5)) ? &facts.right : &facts.left;
            if (!(src_side->known_fields & CKFSS_FACT_SIZE) || bytes > UINT64_MAX - src_side->size) bytes_known=0;
            else if (bytes_known) bytes += src_side->size;
        }

        if (idx >= offset && page_n < SYNC_PAGE_SIZE) {
            SyncRow *r=&fresh[page_n++];
            r->observed.row_id=idx+1;
            r->observed.action=act;
            r->observed.reason=reason;
            r->observed.conflict_class=facts.conflict_class;
            r->observed.allowed_choices=facts.conflict_class<15 ? choices[facts.conflict_class] : facts.allowed_choices;
            r->observed.component_count=tr.component_count;
            r->observed.state = completed_all
                ? (act==CKFET_SKIP ? CKFET_ITEM_SKIPPED : CKFET_ITEM_COMPLETE)
                : CKFET_ITEM_PENDING;
            tw_fill_side(&facts.left,&r->observed.source);
            tw_fill_side(&facts.right,&r->observed.destination);
            for (uint64_t c=0; c<tr.component_count; ++c) {
                if (c && strlen(r->path)+1<sizeof(r->path)) strcat(r->path,"/");
                uint32_t enc=0; uint64_t req=0;
                st=ckfss_two_way_get_component(tw,idx,c,&enc,nullptr,0,&req);
                if ((st!=CKFSS_OK && st!=CKFSS_BUFFER_TOO_SMALL) || enc!=CKFSS_POSIX_BYTES || !req || req>65536) {
                    free(fresh); return CKFSS_INVALID_ARGUMENT;
                }
                unsigned char *comp=malloc((size_t)req);
                if (!comp) { free(fresh); return CKFSS_OUT_OF_MEMORY; }
                st=ckfss_two_way_get_component(tw,idx,c,&enc,comp,req,&req);
                if (st!=CKFSS_OK) { free(comp); free(fresh); return st; }
                append_name(r->path,sizeof(r->path),comp,(size_t)req);
                free(comp);
                if (strlen(r->path)+8>=sizeof(r->path)) break;
            }
            if (dir_label && dir_label[0]) snprintf(r->proposed,sizeof(r->proposed),"%s",dir_label);
        }
    }

    v->offset=offset;
    v->total_rows=total_rows;
    v->row_count=page_n;
    v->has_next=(offset+page_n)<total_rows;
    v->next_offset=v->has_next ? offset+page_n : total_rows;
    memcpy(v->rows,fresh,SYNC_PAGE_SIZE*sizeof(*fresh));
    free(fresh);
    v->planned_copies=copies; v->planned_folders=folders; v->planned_replacements=replacements;
    v->planned_removals=removals; v->planned_skips=skips; v->planned_reports=reports;
    v->planned_bytes=bytes; v->planned_bytes_known=bytes_known;
    v->snapshot.unresolved_rows=reports;
    v->snapshot.known_items=total_rows;
    v->snapshot.known_copy_bytes=bytes;
    v->snapshot.counts_complete=1;
    memcpy(v->class_choices,choices,sizeof(choices));
    memcpy(v->class_rows,classes,sizeof(classes));
    v->cache_generation=v->snapshot.plan_generation;
    v->cache_receipt=v->snapshot.receipt_id;
    v->summary_generation=v->snapshot.plan_generation;
    v->summary_receipt=v->snapshot.receipt_id;
    v->cache_valid=1; v->names_valid=1; v->summary_valid=1;
    return CKFSS_OK;
}

static void *worker_two_way(SyncJob *s, SyncView *v)
{
    ckfe_engine *engine=nullptr;
    ckfjr_root *journal_owner=nullptr;
    ckfss_two_way *tw=nullptr;
    ckfss_two_way_review *active_rev=nullptr;
    ckfe_error error; ckfe_error_init(&error);
    ckfss_error tw_err; memset(&tw_err,0,sizeof(tw_err));
    unsigned char op_id[16]={0};

    v->two_way=1;
    v->snapshot.state=CKFET_INVENTORY;
    publish(s,v);

    ckfe_api_status api=ckfe_engine_create(&engine);
    if (api!=CKFE_API_OK || !engine) { v->snapshot.state=CKFET_FAILED; failure(v,api?api:CKFE_API_OPERATION_UNAVAILABLE,&error); goto done; }
    if (!private_journal(s,v,op_id)) {
        v->snapshot.state=CKFET_FAILED; v->api=CKFE_API_OPERATION_UNAVAILABLE;
        if (!v->error[0]) strcpy(v->error,"Cannot open private operation journal directory; check home ownership and permissions");
        goto done;
    }
    ckfe_directory *home_dir=nullptr;
    api=ckfe_directory_open_utf8(engine,s->home,0,&home_dir,&error);
    if (api!=CKFE_API_OK || !home_dir) { v->snapshot.state=CKFET_FAILED; failure(v,api?api:CKFE_API_OPERATION_UNAVAILABLE,&error); goto done; }
    ckfjr_options_v1 prep; ckfjr_options_init_v1(&prep);
    prep.parent=home_dir;
    prep.disposition=CKFJR_PROVISION_CHILD;
    prep.child.encoding=CKFJR_POSIX_BYTES;
    prep.child.data=".dtfile-operations";
    prep.child.byte_count=strlen(".dtfile-operations");
    api=ckfjr_start_v1(&prep,&journal_owner);
    ckfe_directory_destroy(home_dir);
    if (api!=CKFE_API_OK || !journal_owner) { v->snapshot.state=CKFET_FAILED; failure(v,api?api:CKFE_API_OPERATION_UNAVAILABLE,&error); goto done; }
    ckfjr_snapshot_v1 rsnap; ckfjr_snapshot_init_v1(&rsnap);
    for (;;) {
        if (atomic_load(&s->stop) || atomic_load(&s->cancel)) { (void)ckfjr_cancel_v1(journal_owner); v->snapshot.state=CKFET_CANCELLED; goto done; }
        api=ckfjr_poll_v1(journal_owner,&rsnap);
        if (api!=CKFE_API_OK) { v->snapshot.state=CKFET_FAILED; failure(v,api,&error); goto done; }
        if (rsnap.state!=CKFJR_PREPARING) break;
        (void)ckfjr_wait_v1(journal_owner,50);
    }
    if (rsnap.state!=CKFJR_READY) {
        ckfe_error re={rsnap.failure_state,rsnap.failure_domain,rsnap.failure_code,rsnap.failure_operation[0]?rsnap.failure_operation:nullptr};
        v->snapshot.state=rsnap.state==CKFJR_CANCELLED ? CKFET_CANCELLED : CKFET_FAILED;
        if (rsnap.state!=CKFJR_CANCELLED) failure(v,CKFE_API_OPERATION_UNAVAILABLE,&re);
        goto done;
    }

    ckfss_options o; ckfss_options_init(&o);
    o.left=(ckfss_native_path){s->source,strlen(s->source),CKFSS_POSIX_BYTES,0};
    o.right=(ckfss_native_path){s->destination,strlen(s->destination),CKFSS_POSIX_BYTES,0};
    static const char tag[]="CK-DTFILE-TWOWAY-POLICY-V1";
    memcpy(o.policy_fingerprint,tag,sizeof(tag)-1);
    o.policy_fingerprint[31]=(uint8_t)s->metadata_scope;

    ckfss_metadata_policy mp; ckfss_metadata_policy_init(&mp);
    mp.scope = s->metadata_scope==CKFE_METADATA_ORDINARY_CALLER_VISIBLE_V1 ? CKFSS_METADATA_ORDINARY_V1 : CKFSS_METADATA_COMPLETE_V1;
    mp.ownership = s->metadata_scope==CKFE_METADATA_ORDINARY_CALLER_VISIBLE_V1 ? CKFSS_OWNER_CURRENT_CALLER : CKFSS_OWNER_ORIGINAL;

    ckfss_two_way_policy tp; ckfss_two_way_policy_init(&tp);
    tp.max_deletions=100000;
    tp.max_delete_basis_points=10000;

    ckfss_two_way_tree_options_v1 tree; ckfss_two_way_tree_options_init_v1(&tree);
    tree.allow_recursive_trees=1;
    tree.namespace_policy=CKFSS_NAMESPACE_HELD_PARENT_RENAME;
    tree.max_depth=s->filters.maximum_depth ? s->filters.maximum_depth : 1024;

    ckfss_status st=ckfss_two_way_open_tree_v1(&o,&mp,&tp,&tree,journal_owner,&tw,&tw_err);
    if (st!=CKFSS_OK || !tw) { v->snapshot.state=CKFET_FAILED; tw_failure(v,st,&tw_err); goto done; }

    ckfss_two_way_info info; memset(&info,0,sizeof(info));
    st=ckfss_two_way_get_info(tw,&info);
    if (st!=CKFSS_OK) { v->snapshot.state=CKFET_FAILED; tw_failure(v,st,nullptr); goto done; }

    const uint64_t plan_gen=info.generation+1;
    v->two_way_generation=info.generation;
    v->first_run=(info.generation==0);
    v->snapshot.plan_generation=plan_gen;
    v->snapshot.state=CKFET_AWAITING_REVIEW;
    v->snapshot.sequence=1;
    v->ready=1;

    (void)ckfss_two_way_assess_rules(tw,nullptr,0,1,&active_rev,&tw_err);
    st=tw_populate_view(tw,active_rev,info.rows,0,0,v,&tw_err);
    if (st!=CKFSS_OK) { v->snapshot.state=CKFET_FAILED; tw_failure(v,st,&tw_err); goto done; }
    publish(s,v);

    uint64_t offset=0, next_receipt=0;
    int completed_all=0;
    for (;;) {
        if ((atomic_load(&s->cancel) || atomic_load(&s->stop)) && !terminal(v->snapshot.state)) {
            ckfss_two_way_request_cancel(tw);
            v->snapshot.state=CKFET_CANCELLED;
            break;
        }
        int paused=atomic_load(&s->pause);
        v->snapshot.pause_requested=v->snapshot.paused=(uint32_t)(paused && !terminal(v->snapshot.state));

        Command *command=nullptr;
        pthread_mutex_lock(&s->mutex); command=s->command; s->command=nullptr; pthread_mutex_unlock(&s->mutex);
        if (command) {
            if (command->kind==PAGE_COMMAND) {
                offset=command->offset;
                st=tw_populate_view(tw,active_rev,info.rows,offset,completed_all,v,&tw_err);
                if (st!=CKFSS_OK) { tw_failure(v,st,&tw_err); v->cache_valid=0; }
                else { v->api=CKFE_API_OK; v->error[0]=0; ++v->snapshot.sequence; }
                publish(s,v);
            } else if (command->kind==REVIEW_COMMAND) {
                if (command->generation!=plan_gen) {
                    v->api=CKFE_API_STALE_PLAN;
                    strcpy(v->error,"Command: CKFE_API_STALE_PLAN");
                    action_complete(s,v,command->serial,v->api,0);
                } else if (command->rename && command->rename[0]) {
                    v->api=CKFE_API_OPERATION_UNAVAILABLE;
                    strcpy(v->error,"Manual per-row rename is unavailable in two-way sync; choose Keep Left, Keep Right or Skip.");
                    action_complete(s,v,command->serial,v->api,0);
                } else {
                    ckfss_two_way_rule tw_rules[SYNC_MAX_RULES];
                    uint64_t selected_zero[SYNC_MAX_RULES][SYNC_PAGE_SIZE];
                    unsigned char *subtrees[SYNC_MAX_RULES]={0};
                    ckfss_status build_st=CKFSS_OK;
                    for (unsigned i=0; i<command->count; ++i) {
                        const SyncRule *in=&command->rules[i];
                        ckfss_two_way_rule *out=&tw_rules[i];
                        ckfss_two_way_rule_init(out);
                        out->kind=in->kind; out->choice=in->choice; out->tie=in->tie;
                        out->scope=in->scope;
                        out->direction = in->choice==CKFET_KEEP_LEFT ? CKFSS_DIRECTION_LEFT_TO_RIGHT :
                                         in->choice==CKFET_KEEP_RIGHT ? CKFSS_DIRECTION_RIGHT_TO_LEFT :
                                         CKFSS_DIRECTION_SOURCE_TO_TARGET;
                        out->protect_nonempty=in->protect_nonempty;
                        out->retain_replaced=in->retain_replaced;
                        out->expected_generation=info.generation;
                        if (in->scope==CKFET_SCOPE_SUBTREE && in->selected_count==1) {
                            uint64_t ridx=in->selected[0] ? in->selected[0]-1 : UINT64_MAX;
                            ckfss_two_way_row tr; uint64_t kreq=0;
                            if (ridx>=info.rows) { build_st=CKFSS_INVALID_ARGUMENT; break; }
                            ckfss_status kst=ckfss_two_way_get_row(tw,ridx,&tr,nullptr,0,&kreq);
                            if ((kst!=CKFSS_OK && kst!=CKFSS_BUFFER_TOO_SMALL) || !kreq || kreq>65536) { build_st=CKFSS_INVALID_ARGUMENT; break; }
                            subtrees[i]=malloc((size_t)kreq);
                            if (!subtrees[i]) { build_st=CKFSS_OUT_OF_MEMORY; break; }
                            if (ckfss_two_way_get_row(tw,ridx,&tr,subtrees[i],kreq,&kreq)!=CKFSS_OK) { build_st=CKFSS_INVALID_ARGUMENT; break; }
                            out->subtree_key=subtrees[i]; out->subtree_key_bytes=kreq;
                        } else if (in->selected_count) {
                            for (unsigned j=0; j<in->selected_count; ++j) {
                                if (!in->selected[j] || in->selected[j]>info.rows) { build_st=CKFSS_INVALID_ARGUMENT; break; }
                                selected_zero[i][j]=in->selected[j]-1;
                            }
                            if (build_st!=CKFSS_OK) break;
                            out->selected_rows=selected_zero[i];
                            out->selected_count=in->selected_count;
                        }
                    }
                    ckfss_two_way_review *new_rev=nullptr;
                    if (build_st==CKFSS_OK)
                        build_st=ckfss_two_way_assess_rules(tw,command->count?tw_rules:nullptr,command->count,1,&new_rev,&tw_err);
                    for (unsigned i=0; i<command->count; ++i) free(subtrees[i]);

                    if (build_st!=CKFSS_OK || !new_rev) {
                        v->snapshot.receipt_id=0;
                        tw_failure(v,build_st,&tw_err);
                        action_complete(s,v,command->serial,v->api,0);
                    } else {
                        if (active_rev) ckfss_two_way_review_destroy(active_rev);
                        active_rev=new_rev;
                        ckfss_two_way_review_info rinfo; memset(&rinfo,0,sizeof(rinfo));
                        (void)ckfss_two_way_review_get_info(active_rev,&rinfo);
                        memset(v->counts,0,sizeof(v->counts));
                        for (uint64_t r=0; r<info.rows; ++r) {
                            ckfss_two_way_rule_result rr;
                            if (ckfss_two_way_review_get_row(active_rev,r,&rr)==CKFSS_OK && rr.has_rule_index && rr.rule_index<SYNC_MAX_RULES) {
                                ++v->counts[rr.rule_index].matched_rows;
                                if (rr.state==CKFSS_DECISION_APPLY) {
                                    if (rr.choice==CKFSS_RULE_SKIP || rr.choice==CKFSS_RULE_KEEP_TARGET) ++v->counts[rr.rule_index].skipped_rows;
                                    else ++v->counts[rr.rule_index].applied_rows;
                                } else ++v->counts[rr.rule_index].unresolved_rows;
                            }
                        }
                        v->snapshot.rule_count=command->count;
                        if (rinfo.unresolved_rows > 0) {
                            v->snapshot.receipt_id=0;
                            v->first_run_approved=0;
                            (void)tw_populate_view(tw,active_rev,info.rows,offset,0,v,&tw_err);
                            v->snapshot.unresolved_rows=rinfo.unresolved_rows;
                            ++v->snapshot.sequence;
                            v->api=CKFE_API_OK;
                            snprintf(v->error,sizeof(v->error),"Review rejected: %llu conflict row(s) still require Keep Left, Keep Right or Skip",(unsigned long long)rinfo.unresolved_rows);
                            action_complete(s,v,command->serial,CKFE_API_OPERATION_UNAVAILABLE,0);
                        } else if (v->first_run && !command->approve_first_run) {
                            v->snapshot.receipt_id=0;
                            v->first_run_approved=0;
                            (void)tw_populate_view(tw,active_rev,info.rows,offset,0,v,&tw_err);
                            ++v->snapshot.sequence;
                            v->api=CKFE_API_OK;
                            strcpy(v->error,"Approve initial two-way baseline in Matching rules before reviewing a first-run two-way sync.");
                            action_complete(s,v,command->serial,CKFE_API_OPERATION_UNAVAILABLE,0);
                        } else {
                            v->snapshot.receipt_id=++next_receipt;
                            v->first_run_approved=command->approve_first_run;
                            v->api=CKFE_API_OK; v->error[0]=0;
                            (void)tw_populate_view(tw,active_rev,info.rows,offset,0,v,&tw_err);
                            v->snapshot.unresolved_rows=0;
                            ++v->snapshot.sequence;
                            v->review_submission=command->serial;
                            action_complete(s,v,command->serial,CKFE_API_OK,1);
                        }
                    }
                }
            } else if (command->kind==EXECUTE_COMMAND) {
                if (command->generation!=plan_gen || !command->receipt || command->receipt!=v->snapshot.receipt_id || !active_rev) {
                    v->api=CKFE_API_STALE_PLAN;
                    strcpy(v->error,"Command: CKFE_API_STALE_PLAN");
                    action_complete(s,v,command->serial,v->api,0);
                } else {
                    st=ckfss_two_way_approve_review(tw,active_rev,v->first_run ? v->first_run_approved : 0,&tw_err);
                    if (st!=CKFSS_OK) {
                        v->snapshot.receipt_id=0;
                        tw_failure(v,st,&tw_err);
                        action_complete(s,v,command->serial,v->api,0);
                    } else {
                        v->snapshot.state=CKFET_EXECUTING;
                        v->api=CKFE_API_OK; v->error[0]=0;
                        ++v->snapshot.sequence;
                        action_complete(s,v,command->serial,CKFE_API_OK,1);
                        for (;;) {
                            if (atomic_load(&s->cancel) || atomic_load(&s->stop)) ckfss_two_way_request_cancel(tw);
                            while (atomic_load(&s->pause) && !atomic_load(&s->cancel) && !atomic_load(&s->stop)) {
                                v->snapshot.pause_requested=v->snapshot.paused=1;
                                publish(s,v);
                                struct timespec pts={0,50000000}; nanosleep(&pts,nullptr);
                            }
                            v->snapshot.pause_requested=v->snapshot.paused=0;
                            st=ckfss_two_way_advance(tw,&info,&tw_err);
                            v->snapshot.completed_items=info.completed_operations;
                            if (st!=CKFSS_OK || info.state==CKFSS_TW_COMPLETE || info.state==CKFSS_TW_CANCELLED || info.state==CKFSS_TW_NEEDS_REVIEW)
                                break;
                            ++v->snapshot.sequence;
                            publish(s,v);
                        }
                        if (st==CKFSS_OK && info.state==CKFSS_TW_COMPLETE && !info.has_failure)
                            (void)tw_populate_view(tw,active_rev,info.rows,offset,1,v,&tw_err);
                        ckfss_error fin_err; memset(&fin_err,0,sizeof(fin_err));
                        ckfss_status fin_st=ckfss_two_way_finish_io(tw,&fin_err);
                        if (st==CKFSS_OK && fin_st==CKFSS_OK && info.state==CKFSS_TW_COMPLETE && !info.has_failure) {
                            completed_all=1;
                            v->two_way_generation=info.generation;
                            v->first_run=0;
                            v->snapshot.state=CKFET_COMPLETE;
                            v->snapshot.completed_items=info.rows - v->planned_skips;
                            v->snapshot.skipped_items=v->planned_skips;
                            v->snapshot.copied_bytes=v->planned_bytes;
                            for (unsigned ri=0; ri<v->row_count; ++ri) {
                                v->rows[ri].observed.state =
                                    v->rows[ri].observed.action==CKFET_SKIP ? CKFET_ITEM_SKIPPED : CKFET_ITEM_COMPLETE;
                            }
                            ckfss_two_way_review_destroy(active_rev); active_rev=nullptr;
                            ckfss_two_way_destroy(tw); tw=nullptr;
                            if (journal_owner) {
                                (void)ckfjr_finish_v1(journal_owner);
                                for (unsigned ri=0; ri<600; ++ri) {
                                    uint32_t rdrained=0;
                                    if (ckfjr_worker_drained_v1(journal_owner,&rdrained)!=CKFE_API_OK || rdrained) break;
                                    (void)ckfjr_wait_v1(journal_owner,25);
                                }
                                ckfjr_release(journal_owner); journal_owner=nullptr;
                            }
                            v->drained=1; v->finished=1; ++v->snapshot.sequence;
                            publish(s,v);
                        } else {
                            v->snapshot.state = info.state==CKFSS_TW_CANCELLED ? CKFET_CANCELLED : CKFET_NEEDS_RECONCILIATION;
                            if (info.state!=CKFSS_TW_CANCELLED) {
                                v->snapshot.recovery_required=1;
                                tw_failure(v,st!=CKFSS_OK?st:(fin_st!=CKFSS_OK?fin_st:CKFSS_UNAVAILABLE),
                                           st!=CKFSS_OK?&tw_err:(fin_st!=CKFSS_OK?&fin_err:&info.failure));
                            }
                            ckfss_two_way_review_destroy(active_rev); active_rev=nullptr;
                            ckfss_two_way_destroy(tw); tw=nullptr;
                            if (journal_owner) {
                                (void)ckfjr_finish_v1(journal_owner);
                                for (unsigned ri=0; ri<600; ++ri) {
                                    uint32_t rdrained=0;
                                    if (ckfjr_worker_drained_v1(journal_owner,&rdrained)!=CKFE_API_OK || rdrained) break;
                                    (void)ckfjr_wait_v1(journal_owner,25);
                                }
                                ckfjr_release(journal_owner); journal_owner=nullptr;
                            }
                            v->drained=1; v->finished=1; ++v->snapshot.sequence;
                            publish(s,v);
                        }
                    }
                }
            }
            command_free(command);
        }
        if (v->finished && atomic_load(&s->stop)) break;
        struct timespec delay={0,50000000}; nanosleep(&delay,nullptr);
    }

done:
    if (active_rev) ckfss_two_way_review_destroy(active_rev);
    if (tw) {
        ckfss_error fin_err; memset(&fin_err,0,sizeof(fin_err));
        (void)ckfss_two_way_finish_io(tw,&fin_err);
        ckfss_two_way_destroy(tw);
    }
    if (journal_owner) {
        (void)ckfjr_finish_v1(journal_owner);
        for (unsigned ri=0; ri<600; ++ri) {
            uint32_t rdrained=0;
            if (ckfjr_worker_drained_v1(journal_owner,&rdrained)!=CKFE_API_OK || rdrained) break;
            (void)ckfjr_wait_v1(journal_owner,25);
        }
        ckfjr_release(journal_owner);
    }
    ckfe_engine_destroy(engine); ckfe_error_free(&error);
    v->drained=1; v->finished=1; v->stopped=1; publish(s,v); free(v); return nullptr;
}

static void *worker(void *arg)
{
    SyncJob *s=arg; SyncView *v=calloc(1,sizeof(*v));
    ckfe_engine *engine=nullptr; ckfe_entry *source=nullptr,*destination=nullptr;
    ckfjr_root *journal_owner=nullptr;
    ckfet_job *job=nullptr; ckfe_error error; ckfe_error_init(&error);
    if (!v) {
        pthread_mutex_lock(&s->mutex); s->view.stopped=1; s->view.api=CKFE_API_OUT_OF_MEMORY;
        strcpy(s->view.error,"Cannot allocate operation view"); ++s->view.revision;
        pthread_mutex_unlock(&s->mutex); return nullptr;
    }
    v->snapshot.state=CKFET_INVENTORY;
    if (ckfe_abi_version_major()!=0 || ckfe_abi_version_minor()<3) {
        v->snapshot.state=CKFET_FAILED; v->api=CKFE_API_OPERATION_UNAVAILABLE;
        strcpy(v->error,"Installed filesystem library does not provide the required ABI 0.3");
        goto done;
    }
    if (s->profile==SYNC_PROFILE_TWO_WAY) return worker_two_way(s,v);
    ckfe_api_status api=ckfe_engine_create(&engine);
    if (api==CKFE_API_OK && !engine) api=CKFE_API_OPERATION_UNAVAILABLE;
    if (api!=CKFE_API_OK || !engine) goto failed;
    if (atomic_load(&s->stop) || atomic_load(&s->cancel)) goto cancelled;
    api=open_root(engine,s->source,&source,&error); if (api!=CKFE_API_OK) goto failed;
    api=open_root(engine,s->destination,&destination,&error); if (api!=CKFE_API_OK) goto failed;
    ckfet_options_v2 options_v2; ckfet_options_init_v2(&options_v2);
    options_v2.namespace_policy=CKFET_NAMESPACE_HELD_PARENT_RENAME;
    options_v2.base.profile=s->profile; options_v2.base.metadata_scope=s->metadata_scope;
    options_v2.base.metadata_ownership=s->metadata_scope==CKFE_METADATA_ORDINARY_CALLER_VISIBLE_V1 ?
        CKFE_OWNERSHIP_CURRENT_CALLER : CKFE_OWNERSHIP_ORIGINAL;
    options_v2.base.filters=s->filters; options_v2.base.limits.max_page_rows=SYNC_PAGE_SIZE;
    if (s->profile!=CKFSP_COMPARE) {
        if (!private_journal(s,v,options_v2.base.operation_id)) {
            v->snapshot.state=CKFET_FAILED; v->api=CKFE_API_OPERATION_UNAVAILABLE;
            if (!v->error[0]) strcpy(v->error,"Cannot open private operation journal directory; check home ownership and permissions");
            goto done;
        }
        ckfe_directory *home_dir=nullptr;
        api=ckfe_directory_open_utf8(engine,s->home,0,&home_dir,&error);
        if (api!=CKFE_API_OK || !home_dir) {
            if (api==CKFE_API_OK) api=CKFE_API_OPERATION_UNAVAILABLE;
            goto failed;
        }
        ckfjr_options_v1 prep; ckfjr_options_init_v1(&prep);
        prep.parent=home_dir;
        prep.disposition=CKFJR_PROVISION_CHILD;
        prep.child.encoding=CKFJR_POSIX_BYTES;
        prep.child.data=".dtfile-operations";
        prep.child.byte_count=strlen(".dtfile-operations");
        api=ckfjr_start_v1(&prep,&journal_owner);
        ckfe_directory_destroy(home_dir);
        if (api!=CKFE_API_OK || !journal_owner) {
            if (api==CKFE_API_OK) api=CKFE_API_OPERATION_UNAVAILABLE;
            goto failed;
        }
        ckfjr_snapshot_v1 rsnap; ckfjr_snapshot_init_v1(&rsnap);
        for (;;) {
            if (atomic_load(&s->stop) || atomic_load(&s->cancel)) {
                (void)ckfjr_cancel_v1(journal_owner);
                goto cancelled;
            }
            api=ckfjr_poll_v1(journal_owner,&rsnap);
            if (api!=CKFE_API_OK) goto failed;
            if (rsnap.state!=CKFJR_PREPARING) break;
            (void)ckfjr_wait_v1(journal_owner,50);
        }
        if (rsnap.state==CKFJR_CANCELLED) goto cancelled;
        if (rsnap.state!=CKFJR_READY) {
            ckfe_error re={rsnap.failure_state,rsnap.failure_domain,rsnap.failure_code,
                           rsnap.failure_operation[0]?rsnap.failure_operation:nullptr};
            v->snapshot.state=CKFET_FAILED;
            failure(v,CKFE_API_OPERATION_UNAVAILABLE,&re);
            goto done;
        }
        options_v2.base.journal_root=(ckfet_native){v->journal,strlen(v->journal),CKFET_POSIX_BYTES,0};
    }
    api=journal_owner
        ? ckfet_start_with_journal_root_v2(source,destination,&options_v2,journal_owner,&job,&error)
        : ckfet_start_v2(source,destination,&options_v2,&job,&error);
    if (api==CKFE_API_OK && !job) api=CKFE_API_OPERATION_UNAVAILABLE;
    if (api!=CKFE_API_OK || !job) goto failed;
    v->ready=1;
    uint64_t pending=0, pending_serial=0, offset=0, last_sequence=UINT64_MAX;
    enum CommandKind pending_kind=NO_COMMAND;
    unsigned pending_deletions_approved=0;
    int applied_pause=-1, error_is_cache=0, settling_error=0;
    for (;;) {
        if (atomic_load(&s->cancel) || atomic_load(&s->stop)) ckfet_request_cancel(job);
        int pause=atomic_load(&s->pause);
        if (pause!=applied_pause) { ckfet_request_pause(job,(uint32_t)pause); applied_pause=pause; }
        Command *command=nullptr;
        pthread_mutex_lock(&s->mutex); command=s->command; s->command=nullptr; pthread_mutex_unlock(&s->mutex);
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
                        ckfe_api_status key_api=ckfet_key_copy_v1(job,command->generation,in->selected[0],nullptr,0,&bytes);
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
                uint64_t *all_del=nullptr, all_del_count=0;
                const uint64_t *del_ptr=command->deletions;
                uint64_t del_n=command->deletion_count;
                if (v->api==CKFE_API_OK && command->approve_all_deletions && !del_n) {
                    v->api=collect_all_deletions(s,job,command->generation,&all_del,&all_del_count);
                    del_ptr=all_del; del_n=all_del_count;
                }
                pending_deletions_approved = command->approve_all_deletions || del_n > 0;
                if (v->api==CKFE_API_OK) v->api=ckfet_review_names_v1(job,command->generation,rules,command->count,
                        command->rename ? &name : nullptr,command->rename ? 1 : 0,
                        del_ptr,del_n,&pending);
                free(all_del);
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
            failure(v,api,nullptr);
            v->snapshot.state=CKFET_NEEDS_RECONCILIATION; v->snapshot.recovery_required=1;
            v->cache_valid=0; v->finished=0; v->settling=1; settling_error=1;
            atomic_store(&s->cancel,1); atomic_store(&s->pause,0);
            uint32_t drained=0;
            if (ckfet_worker_drained_v1(job,&drained)==CKFE_API_OK && drained) v->finished=v->drained=1;
            publish(s,v);
            if (v->drained && atomic_load(&s->stop)) break;
            struct timespec delay={0,100000000}; nanosleep(&delay,nullptr);
            continue; /* Never exit a helper while backend writes may continue. */
        }
        v->snapshot=observed_snapshot;
        if (pending && v->snapshot.command_id==pending && v->snapshot.command_state!=CKFET_COMMAND_PENDING) {
            if (v->snapshot.command_state==CKFET_COMMAND_REJECTED)
                snprintf(v->error,sizeof(v->error),"Review rejected (state %u, domain %u, code %lld); inspect unresolved rows",
                    v->snapshot.failure_state,v->snapshot.failure_domain,(long long)v->snapshot.failure_code);
            ckfe_api_status ack=ckfet_acknowledge_command_v1(job,pending);
            if (ack!=CKFE_API_OK) {
                failure(v,ack,nullptr); v->settling=1; settling_error=1; atomic_store(&s->cancel,1); atomic_store(&s->pause,0);
            }
            if (ack==CKFE_API_OK && pending_kind==REVIEW_COMMAND && v->snapshot.command_state==CKFET_COMMAND_ACCEPTED) {
                v->review_submission=pending_serial;
                v->deletions_approved=pending_deletions_approved;
            } else if (pending_kind==REVIEW_COMMAND) {
                v->deletions_approved=0;
            }
            action_complete(s,v,pending_serial,ack!=CKFE_API_OK ? ack : v->snapshot.command_state==CKFET_COMMAND_ACCEPTED ?
                CKFE_API_OK : CKFE_API_OPERATION_UNAVAILABLE,ack==CKFE_API_OK && v->snapshot.command_state==CKFET_COMMAND_ACCEPTED);
            pending=0; pending_serial=0; pending_kind=NO_COMMAND; refresh=1;
        }
        if (v->snapshot.failure_domain!=CKFE_ERROR_NONE && !v->error[0]) {
            ckfet_failure_details_v1 det;
            ckfet_failure_details_init_v1(&det);
            char *op_tag = nullptr;
            if (ckfet_get_failure_details_v1(job, &det) == CKFE_API_OK && det.has_failure && det.operation[0])
                op_tag = det.operation;
            ckfe_error observed={v->snapshot.failure_state,v->snapshot.failure_domain,v->snapshot.failure_code,op_tag};
            failure(v,CKFE_API_OPERATION_UNAVAILABLE,&observed);
            error_is_cache=0;
        }
        if (v->snapshot.plan_generation && (refresh || !v->cache_valid || !v->names_valid || v->snapshot.sequence!=last_sequence)) {
            v->cache_valid=0;
            api=read_page(job,v,offset);
            if (api!=CKFE_API_OK) { failure(v,api,nullptr); error_is_cache=1; }
            else if (error_is_cache) { v->api=CKFE_API_OK; v->error[0]=0; error_is_cache=0; }
            memset(v->counts,0,sizeof(v->counts));
            ckfe_api_status counts_api=v->snapshot.rule_count>SYNC_MAX_RULES ? CKFE_API_INVALID_ARGUMENT : CKFE_API_OK;
            for (uint64_t i=0; counts_api==CKFE_API_OK && i<v->snapshot.rule_count; ++i)
                counts_api=ckfet_rule_counts_get_v1(job,i,&v->counts[i]);
            if (counts_api!=CKFE_API_OK) {
                failure(v,counts_api,nullptr); v->cache_valid=0; error_is_cache=1;
            }
        }
        if (v->snapshot.state==CKFET_AWAITING_REVIEW && (!v->summary_valid ||
            v->summary_generation!=v->snapshot.plan_generation || v->summary_receipt!=v->snapshot.receipt_id)) {
            v->summary_valid=0;
            api=summarize(s,job,v);
            if (api!=CKFE_API_OK) { failure(v,api,nullptr); v->cache_valid=0; error_is_cache=1; }
        }
        if (v->cache_valid && v->names_valid) last_sequence=v->snapshot.sequence;
        if (terminal(v->snapshot.state)) {
            uint32_t drained=0;
            ckfe_api_status drain_api=ckfet_worker_drained_v1(job,&drained);
            if (drain_api==CKFE_API_OK) v->drained=!!drained;
            else { failure(v,drain_api,nullptr); v->settling=1; settling_error=1; }
        }
        v->finished=terminal(v->snapshot.state) && v->drained;
        publish(s,v);
        /* Keep terminal jobs queryable until the UI releases the result view. */
        if (v->finished && atomic_load(&s->stop)) break;
        struct timespec delay={0,100000000}; nanosleep(&delay,nullptr);
    }
    goto done;
cancelled:
    v->snapshot.state=CKFET_CANCELLED; goto done;
failed:
    v->snapshot.state=CKFET_FAILED; failure(v,api,&error);
done:
    if (job) {
        ckfet_request_cancel(job);
        for (unsigned ji=0; ji<600; ++ji) {
            uint32_t jdrained=0;
            if (ckfet_worker_drained_v1(job,&jdrained)!=CKFE_API_OK || jdrained) break;
            struct timespec jts={0,20000000}; nanosleep(&jts,nullptr);
        }
        ckfet_release(job);
    }
    if (journal_owner) {
        (void)ckfjr_finish_v1(journal_owner);
        for (unsigned ri=0; ri<600; ++ri) {
            uint32_t rdrained=0;
            if (ckfjr_worker_drained_v1(journal_owner,&rdrained)!=CKFE_API_OK || rdrained) break;
            (void)ckfjr_wait_v1(journal_owner,25);
        }
        ckfjr_release(journal_owner);
    }
    ckfe_entry_destroy(destination); ckfe_entry_destroy(source); ckfe_engine_destroy(engine); ckfe_error_free(&error);
    v->finished=1; v->stopped=1; publish(s,v); free(v); return nullptr;
}
SyncJob *SyncJobStart(const SyncSetup *setup, char *error, size_t capacity)
{
    if (!setup || !setup->source || !setup->destination || setup->source[0]!='/' || setup->destination[0]!='/' ||
        strnlen(setup->source,65537)>65536 || strnlen(setup->destination,65537)>65536) {
        snprintf(error,capacity,"Choose two absolute folder paths"); return nullptr;
    }
    SyncJob *s=calloc(1,sizeof(*s)); if (!s) return nullptr;
    s->source=strdup(setup->source); s->destination=strdup(setup->destination);
    const char *home=getenv("HOME"); s->home=home ? strdup(home) : nullptr;
    s->profile=setup->profile; s->metadata_scope=setup->metadata_scope; s->filters=setup->filters;
    atomic_init(&s->stop,0); atomic_init(&s->cancel,0); atomic_init(&s->pause,0);
    if (!s->source || !s->destination || (!s->home && setup->profile!=CKFSP_COMPARE)) goto fail_alloc;
    if (pthread_mutex_init(&s->mutex,nullptr)) goto fail_alloc;
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
    snprintf(error,capacity,"Cannot start filesystem worker"); return nullptr;
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
uint64_t SyncJobReviewEx(SyncJob *s, uint64_t generation, const SyncRule *rules, unsigned count,
                  const uint64_t *deletions, unsigned deletion_count,
                  unsigned approve_all_deletions, unsigned approve_first_run,
                  uint64_t rename_row, const char *rename, uint32_t side)
{
    if (count>SYNC_MAX_RULES || deletion_count>100000u || (count && !rules) || (deletion_count && !deletions)) return 0;
    Command *c=calloc(1,sizeof(*c)); if (!c) return 0;
    c->kind=REVIEW_COMMAND; c->generation=generation; c->count=count; c->deletion_count=deletion_count;
    c->approve_all_deletions=!!approve_all_deletions;
    c->approve_first_run=!!approve_first_run;
    for (unsigned i=0; i<count; ++i) {
        c->rules[i]=rules[i]; c->rules[i].subtree=nullptr;
        if (rules[i].selected_count>SYNC_PAGE_SIZE || rules[i].subtree_size>65536 ||
            (rules[i].subtree_size && !rules[i].subtree)) { command_free(c); return 0; }
        if (rules[i].subtree_size) {
            c->rules[i].subtree=malloc(rules[i].subtree_size);
            if (!c->rules[i].subtree) { command_free(c); return 0; }
            memcpy(c->rules[i].subtree,rules[i].subtree,rules[i].subtree_size);
        }
    }
    if (deletion_count) {
        c->deletions=malloc((size_t)deletion_count*sizeof(*deletions));
        if (!c->deletions) { command_free(c); return 0; }
        memcpy(c->deletions,deletions,(size_t)deletion_count*sizeof(*deletions));
    }
    if (rename) { c->rename=strdup(rename); if (!c->rename) { command_free(c); return 0; } }
    c->rename_row=rename_row; c->rename_side=side; return submit(s,c);
}
uint64_t SyncJobReview(SyncJob *s, uint64_t generation, const SyncRule *rules, unsigned count,
                  const uint64_t *deletions, unsigned deletion_count, uint64_t rename_row,
                  const char *rename, uint32_t side)
{
    return SyncJobReviewEx(s,generation,rules,count,deletions,deletion_count,0,0,rename_row,rename,side);
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
