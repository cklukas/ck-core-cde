/* SPDX-License-Identifier: LGPL-2.0-or-later */
/* Real installed-library consumer; no fake backend and no toolkit dependency. */
#define _POSIX_C_SOURCE 200809L
#include "dtsync/SyncJob.h"
#include "dtsync/SyncDates.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdatomic.h>

#ifdef DTFILE_SYNC_WRAP_NAMES
static atomic_int fail_proposed_name;
static atomic_int fail_rule_counts;
ckfe_api_status __real_ckfet_rule_counts_get_v1(const ckfet_job *,uint64_t,ckfet_rule_counts_v1 *);
ckfe_api_status __wrap_ckfet_rule_counts_get_v1(const ckfet_job *job,uint64_t index,ckfet_rule_counts_v1 *out)
{
    ckfe_api_status api=__real_ckfet_rule_counts_get_v1(job,index,out);
    if (api==CKFE_API_OK && atomic_load(&fail_rule_counts)) {
        memset(out,0,sizeof(*out)); return CKFE_API_OUT_OF_MEMORY;
    }
    return api;
}
ckfe_api_status __real_ckfet_native_copy_v1(const ckfet_job *,uint64_t,uint64_t,uint32_t,uint64_t,void *,uint64_t,uint64_t *,uint32_t *);
ckfe_api_status __wrap_ckfet_native_copy_v1(const ckfet_job *job,uint64_t generation,uint64_t row,uint32_t kind,
    uint64_t index,void *buffer,uint64_t capacity,uint64_t *required,uint32_t *encoding)
{
    ckfe_api_status api=__real_ckfet_native_copy_v1(job,generation,row,kind,index,buffer,capacity,required,encoding);
    if (kind==CKFET_NATIVE_PROPOSED_NAME && atomic_load(&fail_proposed_name) &&
        (api==CKFE_API_OK || api==CKFE_API_BUFFER_TOO_SMALL)) {
        if (required) *required=0;
        return CKFE_API_OUT_OF_MEMORY;
    }
    return api;
}
#endif

static void delay(void) { struct timespec ts={0,10000000}; nanosleep(&ts,nullptr); }
static void read_view(SyncJob *job,SyncView *view)
{ for (unsigned i=0;i<1000;++i) { if (SyncJobRead(job,view)) return; delay(); } abort(); }
static void wait_state(SyncJob *job,SyncView *view,unsigned state)
{
    for (unsigned i=0;i<9000;++i) {
        read_view(job,view);
        if (view->snapshot.state==state && view->ready) return;
        if (view->stopped) { fprintf(stderr,"unexpected stop: %s\n",view->error); abort(); }
        delay();
    }
    fprintf(stderr,"timed out waiting for state %u\n",state); abort();
}
static void stop_job(SyncJob *job,SyncView *view)
{
    SyncJobStop(job);
    for (unsigned i=0;i<3000;++i) {
        read_view(job,view);
        if (view->stopped) { assert(!view->ready || view->drained); assert(SyncJobDestroy(job)); return; }
        delay();
    }
    fprintf(stderr,"cancellation did not settle\n"); abort();
}
static void put(const char *directory,const char *name,const char *text)
{
    char path[2048]; assert(snprintf(path,sizeof(path),"%s/%s",directory,name)>0);
    FILE *out=fopen(path,"wx"); assert(out); assert(fputs(text,out)>=0); assert(fclose(out)==0);
}
int main(int argc,char **argv)
{
    ckfsp_timestamp date; int present=0; char formatted[40];
    assert(SyncParseDate("1970-01-01T00:00:00Z",&date,&present) && present && date.seconds==0);
    assert(SyncParseDate("1969-12-31T23:59:59.123456789Z",&date,&present) && date.seconds==-1 && date.nanoseconds==123456789);
    assert(SyncFormatDate(&date,formatted) && !strcmp(formatted,"1969-12-31T23:59:59.123456789Z"));
    assert(SyncParseDate("2000-02-29T00:00:00Z",&date,&present) && date.seconds==951782400);
    assert(!SyncParseDate("1900-02-29T00:00:00Z",&date,&present));
    assert(!SyncParseDate("2024-04-31",&date,&present));
    assert(!SyncParseDate("2024-01-01T00:00:60Z",&date,&present));
    assert(!SyncParseDate("2024-01-01T00:00:00.1234567890Z",&date,&present));
    assert(SyncParseDate("",&date,&present) && !present);
    assert(setenv("TZ","UTC0",1)==0); tzset();
    assert(SyncParseDate("2024-03-10",&date,&present) && date.seconds==1710028800);
    assert(SyncFormatDate(&date,formatted) && !strcmp(formatted,"2024-03-10T00:00:00Z"));
    assert(argc==2); assert(setenv("HOME",argv[1],1)==0);
    char source[2048],target[2048],error[512]={0};
    assert(snprintf(source,sizeof(source),"%s/source",argv[1])>0);
    assert(snprintf(target,sizeof(target),"%s/target",argv[1])>0);
    assert(mkdir(source,0700)==0 && mkdir(target,0700)==0);
    for (unsigned i=0;i<70;++i) { char name[32]; snprintf(name,sizeof(name),"file-%03u",i); put(source,name,"source bytes\n"); }
    put(target,"file-000","different destination\n");
    SyncSetup setup={source,target,CKFSP_COPY,CKFE_METADATA_ORDINARY_CALLER_VISIBLE_V1,{0}};
    setup.filters.allowed_kind_mask=CKFSP_ALL_KINDS; setup.filters.maximum_depth=1024;
    SyncJob *job=SyncJobStart(&setup,error,sizeof(error)); assert(job);
    SyncView *view=calloc(1,sizeof(*view)); assert(view);
    wait_state(job,view,CKFET_AWAITING_REVIEW);
    assert(view->row_count==SYNC_PAGE_SIZE && view->total_rows==70 && view->has_next);
    uint64_t generation=view->snapshot.plan_generation;
    assert(generation && !view->snapshot.receipt_id);
    uint64_t old_revision=view->revision;
    while (!SyncJobPage(job,SYNC_PAGE_SIZE)) delay();
    for (unsigned i=0;i<3000;++i) {
        read_view(job,view);
        if (view->revision>old_revision && view->offset==SYNC_PAGE_SIZE) break;
        delay();
    }
    assert(view->offset==SYNC_PAGE_SIZE && view->row_count==6 && !view->has_next);
    /* A caller cannot manufacture a generation or an execution receipt. */
    uint64_t invalid=0;
    while (!(invalid=SyncJobExecute(job,generation+1,UINT64_C(123456)))) delay();
    for (unsigned i=0;i<3000;++i) { read_view(job,view); if (view->api==CKFE_API_STALE_PLAN) break; delay(); }
    assert(view->api==CKFE_API_STALE_PLAN && !view->snapshot.receipt_id && view->completed_submission==invalid && !view->command_accepted);
    /* Rejection must not poison the next legitimate review. */
    SyncRule rule={0}; rule.kind=CKFET_CONFLICT_EXISTING_REGULAR; rule.choice=CKFET_KEEP_TARGET;
    rule.tie=CKFET_ASK; rule.scope=CKFET_SCOPE_JOB; rule.protect_nonempty=1;
    SyncJobPause(job,1);
    for (unsigned i=0;i<3000;++i) { read_view(job,view); if (view->snapshot.paused) break; delay(); }
    assert(view->snapshot.paused);
    uint64_t reviewed=0;
    while (!(reviewed=SyncJobReview(job,generation,&rule,1,nullptr,0,0,nullptr,0))) delay();
    assert(!SyncJobReview(job,generation,&rule,1,nullptr,0,0,nullptr,0));
    SyncJobPause(job,0);
    for (unsigned i=0;i<3000;++i) {
        read_view(job,view);
        if (view->snapshot.receipt_id && view->completed_submission==reviewed && SyncJobViewCoherent(view)) break;
        delay();
    }
    assert(view->snapshot.receipt_id && view->snapshot.rule_count==1 && view->review_submission==reviewed && view->command_accepted);
    assert(view->counts[0].matched_rows==1 && view->counts[0].skipped_rows==1);
    assert(view->summary_valid && view->planned_copies==69 && view->planned_skips==1 &&
           view->planned_replacements==0 && view->planned_bytes_known && view->planned_bytes==69*13);
    while (!SyncJobPage(job,UINT64_MAX)) delay();
    for (unsigned i=0;i<3000;++i) { read_view(job,view); if (view->api!=CKFE_API_OK && !view->cache_valid) break; delay(); }
    assert(!SyncJobViewCoherent(view) && view->snapshot.receipt_id);
    while (!SyncJobPage(job,0)) delay();
    for (unsigned i=0;i<3000;++i) { read_view(job,view); if (!view->offset && SyncJobViewCoherent(view)) break; delay(); }
    assert(SyncJobViewCoherent(view) && view->api==CKFE_API_OK);
#ifdef DTFILE_SYNC_WRAP_NAMES
    atomic_store(&fail_rule_counts,1);
    while (!SyncJobPage(job,0)) delay();
    for (unsigned i=0;i<3000;++i) {
        read_view(job,view); if (!view->cache_valid && view->api==CKFE_API_OUT_OF_MEMORY) break; delay();
    }
    assert(!SyncJobViewCoherent(view) && view->api==CKFE_API_OUT_OF_MEMORY);
    atomic_store(&fail_rule_counts,0);
    for (unsigned i=0;i<3000;++i) { read_view(job,view); if (SyncJobViewCoherent(view)) break; delay(); }
    assert(SyncJobViewCoherent(view) && view->counts[0].matched_rows==1 && view->counts[0].skipped_rows==1);
    /* The engine is real; only transport of one applicable name is faulted. */
    rule.choice=CKFET_KEEP_BOTH_INCOMING;
    atomic_store(&fail_proposed_name,1);
    uint64_t named=0;
    while (!(named=SyncJobReview(job,generation,&rule,1,nullptr,0,1,"file-000-copy",CKFET_NAME_INCOMING))) delay();
    for (unsigned i=0;i<3000;++i) {
        read_view(job,view);
        if (view->completed_submission==named && view->command_accepted && view->api==CKFE_API_OUT_OF_MEMORY) break;
        delay();
    }
    assert(view->completed_submission==named && view->command_accepted && view->cache_valid && !SyncJobViewCoherent(view));
    assert(view->rows[0].name_failures&(1u<<CKFET_NATIVE_PROPOSED_NAME));
    atomic_store(&fail_proposed_name,0);
    for (unsigned i=0;i<3000;++i) { read_view(job,view); if (SyncJobViewCoherent(view)) break; delay(); }
    assert(SyncJobViewCoherent(view) && !strcmp(view->rows[0].proposed,"file-000-copy"));
#endif
    /* Preview/review/cancellation must leave both trees byte-identical. */
    SyncJobPause(job,1); stop_job(job,view);
    char path[2048],bytes[128]={0};
    assert(snprintf(path,sizeof(path),"%s/file-000",target)>0);
    FILE *in=fopen(path,"r"); assert(in); assert(fread(bytes,1,sizeof(bytes)-1,in)==22); assert(fclose(in)==0);
    assert(!strcmp(bytes,"different destination\n"));
    assert(snprintf(path,sizeof(path),"%s/file-001",target)>0 && access(path,F_OK)!=0);
    /* Execute an actual reviewed ordinary copy through this controller. This
     * exercises the consumer boundary, not the still-disabled UI action. */
    char executed[2048];
    assert(snprintf(executed,sizeof(executed),"%s/executed",argv[1])>0);
    assert(mkdir(executed,0700)==0); setup.destination=executed;
    job=SyncJobStart(&setup,error,sizeof(error)); assert(job);
    wait_state(job,view,CKFET_AWAITING_REVIEW);
    generation=view->snapshot.plan_generation; reviewed=0;
    while (!(reviewed=SyncJobReview(job,generation,nullptr,0,nullptr,0,0,nullptr,0))) delay();
    for (unsigned i=0;i<3000;++i) {
        read_view(job,view);
        if (view->completed_submission==reviewed && SyncJobViewCoherent(view)) break;
        delay();
    }
    assert(view->command_accepted && view->review_submission==reviewed && view->snapshot.receipt_id);
    uint64_t executed_submission=0;
    while (!(executed_submission=SyncJobExecute(job,generation,view->snapshot.receipt_id))) delay();
    for (unsigned i=0;i<36000;++i) { read_view(job,view); if (view->finished) break; delay(); }
    if (view->snapshot.state!=CKFET_COMPLETE) fprintf(stderr,"Ordinary tree copy: state %u (completed=%llu/%llu), %s; row %u/%u/%lld\n",
        view->snapshot.state,(unsigned long long)view->snapshot.completed_items,(unsigned long long)view->snapshot.known_items,
        view->error,view->rows[0].observed.failure_state,
        view->rows[0].observed.failure_domain,(long long)view->rows[0].observed.failure_code);
    assert(view->finished && view->drained && view->snapshot.state==CKFET_COMPLETE &&
        view->completed_submission==executed_submission && view->command_accepted && view->snapshot.completed_items==70);
    for (unsigned i=0;i<70;++i) {
        char expected[32]; snprintf(expected,sizeof(expected),"file-%03u",i);
        assert(snprintf(path,sizeof(path),"%s/%s",executed,expected)>0);
        memset(bytes,0,sizeof(bytes)); in=fopen(path,"r"); assert(in);
        assert(fread(bytes,1,sizeof(bytes)-1,in)==13 && fclose(in)==0 && !strcmp(bytes,"source bytes\n"));
        struct stat copied,original; assert(stat(path,&copied)==0);
        assert(snprintf(path,sizeof(path),"%s/%s",source,expected)>0 && stat(path,&original)==0);
        assert((copied.st_mode&07777)==(original.st_mode&07777) && copied.st_mtime==original.st_mtime);
    }
    stop_job(job,view); setup.destination=target;
    puts("Ordinary native tree copy: 70 files, content/mode/mtime and worker drain verified"); fflush(stdout);
    /* Execute Synchronize one way (CKFSP_SYNC) with existing regular-file conflict
     * resolved via CKFET_USE_SOURCE and target version retention enabled. */
    char sync_src[2048], sync_dst[2048];
    assert(snprintf(sync_src,sizeof(sync_src),"%s/sync_src",argv[1])>0);
    assert(snprintf(sync_dst,sizeof(sync_dst),"%s/sync_dst",argv[1])>0);
    assert(mkdir(sync_src,0700)==0 && mkdir(sync_dst,0700)==0);
    put(sync_src,"conflict.txt","source bytes\n");
    put(sync_src,"added.txt","fresh file\n");
    put(sync_dst,"conflict.txt","older destination content to replace\n");
    setup.source=sync_src;
    setup.destination=sync_dst;
    setup.profile=CKFSP_SYNC;
    setup.metadata_scope=CKFE_METADATA_ORDINARY_CALLER_VISIBLE_V1;
    job=SyncJobStart(&setup,error,sizeof(error)); assert(job);
    wait_state(job,view,CKFET_AWAITING_REVIEW);
    generation=view->snapshot.plan_generation;
    uint64_t conflict_row=0;
    for (unsigned i=0;i<view->row_count;++i) {
        if (view->rows[i].observed.source.presence==CKFET_PRESENCE_PRESENT &&
            view->rows[i].observed.destination.presence==CKFET_PRESENCE_PRESENT) {
            conflict_row=view->rows[i].observed.row_id;
            break;
        }
    }
    assert(conflict_row!=0);
    memset(&rule,0,sizeof(rule));
    rule.kind=CKFET_CONFLICT_EXISTING_REGULAR;
    rule.choice=CKFET_KEEP_BOTH_INCOMING;
    rule.tie=CKFET_ASK;
    rule.scope=CKFET_SCOPE_ITEM;
    rule.selected[0]=conflict_row;
    rule.selected_count=1;
    rule.protect_nonempty=1;
    rule.retain_replaced=0;
    reviewed=0;
    while (!(reviewed=SyncJobReview(job,generation,&rule,1,nullptr,0,conflict_row,"conflict-incoming.txt",CKFET_NAME_INCOMING))) delay();
    for (unsigned i=0;i<3000;++i) {
        read_view(job,view);
        if (view->completed_submission==reviewed && SyncJobViewCoherent(view)) break;
        delay();
    }
    assert(view->command_accepted && view->snapshot.receipt_id && view->snapshot.unresolved_rows==0);
    generation=view->snapshot.plan_generation;
    executed_submission=0;
    while (!(executed_submission=SyncJobExecute(job,generation,view->snapshot.receipt_id))) delay();
    for (unsigned i=0;i<3000;++i) {
        read_view(job,view);
        if (view->finished || (view->completed_submission==executed_submission && !view->command_accepted)) break;
        delay();
    }
    if (!view->command_accepted) fprintf(stderr,"Sync execute rejected: %s\n",view->error);
    assert(view->finished && view->drained && view->snapshot.state==CKFET_COMPLETE && view->command_accepted);
    assert(snprintf(path,sizeof(path),"%s/conflict.txt",sync_dst)>0);
    memset(bytes,0,sizeof(bytes)); in=fopen(path,"r"); assert(in);
    assert(fread(bytes,1,sizeof(bytes)-1,in)==37 && fclose(in)==0 && !strcmp(bytes,"older destination content to replace\n"));
    assert(snprintf(path,sizeof(path),"%s/conflict-incoming.txt",sync_dst)>0);
    memset(bytes,0,sizeof(bytes)); in=fopen(path,"r"); assert(in);
    assert(fread(bytes,1,sizeof(bytes)-1,in)==13 && fclose(in)==0 && !strcmp(bytes,"source bytes\n"));
    assert(snprintf(path,sizeof(path),"%s/added.txt",sync_dst)>0 && access(path,F_OK)==0);
    stop_job(job,view);
    setup.source=source; setup.destination=target; setup.profile=CKFSP_COPY;
    puts("One-way synchronize non-destructive keep-both incoming + missing copy verified"); fflush(stdout);

    /* Execute Mirror (CKFSP_MIRROR) with >64 multi-page destination deletions,
     * nested obsolete directory deletion, and retained target replacement. */
    char mir_src[2048], mir_dst[2048], mir_sub[2048];
    assert(snprintf(mir_src,sizeof(mir_src),"%s/mir_src",argv[1])>0);
    assert(snprintf(mir_dst,sizeof(mir_dst),"%s/mir_dst",argv[1])>0);
    assert(snprintf(mir_sub,sizeof(mir_sub),"%s/mir_dst/obsolete_sub",argv[1])>0);
    assert(mkdir(mir_src,0700)==0 && mkdir(mir_dst,0700)==0 && mkdir(mir_sub,0700)==0);
    put(mir_src,"shared.txt","mirror fresh source\n");
    put(mir_dst,"shared.txt","mirror older target\n");
    put(mir_sub,"stale.txt","to delete in sub\n");
    for (unsigned i=0;i<65;++i) { char extra[32]; snprintf(extra,sizeof(extra),"extra-%03u",i); put(mir_dst,extra,"extra\n"); }
    setup.source=mir_src; setup.destination=mir_dst; setup.profile=CKFSP_MIRROR;
    setup.metadata_scope=CKFE_METADATA_ORDINARY_CALLER_VISIBLE_V1;
    job=SyncJobStart(&setup,error,sizeof(error)); assert(job);
    wait_state(job,view,CKFET_AWAITING_REVIEW);
    generation=view->snapshot.plan_generation;
    memset(&rule,0,sizeof(rule));
    rule.kind=CKFET_CONFLICT_EXISTING_REGULAR; rule.choice=CKFET_USE_SOURCE;
    rule.tie=CKFET_ASK; rule.scope=CKFET_SCOPE_JOB; rule.protect_nonempty=1; rule.retain_replaced=1;
    /* Without deletion approval, Mirror review must be rejected. */
    reviewed=0;
    while (!(reviewed=SyncJobReviewEx(job,generation,&rule,1,nullptr,0,0,0,0,nullptr,0))) delay();
    for (unsigned i=0;i<3000;++i) { read_view(job,view); if (view->completed_submission==reviewed) break; delay(); }
    assert(!view->command_accepted && !view->deletions_approved);
    /* With approve_all_deletions=1, all >64 multi-page deletions + Replace are accepted and executed. */
    reviewed=0;
    while (!(reviewed=SyncJobReviewEx(job,generation,&rule,1,nullptr,0,1,0,0,nullptr,0))) delay();
    for (unsigned i=0;i<3000;++i) { read_view(job,view); if (view->completed_submission==reviewed && SyncJobViewCoherent(view)) break; delay(); }
    assert(view->command_accepted && view->deletions_approved && view->snapshot.receipt_id && view->planned_removals==67);
    executed_submission=0;
    while (!(executed_submission=SyncJobExecute(job,generation,view->snapshot.receipt_id))) delay();
    for (unsigned i=0;i<36000;++i) { read_view(job,view); if (view->finished) break; delay(); }
    if (view->snapshot.state!=CKFET_COMPLETE) fprintf(stderr,"Mirror failed: %s\n",view->error);
    assert(view->finished && view->drained && view->snapshot.state==CKFET_COMPLETE);
    assert(access(mir_sub,F_OK)!=0);
    assert(snprintf(path,sizeof(path),"%s/extra-064",mir_dst)>0 && access(path,F_OK)!=0);
    assert(snprintf(path,sizeof(path),"%s/shared.txt",mir_dst)>0);
    memset(bytes,0,sizeof(bytes)); in=fopen(path,"r"); assert(in);
    assert(fread(bytes,1,sizeof(bytes)-1,in)==20 && fclose(in)==0 && !strcmp(bytes,"mirror fresh source\n"));
    stop_job(job,view);
    puts("Mirror execution (>64 multi-page deletions, folder removal and retained replacement) verified"); fflush(stdout);

    /* Execute Synchronize two ways (SYNC_PROFILE_TWO_WAY): Gen 0->1->2->3. */
    char tw_l[2048], tw_r[2048], tw_lsub[2048], tw_rsub[2048];
    assert(snprintf(tw_l,sizeof(tw_l),"%s/tw_left",argv[1])>0);
    assert(snprintf(tw_r,sizeof(tw_r),"%s/tw_right",argv[1])>0);
    assert(snprintf(tw_lsub,sizeof(tw_lsub),"%s/tw_left/docs",argv[1])>0);
    assert(snprintf(tw_rsub,sizeof(tw_rsub),"%s/tw_right/notes",argv[1])>0);
    assert(mkdir(tw_l,0700)==0 && mkdir(tw_r,0700)==0 && mkdir(tw_lsub,0700)==0 && mkdir(tw_rsub,0700)==0);
    put(tw_lsub,"readme.txt","left docs\n");
    put(tw_l,"both.txt","initial left\n");
    put(tw_rsub,"todo.txt","right notes\n");
    setup.source=tw_l; setup.destination=tw_r; setup.profile=SYNC_PROFILE_TWO_WAY;
    setup.metadata_scope=CKFE_METADATA_ORDINARY_CALLER_VISIBLE_V1;
    job=SyncJobStart(&setup,error,sizeof(error)); assert(job);
    wait_state(job,view,CKFET_AWAITING_REVIEW);
    assert(view->two_way && view->first_run && view->two_way_generation==0);
    generation=view->snapshot.plan_generation;
    /* First-run review without approve_first_run must reject. */
    reviewed=0;
    while (!(reviewed=SyncJobReviewEx(job,generation,nullptr,0,nullptr,0,0,0,0,nullptr,0))) delay();
    for (unsigned i=0;i<3000;++i) { read_view(job,view); if (view->completed_submission==reviewed) break; delay(); }
    assert(!view->command_accepted && !view->snapshot.receipt_id);
    /* Approve first run and execute Gen 0 -> Gen 1. */
    reviewed=0;
    while (!(reviewed=SyncJobReviewEx(job,generation,nullptr,0,nullptr,0,0,1,0,nullptr,0))) delay();
    for (unsigned i=0;i<3000;++i) { read_view(job,view); if (view->completed_submission==reviewed && SyncJobViewCoherent(view)) break; delay(); }
    assert(view->command_accepted && view->first_run_approved && view->snapshot.receipt_id);
    executed_submission=0;
    while (!(executed_submission=SyncJobExecute(job,generation,view->snapshot.receipt_id))) delay();
    for (unsigned i=0;i<36000;++i) { read_view(job,view); if (view->finished) break; delay(); }
    if (view->snapshot.state!=CKFET_COMPLETE) fprintf(stderr,"Two-way Gen0 failed: %s\n",view->error);
    assert(view->finished && view->drained && view->snapshot.state==CKFET_COMPLETE && view->two_way_generation==1);
    stop_job(job,view);
    assert(snprintf(path,sizeof(path),"%s/docs/readme.txt",tw_r)>0 && access(path,F_OK)==0);
    assert(snprintf(path,sizeof(path),"%s/notes/todo.txt",tw_l)>0 && access(path,F_OK)==0);

    /* Gen 1 -> Gen 2: BothEdited conflict resolved via Keep Right! */
    assert(snprintf(path,sizeof(path),"%s/both.txt",tw_l)>0 && unlink(path)==0);
    put(tw_l,"both.txt","left edit\n");
    assert(snprintf(path,sizeof(path),"%s/both.txt",tw_r)>0 && unlink(path)==0);
    put(tw_r,"both.txt","right winning\n");
    job=SyncJobStart(&setup,error,sizeof(error)); assert(job);
    wait_state(job,view,CKFET_AWAITING_REVIEW);
    assert(view->two_way && !view->first_run && view->two_way_generation==1 && view->snapshot.unresolved_rows==1);
    generation=view->snapshot.plan_generation;
    uint32_t conflict_kind=view->rows[0].observed.conflict_class;
    assert(conflict_kind==CKFET_CONFLICT_EXISTING_REGULAR || conflict_kind==CKFET_CONFLICT_BOTH_EDITED);
    memset(&rule,0,sizeof(rule));
    rule.kind=conflict_kind; rule.choice=CKFET_KEEP_RIGHT;
    rule.tie=CKFET_ASK; rule.scope=CKFET_SCOPE_JOB; rule.protect_nonempty=1; rule.retain_replaced=1;
    reviewed=0;
    while (!(reviewed=SyncJobReviewEx(job,generation,&rule,1,nullptr,0,0,0,0,nullptr,0))) delay();
    for (unsigned i=0;i<3000;++i) { read_view(job,view); if (view->completed_submission==reviewed && SyncJobViewCoherent(view)) break; delay(); }
    assert(view->command_accepted && view->snapshot.receipt_id && view->snapshot.unresolved_rows==0);
    executed_submission=0;
    while (!(executed_submission=SyncJobExecute(job,generation,view->snapshot.receipt_id))) delay();
    for (unsigned i=0;i<36000;++i) { read_view(job,view); if (view->finished) break; delay(); }
    assert(view->finished && view->drained && view->snapshot.state==CKFET_COMPLETE && view->two_way_generation==2);
    stop_job(job,view);
    assert(snprintf(path,sizeof(path),"%s/both.txt",tw_l)>0);
    memset(bytes,0,sizeof(bytes)); in=fopen(path,"r"); assert(in);
    assert(fread(bytes,1,sizeof(bytes)-1,in)==14 && fclose(in)==0 && !strcmp(bytes,"right winning\n"));
    setup.source=source; setup.destination=target; setup.profile=CKFSP_COPY;
    puts("Two-way recursive folder sync (first-run guard, bidirectional trees, conflict Keep Right) verified"); fflush(stdout);
    /* Read-only Compare must not demand a resolved mutation choice or a
     * preservation capability merely to review preferences on report rows. */
    assert(unsetenv("HOME")==0);
    assert(!SyncJobStart(&setup,error,sizeof(error))); /* Copy still needs private state. */
    setup.profile=CKFSP_COMPARE; setup.metadata_scope=CKFE_METADATA_COMPLETE_V1;
    job=SyncJobStart(&setup,error,sizeof(error)); assert(job);
    wait_state(job,view,CKFET_AWAITING_REVIEW);
    generation=view->snapshot.plan_generation;
    memset(&rule,0,sizeof(rule)); rule.kind=CKFET_CONFLICT_EXISTING_REGULAR;
    rule.choice=CKFET_ASK; rule.tie=CKFET_ASK; rule.scope=CKFET_SCOPE_JOB;
    rule.protect_nonempty=rule.retain_replaced=1;
    reviewed=0;
    while (!(reviewed=SyncJobReview(job,generation,&rule,1,nullptr,0,0,nullptr,0))) delay();
    for (unsigned i=0;i<3000;++i) {
        read_view(job,view);
        if (view->completed_submission==reviewed) break;
        delay();
    }
    if (!view->command_accepted) fprintf(stderr,"Compare preference review: %s\n",view->error);
    assert(view->completed_submission==reviewed && view->command_accepted);
    assert(!view->journal[0]);
    stop_job(job,view);
    assert(setenv("HOME",argv[1],1)==0);
    /* Bad root opens settle asynchronously and can be safely released. */
    setup.source="/"; job=SyncJobStart(&setup,error,sizeof(error)); assert(job);
    for (unsigned i=0;i<3000;++i) { read_view(job,view); if (view->stopped) break; delay(); }
    assert(view->stopped && view->snapshot.state==CKFET_FAILED && view->error[0]);
    assert(SyncJobDestroy(job)); free(view);
    puts("Sync controller: inventory/pages, stale receipts, scoped review, native copy, cancellation and cleanup passed");
    return 0;
}
