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
    if (text) XmStringGetLtoR(text,XmFONTLIST_DEFAULT_TAG,&plain);
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
        if (event==REPORT && w->job && w->view->ready &&
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
    if (!result && !test_failed) puts("PASS: failed admission clears old result authority and permits fresh Compare retry");
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
