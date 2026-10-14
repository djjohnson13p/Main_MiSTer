#!/usr/bin/env python3
"""Exercise production file opens, names, ZIP lookup and close with bounded spies.

Historical unsafe strcpy/sprintf calls are stopped before writing. Terminal ZIP
splitting uses 2101 physical bytes for a 2100-byte conceptual canary boundary.
Only shared-memory APIs are peripheral spies; file/ZIP/miniz operations are real.
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
import zipfile

ROOT = Path(__file__).resolve().parents[1]


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


def generate(cpp, header):
    types = "struct fileZipArchive;\n" + definition(header, "struct fileTYPE\n", "file_io.h")
    types += definition(cpp, "struct fileZipArchive\n", "file_io.cpp")
    types += re.search(r"^static char full_path\[[^\n]+", cpp, re.MULTILINE).group() + "\n"
    types += re.search(r"^#define MIN[^\n]+", cpp, re.MULTILINE).group() + "\n"
    for name in ("FileOpenEx", "FileOpen", "FileOpenZip", "FileReadAdv", "FileWriteAdv"):
        types += re.search(r"^int\s+" + name + r"\([^\n]+", header, re.MULTILINE).group() + "\n"
    types += "int FileClose(fileTYPE *);\n"
    production = ""
    markers = ["fileTYPE::fileTYPE()", "fileTYPE::~fileTYPE()", "int fileTYPE::opened()",
               "static int FileIsZipped(", "static char* make_fullpath("]
    if "static int make_fullpath_checked(" in cpp:
        markers.append("static int make_fullpath_checked(")
    markers += ["int FileClose(", "static int zip_search_by_crc(", "int FileOpenZip(",
                "int FileOpenEx(", "int FileOpen(", "__off64_t FileGetSize(",
                "int FileSeek(", "int FileReadAdv(", "int FileWriteAdv("]
    for marker in markers:
        part = definition(cpp, marker, "file_io.cpp")
        if marker == "int FileSeek(":
            # The ARM off64_t printf spelling differs from some host ABIs.
            part = '#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored "-Wformat"\n' + part + '#pragma GCC diagnostic pop\n'
        production += part
    return types, production


def nested_name(length, leaf="Inner.gba"):
    remaining = length - len(leaf)
    if remaining < 2:
        raise ValueError("nested fixture is too short")
    if remaining % 2:
        return "ab/" + "d/" * ((remaining - 3) // 2) + leaf
    return "d/" * (remaining // 2) + leaf


def fixtures(root, posix_root):
    (root / "Small.gba").write_bytes(b"ORDINARY_PAYLOAD")
    (root / "Plain").write_bytes(b"NO_EXTENSION")
    (root / "Writable.img").write_bytes(b"WRITABLE_CONTROL")
    (root / "Bad.zip").write_bytes(b"NOT_A_ZIP")
    members = [("a" * (size - 4) + ".gba", f"BASENAME_{size}".encode())
               for size in (255, 260, 261, 300)]
    members += [("\u00e9" * 128 + ".gba", b"UTF8_260"), ("\u00e9" * 129 + ".gba", b"UTF8_262"),
                ("Renamed.bin", b"CRC_SELECTED"), ("Fallback.bin", b"NAME_FALLBACK")]
    prefix = posix_root + "/Data.zip/"
    paths = {}
    for size in (2099, 2100, 2120):
        member = nested_name(size - len(prefix))
        members.append((member, f"FULL_PATH_{size}".encode()))
        paths[str(size)] = prefix + member
    # This existing member is exactly what a clipped 2100-byte path names.
    members.append((paths["2100"][len(prefix):-1], b"WRONG_PREFIX_PAYLOAD"))
    with zipfile.ZipFile(root / "Data.zip", "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for member, payload in members:
            archive.writestr(member, payload)
    plain_length = 2099 - len(posix_root) - 1 - len("Guard.img")
    directories = []
    while plain_length > 101:
        directories.append("p" * 100)
        plain_length -= 101
    if plain_length == 1:
        directories[-1] = directories[-1][:-1]
        plain_length += 1
    directories.append("p" * (plain_length - 1))
    plain = root.joinpath(*directories, "Guard.img")
    physical = Path("\\\\?\\" + str(plain.resolve())) if os.name == "nt" else plain
    try:
        physical.parent.mkdir(parents=True)
        physical.write_bytes(b"PREFIX_ORIGINAL")
        paths["plain"] = posix_root + "/" + "/".join(directories) + "/Guard.img"
        assert len(paths["plain"]) == 2099
        paths["plain_limit"] = None
    except OSError as error:
        paths["plain"] = "-"
        paths["plain_limit"] = str(error)
    return paths


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--source-ref")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    def source(name):
        if args.source_ref:
            return subprocess.check_output(["git", "-C", str(ROOT), "show", f"{args.source_ref}:{name}"], text=True, encoding="utf-8")
        return (ROOT / name).read_text(encoding="utf-8")
    names = ("file_io.cpp", "file_io.h", "lib/miniz/miniz.c", "lib/miniz/miniz.h")
    sources = {name: source(name) for name in names}
    cc, cxx = shlex.split(args.cc, posix=os.name != "nt"), shlex.split(args.cxx, posix=os.name != "nt")
    cc = [value.strip('"') for value in cc]
    cxx = [value.strip('"') for value in cxx]
    target = subprocess.check_output(cxx + ["-dumpmachine"], text=True).strip()
    msys = "cygwin" in target
    tool_path = Path(cxx[0]).resolve().parent
    runtime_env = os.environ.copy()
    if msys:
        runtime_env["PATH"] = str(tool_path) + os.pathsep + runtime_env.get("PATH", "")
    cygpath = shutil.which("cygpath") or str(tool_path / "cygpath.exe")
    def compiler_path(path):
        return subprocess.check_output([cygpath, "-u", str(path)], text=True).strip() if msys else str(path)
    temporary = tempfile.TemporaryDirectory(prefix="mister-file-open-bounds-")
    with temporary as temp:
        # Windows creation and cleanup must both address the long fixture.
        if os.name == "nt":
            temporary.name = "\\\\?\\" + str(Path(temp).resolve())
        work = Path(temp)
        fixture_dir = work / "fixtures"
        fixture_dir.mkdir()
        root = compiler_path(fixture_dir)
        paths = fixtures(fixture_dir, root)
        types, production = generate(sources["file_io.cpp"], sources["file_io.h"])
        (work / "file_open_types.inc").write_text(types, encoding="utf-8")
        (work / "file_open_production.inc").write_text(production, encoding="utf-8")
        for name in ("miniz.c", "miniz.h"):
            (work / name).write_text(sources["lib/miniz/" + name], encoding="utf-8")
        common = ["-D_GNU_SOURCE", "-D_LARGEFILE64_SOURCE", "-I", compiler_path(work)]
        if msys:
            common += [f"-D{name}64={name}" for name in ("stat", "fstat", "fseeko", "ftello", "fopen", "freopen", "open", "lseek")]
        obj, exe = work / "miniz.o", work / "file-open-bounds.exe"
        subprocess.run(cc + ["-std=gnu99", "-O2"] + common + ["-c", compiler_path(work / "miniz.c"), "-o", compiler_path(obj)], check=True, env=runtime_env)
        subprocess.run(cxx + ["-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function"] + common +
                       [compiler_path(ROOT / "tests/file_open_bounds_regression.cpp"), compiler_path(obj), "-o", compiler_path(exe)], check=True, env=runtime_env)
        result = subprocess.run([str(exe), root, paths["2099"], paths["2100"], paths["2120"], paths["plain"]], capture_output=True, env=runtime_env)
        stdout = result.stdout.decode("utf-8", errors="backslashreplace")
        stderr = result.stderr.decode("utf-8", errors="backslashreplace")
        summaries = [json.loads(line[5:]) for line in stdout.splitlines() if line.startswith("JSON ")]
        report = {"source_ref": args.source_ref, "source_sha256": {name: hashlib.sha256(value.encode()).hexdigest() for name, value in sources.items()},
                  "compiler_target": target, "plain_fixture_creation_limit": paths["plain_limit"], "returncode": result.returncode, "observations": summaries, "stdout": stdout, "stderr": stderr,
                  "scope": __doc__}
        if args.output:
            args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(stdout, end="")
        print(stderr, end="", file=sys.stderr)
        if len(summaries) != 1:
            raise RuntimeError("File-open bounds fixture did not emit one complete summary")
        if result.returncode != (1 if summaries[0]["failures"] else 0):
            raise RuntimeError("Fixture exit code and summary do not agree")
        return result.returncode


if __name__ == "__main__":
    sys.exit(main())
