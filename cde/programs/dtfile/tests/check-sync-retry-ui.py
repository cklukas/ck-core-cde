#!/usr/bin/env python3
"""Native Xt retry regression, with production callbacks and the real FS SDK.

Run under a private X display (for example xvfb-run). No backend faults, UI
reset substitutes, pixel matching, or file-operation execution commands are used. The wrapper
retains the realized shell and supplies a bounded Xt event loop. Two UI-only
faults return a NULL basename getter and fail one destination-path strdup.
The SDK and profile worker remain real and receive no injected faults.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import time


HARNESS = r'''
#include <Xm/Xm.h>
#include "SyncJob.h"
#include "SyncDates.h"
#include "SyncProfiles.h"
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
/* Define seams only after every production C/Xm header. They affect this UI
 * translation unit, never the separately compiled SDK/profile worker. */
static char *test_text_get(Widget);
static char *test_ui_strdup(const char *);
#define XmTextFieldGetString test_text_get
#define strdup test_ui_strdup
static void test_realize(Widget);
static void test_loop(XtAppContext);
#define main application_main
#define XtRealizeWidget test_realize
#define XtAppMainLoop test_loop
#include MAIN_SOURCE
#undef main
#undef XtRealizeWidget
#undef XtAppMainLoop
#undef XmTextFieldGetString
#undef strdup

static Widget null_basename_field;
static unsigned null_basename_once,null_basename_hits,path_strdup_once,path_strdup_hits;
static const char *fail_path;
static char *test_text_get(Widget field)
{
    if (null_basename_once && field==null_basename_field) {
        null_basename_once=0; ++null_basename_hits; return NULL;
    }
    return XmTextFieldGetString(field);
}
static char *test_ui_strdup(const char *text)
{
    if (path_strdup_once && fail_path && !strcmp(text,fail_path)) {
        path_strdup_once=0; ++path_strdup_hits; return NULL;
    }
    return strdup(text);
}

static Widget test_shell;
static int test_failed;
static void test_realize(Widget shell)
{
    test_shell=shell;
    XtRealizeWidget(shell);
}
static long long milliseconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC,&ts);
    return (long long)ts.tv_sec*1000+ts.tv_nsec/1000000;
}
static int item_count(Widget list)
{
    int count=0;
    XtVaGetValues(list,XmNitemCount,&count,NULL);
    return count;
}
static char *label_text(Widget widget)
{
    XmString text=NULL;
    char *plain=NULL;
    XtVaGetValues(widget,XmNlabelString,&text,NULL);
    if (text) plain=XmStringUnparse(text,NULL,XmCHARSET_TEXT,XmCHARSET_TEXT,NULL,0,XmOUTPUT_ALL);
    return plain ? plain : XtNewString("");
}
static void assertion(int ok,const char *message)
{
    if (!ok) { fprintf(stderr,"FAIL: %s\n",message); test_failed=1; }
}
enum WaitFor { REPORT, ADMISSION_FAILURE, EXIT, PREFERENCES_READY, REVIEWED, CANCELLED_DRAINED };
static int wait_for(SyncWindow *w,enum WaitFor event)
{
    long long end=milliseconds()+15000;
    do {
        while (XtAppPending(w->context)) XtAppProcessEvent(w->context,XtIMAll);
        if (event==EXIT && XtAppGetExitFlag(w->context)) return 1;
        if (event==CANCELLED_DRAINED && w->job && w->view->snapshot.state==CKFET_CANCELLED &&
            w->view->finished && w->view->drained) return 1;
        if (event==REPORT && w->job && !w->recompare && w->view->ready &&
            w->view->snapshot.state==CKFET_AWAITING_REVIEW &&
            item_count(w->list)>0 && XtIsSensitive(w->compare)) return 1;
        SyncProfilesSnapshot preferences;
        if (event==PREFERENCES_READY && w->preferences &&
            SyncProfilesGetSnapshot(w->preferences,&preferences)==SYNC_PROFILES_OK) {
            if (preferences.state==SYNC_PROFILES_ERROR || preferences.state==SYNC_PROFILES_FINISHED) {
                fprintf(stderr,"PROFILE FAILURE: state=%u result=%u: %s\n",
                        preferences.state,preferences.result,preferences.error);
                assertion(0,"private profile fixture must satisfy the real storage contract"); return 0;
            }
            if (preferences.state==SYNC_PROFILES_READY && preferences.result==SYNC_PROFILES_OK &&
                preferences.submitted==preferences.completed && XtIsSensitive(w->profile_load)) return 1;
        }
        if (event==REVIEWED && w->review_serial && !w->review_pending && !w->dirty &&
            w->view->snapshot.receipt_id && w->view->review_submission==w->review_serial &&
            SyncJobViewCoherent(w->view)) return 1;
        char *status=label_text(w->status);
        int failed=event==ADMISSION_FAILURE && !w->job && !w->recompare &&
                   profiles[w->profile_index]==CKFSP_COPY && getenv("HOME")==NULL && status[0];
        XtFree(status);
        if (failed) return 1;
        struct timespec delay={0,5000000}; nanosleep(&delay,NULL);
    } while (milliseconds()<end);
    fprintf(stderr,"TIMEOUT: native event wait %d after15000ms\n",event);
    assertion(0,"bounded native event wait timed out");
    return 0;
}
static void choose_option(Widget option,unsigned index)
{
    Widget menu=NULL;
    WidgetList items=NULL;
    Cardinal count=0;
    XtVaGetValues(option,XmNsubMenuId,&menu,NULL);
    XtVaGetValues(menu,XmNchildren,&items,XmNnumChildren,&count,NULL);
    assertion(index<count,"operation menu item exists");
    if (index>=count) return;
    XtVaSetValues(option,XmNmenuHistory,items[index],NULL);
    XtCallCallbacks(items[index],XmNactivateCallback,NULL);
}
static Widget rule_option(SyncWindow *w,const char *name)
{
    Widget editor=XtNameToWidget(w->rule_page,"*ruleEditor");
    WidgetList children=NULL;
    Cardinal count=0;
    XtVaGetValues(editor,XmNchildren,&children,XmNnumChildren,&count,NULL);
    for (Cardinal i=0;i<count;++i) {
        if (!strcmp(XtName(children[i]),name)) {
            unsigned char type=0;
            XtVaGetValues(children[i],XmNrowColumnType,&type,NULL);
            if (type==XmMENU_OPTION) return children[i];
        }
    }
    return NULL;
}
static int same_job_preference(const SyncRule *a,const SyncRule *b)
{
    return a->scope==CKFET_SCOPE_JOB && b->scope==CKFET_SCOPE_JOB &&
        a->kind==b->kind && a->choice==b->choice && a->tie==b->tie &&
        a->protect_nonempty==b->protect_nonempty && a->retain_replaced==b->retain_replaced &&
        !a->selected_count && !b->selected_count && !a->subtree && !b->subtree &&
        !a->subtree_size && !b->subtree_size;
}
typedef struct { char *source,*destination,*native_source,*native_destination; } Paths;
static Paths save_paths(SyncWindow *w)
{
    Paths p={XmTextFieldGetString(w->source),XmTextFieldGetString(w->destination),
             w->native_source?XtNewString(w->native_source):NULL,
             w->native_destination?XtNewString(w->native_destination):NULL};
    return p;
}
static int same_nullable(const char *a,const char *b)
{ return (!a && !b) || (a && b && !strcmp(a,b)); }
static void check_paths(SyncWindow *w,const Paths *p,int swapped)
{
    Paths actual=save_paths(w);
    assertion(!strcmp(actual.source,swapped?p->destination:p->source) &&
              !strcmp(actual.destination,swapped?p->source:p->destination) &&
              same_nullable(actual.native_source,swapped?p->native_destination:p->native_source) &&
              same_nullable(actual.native_destination,swapped?p->native_source:p->native_destination),
              "native bytes, edited ownership and displayed paths stay paired without decoding");
    XtFree(actual.source); XtFree(actual.destination);
    XtFree(actual.native_source); XtFree(actual.native_destination);
}
static void free_paths(Paths *p)
{
    XtFree(p->source); XtFree(p->destination);
    XtFree(p->native_source); XtFree(p->native_destination);
}
static void activate(SyncWindow *w,const char *name)
{
    Widget button=XtNameToWidget(w->shell,(char *)name);
    assertion(button && XtIsSensitive(button),"setup action exists and is usable");
    if (button) XtCallCallbacks(button,XmNactivateCallback,NULL);
}
static void check_clear_filters(SyncWindow *w)
{
    Paths paths=save_paths(w);
    unsigned operation=w->profile_index,metadata=w->metadata,count=w->rule_count;
    SyncRule preference={0};
    if (count==1) preference=w->rules[0];
    XmTextFieldSetString(w->size_min,"1"); XmTextFieldSetString(w->size_max,"99");
    XmTextFieldSetString(w->date_after,"2000-01-01"); XmTextFieldSetString(w->date_before,"2099-01-01");
    activate(w,"*clearFilters");
    Widget fields[]={w->size_min,w->size_max,w->date_after,w->date_before};
    for (unsigned i=0;i<XtNumber(fields);++i) {
        char *text=XmTextFieldGetString(fields[i]);
        assertion(!*text,"Clear filters empties each of the four filter fields"); XtFree(text);
    }
    check_paths(w,&paths,0); free_paths(&paths);
    assertion(w->profile_index==operation && w->metadata==metadata && w->rule_count==count &&
              (count!=1 || same_job_preference(&w->rules[0],&preference)),
              "Clear filters retains paths, operation, metadata and JOB preference");
}
static void invalid_setup(SyncWindow *w,Widget field,const char *value,const char *message)
{
    XmTextFieldSetString(field,(char *)value);
    XtVaSetValues(w->notebook,XmNcurrentPageNumber,2,NULL);
    XtCallCallbacks(w->compare,XmNactivateCallback,NULL);
    XSync(XtDisplay(w->shell),False);
    while (XtAppPending(w->context)) XtAppProcessEvent(w->context,XtIMAll);
    int page=0; XtVaGetValues(w->notebook,XmNcurrentPageNumber,&page,NULL);
    char *status=label_text(w->status);
    fprintf(stderr,"VALIDATION: field=%s page=%d focus=%s status=%s\n",XtName(field),page,
            XmGetFocusWidget(w->shell)?XtName(XmGetFocusWidget(w->shell)):"none",status);
    assertion(!w->job && !w->recompare,"invalid setup never admits a worker");
    assertion(page==1 && XmGetFocusWidget(w->shell)==field,"Compare returns to setup and focuses the invalid field");
    assertion(strstr(status,message)!=NULL,"Compare presents the field-specific validation message");
    assertion(XtIsSensitive(w->compare) && !XtIsSensitive(w->execute),"invalid setup permits correction and never Apply");
    XtFree(status);
}
static void setup_cases(SyncWindow *w)
{
    Paths original=save_paths(w);
    Widget explanation=XtNameToWidget(w->shell,"*operationExplanation");
    assertion(explanation && XtNameToWidget(w->shell,"*swapFolders") &&
              XtNameToWidget(w->shell,"*clearFilters"),"new setup controls exist");
    if (!explanation) { free_paths(&original); return; }
    const char *titles[]={"Compare only:","Copy preview:","Update preview:",
                          "Copy missing only preview:","Synchronize one way preview:","Mirror preview:"};
    const char *meaning[]={"No file changes are proposed or applied", "Newer targets are not implicitly protected",
                          "Equal-time differences need review", "Existing destination objects are kept",
                          "destination-only objects remain", "plan can contain deletions"};
    for (unsigned i=0;i<6;++i) {
        choose_option(w->operation_option,i);
        char *text=label_text(explanation);
        assertion(w->profile_index==i && strstr(text,titles[i]) && strstr(text,meaning[i]),
                  "actual operation menu callback updates its complete explanation");
        if (i) assertion(strstr(text,"Preview only")!=NULL,"mutating operation remains explicitly preview-only");
        assertion(!XtIsSensitive(w->execute),"operation explanations never enable Apply"); XtFree(text);
    }
    set_native_path(w,w->source,"/native/left\\\001");
    set_native_path(w,w->destination,"/native/right\\\002");
    Paths native=save_paths(w);
    assertion(strstr(native.source,"\\x5c") && strstr(native.source,"\\x01"),"swap fixture includes escaped backslash and nonprintable bytes");
    activate(w,"*swapFolders"); check_paths(w,&native,1);
    activate(w,"*swapFolders"); check_paths(w,&native,0); free_paths(&native);
    XmTextFieldSetString(w->destination,"/edited/literal\\x41");
    Paths mixed=save_paths(w);
    assertion(mixed.native_source && !mixed.native_destination,"editing removes only the edited field's retained native path");
    activate(w,"*swapFolders"); check_paths(w,&mixed,1);
    activate(w,"*swapFolders"); check_paths(w,&mixed,0); free_paths(&mixed);
    set_native_path(w,w->source,original.native_source);
    set_native_path(w,w->destination,original.native_destination);
    choose_option(w->metadata_option,1); check_clear_filters(w);
    choose_option(w->operation_option,0); choose_option(w->metadata_option,0);
    const char *bad_bytes[]={"word","-1","+1","18446744073709551616","1x"," 1"};
    for (unsigned i=0;i<XtNumber(bad_bytes);++i) {
        invalid_setup(w,w->size_min,bad_bytes[i],"Minimum bytes must be empty or digits"); activate(w,"*clearFilters");
        invalid_setup(w,w->size_max,bad_bytes[i],"Maximum bytes must be empty or digits"); activate(w,"*clearFilters");
    }
    XmTextFieldSetString(w->size_min,"10");
    invalid_setup(w,w->size_max,"9","Maximum bytes must be at least minimum bytes"); activate(w,"*clearFilters");
    invalid_setup(w,w->date_after,"2025-02-29","Modified on/after: enter a valid local"); activate(w,"*clearFilters");
    invalid_setup(w,w->date_before,"2024-02-30","Modified before: enter a valid local"); activate(w,"*clearFilters");
    XmTextFieldSetString(w->date_after,"2026-01-02");
    invalid_setup(w,w->date_before,"2026-01-01","Modified before must be later than modified on/after"); activate(w,"*clearFilters");
    XmTextFieldSetString(w->date_after,"2026-01-01T00:00:00.123Z");
    invalid_setup(w,w->date_before,"2026-01-01T00:00:00.123Z","Modified before must be later than modified on/after"); activate(w,"*clearFilters");
    invalid_setup(w,w->source,"","Choose a source folder"); set_native_path(w,w->source,original.native_source);
    invalid_setup(w,w->destination,"","Choose a destination folder"); set_native_path(w,w->destination,original.native_destination);
    invalid_setup(w,w->source,"relative","Enter an absolute source folder path"); set_native_path(w,w->source,original.native_source);
    invalid_setup(w,w->destination,"relative","Enter an absolute destination folder path"); set_native_path(w,w->destination,original.native_destination);
    char *long_path=malloc(SYNC_PROFILE_PATH_MAX+2);
    if (long_path) {
        memset(long_path,'x',SYNC_PROFILE_PATH_MAX+1); long_path[0]='/'; long_path[SYNC_PROFILE_PATH_MAX+1]=0;
        invalid_setup(w,w->source,long_path,"The source folder path is too long"); set_native_path(w,w->source,original.native_source);
        invalid_setup(w,w->destination,long_path,"The destination folder path is too long"); set_native_path(w,w->destination,original.native_destination);
        free(long_path);
    } else assertion(0,"allocate oversized path fixture");
    check_paths(w,&original,0); free_paths(&original);
    /* A positive equal-bound case uses the real Compare worker, then clearing
     * filters restores the original unfiltered70-object retry regression. */
    XmTextFieldSetString(w->size_min,"10"); XmTextFieldSetString(w->size_max,"10");
}

static void pump_ticks(SyncWindow *w)
{
    long long end=milliseconds()+300;
    do {
        while (XtAppPending(w->context)) XtAppProcessEvent(w->context,XtIMAll);
        struct timespec delay={0,5000000}; nanosleep(&delay,NULL);
    } while (milliseconds()<end);
}
/* Visible feedback is observed across real scheduled ticks, without reading
 * command-notice storage or manufacturing a backend error/state. */
static void retained_feedback(SyncWindow *w,const char *message)
{
    char *before=label_text(w->status);
    assertion(strstr(before,message)!=NULL,"actual callback presents its command feedback");
    XtFree(before);
    pump_ticks(w);
    char *after=label_text(w->status);
    if (!strstr(after,message)) fprintf(stderr,"FEEDBACK LOST: expected [%s], observed [%s]\n",message,after);
    assertion(strstr(after,message)!=NULL,"command feedback survives300ms of real Xt polling");
    XtFree(after);
}
static void feedback_cleared(SyncWindow *w,const char *message)
{
    pump_ticks(w);
    char *text=label_text(w->status);
    if (strstr(text,message)) fprintf(stderr,"FEEDBACK STALE: expected cleared [%s], observed [%s]\n",message,text);
    assertion(strstr(text,message)==NULL,"relevant real input edit clears obsolete command feedback");
    XtFree(text);
}
static void no_job_feedback_case(SyncWindow *w)
{
    assertion(!w->job,"no-job feedback is checked before worker startup");
    invalid_setup(w,w->size_min,"bad","Minimum bytes must be empty or digits");
    retained_feedback(w,"Minimum bytes must be empty or digits");
    XmTextFieldSetString(w->size_min,"");
    feedback_cleared(w,"Minimum bytes must be empty or digits");
    assertion(!w->job && !w->recompare,"correcting local validation does not start a worker");
}
static int same_rule(const SyncRule *a,const SyncRule *b)
{
    return a->kind==b->kind && a->scope==b->scope && a->choice==b->choice && a->tie==b->tie &&
        a->protect_nonempty==b->protect_nonempty && a->retain_replaced==b->retain_replaced &&
        a->selected_count==b->selected_count && a->selected_count<=SYNC_PAGE_SIZE &&
        !memcmp(a->selected,b->selected,a->selected_count*sizeof(a->selected[0])) &&
        a->subtree==b->subtree && a->subtree_size==b->subtree_size;
}
static void select_pair(SyncWindow *w,unsigned first,unsigned second)
{
    XmStringTable items=NULL; int count=0;
    XtVaGetValues(w->list,XmNitems,&items,XmNitemCount,&count,NULL);
    assertion(first && second && first<=(unsigned)count && second<=(unsigned)count,
              "pair selection refers to real displayed rows");
    if (!first || !second || first>(unsigned)count || second>(unsigned)count) return;
    XmString selected[]={items[first-1],items[second-1]};
    /* This public settable resource copies the strings and selects both real
     * items without an intermediate single-selection notification. */
    XtVaSetValues(w->list,XmNselectedItems,selected,XmNselectedItemCount,2,NULL);
    XmListCallbackStruct event={0}; event.reason=XmCR_EXTENDED_SELECT;
    event.item_position=(int)first;
    XtCallCallbacks(w->list,XmNextendedSelectionCallback,&event);
}
static void rule_feedback_cases(SyncWindow *w,Widget scope,Widget choice)
{
    assertion(!w->rule_count && w->view->row_count==64 && item_count(w->list)==64,
              "capacity regression starts with64 real Copy-preview rows and no rules");
    if (w->rule_count || w->view->row_count!=64) return;
    choose_option(scope,2); /* Actual This item menu callback. */
    XmListDeselectAllItems(w->list); XmListSelectPos(w->list,1,True);
    assertion(XtIsSensitive(w->choice_menu[CKFET_KEEP_BOTH_INCOMING]),
              "first real row offers Keep Both before the no-selection Add case");
    choose_option(choice,CKFET_KEEP_BOTH_INCOMING);
    XmListDeselectAllItems(w->list);
    choose_option(w->name_side_option,1);
    unsigned side=w->name_side; uint64_t revision=w->local_rules_revision;
    activate(w,"*addRule");
    retained_feedback(w,"Select matching rows first.");
    assertion(!w->rule_count && w->name_side==side && w->local_rules_revision==revision,
              "failed no-row Add neither adds a rule nor changes rename side/authority");
    XmListSelectPos(w->list,1,True);
    feedback_cleared(w,"Select matching rows first.");
    select_pair(w,1,2);
    uint64_t selected[SYNC_PAGE_SIZE]; int first;
    assertion(selected_rows(w,selected,&first)==2,"actual item Add case selects two real rows");
    activate(w,"*addRule");
    retained_feedback(w,"Select exactly one item or subtree root.");
    assertion(!w->rule_count,"two-row This item Add leaves the rule model unchanged");
    /* No callbacks during the intermediate selection changes: only the final
     * actual list callback observes the replacement set with the same count. */
    select_pair(w,3,4);
    assertion(selected_rows(w,selected,&first)==2 &&
              selected[0]==w->view->rows[2].observed.row_id && selected[1]==w->view->rows[3].observed.row_id,
              "two-row selection is replaced by different real row IDs without an intermediate notification");
    feedback_cleared(w,"Select exactly one item or subtree root.");
    choose_option(choice,CKFET_ASK);
    Boolean protect=XmToggleButtonGetState(w->zero);
    for (unsigned i=0;i<64;++i) {
        XmListDeselectAllItems(w->list); XmListSelectPos(w->list,(int)i+1,True);
        assertion(XtIsSensitive(w->choice_menu[CKFET_ASK]),"real selected row permits an Ask item rule");
        activate(w,"*addRule");
        assertion(w->rule_count==i+1,"actual Add callback creates one distinct row-bound item rule");
        if (w->rule_count!=i+1) break;
        assertion(w->rules[i].scope==CKFET_SCOPE_ITEM && w->rules[i].kind==w->view->rows[i].observed.conflict_class &&
                  w->rules[i].choice==CKFET_ASK && w->rules[i].selected_count==1 &&
                  w->rules[i].selected[0]==w->view->rows[i].observed.row_id,
                  "item rule uses the real row ID and automatically selected matching class");
    }
    if (w->rule_count==64) {
        XmListDeselectAllItems(w->list); XmListSelectPos(w->list,1,True);
        XmToggleButtonSetState(w->zero,!protect,True);
        activate(w,"*addRule");
        assertion(w->rule_count==64 && w->rules[0].protect_nonempty==!protect,
                  "existing first item rule remains editable at the64-rule capacity");
        retained_feedback(w,"Rule updated.");
        SyncRule original[64]; memcpy(original,w->rules,sizeof(original));
        choose_option(scope,0); choose_option(choice,CKFET_KEEP_BOTH_INCOMING);
        assertion(XtIsSensitive(w->choice_menu[CKFET_KEEP_BOTH_INCOMING]),
                  "capacity rejection uses a valid JOB action rather than unavailable-choice validation");
        choose_option(w->name_side_option,1);
        side=w->name_side; revision=w->local_rules_revision;
        activate(w,"*addRule");
        retained_feedback(w,"Matching rule limit reached. Remove a rule before adding another.");
        assertion(w->rule_count==64 && w->name_side==side && w->local_rules_revision==revision,
                  "new JOB rule at capacity preserves count, rename side and authority");
        for (unsigned i=0;i<64;++i)
            assertion(same_rule(&original[i],&w->rules[i]),"capacity refusal preserves every existing rule semantically");
    }
    while (w->rule_count) {
        unsigned count=w->rule_count;
        XmListDeselectAllItems(w->rules_list); XmListSelectPos(w->rules_list,1,False);
        activate(w,"*removeRule");
        assertion(w->rule_count+1==count,"actual Remove callback removes one item rule");
        if (w->rule_count>=count) break;
    }
    XmListDeselectAllItems(w->rules_list);
    activate(w,"*removeRule");
    retained_feedback(w,"Select one rule to remove.");
    assertion(!w->rule_count,"invalid Remove cannot change the empty rule model");
    XmToggleButtonSetState(w->zero,protect,True);
    choose_option(scope,0); choose_option(choice,CKFET_ASK); choose_option(w->name_side_option,0);
    XmListDeselectAllItems(w->list); XmListSelectPos(w->list,1,True);
}
static int review_receipt(SyncWindow *w)
{
    XtCallCallbacks(w->review,XmNactivateCallback,NULL);
    if (wait_for(w,REVIEWED)) return 1;
    fprintf(stderr,"UNCERTAIN: real SDK Copy-preview manual-basename review did not establish a clean receipt: %s\n",
            w->view->error);
    assertion(0,"rename-intent checks require a real accepted SDK review; no synthetic clean state");
    return 0;
}
static void review_intent_cases(SyncWindow *w)
{
    /* Compare-only does not accept manual-name decisions. Establish a real
     * ordinary Copy preview, with a real private journal and explicit Keep Both
     * rule, as in the existing controller qualification. Apply stays OFF. */
    const char *home=getenv("DTFILE_UI_TEST_PREFERENCES_HOME");
    assertion(home && getenv("HOME")==NULL,"rename preview starts with absent process HOME");
    if (!home) return;
    assertion(setenv("HOME",home,1)==0,"private HOME supplied for Copy preview admission");
    choose_option(w->operation_option,1); choose_option(w->metadata_option,1);
    XtCallCallbacks(w->compare,XmNactivateCallback,NULL);
    int ready=wait_for(w,REPORT);
    assertion(unsetenv("HOME")==0,"process HOME restored after real Copy preview admission");
    if (!ready) return;
    XmListDeselectAllItems(w->list); XmListSelectPos(w->list,1,True);
    Widget choice=rule_option(w,"choice"),scope=rule_option(w,"scope");
    assertion(choice && scope,"actual rule choices exist for rename preview");
    if (!choice || !scope) return;
    rule_feedback_cases(w,scope,choice);
    choose_option(scope,0); choose_option(choice,CKFET_KEEP_BOTH_INCOMING);
    activate(w,"*addRule");
    assertion(w->rule_count==1 && w->rules[0].choice==CKFET_KEEP_BOTH_INCOMING,
              "actual UI rule permits incoming Keep Both naming");
    XmTextFieldSetString(w->new_name,"not/a-basename");
    uint64_t invalid_serial=w->review_serial;
    XtCallCallbacks(w->review,XmNactivateCallback,NULL);
    retained_feedback(w,"For a reviewed rename, select one row and enter one basename.");
    assertion(w->review_serial==invalid_serial && !w->review_pending,
              "invalid basename is rejected locally without a backend review submission");
    XmTextFieldSetString(w->new_name,"review-intent-name");
    feedback_cleared(w,"For a reviewed rename, select one row and enter one basename.");
    choose_option(w->name_side_option,0);
    uint64_t serial=w->review_serial,completed=w->view->completed_submission;
    unsigned original_dirty=w->dirty;
    null_basename_field=w->new_name; null_basename_once=1;
    XtCallCallbacks(w->review,XmNactivateCallback,NULL);
    assertion(null_basename_hits==1 && !null_basename_once && w->review_serial==serial && !w->review_pending,
              "NULL basename getter aborts before queueing a review submission");
    retained_feedback(w,"Not enough memory.");
    assertion(w->view->completed_submission==completed && w->dirty==original_dirty,
              "NULL basename getter never reaches the backend or changes dirty authority");
    if (!review_receipt(w)) return;
    uint64_t revision=w->local_rules_revision,receipt=w->view->snapshot.receipt_id;
    pump_ticks(w);
    assertion(!w->dirty && w->local_rules_revision==revision && w->view->snapshot.receipt_id==receipt,
              "unchanged real polling with a nonempty basename does not redirty review");
    choose_option(w->name_side_option,1);
    assertion(w->name_side==1 && w->dirty && w->local_rules_revision>revision && !XtIsSensitive(w->execute),
              "actual name-side callback invalidates the reviewed rename tuple");
    choose_option(w->name_side_option,0);
    if (!review_receipt(w)) return;
    revision=w->local_rules_revision;
    uint64_t selected[SYNC_PAGE_SIZE]; int first;
    assertion(selected_rows(w,selected,&first)==1,"reviewed rename has one selected row");
    uint64_t row=selected[0];
    XmListDeselectAllItems(w->list); XmListSelectPos(w->list,2,True);
    assertion(selected_rows(w,selected,&first)==1 && selected[0]!=row && w->dirty &&
              w->local_rules_revision>revision && !XtIsSensitive(w->execute),
              "actual row selection invalidates a reviewed nonempty basename binding");
    XmTextFieldSetString(w->new_name,"");
    XmListDeselectAllItems(w->list);
    XmListSelectPos(w->rules_list,1,False); activate(w,"*removeRule");
    assertion(!w->rule_count,"rename-only rule removed through the actual UI");
    choose_option(choice,CKFET_ASK); choose_option(scope,0);
    choose_option(w->operation_option,0); choose_option(w->metadata_option,0);
    XtCallCallbacks(w->compare,XmNactivateCallback,NULL);
    assertion(wait_for(w,REPORT),"fresh Compare-only report restored after rename checks");
}
static int save_preferences(SyncWindow *w,SyncProfile *profile)
{
    SyncProfilesResult result=SyncProfilesSave(w->preferences,profile);
    assertion(result==SYNC_PROFILES_OK,"real private profile save is admitted");
    return result==SYNC_PROFILES_OK && wait_for(w,PREFERENCES_READY);
}
static void profile_load_fault(SyncWindow *w)
{
    Paths original=save_paths(w);
    char *original_name=XmTextFieldGetString(w->profile_name),error[512]={0};
    const char *home=getenv("DTFILE_UI_TEST_PREFERENCES_HOME");
    assertion(home && !w->job && !w->preferences && getenv("HOME")==NULL,
              "profile test starts before jobs using explicit private fixture, not process HOME");
    if (!home || w->job || w->preferences) goto done;
    w->preferences=SyncProfilesStart(home,error,sizeof(error));
    assertion(w->preferences!=NULL,"actual private profile worker starts");
    if (!w->preferences || !wait_for(w,PREFERENCES_READY)) goto done;
    Widget delete_profile_button=XtNameToWidget(w->shell,"*deleteProfile");
    assertion(delete_profile_button && XtIsSensitive(delete_profile_button),
              "Delete profile becomes available after the real preferences worker is ready");
    XmTextFieldSetString(w->profile_name,"initial-settings");
    activate(w,"*saveProfile");
    if (!wait_for(w,PREFERENCES_READY)) goto done;
    SyncProfile *initial=calloc(1,sizeof(*initial)),*saved=calloc(1,sizeof(*saved));
    assertion(initial && saved,"bounded profile payload allocations succeed");
    if (!initial || !saved) { free(initial); free(saved); goto done; }
    assertion(SyncProfilesGetProfile(w->preferences,"initial-settings",initial)==SYNC_PROFILES_OK,
              "saved initial settings are read from the actual profile cache");
    *saved=*initial;
    strcpy(saved->name,"original-settings");
    strcpy(saved->source,"/profile/original\\\001"); strcpy(saved->destination,"/profile/original-target\\\002");
    saved->profile=CKFSP_UPDATE; saved->metadata_scope=1;
    saved->filters.has_minimum_size=saved->filters.has_maximum_size=1;
    saved->filters.minimum_size=11; saved->filters.maximum_size=22;
    saved->rule_count=1; saved->rules[0]=(SyncProfileRule){0,CKFET_ASK,CKFET_ASK,1,1};
    if (!save_preferences(w,saved)) goto free_profiles;
    XmTextFieldSetString(w->profile_name,"original-settings"); activate(w,"*loadProfile");
    assertion(w->rule_count==1 && w->native_source && !strcmp(w->native_source,saved->source),
              "actual Load establishes nontrivial original paths/options/rule state");
    strcpy(saved->name,"replacement-settings");
    strcpy(saved->source,"/profile/replacement-source"); strcpy(saved->destination,"/profile/replacement-target");
    saved->profile=CKFSP_COPY; saved->metadata_scope=0;
    saved->filters.minimum_size=3; saved->filters.maximum_size=7;
    saved->rules[0]=(SyncProfileRule){0,CKFET_KEEP_TARGET,CKFET_ASK,0,0};
    if (!save_preferences(w,saved)) goto free_profiles;
    XmTextFieldSetString(w->profile_name,"replacement-settings");
    Paths before=save_paths(w);
    Widget fields[]={w->size_min,w->size_max,w->date_after,w->date_before,w->new_name,w->profile_name};
    char *values[XtNumber(fields)];
    for (unsigned i=0;i<XtNumber(fields);++i) values[i]=XmTextFieldGetString(fields[i]);
    unsigned operation=w->profile_index,metadata=w->metadata,count=w->rule_count;
    unsigned dirty=w->dirty,setup_dirty=w->setup_dirty,review_pending=w->review_pending,execute_pending=w->execute_pending;
    uint64_t revision=w->local_rules_revision,reviewed_revision=w->reviewed_rules_revision;
    uint64_t serial=w->review_serial,execute_serial=w->execute_serial,receipt=w->view->snapshot.receipt_id;
    SyncRule preference={0}; if (count==1) preference=w->rules[0];
    Widget operation_item=NULL,metadata_item=NULL;
    XtVaGetValues(w->operation_option,XmNmenuHistory,&operation_item,NULL);
    XtVaGetValues(w->metadata_option,XmNmenuHistory,&metadata_item,NULL);
    char *explanation=label_text(XtNameToWidget(w->shell,"*operationExplanation"));
    fail_path=saved->destination; path_strdup_once=1;
    activate(w,"*loadProfile");
    assertion(path_strdup_hits==1 && !path_strdup_once,"UI-only destination raw-path allocation fault was reached");
    fail_path=NULL;
    check_paths(w,&before,0); free_paths(&before);
    for (unsigned i=0;i<XtNumber(fields);++i) {
        char *actual=XmTextFieldGetString(fields[i]);
        assertion(!strcmp(actual,values[i]),"failed Load leaves every editable field unchanged");
        XtFree(actual); XtFree(values[i]);
    }
    Widget operation_after=NULL,metadata_after=NULL;
    XtVaGetValues(w->operation_option,XmNmenuHistory,&operation_after,NULL);
    XtVaGetValues(w->metadata_option,XmNmenuHistory,&metadata_after,NULL);
    char *actual_explanation=label_text(XtNameToWidget(w->shell,"*operationExplanation"));
    assertion(w->profile_index==operation && w->metadata==metadata && operation_after==operation_item &&
              metadata_after==metadata_item && !strcmp(actual_explanation,explanation) && w->rule_count==count &&
              count==1 && same_job_preference(&w->rules[0],&preference),
              "failed Load leaves option models/widgets, explanation and all original rules unchanged");
    XtFree(explanation); XtFree(actual_explanation);
    assertion(!w->job && w->dirty==dirty && w->setup_dirty==setup_dirty && w->review_pending==review_pending &&
              w->execute_pending==execute_pending && w->local_rules_revision==revision &&
              w->reviewed_rules_revision==reviewed_revision && w->review_serial==serial &&
              w->execute_serial==execute_serial && w->view->snapshot.receipt_id==receipt,
              "failed Load preserves original dirty/review/receipt authority transactionally");
    activate(w,"*loadProfile");
    assertion(w->native_source && w->native_destination && !strcmp(w->native_source,saved->source) &&
              !strcmp(w->native_destination,saved->destination) && w->profile_index==1 && w->metadata==0 &&
              w->rule_count==1 && w->rules[0].choice==CKFET_KEEP_TARGET && !w->rules[0].protect_nonempty &&
              !w->rules[0].retain_replaced && w->dirty && w->setup_dirty,
              "retry Load commits both paths and the complete replacement profile");
    const char *expected[]={"3","7","",""};
    for (unsigned i=0;i<4;++i) {
        char *actual=XmTextFieldGetString(fields[i]);
        assertion(!strcmp(actual,expected[i]),"retry Load commits all four filter fields"); XtFree(actual);
    }
    XmTextFieldSetString(w->profile_name,"initial-settings"); activate(w,"*loadProfile");
    check_paths(w,&original,0);
    assertion(!w->rule_count && w->profile_index==0 && w->metadata==0 && getenv("HOME")==NULL,
              "initial setup and absent process HOME restored before the existing retry regression");
free_profiles:
    free(initial); free(saved);
done:
    XmTextFieldSetString(w->profile_name,original_name); XtFree(original_name); free_paths(&original);
}

static void test_loop(XtAppContext context)
{
    (void)context;
    Widget compare_button=XtNameToWidget(test_shell,"*compare");
    XtCallbackList callbacks=NULL;
    XtVaGetValues(compare_button,XmNactivateCallback,&callbacks,NULL);
    assertion(callbacks && callbacks[0].closure,"production comparison callback exists");
    if (!callbacks || !callbacks[0].closure) exit(2);
    SyncWindow *w=callbacks[0].closure;
    assertion(getenv("HOME")==NULL,"HOME is genuinely absent");
    assertion(!XtIsSensitive(w->execute),"Apply starts disabled");
    Widget delete_profile_button=XtNameToWidget(w->shell,"*deleteProfile");
    assertion(delete_profile_button && !XtIsSensitive(delete_profile_button),
              "Delete profile starts disabled without a preferences worker; no confirmation is opened");
    no_job_feedback_case(w);
    if (test_failed) goto close;
    profile_load_fault(w);
    if (test_failed) goto close;
    setup_cases(w);
    if (test_failed) goto close;
    XtCallCallbacks(w->compare,XmNactivateCallback,NULL);
    assertion(w->job!=NULL,"equal inclusive byte bounds admit a real Compare worker");
    if (!wait_for(w,REPORT)) { assertion(0,"equal-bound Compare reaches a real report"); goto close; }
    activate(w,"*clearFilters");
    XtCallCallbacks(w->compare,XmNactivateCallback,NULL);
    if (!wait_for(w,REPORT)) { assertion(0,"initial real Compare completes"); goto close; }
    assertion(w->view->total_rows==70 && item_count(w->list)==64,"real Compare has70 objects,64 first-page rows");
    assertion(XtIsSensitive(w->list),"fresh report rows are selectable");
    review_intent_cases(w);
    if (test_failed) goto close;
    XmListSelectPos(w->list,1,True);
    char *old_details=XmTextGetString(w->details);
    assertion(strstr(old_details,"file-000")!=NULL,"selected real row populated details before retry");
    XtFree(old_details);
    Widget scope=rule_option(w,"scope"),add=XtNameToWidget(w->rule_page,"*addRule");
    assertion(scope && add && XtIsSensitive(add),"actual scope and Add-rule controls are usable");
    if (!scope || !add) goto close;
    choose_option(scope,0); /* Job preference, then a decision tied to the selected row. */
    XtCallCallbacks(add,XmNactivateCallback,NULL);
    assertion(w->rule_count==1 && w->rules[0].scope==CKFET_SCOPE_JOB,"job-wide preference added by UI callback");
    if (w->rule_count!=1) goto close;
    SyncRule job_preference=w->rules[0];
    Paths assessed=save_paths(w);
    activate(w,"*swapFolders"); check_paths(w,&assessed,1);
    assertion(w->setup_dirty && !XtIsSensitive(w->review) && !XtIsSensitive(w->rule_page) &&
              !XtIsSensitive(w->execute),"Swap immediately invalidates old assessment and review authority");
    char *notice=label_text(XtNameToWidget(w->shell,"*setupNotice"));
    assertion(strstr(notice,"earlier folder options")!=NULL,"comparison page preserves the stale-options explanation"); XtFree(notice);
    activate(w,"*swapFolders"); check_paths(w,&assessed,0); free_paths(&assessed);
    check_clear_filters(w);
    XtCallCallbacks(w->compare,XmNactivateCallback,NULL);
    if (!wait_for(w,REPORT)) { assertion(0,"fresh Compare after setup edits completes"); goto close; }
    assertion(w->view->total_rows==70 && w->rule_count==1 && same_job_preference(&w->rules[0],&job_preference),
              "setup edits retain only the JOB preference for a fresh70-object assessment");
    XmListSelectPos(w->list,1,True);
    choose_option(scope,2);
    XtCallCallbacks(add,XmNactivateCallback,NULL);
    assertion(w->rule_count==2 && w->rules[1].scope==CKFET_SCOPE_ITEM && w->rules[1].selected_count==1,
              "item rule binds to the selected real row through UI callbacks");
    choose_option(w->operation_option,1);
    assertion(profiles[w->profile_index]==CKFSP_COPY,"Copy is selected by the actual option callback");
    XtCallCallbacks(w->compare,XmNactivateCallback,NULL);
    assertion(w->recompare && w->job,"comparison queues stop of the live old job");
    if (!wait_for(w,ADMISSION_FAILURE)) { assertion(0,"queued Copy reaches real no-HOME admission failure"); goto close; }
    char *summary=label_text(w->plan_summary),*page=label_text(w->page_info);
    char *details=XmTextGetString(w->details),*roots=label_text(w->compared_roots);
    fprintf(stderr,"AFTER FAILED ADMISSION: rows=%d source_sensitive=%d compare_sensitive=%d next_sensitive=%d\nplan=%s\npage=%s\ndetails=%s\n",
            item_count(w->list),XtIsSensitive(w->source),XtIsSensitive(w->compare),XtIsSensitive(w->next),summary,page,details);
    assertion(item_count(w->list)==0,"failed admission leaves no old rows");
    assertion(strstr(summary,"Plan totals are not yet available.")!=NULL && strstr(page,"Comparison results are not yet available.")!=NULL,"old totals and page range are replaced by unavailable state");
    assertion(strstr(details,"file-000")==NULL,"old selected-row details are gone");
    assertion(strstr(roots,"No comparison has started")!=NULL,"old compared roots have no current authority");
    assertion(XtIsSensitive(w->source) && XtIsSensitive(w->compare),"Setup and Compare remain usable after failed start");
    assertion(w->rule_count==1 && same_job_preference(&w->rules[0],&job_preference),
              "only the unchanged JOB preference survives failed recompare; row rule is discarded");
    assertion(w->dirty && !w->review_pending && !w->execute_pending,"failed admission requires fresh rule review");
    assertion(!XtIsSensitive(w->previous) && !XtIsSensitive(w->next) && !XtIsSensitive(w->review) &&
              !XtIsSensitive(w->execute) && !XtIsSensitive(w->pause) && !XtIsSensitive(w->cancel),
              "result actions have no old-job authority");
    XtFree(summary); XtFree(page); XtFree(details); XtFree(roots);
    if (test_failed) goto close;
    choose_option(w->operation_option,0);
    XtCallCallbacks(w->compare,XmNactivateCallback,NULL);
    if (!wait_for(w,REPORT)) { assertion(0,"Compare-only retry works after failed admission"); goto close; }
    assertion(w->view->total_rows==70 && item_count(w->list)==64 && XtIsSensitive(w->list),"retry has70 fresh selectable report objects");
    assertion(w->rule_count==1 && same_job_preference(&w->rules[0],&job_preference) && w->dirty &&
              w->view->snapshot.rule_count==0 && !w->review_pending,
              "retained preference remains unreviewed against the fresh inventory");
    assertion(!XtIsSensitive(w->execute),"retry never enables Apply");
    /* Last scenario: real cancellation supersedes a retained local notice.
     * The real Compare-only worker supplies its terminal state and drain; no
     * recovery flag, snapshot, error or worker function is substituted. */
    assertion(XtIsSensitive(w->setup),"setup is editable before the real Cancel request");
    XmTextFieldSetString(w->size_min,"bad");
    XmTextFieldSetString(w->profile_name,"cancel-feedback");
    activate(w,"*saveProfile");
    retained_feedback(w,"Minimum bytes must be empty or digits");
    assertion(XtIsSensitive(w->cancel),"real awaiting-review worker can be cancelled");
    XtCallCallbacks(w->cancel,XmNactivateCallback,NULL);
    char *cancelling=label_text(w->status);
    assertion(strstr(cancelling,"Cancelling - finishing safe cleanup...")!=NULL &&
              !strstr(cancelling,"Minimum bytes must be empty or digits"),
              "actual Cancel immediately supersedes retained local command feedback");
    XtFree(cancelling);
    Widget cancelled_controls[]={w->setup,w->compare,w->review,w->execute,w->rule_page,
        w->pause,w->cancel,w->previous,w->next};
    for (unsigned i=0;i<XtNumber(cancelled_controls);++i)
        assertion(!XtIsSensitive(cancelled_controls[i]),"Cancel immediately disables operation and setup controls");
    uint64_t cancelled_serial=w->review_serial;
    XtCallCallbacks(w->review,XmNactivateCallback,NULL);
    assertion(w->review_serial==cancelled_serial && !w->review_pending,
              "direct Review callback after Cancel cannot submit a new command");
    SyncProfilesSnapshot before_save={0},after_save={0};
    assertion(SyncProfilesGetSnapshot(w->preferences,&before_save)==SYNC_PROFILES_OK,
              "real preference worker snapshot is available before invalid Save during cancellation");
    int cleanup_pending=!w->view->finished && !w->view->drained;
    activate(w,"*saveProfile");
    assertion(SyncProfilesGetSnapshot(w->preferences,&after_save)==SYNC_PROFILES_OK &&
              after_save.submitted==before_save.submitted,
              "Save with invalid minimum bytes queues no real preferences operation");
    if (cleanup_pending && !w->view->finished && !w->view->drained) {
        char *critical=label_text(w->status);
        assertion(strstr(critical,"Cancelling - finishing safe cleanup...")!=NULL &&
                  !strstr(critical,"Minimum bytes must be empty or digits"),
                  "actual invalid Save cannot replace immediate cancellation feedback while cleanup is pending");
        XtFree(critical);
    }
    pump_ticks(w);
    char *after_cancel=label_text(w->status);
    assertion(!strstr(after_cancel,"Select one rule to remove."),
              "real cancellation or terminal feedback keeps priority across scheduled ticks");
    XtFree(after_cancel);
    assertion(wait_for(w,CANCELLED_DRAINED),"real cancelled Compare-only worker reaches terminal state and drains");
close:
    XtCallCallbacks(XtNameToWidget(test_shell,"*close"),XmNactivateCallback,NULL);
    char *closing=label_text(w->status);
    assertion(strstr(closing,"Closing - finishing safe cleanup...")!=NULL,
              "Close immediately presents its own cleanup feedback");
    XtFree(closing);
    assertion(wait_for(w,EXIT),"native job drains and UI closes normally");
}
int main(int argc,char **argv)
{
    int result=application_main(argc,argv);
    if (!result && !test_failed) puts("PASS: retained command feedback, rule capacity, transactional profile loading, reviewed rename intent, setup validation and comparison retry");
    return result ? result : test_failed;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fs-prefix", type=Path, required=True)
    parser.add_argument("--motif-prefix", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True,
                        help="new isolated directory for all generated files and fixtures")
    parser.add_argument("--module-source", type=Path,
                        default=Path(__file__).resolve().parents[1] / "dtsync")
    args = parser.parse_args()
    work = args.work_dir.resolve()
    if not work.is_absolute() or work.exists():
        parser.error("--work-dir must identify a new isolated directory")
    if not os.environ.get("DISPLAY"):
        parser.error("run under a private native X display, for example xvfb-run")
    if os.uname().sysname == "Darwin" and not any(
            root.is_mount() and work.is_relative_to(root / "tmp")
            for root in (Path("/Volumes/S8T"), Path("/Volumes/PRO-BLADE"))):
        parser.error("Mac scratch must use a mounted approved temporary volume")
    work.mkdir(parents=True, mode=0o700)
    for name in ("tmp", "source", "target", "cache", "config", "data", "runtime", "preferences-home"):
        (work / name).mkdir(mode=0o700)
    env = dict(os.environ, TMPDIR=str(work / "tmp"), LANG="C", LC_ALL="C",
               DTFILE_UI_TEST_PREFERENCES_HOME=str(work / "preferences-home"))
    env.pop("HOME", None)
    env.pop("LD_PRELOAD", None)
    for key, name in (("XDG_CACHE_HOME", "cache"), ("XDG_CONFIG_HOME", "config"),
                      ("XDG_DATA_HOME", "data"), ("XDG_RUNTIME_DIR", "runtime")):
        env[key] = str(work / name)
    env["LD_LIBRARY_PATH"] = f"{args.motif_prefix}/lib:{args.fs_prefix}/lib"
    env["NLSPATH"] = str(work / "%N.cat")
    source = args.module_source.resolve()
    for i in range(70):
        (work / "source" / f"file-{i:03}").write_text(f"source {i}\n")
    (work / "target" / "file-000").write_text("destination version\n")
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    inventory = lambda: {str(p.relative_to(work)): sha(p) for tree in ("source", "target")
                         for p in sorted((work / tree).iterdir())}
    before = inventory()
    wrapper = work / "sync-retry-ui.c"
    wrapper.write_text(HARNESS.replace("MAIN_SOURCE", json.dumps(str(source / "main.c"))))
    xft_cflags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "xft"], env=env, text=True))
    command = [*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-O0", "-g", "-Wall", "-Wextra", "-Werror", *xft_cflags,
               "-D_POSIX_C_SOURCE=200809L", "-D_DEFAULT_SOURCE=1", "-D_XOPEN_SOURCE=700",
               f"-I{args.fs_prefix}/include", f"-I{args.motif_prefix}/include", f"-I{source}", str(wrapper),
               str(source / "SyncJob.c"), str(source / "SyncProfiles.c"),
               f"-L{args.fs_prefix}/lib", f"-L{args.motif_prefix}/lib",
               "-lck-core-fs-lib", "-lXm", "-lXt", "-lX11", "-pthread", "-o", str(work / "sync-retry-ui")]
    utc = lambda: datetime.now(timezone.utc).isoformat()
    start = time.monotonic()
    record = {"build_start_utc": utc(), "jobs": 1, "compile_command": command, "source_hashes": {p.name: sha(p) for p in source.iterdir()
              if p.suffix in (".c", ".h", ".msg")}, "fixture_before": before}
    try:
        with (work / "build.log").open("w") as log:
            subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
            subprocess.run(["gencat", str(work / "dtfile_sync.cat"), str(source / "dtfile_sync.msg")],
                           env=env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=15)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        record.update(build_end_utc=utc(), build_seconds=time.monotonic() - start,
                      passed=False, build_error=str(error),
                      timeout_failures=[str(error)] if isinstance(error, subprocess.TimeoutExpired) else [])
        (work / "result.json").write_text(json.dumps(record, indent=2) + "\n")
        print((work / "build.log").read_text(), end="")
        raise SystemExit(1)
    record["build_end_utc"] = utc()
    record["build_seconds"] = time.monotonic() - start
    record["executable_path"] = str(work / "sync-retry-ui")
    record["executable_sha256"] = sha(work / "sync-retry-ui")
    record["catalog_path"] = str(work / "dtfile_sync.cat")
    record["catalog_sha256"] = sha(work / "dtfile_sync.cat")
    start = time.monotonic()
    record["run_start_utc"] = utc()
    timeout_failures = []
    try:
        with (work / "run.log").open("w") as log:
            result = subprocess.run([str(work / "sync-retry-ui"), str(work / "source"), str(work / "target")],
                                    env=env, cwd=work, stdout=log, stderr=subprocess.STDOUT, timeout=65)
        exit_code = result.returncode
    except subprocess.TimeoutExpired as error:
        exit_code = 124
        timeout_failures.append(str(error))
    run_log = (work / "run.log").read_text()
    timeout_failures.extend(line for line in run_log.splitlines() if line.startswith("TIMEOUT:"))
    record.update(run_end_utc=utc(), run_seconds=time.monotonic() - start, exit_code=exit_code,
                  timeout_failures=timeout_failures,
                  fixture_unchanged=inventory() == before)
    record["passed"] = not exit_code and record["fixture_unchanged"] and not timeout_failures
    (work / "result.json").write_text(json.dumps(record, indent=2) + "\n")
    print(run_log, end="")
    if not record["fixture_unchanged"]:
        raise SystemExit("FAIL: source or target bytes changed")
    raise SystemExit(exit_code)


if __name__ == "__main__":
    main()
