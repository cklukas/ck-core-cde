#!/usr/bin/env python3
"""Exercise production dtfile directory control-packet helpers."""
import argparse, hashlib, json, os, re, subprocess, sys, time
from pathlib import Path

CASES=("array-valid","array-negative","array-over-limit","path-valid",
       "path-negative","path-over-limit","path-null-type","path-truncated",
       "position-valid","position-negative","position-over-limit",
       "position-truncated","position-file-valid","position-file-negative",
       "position-file-over-limit","position-file-long-name",
       "position-file-coordinate-range","position-file-truncated",
       "file-path-valid","file-path-root",
       "file-path-long","write-done",
       "write-error","write-path","write-position","status-write",
       "timer-valid","timer-truncated-time","timer-truncated-boolean",
       "timer-invalid-boolean","timer-write")

def balanced(text,start,opening,closing):
    depth=0;i=start
    while i<len(text):
        if text.startswith("/*",i):
            end=text.find("*/",i+2)
            if end<0:raise ValueError("unterminated comment")
            i=end+2;continue
        if text[i] in "\"'":
            quote=text[i];i+=1
            while i<len(text) and text[i]!=quote:i+=2 if text[i]=="\\" else 1
            if i>=len(text):raise ValueError("unterminated literal")
        elif text[i]==opening:depth+=1
        elif text[i]==closing:
            depth-=1
            if depth==0:return i+1
        i+=1
    raise ValueError("unbalanced definition")

def extract(text,name):
    pattern=re.compile(r"(?m)^(?:static[ \t\r\n]+)?(?:int|void)[ \t\r\n]+"+re.escape(name)+r"[ \t\r\n]*\(")
    found=[]
    for match in pattern.finditer(text):
        args=balanced(text,match.end()-1,"(",")");body=args
        while body<len(text) and text[body].isspace():body+=1
        if body<len(text) and text[body]=="{":found.append(text[match.start():balanced(text,body,"{","}")])
    if len(found)!=1:raise ValueError(f"expected one {name}, found {len(found)}")
    return found[0]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--module-source",type=Path)
    parser.add_argument("--work-dir",type=Path)
    parser.add_argument("--case",choices=CASES)
    parser.add_argument("--sanitizers",default="address,undefined")
    args=parser.parse_args();tests=Path(__file__).resolve().parent
    source=(args.module_source or tests.parent).resolve();work=args.work_dir
    if work is None:
        if sys.platform=="darwin":parser.error("--work-dir is required on macOS")
        work=Path.cwd()/"directory-control-check"
    work=work.resolve();work.mkdir(parents=True,exist_ok=False);(work/"tmp").mkdir()
    texts={name:(source/name).read_text() for name in ("Directory.c","FileOp.c")}
    callback=extract(texts["Directory.c"],"ReaddirPipeCallback")
    read_process=extract(texts["Directory.c"],"ReadDirectoryProcess")
    update_all=extract(texts["Directory.c"],"UpdateAllProcess")
    update_some=extract(texts["Directory.c"],"UpdateSomeProcess")
    write_position=extract(texts["Directory.c"],"WritePosInfoProcess")
    write_position_callback=extract(texts["Directory.c"],"WritePosInfoPipeCallback")
    timer_process=extract(texts["Directory.c"],"TimerEventProcess")
    timer_callback=extract(texts["Directory.c"],"TimerPipeCallback")
    producers=read_process+update_all+update_some
    contract={
        "path_transactional":"DirectoryReadPathInfo(*fd, &new_count" in callback and
                             "directory->path_logical_types = new_types" in callback,
        "position_transactional":"DirectoryReadPositionInfo(*fd, &new_count" in callback and
                                 "directory->position_info = new_positions" in callback,
        "done_error_exact":callback.count("PipeReadExact(*fd") >= 7,
        "unknown_tag_malformed":"default:" in callback and "goto malformed_response;" in callback,
        "path_producers_exact":producers.count("DirectoryWritePathInfo(") == 2,
        "position_producer_exact":"PipeWritePositionInfo(pipe_fd" in read_process,
        "position_file_transactional":"if (position_count >= 0)" in read_process and
                                      "if (position_count < 0) position_count = 0" not in read_process,
        "status_producers_exact":producers.count("DirectoryWriteDone(") == 3 and
                                 producers.count("DirectoryWriteError(") >= 6,
        "resolved_path_checked":"directory_status != TT_OK || full_directory_name == NULL" in read_process,
        "component_paths_checked":read_process.count("component_status != TT_OK || full_directory_name == NULL") == 1 and
                                  update_all.count("component_status != TT_OK || full_directory_name == NULL") == 1,
        "update_paths_checked":update_all.count("tt_status != TT_OK || full_directory_name == NULL") == 2 and
                               "tt_status != TT_OK || full_directory_name == NULL" in update_some,
        "permission_error_stable":"rc = EACCES;" in update_some and
                                  "stat(full_directory_name, &stat_buf) < 0" in update_some,
        "position_status_exact":"DirectoryWriteStatus(pipe_fd" in write_position and
                                "PipeReadExact(*fd, &rc" in write_position_callback and
                                "errno == ENOENT" in write_position,
        "timer_status_exact":"DirectoryWriteTimerResult(pipe_fd" in timer_process and
                             "DirectoryReadTimerResult(*fd" in timer_callback and
                             "errno != 0 ? errno : EACCES" in timer_process,
        "timer_path_bounded":"DirectoryBuildChildPath(full_name" in timer_process and
                             "strcpy(namep" not in timer_process,
        "timer_malformed_retires_sticky":"StickyProcDiscard(activity" in timer_callback and
                                         "directory->was_up_to_date = False" in timer_callback,
    }
    if not all(contract.values()):raise ValueError(f"directory control contract mismatch: {contract}")
    names=(("FileOp.c","PipeRead"),("FileOp.c","PipeReadExact"),
           ("FileOp.c","PipeWriteString"),("FileOp.c","PipeReadStringField"),
           ("Directory.c","DirectoryFreeStrings"),("Directory.c","DirectoryArrayBytes"),
           ("Directory.c","DirectoryReadPathInfo"),("Directory.c","DirectoryWritePathInfo"),
           ("Directory.c","DirectoryWriteDone"),("Directory.c","DirectoryWriteError"),
           ("Directory.c","PipeWritePositionInfo"),("Directory.c","PipeReadPositionInfo"),
           ("Directory.c","DirectoryReadPositionInfo"),("Directory.c","DirectoryBuildChildPath"),
           ("Directory.c","DirectoryWriteStatus"),("Directory.c","DirectoryWriteTimerResult"),
           ("Directory.c","DirectoryReadTimerResult"),("Directory.c","DirectoryLoadPositionInfo"))
    functions="\n\n".join(extract(texts[file],name) for file,name in names)
    fixture=(tests/"directory-control-packets.c").read_text().replace("/* PRODUCTION_FUNCTIONS */",functions)
    generated=work/"directory-control-packets.c";generated.write_text(fixture);binary=work/"directory-control-packets"
    cc=os.environ.get("CC","cc").split();flags=["-std=c11","-D_DEFAULT_SOURCE","-Wall","-Wextra","-Werror","-O1","-g"]
    if args.sanitizers!="none":flags += [f"-fsanitize={args.sanitizers}","-fno-omit-frame-pointer"]
    env=dict(os.environ,TMPDIR=str(work/"tmp"),LANG="C",LC_ALL="C")
    env.setdefault("ASAN_OPTIONS",f"detect_leaks={'0' if sys.platform=='darwin' else '1'}:abort_on_error=1")
    env.setdefault("UBSAN_OPTIONS","halt_on_error=1:print_stacktrace=1")
    subprocess.run(cc+flags+[str(generated),"-o",str(binary)],check=True,env=env)
    selected=(args.case,) if args.case else CASES;started=time.monotonic();results=[]
    for case in selected:
        run=subprocess.run([str(binary),case],text=True,capture_output=True,env=env)
        results.append({"case":case,"returncode":run.returncode,"stdout":run.stdout,"stderr":run.stderr})
        if run.stdout:print(run.stdout,end="")
        if run.stderr:print(run.stderr,end="",file=sys.stderr)
        if run.returncode:raise SystemExit(run.returncode)
    paths=[source/name for name in texts]+[Path(__file__),tests/"directory-control-packets.c"]
    receipt={"passed":True,"cases":len(selected),"seconds":time.monotonic()-started,"sanitizers":args.sanitizers,
             "contract":contract,
             "source_sha256":{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},"results":results}
    (work/"result.json").write_text(json.dumps(receipt,indent=2)+"\n");print(f"Receipt: {work/'result.json'}")

if __name__=="__main__":main()
