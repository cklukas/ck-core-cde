#!/usr/bin/env python3
"""Native Xt retry regression, with production callbacks and the real FS SDK.

Run under a private X display (for example xvfb-run). No backend faults, UI
reset substitutes, pixel matching, or mutation commands are used. The wrapper
only retains the realized shell and supplies a bounded Xt event loop.
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
static void test_realize(Widget);
static void test_loop(XtAppContext);
#define main application_main
#define XtRealizeWidget test_realize
#define XtAppMainLoop test_loop
#include MAIN_SOURCE
#undef main
#undef XtRealizeWidget
#undef XtAppMainLoop

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
enum WaitFor { REPORT, ADMISSION_FAILURE, EXIT };
static int wait_for(SyncWindow *w,enum WaitFor event)
{
    long long end=milliseconds()+15000;
    do {
        while (XtAppPending(w->context)) XtAppProcessEvent(w->context,XtIMAll);
        if (event==EXIT && XtAppGetExitFlag(w->context)) return 1;
        if (event==REPORT && w->job && !w->recompare && w->view->ready &&
            w->view->snapshot.state==CKFET_AWAITING_REVIEW &&
            item_count(w->list)>0 && XtIsSensitive(w->compare)) return 1;
        char *status=label_text(w->status);
        int failed=event==ADMISSION_FAILURE && !w->job && !w->recompare &&
                   profiles[w->profile_index]==CKFSP_COPY && getenv("HOME")==NULL && status[0];
        XtFree(status);
        if (failed) return 1;
        struct timespec delay={0,5000000}; nanosleep(&delay,NULL);
    } while (milliseconds()<end);
    fprintf(stderr,"TIMEOUT: native event wait %d after15000ms\n",event);
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
static Widget scope_option(SyncWindow *w)
{
    Widget editor=XtNameToWidget(w->rule_page,"*ruleEditor");
    WidgetList children=NULL;
    Cardinal count=0;
    XtVaGetValues(editor,XmNchildren,&children,XmNnumChildren,&count,NULL);
    for (Cardinal i=0;i<count;++i) {
        if (!strcmp(XtName(children[i]),"scope")) {
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
    XmListSelectPos(w->list,1,True);
    char *old_details=XmTextGetString(w->details);
    assertion(strstr(old_details,"file-000")!=NULL,"selected real row populated details before retry");
    XtFree(old_details);
    Widget scope=scope_option(w),add=XtNameToWidget(w->rule_page,"*addRule");
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
close:
    XtCallCallbacks(XtNameToWidget(test_shell,"*close"),XmNactivateCallback,NULL);
    assertion(wait_for(w,EXIT),"native job drains and UI closes normally");
}
int main(int argc,char **argv)
{
    int result=application_main(argc,argv);
    if (!result && !test_failed) puts("PASS: setup explanations, byte-preserving swap, filter clearing, focused validation and comparison retry");
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
    for name in ("tmp", "source", "target", "cache", "config", "data", "runtime"):
        (work / name).mkdir(mode=0o700)
    env = dict(os.environ, TMPDIR=str(work / "tmp"), LANG="C", LC_ALL="C")
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
               f"-I{args.fs_prefix}/include", f"-I{args.motif_prefix}/include", str(wrapper),
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
