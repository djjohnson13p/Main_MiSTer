#!/usr/bin/env python3
"""Run intact Arcade NVRAM/file-SAX production against finite endpoint spies.

The parser, buffer, callback, selector, binding helpers, file helpers and core
helpers are actual selected source. Historical unsafe libc/allocation boundaries
end the case explicitly. Compilation, crash and timeout are fixture errors.
Disposable parser streams and temporary build files are the only host I/O.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ("support/arcade/mra_loader.cpp", "support/arcade/mra_loader.h",
           "support/arcade/buffer.cpp", "support/arcade/buffer.h",
           "sxmlc.c", "sxmlc.h", "file_io.cpp", "file_io.h",
           "user_io.cpp", "user_io.h", "spi.h", "lib/md5/md5.h")
SCENARIOS = (
    "ordinary-missing", "ordinary-full", "ordinary-short", "ordinary-empty",
    "stale-absent", "stale-index-only", "stale-size-only", "stale-empty",
    "stale-open-error", "malformed-before", "malformed-after", "unfinished-node",
    "unmatched-end", "nested-switches", "nested-rom", "nested-cheats", "nested-nvram",
    "repeated-complete", "repeated-partial", "declaration-order",
    "duplicate-same", "duplicate-case", "duplicate-invalid-valid", "duplicate-valid-invalid",
    "attributes-reordered", "index-zero", "index-255", "index-256", "index-negative",
    "index-prefix", "index-hex", "index-octal", "index-spaces-plus",
    "size-zero", "size-prefix", "size-negative", "size-overflow", "size-octal",
    "recovery", "literal-percent", "escaped-percent", "format-n", "name-199",
    "name-200", "name-243", "name-244", "name-255", "name-distinct",
    "name-internal-suffix", "name-case-suffix", "name-extensionless",
    "name-empty", "name-over-component", "root-null", "root-relative",
    "root-path-reject", "captured-root", "snapshot-binding", "allocation-load",
    "allocation-save", "save-open-failure", "save-write-failure",
    "save-close-failure", "save-rename-failure",
    "number-builder", "name-builder", "target-builder", "large-legal-budget",
)
PRIOR_EXCLUSIONS = frozenset(("number-builder", "name-builder", "target-builder"))


def digest(data):
    return hashlib.sha256(data).hexdigest()


def definition(source, marker, filename):
    start = source.index(marker)
    opening = source.index("{", start)
    depth = 0
    tokens = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)
    for token in tokens.finditer(source, opening):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if not depth:
                end = token.end() + int(source[token.end():token.end()+1] == ";")
                return '#line %d "%s"\n%s\n' % (source.count("\n", 0, start)+1, filename, source[start:end])
    raise ValueError("Unclosed actual production definition: " + marker)


def generate(source):
    arcade, header, io, ioh, user, userh = (source[p] for p in
        ("support/arcade/mra_loader.cpp", "support/arcade/mra_loader.h",
         "file_io.cpp", "file_io.h", "user_io.cpp", "user_io.h"))
    # Candidate detection is updated to the actual private helper marker.
    checked = "static int arcade_nvm_name(" in arcade
    file_checked = "static int resolve_mutation_path(" in io
    types = "#define HAVE_NVRAM_BINDING %d\n#define HAVE_CHECKED_FILE %d\n" % (checked,file_checked)
    types += "struct fileZipArchive;\n" + definition(ioh, "struct fileTYPE\n", "file_io.h")
    for marker in ("struct dip_struct\n", "struct sw_struct\n"):
        types += definition(header, marker, "support/arcade/mra_loader.h")
    for pattern, text in ((r"^static char full_path\[[^\n]+", io),
                          (r"^#define CONFIG_DIR[^\n]+", ioh),
                          (r"^#define kBigTextSize[^\n]+", arcade)):
        types += re.search(pattern, text, re.M).group() + "\n"
    types += definition(arcade, "struct arc_struct {", "support/arcade/mra_loader.cpp")
    for marker in ("static sw_struct switches", "static char arcade_error_msg",
                   "static char arcade_setname", "static int      unitlen", "static int      romlen"):
        types += re.search(r"^" + re.escape(marker) + r"[^\n]+", arcade, re.M).group() + "\n"
    for name in ("FileOpen", "FileClose", "FileReadAdv", "FileLoad", "FileSave",
                 "FileLoadConfig", "FileCreatePath"):
        types += re.search(r"^int\s+" + name + r"\([^\n]+", ioh, re.M).group() + "\n"
    if file_checked:
        types += re.search(r"^int FileResolveFullPath\([^\n]+", ioh, re.M).group() + "\n"
    for name in ("user_io_set_index", "user_io_set_download", "user_io_set_upload",
                 "user_io_file_tx_data", "user_io_file_rx_data"):
        types += re.search(r"^void " + name + r"\([^\n]+", userh, re.M).group() + "\n"
    for name in ("FIO_FILE_INDEX", "FIO_FILE_TX", "FIO_FILE_TX_DAT"):
        types += re.search(r"^#define " + name + r"[^\n]+", userh, re.M).group() + "\n"
    for name in ("last_file_ext_idx", "fio_size"):
        types += re.search(r"^static [^\n]+ " + name + r"[^\n]+", user, re.M).group() + "\n"

    production = ""
    for marker in ("fileTYPE::fileTYPE()", "fileTYPE::~fileTYPE()", "int fileTYPE::opened()"):
        production += definition(io, marker, "file_io.cpp")
    if file_checked:
        production += definition(io, "int FileResolveFullPath(", "file_io.cpp")
    else:
        production += "#define sprintf observed_sprintf\n" + definition(io, "static char* make_fullpath(", "file_io.cpp") + "#undef sprintf\n"
    production += "#define open fixture_open\n#define write fixture_write\n#define close fixture_close\n#define rename fixture_rename\n#define unlink fixture_unlink\n"
    if file_checked:
        for marker in ("static int resolve_mutation_path(", "static int prepare_save_temp(", "static int save_prepared("):
            production += definition(io, marker, "file_io.cpp")
    production += definition(io, "int FileSave(", "file_io.cpp")
    production += "#undef unlink\n#undef rename\n#undef close\n#undef write\n#undef open\n"
    production += definition(io, "int FileLoad(", "file_io.cpp")
    if file_checked:
        production += definition(io, "static int resolve_config_path(", "file_io.cpp")
    else:
        production += "#define strcat observed_strcat\n"
    production += definition(io, "int FileLoadConfig(", "file_io.cpp")
    if not file_checked:
        production += "#undef strcat\n"
    for marker in ("void user_io_set_index(", "void user_io_set_download(",
                   "void user_io_file_tx_data(", "void user_io_set_upload(", "void user_io_file_rx_data("):
        production += definition(user, marker, "user_io.cpp")

    start = arcade.index("\n", arcade.index("static sw_struct switches")) + 1
    end = arcade.index("sw_struct *arcade_sw()", start)
    production += "__attribute__((unused)) static int observed_FileLoad(const char *n,void *b,int z) { int r=FileLoad(n,b,z); observe_file_return(\"FileLoad\",n,r); return r; }\n"
    production += "__attribute__((unused)) static int observed_FileLoadConfig(const char *n,void *b,int z) { int r=FileLoadConfig(n,b,z); observe_file_return(\"FileLoadConfig\",n,r); return r; }\n"
    production += "static int observed_FileSave(const char *n,void *b,int z) { int r=FileSave(n,b,z); observe_file_return(\"FileSave\",n,r); return r; }\n"
    production += "#define FileLoad observed_FileLoad\n#define FileLoadConfig observed_FileLoadConfig\n#define FileSave observed_FileSave\n"
    production += '#line %d "support/arcade/mra_loader.cpp"\n' % (arcade.count("\n", 0, start)+1)
    production += "#define strcat observed_strcat\n#define snprintf observed_snprintf\n#define strtoul observed_strtoul\n"
    production += arcade[start:end]
    production += "#undef strtoul\n#undef snprintf\n#undef strcat\n#undef FileSave\n#undef FileLoadConfig\n#undef FileLoad\n"
    for marker in ("sw_struct *arcade_sw()", "void arcade_sw_send()"):
        production += definition(arcade, marker, "support/arcade/mra_loader.cpp")
    if "static int arcade_dip_path(" in arcade:
        production += definition(arcade, "static int arcade_dip_path(", "support/arcade/mra_loader.cpp")
    production += "#define strcat observed_strcat\n" + definition(arcade, "void arcade_sw_load()", "support/arcade/mra_loader.cpp") + "#undef strcat\n"
    production += "#define snprintf observed_snprintf\n#define strtoul observed_strtoul\n"
    production += definition(arcade, "static int xml_send_rom(", "support/arcade/mra_loader.cpp")
    production += "#define XMLDoc_parse_file_SAX observed_parse_file\n"
    production += definition(arcade, "int arcade_send_rom(", "support/arcade/mra_loader.cpp")
    production += "#undef XMLDoc_parse_file_SAX\n#undef strtoul\n#undef snprintf\n"
    parser = source["sxmlc.c"]
    opening = '#include "sxmlc.h"'
    if parser.count(opening) != 1:
        raise ValueError("Parser instrumentation point is ambiguous")
    parser = parser.replace(opening, opening + '\nextern FILE *nvram_fixture_fopen(const SXML_CHAR *, const SXML_CHAR *);\n#undef sx_fopen\n#define sx_fopen nvram_fixture_fopen\n', 1)
    return types, production, parser, checked


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--cc", default=os.environ.get("CC"))
    parser.add_argument("--source-ref")
    parser.add_argument("--report", "--output", dest="report", type=Path)
    args = parser.parse_args()
    raw = {n: subprocess.check_output(["git", "-C", str(ROOT), "show", args.source_ref+":"+n])
           if args.source_ref else (ROOT/n).read_bytes() for n in SOURCES}
    source = {n:b.decode("utf-8").replace("\r\n", "\n") for n,b in raw.items()}
    types, production, parser_source, checked = generate(source)
    cxx = [args.cxx] if Path(args.cxx).is_file() else shlex.split(args.cxx, posix=os.name!="nt")
    default_cc = str(Path(cxx[0]).with_name(Path(cxx[0]).name.replace("g++", "gcc")))
    cc_value = args.cc or default_cc
    cc = [cc_value] if Path(cc_value).is_file() else shlex.split(cc_value, posix=os.name!="nt")
    env = os.environ.copy()
    if Path(cxx[0]).is_file():
        env["PATH"] = str(Path(cxx[0]).resolve().parent)+os.pathsep+env.get("PATH","")
    generated = {"arcade_nvram_types.inc":types, "arcade_nvram_production.inc":production,
                 "arcade_nvram_sxmlc.c":parser_source}
    builds, runs = [], []
    with tempfile.TemporaryDirectory(prefix="mister-arcade-nvram-") as temporary:
        work = Path(temporary)
        for n in ("sxmlc.h", "support/arcade/buffer.cpp", "support/arcade/buffer.h", "lib/md5/md5.h"):
            target = work/n
            target.parent.mkdir(parents=True,exist_ok=True)
            target.write_text(source[n],encoding="utf-8",newline="\n")
        for n,text in generated.items():
            (work/n).write_text(text,encoding="utf-8",newline="\n")
        obj, executable = work/"parser.o", work/("probe.exe" if os.name=="nt" else "probe")
        common = ["-O2","-Wall","-Wextra","-Werror","-I",str(work)]
        commands = [cc+["-std=gnu99"]+common+["-c",str(work/"arcade_nvram_sxmlc.c"),"-o",str(obj)],
                    cxx+["-std=gnu++14","-D_GNU_SOURCE","-D_LARGEFILE64_SOURCE"]+common+
                    ["-I",str(work/"support/arcade"),str(ROOT/"tests/arcade_nvram_regression.cpp"),
                     str(work/"support/arcade/buffer.cpp"),str(obj),"-o",str(executable)]]
        for command in commands:
            result = subprocess.run(command,capture_output=True,text=True,env=env,timeout=60)
            builds.append({"command":command,"returncode":result.returncode,"stdout":result.stdout,"stderr":result.stderr})
            if result.returncode:
                error={"fixture_error":"Compilation is not behavioral evidence","source_ref":args.source_ref or "working tree","builds":builds}
                if args.report:
                    args.report.write_text(json.dumps(error,indent=2)+"\n",encoding="utf-8",newline="\n")
                raise RuntimeError(error["fixture_error"]+"\n"+result.stdout+result.stderr)
        for scenario in SCENARIOS:
            result = subprocess.run([str(executable),scenario],capture_output=True,text=True,env=env,timeout=15)
            rows = [json.loads(line[5:]) for line in result.stdout.splitlines() if line.startswith("JSON ")]
            if len(rows)!=1 or rows[0]["case"]!=scenario or rows[0]["checks"]<=0 or rows[0]["failures"]<0 or rows[0]["failures"]>rows[0]["checks"] or result.returncode!=(1 if rows[0]["failures"] else 0):
                raise RuntimeError("Incomplete/crashed scenario is not evidence: "+scenario+"\n"+result.stdout+result.stderr)
            if not rows[0]["trapped"] and not rows[0]["case_body_completed"]:
                raise RuntimeError("Case body did not complete: "+scenario)
            if rows[0]["trapped"] and not rows[0]["failures"]:
                raise RuntimeError("Guard trap cannot count as repaired success: "+scenario)
            if rows[0]["excluded"] != (not checked and scenario in PRIOR_EXCLUSIONS):
                raise RuntimeError("Unexpected historical exclusion: "+scenario)
            runs.append({"scenario":scenario,"returncode":result.returncode,"summary":rows[0],
                         "stdout":result.stdout,"stderr":result.stderr})
            print(("EXCLUDED" if rows[0]["excluded"] else "FAIL" if result.returncode else "PASS")+": "+scenario)
    if len(runs)!=len(SCENARIOS) or len(set(r["scenario"] for r in runs))!=len(SCENARIOS) or [r["scenario"] for r in runs]!=list(SCENARIOS):
        raise RuntimeError("Incomplete or duplicate case coverage")
    if not args.source_ref and {n:digest((ROOT/n).read_bytes()) for n in SOURCES}!={n:digest(b) for n,b in raw.items()}:
        raise RuntimeError("Production changed during fixture")
    summary = {"cases":len(runs),"checks":sum(r["summary"]["checks"] for r in runs),
               "failures":sum(r["summary"]["failures"] for r in runs),
               "excluded":sum(r["summary"]["excluded"] for r in runs),
               "prevented_boundaries":sum(r["summary"]["trapped"] for r in runs),
               "user_target_io":0,"hardware_io":0}
    report = {"source_ref":args.source_ref or "working tree","checked_candidate":checked,
              "source_raw_sha256":{n:digest(b) for n,b in raw.items()},
              "source_normalized_sha256":{n:digest(t.encode()) for n,t in source.items()},
              "test_sha256":{n:digest((ROOT/"tests"/n).read_bytes()) for n in ("arcade_nvram_regression.py","arcade_nvram_regression.cpp")},
              "extracted_sha256":{n:digest(t.encode()) for n,t in generated.items()},
              "builds":builds,"coverage_complete":True,"scenario_order":list(SCENARIOS),
              "summary":summary,"runs":runs,
              "limits":"Actual selected file-SAX parser with fopen redirected to disposable seekable streams; actual buffer, callback/selector/NVRAM helpers and endpoints/file helpers/core helpers above lower spies. Historical traps end cases without invented return/transfer. New private-helper cases are explicitly excluded on older source. Nonthrowing array budget failures are injected local allocation failures, not product size caps or successful payload operations. Host widths are recorded; ARM execution/hardware/persistence/timing/durability untested."}
    if args.report:
        args.report.write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8",newline="\n")
    print(json.dumps(summary))
    return 1 if summary["failures"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
