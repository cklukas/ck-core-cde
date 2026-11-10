#!/usr/bin/env python3
"""Exercise dtfile's production FileData2 packing and packet decoding."""
import argparse, hashlib, json, os, re, subprocess, sys, time
from pathlib import Path

CASES=("pack-valid","pack-overflow","pack-filedata-valid",
       "pack-filedata-null","pack-filedata-overflow","stat-failed","stat-link",
       "decode-valid","decode-negative",
       "decode-short-header","decode-short-text","packet-valid",
       "packet-zero-count","packet-large-count","packet-extra-byte",
       "packet-short-second","desktop-write-changed",
       "desktop-write-removed","desktop-truncated-host",
       "desktop-truncated-data","desktop-invalid-data",
       "desktop-invalid-message","desktop-null-name",
       "desktop-wire-pointer","desktop-mismatched-data")

def balanced(text,start,opening,closing):
    depth=0;i=start
    while i<len(text):
        if text.startswith("/*",i):
            end=text.find("*/",i+2)
            if end<0:raise ValueError("unterminated comment")
            i=end+2;continue
        if text[i] in "\"'":
            quote=text[i];i+=1
            while i<len(text) and text[i]!=quote:
                i+=2 if text[i]=="\\" else 1
            if i>=len(text):raise ValueError("unterminated literal")
        elif text[i]==opening:depth+=1
        elif text[i]==closing:
            depth-=1
            if depth==0:return i+1
        i+=1
    raise ValueError("unbalanced definition")

def extract_function(text,name):
    pattern=re.compile(r"(?m)^(?:static[ \t\r\n]+)?(?:int|void|Boolean|FileData[ \t]*\*)[ \t\r\n]+"+
                       re.escape(name)+r"[ \t\r\n]*\(")
    matches=[]
    for match in pattern.finditer(text):
        args=balanced(text,match.end()-1,"(",")")
        body=args
        while body<len(text) and text[body].isspace():body+=1
        if body<len(text) and text[body]=="{":matches.append(text[match.start():balanced(text,body,"{","}")])
    if len(matches)!=1:raise ValueError(f"expected one {name}, found {len(matches)}")
    return matches[0]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--module-source",type=Path)
    parser.add_argument("--work-dir",type=Path)
    parser.add_argument("--case",choices=CASES)
    parser.add_argument("--sanitizers",default="address,undefined")
    parser.add_argument("--baseline-control",action="store_true",
                        help="prove the legacy decoder fails on a negative field length")
    args=parser.parse_args()
    tests=Path(__file__).resolve().parent
    source=(args.module_source or tests.parent).resolve()
    work=args.work_dir
    if work is None:
        if sys.platform=="darwin":parser.error("--work-dir is required on macOS")
        work=Path.cwd()/"directory-packet-check"
    work=work.resolve();work.mkdir(parents=True,exist_ok=False);(work/"tmp").mkdir()
    directory_path=source/"Directory.c";directory=directory_path.read_text()
    if args.baseline_control:
        pattern=re.compile(r"(?m)^FileData[ \t]*\*[ \t\r\n]+FileData2toFileData[ \t\r\n]*\(")
        match=pattern.search(directory)
        if match is None:raise ValueError("legacy FileData2toFileData definition not found")
        args_end=balanced(directory,match.end()-1,"(",")");body=args_end
        while body<len(directory) and directory[body].isspace():body+=1
        definition=directory[match.start():balanced(directory,body,"{","}")]
        fixture=(tests/"directory-packets-baseline.c").read_text().replace("/* PRODUCTION_FUNCTION */",definition)
        generated=work/"directory-packets-baseline.c";generated.write_text(fixture)
        binary=work/"directory-packets-baseline";cc=os.environ.get("CC","cc").split()
        flags=["-std=c11","-O1","-g",f"-fsanitize={args.sanitizers}","-fno-omit-frame-pointer"]
        env=dict(os.environ,TMPDIR=str(work/"tmp"),LANG="C",LC_ALL="C",
                 ASAN_OPTIONS="detect_leaks=0:abort_on_error=1",
                 UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        subprocess.run(cc+flags+[str(generated),"-o",str(binary)],check=True,env=env)
        run=subprocess.run([str(binary)],text=True,capture_output=True,env=env)
        triggered=run.returncode!=0 and ("AddressSanitizer" in run.stderr or "negative-size-param" in run.stderr)
        receipt={"passed":triggered,"baseline_triggered":triggered,"returncode":run.returncode,
                 "stdout":run.stdout,"stderr":run.stderr,"source":str(source)}
        (work/"result.json").write_text(json.dumps(receipt,indent=2)+"\n")
        if not triggered:
            print(run.stdout,end="");print(run.stderr,end="",file=sys.stderr);raise SystemExit(1)
        print("PASS baseline-negative-length-trigger")
        print(f"Receipt: {work/'result.json'}")
        return
    desktop=(source/"Desktop.c").read_text();fileop=(source/"FileOp.c").read_text()
    desktop_callback=extract_function(directory,"CheckDesktopPipeCallback")
    desktop_process=extract_function(directory,"CheckDesktopProcess")
    update_all=extract_function(directory,"UpdateAllProcess")
    contract={
        "packet_capacity_includes_header":"PIPEMSG_HDR_LEN + FILEDATABUF * sizeof(FileData2)" in directory,
        "packet_producer_exact":directory.count("DtfileProcessWriteAll(pipe_fd, file_data_buffer") == 1,
        "packet_consumer_bounded":"file_data_count > FILEDATABUF" in directory and "FileData2DecodePacket(" in directory,
        "filedata2_exact_io":directory.count("DtfileProcessWriteAll(pipe_fd, &file_data2") >= 2 and
                             "PipeReadExact(*fd, &file_data2" in directory,
        "record_initialized":"memset(file_data2, 0, sizeof(*file_data2))" in directory,
        "failed_lstat_not_inspected":directory.count("FileDataStatIsLink(stat_result, &stat_buf)") == 2,
        "malformed_discards_pending":"while (directory->new_data != NULL)" in directory and
                                     "FileOpProtocolError(toplevel)" in directory,
        "desktop_packet_transactional":"DirectoryReadDesktopRecord(*fd, msg" in desktop_callback and
                                       "PipeReadExact(*fd, &msg" in desktop_callback and
                                       "PipeReadFileData(*fd)" not in desktop_callback,
        "desktop_producer_exact":desktop_process.count("DirectoryWriteDesktopRecord(pipe_fd") == 2 and
                                 "tt_status != TT_OK || full_path == NULL" in desktop_process and
                                 "DtfileProcessWriteAll(pipe_fd, &pipe_msg" in desktop_process,
        "desktop_malformed_retires":"FileOpProtocolError(toplevel)" in desktop_callback and
                                    "kill(pipe_data->child, SIGKILL)" in desktop_callback and
                                    "directory->busy[activity] = False" in desktop_callback,
        "tombstone_filedata2":"FileData2PackFileData(&file_data2, old_data)" in update_all and
                              update_all.count("DtfileProcessWriteAll(pipe_fd, &file_data2") >= 2 and
                              re.search(r"\bPIPEMSG_FILEDATA\b",directory) is None and
                              "PipeWriteFileData" not in directory and
                              "PipeReadFileData" not in directory,
        "desktop_fallback":"ReadFileData(full_dir_name, entry)" in desktop,
        "fileop_fallback":"ReadFileData(full_dir_name, entry)" in fileop,
    }
    if not all(contract.values()):raise ValueError(f"directory packet contract mismatch: {contract}")
    functions="\n\n".join(extract_function(directory,n) for n in
                            ("FileData2Decode","FileData2toFileData",
                             "DirectoryWriteDesktopRecord","DirectoryReadDesktopRecord",
                             "FileData2DecodePacket","FileData2PackText",
                             "FileData2PackFileData",
                             "FileDataStatIsLink"))
    fixture=(tests/"directory-packets.c").read_text().replace("/* PRODUCTION_FUNCTIONS */",functions)
    generated=work/"directory-packets.c";generated.write_text(fixture)
    binary=work/"directory-packets"
    cc=os.environ.get("CC","cc").split()
    flags=["-std=c11","-D_DEFAULT_SOURCE","-Wall","-Wextra","-Werror","-O1","-g"]
    if args.sanitizers!="none":flags += [f"-fsanitize={args.sanitizers}","-fno-omit-frame-pointer"]
    env=dict(os.environ,TMPDIR=str(work/"tmp"),LANG="C",LC_ALL="C")
    env.setdefault("ASAN_OPTIONS",f"detect_leaks={'0' if sys.platform=='darwin' else '1'}:abort_on_error=1")
    env.setdefault("UBSAN_OPTIONS","halt_on_error=1:print_stacktrace=1")
    subprocess.run(cc+flags+[str(generated),"-o",str(binary)],check=True,env=env)
    selected=(args.case,) if args.case else CASES;started=time.monotonic();outputs=[]
    for case in selected:
        run=subprocess.run([str(binary),case],check=False,text=True,capture_output=True,env=env)
        outputs.append({"case":case,"returncode":run.returncode,"stdout":run.stdout,"stderr":run.stderr})
        if run.stdout:print(run.stdout,end="")
        if run.stderr:print(run.stderr,end="",file=sys.stderr)
        if run.returncode:raise SystemExit(run.returncode)
    hashes={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in
            (directory_path,source/"Desktop.c",source/"FileOp.c",Path(__file__),
             tests/"directory-packets.c",tests/"directory-packets-baseline.c")}
    receipt={"passed":True,"cases":len(selected),"seconds":time.monotonic()-started,
             "sanitizers":args.sanitizers,"source":str(source),"contract":contract,
             "source_sha256":hashes,"results":outputs}
    (work/"result.json").write_text(json.dumps(receipt,indent=2)+"\n")
    print(f"Receipt: {work/'result.json'}")

if __name__=="__main__":main()
