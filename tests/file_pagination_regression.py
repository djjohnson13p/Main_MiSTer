#!/usr/bin/env python3
"""Test production file-browser paging and rendering without FPGA hardware.

Run: python3 tests/file_pagination_regression.py [--cxx g++]
Use --source FILE for a historical file_io.cpp, or --source-ref COMMIT for
historical production files from local Git history. --only-original isolates
the 60-long-filename reproduction of upstream issue 1251 (already fixed by
upstream PR 1256). No production files or build artifacts are changed.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def definition(source, marker, filename):
    """Copy an intact definition and retain production line references."""
    start = source.index(marker)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        # Selected definitions contain no unmatched braces in strings/comments.
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    if source[end:end + 1] == ";":
        end += 1
    line = source.count("\n", 0, start) + 1
    return f'#line {line} "{filename}"\n' + source[start:end] + "\n"


def generate(cpp, header, menu):
    generated = definition(header, "struct direntext_t\n", "file_io.h")
    generated += "\n".join(re.findall(r"^#define SCAN[FO]_[^\n]+", header, re.MULTILINE)) + "\n"
    generated += "static std::vector<direntext_t> DirItem;\nstatic int iSelectedEntry = 0;\nstatic int iFirstEntry = 0;\n"
    for marker in (
        "static int flist_last_first_entry(",
        "static int flist_entry_is_expanded(",
        "static int flist_page_step(",
        "static void flist_center_selected(",
        "int flist_nDirEntries(",
        "int flist_iFirstEntry(",
        "void flist_iFirstEntryInc(",
        "int flist_iSelectedEntry(",
        "direntext_t* flist_DirItem(",
        "direntext_t* flist_SelectedItem(",
        "static bool flist_core_display_initial_matches(",
    ):
        # The pre-1256 baseline has no expansion-aware helpers.
        if marker in cpp:
            generated += definition(cpp, marker, "file_io.cpp")

    # Compile ScanDirectory's entire non-init navigation branch unchanged.
    # INIT's filesystem and core dependencies are replaced only by a fixture
    # list; the actual paging, centering and rendering logic remains intact.
    scan = cpp.index("int ScanDirectory(")
    begin = cpp.index("\n\telse\n\t{\n\t\tif (flist_nDirEntries() == 0)", scan)
    opening = cpp.index("{", begin)
    depth, end = 1, opening + 1
    while depth:
        depth += (cpp[end] == "{") - (cpp[end] == "}")
        end += 1
    line = cpp.count("\n", 0, opening) + 1
    generated += "int scan_navigation(int mode, const char *extension, int options)\n"
    generated += f'#line {line} "file_io.cpp"\n'
    generated += cpp[opening:end - 1] + "\n\treturn 0;\n}\n"
    generated += definition(menu, "void PrintDirectory(int expand)", "menu.cpp")
    return generated


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    historical = parser.add_mutually_exclusive_group()
    historical.add_argument("--source", type=Path, help="alternate file_io.cpp; use current header and renderer")
    historical.add_argument("--source-ref", help="load production definitions from this local Git ref")
    cases = parser.add_mutually_exclusive_group()
    cases.add_argument("--only-original", action="store_true")
    cases.add_argument("--only-destination", action="store_true", help="isolate short-to-expanded destination paging")
    args = parser.parse_args()

    def read_source(name):
        if args.source_ref:
            return subprocess.check_output(["git", "-C", str(ROOT), "show", f"{args.source_ref}:{name}"],
                                           text=True, encoding="utf-8")
        path = args.source if name == "file_io.cpp" and args.source else ROOT / name
        return path.read_text(encoding="utf-8")

    production = generate(read_source("file_io.cpp"), read_source("file_io.h"), read_source("menu.cpp"))
    command = [args.cxx] if Path(args.cxx).is_file() else shlex.split(args.cxx, posix=os.name != "nt")
    environment = os.environ.copy()
    if Path(command[0]).is_file():
        environment["PATH"] = str(Path(command[0]).resolve().parent) + os.pathsep + environment.get("PATH", "")

    with tempfile.TemporaryDirectory(prefix="mister-pagination-") as directory:
        directory = Path(directory)
        (directory / "file_pagination_production.inc").write_text(production, encoding="utf-8")
        executable = directory / ("pagination.exe" if os.name == "nt" else "pagination")
        subprocess.run(command + ["-std=gnu++14", "-O2", "-Wall", "-Wextra", "-Werror",
                                   "-I", str(ROOT), "-I", str(directory),
                                   str(ROOT / "tests/file_pagination_regression.cpp"), "-o", str(executable)],
                       check=True, env=environment)
        selected_case = ["--only-original"] if args.only_original else ["--only-destination"] if args.only_destination else []
        result = subprocess.run([str(executable)] + selected_case,
                                text=True, capture_output=True, env=environment)
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
