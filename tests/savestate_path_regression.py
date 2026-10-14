#!/usr/bin/env python3
"""Exercise checked savestate generation and actual binding/polling production.

Memory maps and filesystem endpoints are spies. No hardware or user files are
used. Historical unsafe libc writes are intercepted before execution; a trapped
binding ends its scenario, never continuing after hypothetical undefined behavior.
Synthetic long names/roots validate API bounds rather than local-file reachability.
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
SCENARIOS = ("builder", "valid", "legacy", "no-extension", "dotted-parent", "empty-reject",
             "long-reject", "allocation-reject", "root-reject", "null-root", "disable", "no-base",
             "recovery", "root-change", "timer", "size", "counter", "map-fail", "open-fail",
             "mkdir-fail", "arcade", "apple-fallback", "apple-priority", "apple-dotless")


def definition(source, marker, filename):
    start = source.index(marker)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    if source[end:end + 1] == ";":
        end += 1
    return f'#line {source.count(chr(10), 0, start) + 1} "{filename}"\n' + source[start:end] + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--source-ref")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    def read(name):
        if args.source_ref:
            return subprocess.check_output(["git", "-C", str(ROOT), "show", f"{args.source_ref}:{name}"], text=True, encoding="utf-8")
        return (ROOT / name).read_text(encoding="utf-8")
    names = ("file_io.cpp", "file_io.h", "user_io.cpp", "user_io.h")
    source = {name: read(name) for name in names}
    io, header = source["file_io.cpp"], source["file_io.h"]
    checked = "int FileGenerateSavestatePath(" in io
    resolver = "int FileResolveFullPath(" in io
    types = f"#define HAVE_CHECKED_SAVESTATE {int(checked)}\n#define HAVE_RESOLVER {int(resolver)}\n"
    types += "struct fileZipArchive;\n" + definition(header, "struct fileTYPE\n", "file_io.h")
    types += re.search(r"^#define SAVESTATE_DIR[^\n]+", header, re.M).group() + "\n"
    for name in ("FileGenerateSavestatePath", "FileClose", "FileOpen", "FileReadAdv", "FileExists", "FileSave"):
        types += re.search(r"^(?:int|void)\s+" + name + r"\([^\n]+", header, re.M).group() + "\n"
    if resolver:
        types += re.search(r"^int FileResolveFullPath\([^\n]+", header, re.M).group() + "\n"
    types += re.search(r"^int process_ss\([^\n]+", source["user_io.h"], re.M).group() + "\n"
    paths = ""
    if resolver:
        paths += "#define FileResolveFullPath production_resolve\n" + definition(io, "int FileResolveFullPath(", "file_io.cpp") + "#undef FileResolveFullPath\n"
    if not checked:
        paths += "#define sprintf bounded_sprintf\n#define strcat bounded_strcat\n"
    paths += "#define FileGenerateSavestatePath production_generate\n"
    paths += definition(io, ("int" if checked else "void") + " FileGenerateSavestatePath(", "file_io.cpp")
    paths += "#undef FileGenerateSavestatePath\n"
    if not checked:
        paths += "#undef strcat\n#undef sprintf\n"
    constructors = "".join(definition(io, marker, "file_io.cpp") for marker in ("fileTYPE::fileTYPE()", "fileTYPE::~fileTYPE()", "int fileTYPE::opened()"))
    binding = "#define mkdir fixture_mkdir\n#define memset trace_memset\n" + definition(source["user_io.cpp"], "int process_ss(", "user_io.cpp") + "#undef memset\n#undef mkdir\n"
    command = [args.cxx] if Path(args.cxx).is_file() else shlex.split(args.cxx, posix=os.name != "nt")
    environment = os.environ.copy()
    if Path(command[0]).is_file():
        environment["PATH"] = str(Path(command[0]).resolve().parent) + os.pathsep + environment.get("PATH", "")
    with tempfile.TemporaryDirectory(prefix="mister-savestate-path-") as temporary:
        work = Path(temporary)
        for name, content in (("savestate_types.inc", types), ("savestate_paths.inc", paths),
                              ("savestate_constructors.inc", constructors), ("savestate_binding.inc", binding)):
            (work / name).write_text(content, encoding="utf-8")
        executable = work / ("savestate.exe" if os.name == "nt" else "savestate")
        subprocess.run(command + ["-std=gnu++14", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
            "-D_GNU_SOURCE", "-D_LARGEFILE64_SOURCE", "-I", str(work), str(ROOT / "tests/savestate_path_regression.cpp"),
            "-o", str(executable)], check=True, env=environment)
        runs = []
        for scenario in SCENARIOS:
            result = subprocess.run([str(executable), scenario], capture_output=True, text=True, env=environment, timeout=15)
            rows = [json.loads(line[5:]) for line in result.stdout.splitlines() if line.startswith("JSON ")]
            if len(rows) != 1 or result.returncode != (1 if rows[0]["failures"] else 0):
                raise RuntimeError(f"Incomplete savestate scenario {scenario}: {result.returncode} {result.stderr}")
            runs.append({"scenario": scenario, "returncode": result.returncode, "summary": rows[0], "stdout": result.stdout, "stderr": result.stderr})
            outcome = "SKIP" if rows[0]["excluded"] else "FAIL" if result.returncode else "PASS"
            print(f"{outcome}: {scenario} ({rows[0]['checks']} checks)")
        if args.output:
            args.output.write_text(json.dumps({"source_ref": args.source_ref, "scope": __doc__,
                "source_sha256": {name: hashlib.sha256(text.encode()).hexdigest() for name, text in source.items()},
                "runs": runs}, indent=2) + "\n", encoding="utf-8")
        return 1 if any(run["returncode"] for run in runs) else 0


if __name__ == "__main__":
    sys.exit(main())
