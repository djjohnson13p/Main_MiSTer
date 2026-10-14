#!/usr/bin/env python3
"""Exercise intact config/mutation/DIP production against filesystem spies.

Historical sprintf/strcat boundaries are intercepted before unsafe writes. A
trap ends that scenario without inventing a later status or transfer. Synthetic
invalid-input/private-builder controls are explicitly excluded on prior code.
The MRA selector uses its exact basename/DIP identity block, after optional
entry ownership and ending before NVRAM/XML/core work. Entry admission is
exercised by the separate MRA path and full selector fixtures.
No user files, real filesystem operations, hardware, or durability are tested.
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
SOURCES = ("file_io.cpp", "file_io.h", "support/arcade/mra_loader.cpp", "support/arcade/mra_loader.h")
SCENARIOS = (
    "save-relative", "save-absolute", "save-empty", "save-zero",
    "save-absolute-bound", "save-relative-bound", "save-absolute-reject", "save-relative-reject",
    "delete-bound", "rmdir-bound", "delete-reject", "rmdir-reject",
    "save-alias-full", "save-alias-substring", "delete-alias-full", "delete-alias-substring",
    "rmdir-alias-full", "rmdir-alias-substring", "save-global-overwrite",
    "save-open-failure", "save-write-failure", "save-short-write", "save-close-failure",
    "save-rename-failure", "save-cleanup-errno", "delete-valid", "delete-failure",
    "rmdir-valid", "rmdir-failure", "config-empty", "config-leading-slash",
    "config-load-empty", "config-load-leading-slash", "config-delete-empty", "config-delete-leading-slash", "config-nested",
    "config-mkdir-failure", "config-preflight-reject", "config-load-long", "config-save-long",
    "config-delete-long", "config-load-buffer-alias", "config-save-buffer-alias",
    "config-target-bound", "config-directory-bound", "config-directory-root-change",
    "mutation-null", "mutation-root-reject", "mutation-null-root", "mutation-allocation",
    "config-null", "config-root-reject", "config-null-root", "config-allocation", "recovery",
    "config-builder", "temp-builder", "dip-builder", "dip-mra-243", "dip-mra-244", "dip-mra-255",
    "dip-reject-load", "dip-reject-save", "dip-save-failure", "dip-load-failure", "dip-noop",
    "dip-internal-suffix", "dip-internal-case", "dip-internal-adjacent", "dip-internal-distinct",
)
PRIOR_EXCLUSIONS = frozenset(("mutation-null", "mutation-root-reject", "mutation-null-root", "mutation-allocation",
                              "config-null", "config-root-reject", "config-null-root", "config-allocation",
                              "recovery", "config-builder", "temp-builder", "dip-builder"))


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
                end = token.end() + int(source[token.end():token.end() + 1] == ";")
                return '#line %d "%s"\n%s\n' % (source.count("\n", 0, start) + 1, filename, source[start:end])
    raise ValueError("Unclosed production definition: " + marker)


def generate(source):
    io, header, arcade, arcade_header = (source[name] for name in SOURCES)
    checked = "static int resolve_mutation_path(" in io
    prepared_directory = "static int create_config_directory(" in io
    if checked != ("static int resolve_config_path(" in io) or checked != ("static int arcade_dip_path(" in arcade):
        raise ValueError("Config/mutation/DIP production must be tested as a complete candidate")
    types = "#define HAVE_CHECKED_MUTATION %d\n" % checked
    types += "#define HAVE_PREPARED_DIRECTORY %d\n" % prepared_directory
    types += "struct fileZipArchive;\n" + definition(header, "struct fileTYPE\n", "file_io.h")
    types += re.search(r"^#define CONFIG_DIR[^\n]+", header, re.M).group() + "\n"
    types += re.search(r"^static char full_path\[[^\n]+", io, re.M).group() + "\n"
    for name in ("FileOpen", "FileClose", "FileReadAdv", "FileLoad", "FileSave", "FileDelete", "DirDelete",
                 "FileLoadConfig", "FileSaveConfig", "FileDeleteConfig", "FileCreatePath"):
        types += re.search(r"^int\s+" + name + r"\([^\n]+", header, re.M).group() + "\n"
    resolver = "int FileResolveFullPath(" in io
    if resolver:
        types += re.search(r"^int FileResolveFullPath\([^\n]+", header, re.M).group() + "\n"
    types += definition(arcade_header, "struct dip_struct\n", "support/arcade/mra_loader.h")
    types += definition(arcade_header, "struct sw_struct\n", "support/arcade/mra_loader.h")
    production = ""
    for marker in ("fileTYPE::fileTYPE()", "fileTYPE::~fileTYPE()", "int fileTYPE::opened()"):
        production += definition(io, marker, "file_io.cpp")
    # The unchanged legacy helper remains used by directory creation and the
    # deliberate same-thread overwrite spy, so guard it in every revision.
    production += "#define sprintf guarded_sprintf\n" + definition(io, "static char* make_fullpath(", "file_io.cpp") + "#undef sprintf\n"
    if resolver:
        production += definition(io, "int FileResolveFullPath(", "file_io.cpp")
    production += "#define FileIsZipped production_FileIsZipped\n" + definition(io, "static int FileIsZipped(", "file_io.cpp") + "#undef FileIsZipped\n"
    production += "#define FileIsZipped observed_FileIsZipped\n#define strcat guarded_query_strcat\n"
    if prepared_directory:
        production += definition(io, "static int isPathDirectoryPrepared(", "file_io.cpp")
    production += definition(io, "static int isPathDirectory(", "file_io.cpp")
    production += "#undef strcat\n#undef FileIsZipped\n"
    if prepared_directory:
        production += "#define mkdir fixture_mkdir\n" + definition(io, "static int create_config_directory(", "file_io.cpp") + "#undef mkdir\n"
    production += "#define open fixture_open\n#define write fixture_write\n#define close fixture_close\n#define rename fixture_rename\n#define unlink fixture_unlink\n#define rmdir fixture_rmdir\n"
    if checked:
        for marker in ("static int resolve_mutation_path(", "static int prepare_save_temp(",
                       "static int save_prepared(", "static int delete_prepared("):
            production += definition(io, marker, "file_io.cpp")
    for marker in ("int FileSave(", "int FileDelete(", "int DirDelete("):
        production += definition(io, marker, "file_io.cpp")
    production += "#undef rmdir\n#undef unlink\n#undef rename\n#undef close\n#undef write\n#undef open\n"
    production += definition(io, "int FileLoad(", "file_io.cpp")
    if checked:
        production += definition(io, "static int resolve_config_path(", "file_io.cpp")
    else:
        production += "#define strcat guarded_strcat\n#define strncat guarded_strncat\n"
    for marker in ("int FileLoadConfig(", "int FileSaveConfig(", "int FileDeleteConfig("):
        production += definition(io, marker, "file_io.cpp")
    if not checked:
        production += "#undef strncat\n#undef strcat\n"
    production += "#define mkdir fixture_mkdir\n" + definition(io, "int FileCreatePath(", "file_io.cpp") + "#undef mkdir\n"
    production += "static sw_struct switches; static sw_struct *arcade_sw() { return &switches; }\n"
    if checked:
        production += definition(arcade, "static int arcade_dip_path(", "support/arcade/mra_loader.cpp")
    else:
        production += "#define strcat guarded_strcat\n"
    production += "__attribute__((unused)) static int observed_FileLoadConfig(const char *n, void *b, int z) { int r=FileLoadConfig(n,b,z); record_file_api(\"FileLoadConfig\",n,r); return r; }\n"
    production += "__attribute__((unused)) static int observed_FileSaveConfig(const char *n, void *b, int z) { int r=FileSaveConfig(n,b,z); record_file_api(\"FileSaveConfig\",n,r); return r; }\n"
    production += "__attribute__((unused)) static int observed_FileSave(const char *n, void *b, int z) { int r=FileSave(n,b,z); record_file_api(\"FileSave\",n,r); return r; }\n"
    production += "#define FileLoadConfig observed_FileLoadConfig\n#define FileSaveConfig observed_FileSaveConfig\n#define FileSave observed_FileSave\n"
    for marker in ("void arcade_sw_save()", "void arcade_sw_load()"):
        production += definition(arcade, marker, "support/arcade/mra_loader.cpp")
    production += "#undef FileSave\n#undef FileSaveConfig\n#undef FileLoadConfig\n"
    if not checked:
        production += "#undef strcat\n"
    selector = definition(arcade, "int arcade_send_rom(", "support/arcade/mra_loader.cpp")
    identity_start = selector.index("\n\tconst char *p = strrchr(xml, '/');") if "std::string owned_xml;" in selector else selector.index("{")
    nvram_start = "\n\tarcade_nvm_reset();" if "\n\tarcade_nvm_reset();" in selector else "\n\tsnprintf(nvram_name"
    prefix = selector[identity_start + 1:selector.index(nvram_start, identity_start)]
    production += "static void select_arcade_dip_name(const char *xml) {\n" + prefix + "\n}\n"
    return types, production, checked


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--source-ref")
    parser.add_argument("--report", "--output", dest="report", type=Path)
    args = parser.parse_args()
    raw = {name: subprocess.check_output(["git", "-C", str(ROOT), "show", args.source_ref + ":" + name])
           if args.source_ref else (ROOT / name).read_bytes() for name in SOURCES}
    source = {name: data.decode("utf-8").replace("\r\n", "\n") for name, data in raw.items()}
    test_before = {name: hashlib.sha256((ROOT / "tests" / name).read_bytes()).hexdigest()
                   for name in ("config_mutation_regression.cpp", "config_mutation_regression.py")}
    types, production, checked = generate(source)
    environment = os.environ.copy()
    compiler = [args.cxx] if Path(args.cxx).is_file() else shlex.split(args.cxx, posix=os.name != "nt")
    if Path(compiler[0]).is_file():
        environment["PATH"] = str(Path(compiler[0]).resolve().parent) + os.pathsep + environment.get("PATH", "")
    with tempfile.TemporaryDirectory(prefix="mister-config-mutation-") as temporary:
        work = Path(temporary)
        (work / "config_mutation_types.inc").write_text(types, encoding="utf-8")
        (work / "config_mutation_production.inc").write_text(production, encoding="utf-8")
        executable = work / ("config_mutation.exe" if os.name == "nt" else "config_mutation")
        command = compiler + ["-std=gnu++14", "-O2", "-Wall", "-Wextra", "-Werror", "-D_GNU_SOURCE", "-D_LARGEFILE64_SOURCE",
                              "-I", str(work), str(ROOT / "tests/config_mutation_regression.cpp"), "-o", str(executable)]
        build = subprocess.run(command, capture_output=True, text=True, env=environment)
        if build.returncode:
            raise RuntimeError("Compilation is not behavioral evidence:\n" + build.stdout + build.stderr)
        if build.stderr:
            print(build.stderr, file=sys.stderr, end="")
        runs = []
        for scenario in SCENARIOS:
            result = subprocess.run([str(executable), scenario], capture_output=True, text=True, env=environment, timeout=15)
            rows = [json.loads(line[5:]) for line in result.stdout.splitlines() if line.startswith("JSON ")]
            if (len(rows) != 1 or rows[0]["case"] != scenario or rows[0]["checks"] != 1 or
                    rows[0]["failures"] not in (0, 1) or result.returncode != rows[0]["failures"] or
                    rows[0]["excluded"] != (not checked and scenario in PRIOR_EXCLUSIONS)):
                raise RuntimeError("Incomplete scenario is not behavioral evidence: %s\n%s\n%s" % (scenario, result.stdout, result.stderr))
            if scenario.startswith("dip-internal-") and (rows[0]["trapped"] or not rows[0]["dip_identity_completed"]):
                raise RuntimeError("Incomplete DIP identity control is not behavioral evidence: " + scenario)
            runs.append({"scenario": scenario, "returncode": result.returncode, "summary": rows[0], "stdout": result.stdout, "stderr": result.stderr})
            print("%s: %s" % ("EXCLUDED" if rows[0]["excluded"] else "FAIL" if result.returncode else "PASS", scenario))
        names = [run["scenario"] for run in runs]
        if len(names) != len(SCENARIOS) or len(set(names)) != len(names) or set(names) != set(SCENARIOS):
            raise RuntimeError("Incomplete scenario coverage")
        test_after = {name: hashlib.sha256((ROOT / "tests" / name).read_bytes()).hexdigest() for name in test_before}
        if test_after != test_before:
            raise RuntimeError("Fixture changed during run")
        raw_after = {name: (ROOT / name).read_bytes() for name in SOURCES} if not args.source_ref else raw
        if raw_after != raw:
            raise RuntimeError("Production source changed during run")
        failures = sum(run["summary"]["failures"] for run in runs)
        exclusions = sum(run["summary"]["excluded"] for run in runs)
        report = {"source_ref": args.source_ref or "working tree", "checked_candidate": checked,
                  "source_hash_basis": "UTF-8 source text with CRLF normalized to LF; test hashes use raw file bytes",
                  "source_sha256": {name: hashlib.sha256(text.encode()).hexdigest() for name, text in source.items()},
                  "source_raw_sha256_before": {name: hashlib.sha256(data).hexdigest() for name, data in raw.items()},
                  "source_raw_sha256_after": {name: hashlib.sha256(data).hexdigest() for name, data in raw_after.items()},
                  "test_sha256": test_after, "test_raw_sha256_before": test_before, "test_raw_sha256_after": test_after,
                  "extracted_sha256": {"types": hashlib.sha256(types.encode()).hexdigest(), "production": hashlib.sha256(production.encode()).hexdigest()},
                  "compile_command": command, "compile_stdout": build.stdout, "compile_stderr": build.stderr, "coverage_complete": True,
                  "summary": {"scenarios": len(runs), "checks": len(runs), "failures": failures, "excluded": exclusions,
                              "prevented_boundaries": sum(run["summary"]["trapped"] for run in runs), "real_filesystem_operations": 0},
                  "runs": runs, "hardware_tested": False,
                  "limits": "Intact production helpers/mutation/config/DIP bodies, FileLoad/FileCreatePath, FileIsZipped and prepared/legacy directory queries. New directory bridge executes when present; existing directory positive controls use actual legacy FileCreatePath on prior refs. FileOpen/read/close, ZIP cache/metadata, get_stmode and mutation syscalls are fixtures. Exact safe MRA prefix only; no NVRAM/XML/FPGA work. Synthetic large names/roots, aliases and same-thread overwrite prove API contracts, not an observed hardware race or real disk durability. Historical guarded traps end their scenario with no fabricated return value."}
        if args.report:
            args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
        print("Config mutation regression: %d scenarios, %d checks, %d failures, %d excluded." % (len(runs), len(runs), failures, exclusions))
        return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())
