#!/usr/bin/env python3
"""Compile selected, unchanged production definitions and bundled miniz on a host.

Run on Linux/MSYS2: python3 tests/file_browser_tests.py [--cc gcc --cxx g++]
Use --source-ref <commit> to reproduce failures against a Git baseline.
The full MiSTer binary needs Linux ARM device interfaces. These tests exercise
the production comparator and ZIP directory lookup with real archives, without
duplicating their logic or mocking the archive library. All generated sources,
objects, binaries, and fixtures live in a temporary directory.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def definition(source: str, marker: str, filename: str) -> str:
    """Extract one complete definition, keeping its original text and line refs."""
    start = source.index(marker)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        # Definitions selected below have no unmatched braces in strings/comments.
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    if source[end:end + 1] == ";":
        end += 1
    line = source.count("\n", 0, start) + 1
    return f'#line {line} "{filename}"\n' + source[start:end] + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--source-ref", help="extract definitions from this Git ref instead of the working tree")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    def read_source(name: str) -> str:
        if args.source_ref:
            return subprocess.check_output(
                ["git", "-C", str(repo), "show", f"{args.source_ref}:{name}"],
                text=True, encoding="utf-8")
        return (repo / name).read_text(encoding="utf-8")

    cpp = read_source("file_io.cpp")
    header = read_source("file_io.h")
    generated = definition(header, "struct direntext_t\n", "file_io.h")
    generated += "\n".join(re.findall(
        r"^static [^\n]*\b(?:last_zip_archive|last_zip_fd|last_zip_cfile|last_zip_fname|full_path)\b[^\n]*$",
        cpp, re.MULTILINE)) + "\n"
    for marker in (
        "static int OpenZipfileCached(",
        "static int FileIsZipped(",
        "static char* make_fullpath(",
        "static int get_stmode(",
        "static int isPathDirectory(",
        "struct DirentComp\n",
    ):
        generated += definition(cpp, marker, "file_io.cpp")

    with tempfile.TemporaryDirectory(prefix="mister-file-tests-") as temp:
        work = Path(temp)
        (work / "file_browser_production.inc").write_text(generated, encoding="utf-8")
        common = ["-D_GNU_SOURCE", "-D_LARGEFILE64_SOURCE", "-I", str(repo / "lib/miniz")]
        target = subprocess.check_output([args.cc, "-dumpmachine"], text=True).strip()
        if "cygwin" in target:
            # MSYS2/Cygwin uses 64-bit offsets in its unsuffixed POSIX APIs.
            common += [f"-D{name}64={name}" for name in
                       ("stat", "fopen", "freopen", "ftello", "fseeko")]
        subprocess.run([args.cc, "-std=gnu99", "-O2", *common, "-c",
                        str(repo / "lib/miniz/miniz.c"), "-o", str(work / "miniz.o")], check=True)
        executable = work / ("file_browser_tests.exe" if os.name == "nt" else "file_browser_tests")
        subprocess.run([args.cxx, "-std=gnu++14", "-O2", "-Wall", "-Wextra", *common,
                        "-I", str(work), str(repo / "tests/file_browser_tests.cpp"),
                        str(work / "miniz.o"), "-o", str(executable)], check=True)
        subprocess.run([str(executable), work.as_posix()], check=True)


if __name__ == "__main__":
    main()
