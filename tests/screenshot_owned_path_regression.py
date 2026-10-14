#!/usr/bin/env python3
"""Check owned screenshot paths through actual capture, offload and callback code.

Imlib, scaler hardware, directory creation and affinity setup are spies. No
screenshot is written. A controlled same-thread save-boundary interleaving
mutates the historical borrowed buffer without a C++ data race or overflow.
The resolver overflow uses a synthetic long root; it validates the new API,
not a real storage-root failure. Historical source explicitly skips the absent
resolver, its rejection and its extra path allocation rather than failing to compile.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SCENARIOS = ("native", "root-change", "foreign-path", "scaled", "rotated", "bmp", "unknown-format",
             "default-name", "nodate", "image-fail", "scale-fail", "save-fail", "request-alloc-fail",
             "filename-alloc-fail", "savepath-alloc-fail", "result-alloc-fail", "init-fail", "oversize",
             "resolve-fail", "resolver")


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


def generate(sources):
    io, header, scaler = sources["file_io.cpp"], sources["file_io.h"], sources["scaler.cpp"]
    present = "int FileResolveFullPath(" in io
    declarations = f"#define HAVE_OWNED_RESOLVER {int(present)}\n"
    declarations += re.search(r"^static char full_path\[[^\n]+", io, re.M).group() + "\n"
    declarations += "\n".join(re.findall(r"^#define SCREENSHOT_[^\n]+", header, re.M)) + "\n"
    if present:
        declarations += re.search(r"^int FileResolveFullPath\([^\n]+", header, re.M).group() + "\n"
    paths = definition(io, "static char* make_fullpath(", "file_io.cpp")
    paths += "#define getFullPath production_getFullPath\n" + definition(io, "const char *getFullPath(", "file_io.cpp") + "#undef getFullPath\n"
    if present:
        paths += definition(io, "int FileResolveFullPath(", "file_io.cpp")
    paths += "#define time fixture_time\n#define localtime fixture_localtime\n"
    paths += definition(io, "void FileGenerateScreenshotName(", "file_io.cpp")
    paths += "#undef localtime\n#undef time\n"
    start = scaler.index("static uint8_t screenshot_outputbuf[")
    screenshot = f'#line {scaler.count(chr(10), 0, start) + 1} "scaler.cpp"\n' + scaler[start:]
    # This screenshot section is the final section of scaler.cpp.
    assert screenshot.rstrip().endswith("}")
    offload = "\n".join(line for line in sources["offload.cpp"].splitlines() if not line.startswith("#include")) + "\n"
    return declarations, paths, screenshot, offload


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
    names = ("file_io.cpp", "file_io.h", "scaler.cpp", "scaler.h", "offload.cpp", "offload.h", "profiling.h", "cfg.h", "lib/imlib2/Imlib2.h")
    sources = {name: read(name) for name in names}
    copied_headers = ["scaler.h", "offload.h", "profiling.h", "cfg.h"]
    if '#include "controller_axis.h"' in sources["cfg.h"]:
        sources["controller_axis.h"] = read("controller_axis.h")
        copied_headers.append("controller_axis.h")
    declarations, paths, screenshot, offload = generate(sources)
    command = [args.cxx] if Path(args.cxx).is_file() else shlex.split(args.cxx)
    compiler = shutil.which(command[0]) or command[0]
    environment = os.environ.copy()
    environment["PATH"] = str(Path(compiler).resolve().parent) + os.pathsep + environment.get("PATH", "")
    target = subprocess.check_output(command + ["-dumpmachine"], text=True, env=environment).strip()
    converter = shutil.which("cygpath") or str(Path(compiler).with_name("cygpath.exe"))
    def host_path(path):
        return subprocess.check_output([converter, "-u", str(path)], text=True, env=environment).strip() if "cygwin" in target else str(path)
    with tempfile.TemporaryDirectory(prefix="mister-screenshot-path-") as temporary:
        work = Path(temporary)
        for name in copied_headers:
            (work / name).write_text(sources[name], encoding="utf-8")
        (work / "Imlib2.h").write_text(sources["lib/imlib2/Imlib2.h"], encoding="utf-8")
        for name, content in (("screenshot_types.inc", declarations), ("screenshot_paths.inc", paths),
                              ("screenshot_production.inc", screenshot), ("offload_production.inc", offload)):
            (work / name).write_text(content, encoding="utf-8")
        executable = work / "screenshot-path.exe"
        subprocess.run(command + ["-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                                  "-D_GNU_SOURCE", "-pthread", "-I", host_path(work), host_path(ROOT / "tests/screenshot_owned_path_regression.cpp"),
                                  "-o", host_path(executable)], check=True, env=environment)
        runs = []
        for scenario in SCENARIOS:
            result = subprocess.run([str(executable), scenario], capture_output=True, env=environment, timeout=15)
            stdout, stderr = result.stdout.decode("utf-8", errors="backslashreplace"), result.stderr.decode("utf-8", errors="backslashreplace")
            summaries = [json.loads(line[5:]) for line in stdout.splitlines() if line.startswith("JSON ")]
            if len(summaries) != 1 or result.returncode != (1 if summaries[0]["failures"] else 0):
                raise RuntimeError(f"Screenshot fixture {scenario} did not finish with one consistent summary: {stderr}")
            runs.append({"scenario": scenario, "returncode": result.returncode, "summary": summaries[0], "stdout": stdout, "stderr": stderr})
            outcome = "SKIP" if summaries[0]["excluded_absent_or_unsafe_api_case"] else "FAIL" if result.returncode else "PASS"
            print(f"{outcome}: {scenario} ({summaries[0]['checks']} checks)")
        report = {"source_ref": args.source_ref, "source_sha256": {name: hashlib.sha256(text.encode()).hexdigest() for name, text in sources.items()},
                  "compiler_target": target, "scope": __doc__, "runs": runs}
        if args.output:
            args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        return 1 if any(run["returncode"] for run in runs) else 0


if __name__ == "__main__":
    sys.exit(main())
