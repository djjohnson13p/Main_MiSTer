#!/usr/bin/env python3
"""Exercise intact MGL/file-SAX element boundaries above a disposable fopen spy.

Actual selected scan_mgl, mgl_parse/get, declarations and file-SAX execute.
Historical assertion failures remain complete finite observations; compiler
failure, crash, timeout or incomplete coverage cannot count as defect proof.
No menu/timer, selected action, hardware, media or user-target I/O executes.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ("support/arcade/mra_loader.cpp","support/arcade/mra_loader.h","sxmlc.c","sxmlc.h")
SCENARIOS = (
    "ordinary", "neutral-comment", "neutral-pi", "neutral-cdata",
    "root-comment", "root-pi", "root-cdata",
    "root-comment-mixed", "root-pi-mixed", "root-cdata-mixed",
    "file-comment", "file-pi", "file-cdata",
    "reset-comment", "reset-pi", "reset-cdata",
    "real-paired", "real-self-root", "real-empty-root", "real-incomplete-actions",
    "empty-then-valid", "valid-empty-recovery", "real-mixed-case", "open-error-recovery",
)

def digest(data):
    return hashlib.sha256(data).hexdigest()

def definition(source,marker,filename):
    start = source.index(marker)
    opening = source.index("{",start)
    depth = 0
    tokens = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',re.S)
    for token in tokens.finditer(source,opening):
        if token.group()=="{":
            depth += 1
        elif token.group()=="}":
            depth -= 1
            if not depth:
                end = token.end()+int(source[token.end():token.end()+1]==";")
                return '#line %d "%s"\n%s\n' % (source.count("\n",0,start)+1,filename,source[start:end])
    raise ValueError("Unclosed actual production definition: "+marker)

def generate(source):
    arcade,header = (source[p] for p in SOURCES[:2])
    types = ""
    for marker in ("struct mgl_item_struct\n","struct mgl_struct\n"):
        types += definition(header,marker,"support/arcade/mra_loader.h")
    for prefix in ("#define MGL_ACTION_","static mgl_struct mgl"):
        text = header if prefix.startswith("#") else arcade
        lines = [line for line in text.splitlines() if line.startswith(prefix)]
        if not lines:
            raise ValueError("Actual MGL declaration missing: "+prefix)
        types += "\n".join(lines)+"\n"
    production = definition(arcade,"static int scan_mgl(","support/arcade/mra_loader.cpp")
    production += "#define XMLDoc_parse_file_SAX observed_parse_file\n"
    for marker in ("mgl_struct* mgl_parse(","mgl_struct* mgl_get("):
        production += definition(arcade,marker,"support/arcade/mra_loader.cpp")
    production += "#undef XMLDoc_parse_file_SAX\n"
    parser = source["sxmlc.c"]
    point = '#include "sxmlc.h"'
    if parser.count(point)!=1:
        raise ValueError("Parser fopen adapter point is ambiguous")
    parser = parser.replace(point,point+'\nextern FILE *mgl_fixture_fopen(const SXML_CHAR *,const SXML_CHAR *);\n#undef sx_fopen\n#define sx_fopen mgl_fixture_fopen\n',1)
    return {"mgl_xml_events_types.inc":types,"mgl_xml_events_production.inc":production,
            "mgl_xml_events_sxmlc.c":parser}

def command(value):
    return [value] if Path(value).is_file() else shlex.split(value,posix=os.name!="nt")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx",default=os.environ.get("CXX","g++"))
    parser.add_argument("--cc",default=os.environ.get("CC"))
    parser.add_argument("--source-ref")
    parser.add_argument("--report","--output",dest="report",type=Path)
    args = parser.parse_args()
    raw = {name:subprocess.check_output(["git","-C",str(ROOT),"show",args.source_ref+":"+name])
           if args.source_ref else (ROOT/name).read_bytes() for name in SOURCES}
    source = {name:data.decode("utf-8").replace("\r\n","\n") for name,data in raw.items()}
    generated = generate(source)
    cxx = command(args.cxx)
    compiler = Path(cxx[0])
    default_cc = str(compiler.with_name(compiler.name.replace("g++","gcc")))
    cc = command(args.cc or default_cc)
    environment = os.environ.copy()
    if compiler.is_file():
        environment["PATH"] = str(compiler.resolve().parent)+os.pathsep+environment.get("PATH","")
    builds,runs = [],[]
    pair_raw = {name:(ROOT/"tests"/name).read_bytes() for name in
                ("mgl_xml_events_regression.py","mgl_xml_events_regression.cpp")}
    try:
        with tempfile.TemporaryDirectory(prefix="mister-mgl-xml-events-") as temporary:
            work = Path(temporary)
            (work/"sxmlc.h").write_text(source["sxmlc.h"],encoding="utf-8",newline="\n")
            for name,text in generated.items():
                (work/name).write_text(text,encoding="utf-8",newline="\n")
            executable = work/("probe.exe" if os.name=="nt" else "probe")
            common = ["-O2","-Wall","-Wextra","-Werror","-I",str(work)]
            commands = [cc+["-std=gnu99"]+common+["-Wno-error=format-security","-c",str(work/"mgl_xml_events_sxmlc.c"),"-o",str(work/"parser.o")],
                        cxx+["-std=gnu++14","-D_GNU_SOURCE"]+common+[str(ROOT/"tests/mgl_xml_events_regression.cpp"),str(work/"parser.o"),"-o",str(executable)]]
            for invocation in commands:
                result = subprocess.run(invocation,capture_output=True,text=True,env=environment,timeout=60)
                builds.append({"command":invocation,"returncode":result.returncode,"stdout":result.stdout,"stderr":result.stderr})
                if result.returncode:
                    raise RuntimeError("Compilation is fixture error, not behavioral evidence: "+result.stdout+result.stderr)
                if result.stderr:
                    print(result.stderr,file=sys.stderr,end="")
            for scenario in SCENARIOS:
                result = subprocess.run([str(executable),scenario],capture_output=True,text=True,env=environment,timeout=15)
                rows = [json.loads(line[5:]) for line in result.stdout.splitlines() if line.startswith("JSON ")]
                if len(rows)!=1 or rows[0]["case"]!=scenario or not rows[0]["case_body_completed"] or rows[0]["checks"]<1 or rows[0]["failures"]<0 or rows[0]["failures"]>rows[0]["checks"] or result.returncode!=(1 if rows[0]["failures"] else 0):
                    raise RuntimeError("Incomplete/crashed/timed-out scenario is not proof: "+scenario+"\n"+result.stdout+result.stderr)
                expected_parses = {"empty-then-valid":2,"valid-empty-recovery":3,"open-error-recovery":3}.get(scenario,1)
                if rows[0]["excluded"] or rows[0]["trapped"]:
                    raise RuntimeError("Unexpected exclusion/guard is not complete MGL behavior proof: "+scenario)
                if len(rows[0]["parses"])!=expected_parses or any(not p["parse_call_completed"] or not p["parser_return_observed"] for p in rows[0]["parses"]):
                    raise RuntimeError("Actual parser/helper return coverage missing: "+scenario)
                runs.append({"scenario":scenario,"returncode":result.returncode,"summary":rows[0],"stdout":result.stdout,"stderr":result.stderr})
                print(("FAIL" if result.returncode else "PASS")+": "+scenario)
    except Exception as error:
        if args.report:
            args.report.write_text(json.dumps({"fixture_error":str(error),"source_ref":args.source_ref or "working tree","builds":builds,"completed_runs":runs},indent=2)+"\n",encoding="utf-8",newline="\n")
        raise
    if [r["scenario"] for r in runs]!=list(SCENARIOS) or len(set(r["scenario"] for r in runs))!=len(SCENARIOS):
        raise RuntimeError("Exact scenario coverage is incomplete or duplicated")
    if not args.source_ref and {name:digest((ROOT/name).read_bytes()) for name in SOURCES}!={name:digest(data) for name,data in raw.items()}:
        raise RuntimeError("Selected production changed during fixture")
    if {name:digest((ROOT/"tests"/name).read_bytes()) for name in pair_raw}!={name:digest(data) for name,data in pair_raw.items()}:
        raise RuntimeError("Fixture pair changed during compilation/run")
    summary = {"cases":len(runs),"checks":sum(r["summary"]["checks"] for r in runs),
               "failures":sum(r["summary"]["failures"] for r in runs),"failed_cases":sum(bool(r["summary"]["failures"]) for r in runs),
               "parses":sum(len(r["summary"]["parses"]) for r in runs),"excluded":0,"prevented_boundaries":0,"user_target_io":0,"hardware_io":0}
    report = {"source_ref":args.source_ref or "working tree","source_raw_sha256":{name:digest(data) for name,data in raw.items()},
        "source_normalized_sha256":{name:digest(text.encode()) for name,text in source.items()},
        "test_sha256":{name:digest(data) for name,data in pair_raw.items()},"extracted_sha256":{name:digest(text.encode()) for name,text in generated.items()},
        "scenario_order":list(SCENARIOS),"coverage_complete":True,"builds":builds,"runs":runs,"summary":summary,
        "scope":"Complete selected scan_mgl, mgl_parse/get and actual declarations/globals with actual file-SAX parser. Only parser fopen is redirected to disposable seekable streams or explicitly injected ENOENT. Callback observer forwards all events/arguments/returns unchanged and captures full typed MGL state and ordered action metadata.",
        "limits":"Finite synthetic XML; no general malformed-XML validator or corpus-frequency claim. No menu/timer/action execution, core hardware, media/path/file loading, persistence or performance evidence. C-only inherited serializer format-security diagnostics remain visible; C++ is strict -Werror. Historical cases complete with genuine parser/helper observations; no traps/exclusions/compiler/crash/timeout proof."}
    if args.report:
        args.report.write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8",newline="\n")
    print(json.dumps(summary))
    return 1 if summary["failures"] else 0

if __name__=="__main__":
    raise SystemExit(main())
