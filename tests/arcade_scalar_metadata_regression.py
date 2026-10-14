#!/usr/bin/env python3
"""Exercise actual scalar MRA/file-SAX helpers above bounded lower spies."""
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
SOURCES = ("support/arcade/mra_loader.cpp", "support/arcade/mra_loader.h",
           "user_io.cpp", "user_io.h", "sxmlc.c", "sxmlc.h")
SCENARIOS = (
    "rbf-ordinary", "rbf-nonarcade", "rbf-comment", "rbf-pi", "rbf-cdata",
    "rbf-special-names", "rbf-cdata-only", "rbf-repeat", "rbf-empty", "rbf-missing",
    "rbf-fit1023", "rbf-overflow1024", "rbf-seam-fit", "rbf-seam-reject",
    "rbf-utf8", "rbf-entity", "rbf-nested", "rbf-nested-rbf", "rbf-wrong",
    "rbf-unmatched", "rbf-unclosed", "rbf-prior-invalid", "rbf-error", "rbf-open-recovery",
    "name-ordinary", "name-joined", "name-31-32", "name-limit", "name-saturation",
    "name-utf8", "name-entity", "name-self-repeat", "name-samedir", "name-nested-latch",
    "name-wrong", "name-unclosed", "name-malformed-depth", "name-error", "name-recovery",
    "name-open-nocallback", "name-early-stop", "name-reverse",
    "rotation-tokens", "rotation-case-priority", "rotation-prefix-seams", "rotation-long-middle",
    "rotation-long-late", "rotation-whitespace", "rotation-repeat", "rotation-nested",
    "rotation-recovery", "rotation-cdata-only", "private-bounds",
)
PARSE_COUNTS = {"rbf-prior-invalid": 3, "rbf-open-recovery": 3, "rbf-nested": 2,
                "name-31-32": 2, "name-limit": 2, "name-self-repeat": 4,
                "name-open-nocallback": 4, "rotation-tokens": 4,
                "rotation-case-priority": 8, "rotation-prefix-seams": 4,
                "rotation-whitespace": 4, "rotation-recovery": 3, "private-bounds": 0}

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
    raise ValueError("Unclosed actual source definition: " + marker)

def generate(source):
    arcade, user = source[SOURCES[0]], source["user_io.cpp"]
    has_contexts = "struct arcade_rbf_scan {" in arcade
    types = "#define HAVE_SCALAR_CONTEXTS %d\n" % has_contexts
    types += next(line for line in arcade.splitlines() if line.startswith("#define kBigTextSize")) + "\n"
    for marker, text in (("static char arcade_setname", arcade), ("static bool is_vertical", arcade),
                         ("static int rotation_dir", arcade), ("static char ovr_name", user),
                         ("static int  ovr_samedir", user)):
        types += next(line for line in text.splitlines() if line.startswith(marker)) + "\n"
    helpers = ""
    if has_contexts:
        for marker in ("enum arcade_scalar_kind {", "struct arcade_scalar_owner {",
                       "struct arcade_rotation_scan {", "struct arcade_rbf_scan {", "struct arcade_preparse_scan {"):
            types += definition(arcade, marker, SOURCES[0])
        for marker in ("static arcade_scalar_kind arcade_scalar_type(", "static bool arcade_scalar_start(",
                       "static bool arcade_scalar_close(", "static bool arcade_scalar_append(",
                       "static void arcade_rotation_append("):
            helpers += definition(arcade, marker, SOURCES[0])
    production = helpers + definition(user, "void user_io_name_override(", "user_io.cpp")
    production += "static void observed_name_override(const char *name,int same) { void (*volatile call)(const char *,int)=user_io_name_override; call(name,same); observe_override(name,same); }\n"
    if not has_contexts:
        production += "#define strncpy guarded_strncpy\n"
    production += definition(arcade, "static int xml_scan_rbf(", SOURCES[0])
    production += "#undef strncpy\n#define user_io_name_override observed_name_override\n"
    production += definition(arcade, "static int xml_read_pre_parse(", SOURCES[0])
    production += "#undef user_io_name_override\n#define XMLDoc_parse_file_SAX observed_parse_file\n"
    for marker in ("void arcade_pre_parse(", "bool arcade_is_vertical(", "int arcade_get_direction(",
                   "static const char *get_rbf_path("):
        production += definition(arcade, marker, SOURCES[0])
    production += "static const char *observed_rbf_path(const char *name) { ++active->path_calls; const char *(*volatile call)(const char *)=get_rbf_path; return call(name); }\n#define get_rbf_path observed_rbf_path\n"
    production += "#define opendir fixture_opendir\n#define readdir fixture_readdir\n#define closedir fixture_closedir\n"
    production += definition(arcade, "static const char *get_rbf(", SOURCES[0])
    production += "#undef closedir\n#undef readdir\n#undef opendir\n#undef get_rbf_path\n#undef XMLDoc_parse_file_SAX\n"
    parser = source["sxmlc.c"]
    point = '#include "sxmlc.h"'
    if parser.count(point) != 1:
        raise ValueError("Ambiguous parser fopen adapter point")
    parser = parser.replace(point, point + '\nextern FILE *scalar_fixture_fopen(const SXML_CHAR *,const SXML_CHAR *);\n#undef sx_fopen\n#define sx_fopen scalar_fixture_fopen\n', 1)
    return {"arcade_scalar_types.inc": types, "arcade_scalar_production.inc": production,
            "arcade_scalar_sxmlc.c": parser}, has_contexts

def command(value):
    return [value] if Path(value).is_file() else shlex.split(value, posix=os.name != "nt")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--cc", default=os.environ.get("CC"))
    parser.add_argument("--source-ref")
    parser.add_argument("--report", "--output", dest="report", type=Path)
    args = parser.parse_args()
    raw = {name: subprocess.check_output(["git", "-C", str(ROOT), "show", args.source_ref+":"+name])
           if args.source_ref else (ROOT/name).read_bytes() for name in SOURCES}
    source = {name: data.decode("utf-8").replace("\r\n", "\n") for name, data in raw.items()}
    generated, candidate = generate(source)
    pair = {name: (ROOT/"tests"/name).read_bytes() for name in
            ("arcade_scalar_metadata_regression.py", "arcade_scalar_metadata_regression.cpp")}
    cxx = command(args.cxx)
    compiler = Path(cxx[0])
    cc = command(args.cc or str(compiler.with_name(compiler.name.replace("g++", "gcc"))))
    env = os.environ.copy()
    if compiler.is_file():
        env["PATH"] = str(compiler.resolve().parent) + os.pathsep + env.get("PATH", "")
    builds, runs = [], []
    try:
        with tempfile.TemporaryDirectory(prefix="mister-arcade-scalar-metadata-") as temporary:
            work = Path(temporary)
            (work/"sxmlc.h").write_text(source["sxmlc.h"], encoding="utf-8", newline="\n")
            for name, text in generated.items():
                (work/name).write_text(text, encoding="utf-8", newline="\n")
            executable = work/("probe.exe" if os.name == "nt" else "probe")
            common = ["-O2", "-Wall", "-Wextra", "-Werror", "-I", str(work)]
            commands = [cc+["-std=gnu99"]+common+["-Wno-error=format-security", "-c", str(work/"arcade_scalar_sxmlc.c"), "-o", str(work/"parser.o")],
                        cxx+["-std=gnu++14", "-D_GNU_SOURCE"]+common+[str(ROOT/"tests/arcade_scalar_metadata_regression.cpp"), str(work/"parser.o"), "-o", str(executable)]]
            for invocation in commands:
                result = subprocess.run(invocation, capture_output=True, text=True, env=env, timeout=60)
                builds.append({"command": invocation, "returncode": result.returncode, "stdout": result.stdout, "stderr": result.stderr})
                if result.returncode:
                    raise RuntimeError("Compilation is fixture error, not behavioral evidence: " + result.stdout + result.stderr)
                if result.stderr:
                    print(result.stderr, file=sys.stderr, end="")
            for scenario in SCENARIOS:
                result = subprocess.run([str(executable), scenario], capture_output=True, text=True, env=env, timeout=15)
                rows = [json.loads(line[5:]) for line in result.stdout.splitlines() if line.startswith("JSON ")]
                if len(rows) != 1 or rows[0]["case"] != scenario or not rows[0]["case_body_completed"]:
                    raise RuntimeError("Incomplete/crashed/timed-out scenario is not proof: " + scenario + "\n" + result.stdout + result.stderr)
                row = rows[0]
                allowed_guard = not candidate and scenario == "rbf-overflow1024"
                allowed_private_exclusion = not candidate and scenario == "private-bounds"
                if row["trapped"] != allowed_guard or row["excluded"] != (allowed_guard or allowed_private_exclusion):
                    raise RuntimeError("Unexpected or missing ended guard/exclusion: " + scenario)
                if row["checks"] < (0 if allowed_private_exclusion else 1) or not 0 <= row["failures"] <= row["checks"] or result.returncode != (1 if row["failures"] else 0):
                    raise RuntimeError("Invalid complete scenario/check status: " + scenario)
                if len(row["failure_labels"]) != row["failures"] or len(row["parses"]) != PARSE_COUNTS.get(scenario, 1):
                    raise RuntimeError("Complete assertion/parser count parity missing: " + scenario)
                if not row["excluded"] and (not row["parses"] and scenario != "private-bounds" or any(not p["helper_completed"] or not p["parser_return_observed"] for p in row["parses"])):
                    raise RuntimeError("Actual completed helper/parser coverage missing: " + scenario)
                if allowed_guard and (len(row["parses"]) != 1 or row["parses"][0]["helper_completed"] or row["parses"][0]["parser_return_observed"] or row["parses"][0]["directory_events"]):
                    raise RuntimeError("Unsafe historical control continued past its boundary")
                if any(p["result_observed"] != (p["helper_completed"] and p["api"] == "get_rbf") for p in row["parses"]):
                    raise RuntimeError("Returned pointer observation label is inconsistent")
                runs.append({"scenario": scenario, "returncode": result.returncode, "summary": row, "stdout": result.stdout, "stderr": result.stderr})
                print(("EXCLUDED" if row["excluded"] else "FAIL" if row["failures"] else "PASS") + ": " + scenario)
    except Exception as error:
        if args.report:
            args.report.write_text(json.dumps({"fixture_error": str(error), "builds": builds, "completed_runs": runs}, indent=2)+"\n", encoding="utf-8", newline="\n")
        raise
    if [r["scenario"] for r in runs] != list(SCENARIOS) or len({r["scenario"] for r in runs}) != len(SCENARIOS):
        raise RuntimeError("Exact scenario coverage is incomplete or duplicated")
    if not args.source_ref and {n: digest((ROOT/n).read_bytes()) for n in SOURCES} != {n: digest(b) for n, b in raw.items()}:
        raise RuntimeError("Selected source changed during fixture")
    if {n: digest((ROOT/"tests"/n).read_bytes()) for n in pair} != {n: digest(b) for n, b in pair.items()}:
        raise RuntimeError("Fixture pair changed during run")
    summary = {"cases": len(runs), "checks": sum(r["summary"]["checks"] for r in runs),
               "failures": sum(r["summary"]["failures"] for r in runs), "failed_cases": sum(bool(r["summary"]["failures"]) for r in runs),
               "parses": sum(len(r["summary"]["parses"]) for r in runs), "excluded": sum(r["summary"]["excluded"] for r in runs),
               "prevented_boundaries": sum(r["summary"]["trapped"] for r in runs),
               "parser_returns_observed": sum(p["parser_return_observed"] for r in runs for p in r["summary"]["parses"]),
               "helper_returns_observed": sum(p["helper_completed"] for r in runs for p in r["summary"]["parses"]), "user_target_io": 0, "hardware_io": 0}
    report = {"source_ref": args.source_ref or "working tree", "candidate_private_contexts": candidate,
        "source_raw_sha256": {n: digest(b) for n, b in raw.items()}, "source_normalized_sha256": {n: digest(t.encode()) for n, t in source.items()},
        "test_sha256": {n: digest(b) for n, b in pair.items()}, "extracted_sha256": {n: digest(t.encode()) for n, t in generated.items()},
        "scenario_order": list(SCENARIOS), "coverage_complete": True, "builds": builds, "runs": runs, "summary": summary,
        "scope": "Full actual file-SAX, scalar callbacks/private contexts/helpers, get_rbf/path/directory matcher, preparse/accessors and actual user_io_name_override/globals. Forwarded events/statuses unchanged. Lower fopen uses disposable seekable streams; root providers and DIR enumeration are finite metadata spies. Byte strings escape every non-ASCII byte; lengths/hex retain delivered bytes.",
        "limits": "Finite synthetic XML and primitive private-helper controls. No corpus prevalence, general XML validation, FPGA/main restart/core hardware, INI/video/game-ID/menu/config consumer or performance execution. C-only inherited serializer format-security diagnostics remain visible; C++ strict -Werror. Historical >=1024 RBF strncpy ends before copy/consumer as missing-NUL risk, with no inferred helper/parser/query result; absent private API excluded. Compile/crash/timeout/incomplete output cannot prove a defect."}
    if args.report:
        args.report.write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8", newline="\n")
    print(json.dumps(summary))
    return 1 if summary["failures"] else 0

if __name__ == "__main__":
    raise SystemExit(main())
