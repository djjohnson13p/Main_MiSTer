#!/usr/bin/env python3
"""Run intact MGL selected cases and file APIs with real synthetic ZIP fixtures.

Use --source-ref REF to reproduce the missing fallback on local Git history.
Hardware/core output, logging and persistence are spies; the file/ZIP reader,
new resolver and menu dispatch cases execute production code unchanged.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import warnings
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


RESULT_ENUM = """enum MglZipFallbackResult {
MGL_ZIP_UNCHANGED, MGL_ZIP_RESOLVED, MGL_ZIP_NO_CANDIDATE,
MGL_ZIP_UNSUPPORTED, MGL_ZIP_AMBIGUOUS, MGL_ZIP_INVALID_ARCHIVE,
MGL_ZIP_INVALID_PATH, MGL_ZIP_PATH_TOO_LONG };\n"""


def generate(cpp, header, user, menu, mgl_header, user_header=None):
    types = "struct fileZipArchive;\n" + definition(header, "struct fileTYPE\n", "file_io.h")
    types += definition(header, "struct direntext_t\n", "file_io.h")
    types += definition(cpp, "struct fileZipArchive\n", "file_io.cpp")
    cache = [re.search(r"^static[^\n]*\b" + field + r"\b[^\n]*;", cpp, re.MULTILINE)
             for field in ("last_zip_archive", "last_zip_fd", "last_zip_cfile", "last_zip_fname", "full_path")]
    types += "\n".join(item.group() for item in sorted(cache, key=lambda item: item.start())) + "\n"
    types += re.search(r"^int\s+FileOpenEx\([^\n]+", header, re.MULTILINE).group() + "\n"
    types += re.search(r"^int\s+FileOpen\([^\n]+", header, re.MULTILINE).group() + "\n"
    checked_state = "int FileGenerateSavestatePath(" in cpp
    types += f"#define HAVE_CHECKED_SAVESTATE_PATH {int(checked_state)}\n"
    types += re.search(r"^(?:int|void) FileGenerateSavestatePath\([^\n]+", header, re.M).group() + "\n"
    types += "\n".join(re.findall(r"^#define (?:SCANO_|SAVE_DIR|SAVESTATE_DIR)[^\n]+", header, re.MULTILINE)) + "\n"
    types += definition(mgl_header, "struct mgl_item_struct\n", "mra_loader.h")
    types += definition(mgl_header, "struct mgl_struct\n", "mra_loader.h")
    # Historical int-only callers still run unchanged. The current fixture API
    # uses the actual checked-result declaration for its peripheral boundary.
    user_header = user_header or (ROOT / "user_io.h").read_text(encoding="utf-8")
    if "enum class UserIoFileTxResult" not in user_header:
        user_header = (ROOT / "user_io.h").read_text(encoding="utf-8")
    types += definition(user_header, "enum class UserIoFileTxResult", "user_io.h")
    for marker in ("user_io_file_tx_checked", "user_io_file_tx"):
        types += re.search(r"^(?:UserIoFileTxResult|int) " + marker + r"\([^\n]+", user_header, re.M).group() + "\n"
    types += "\n".join(re.findall(r"^#define MGL_ACTION_[^\n]+", mgl_header, re.MULTILINE)) + "\n"
    present = "MglZipFallbackResult FileResolveMglZipFallback(" in cpp
    types += definition(header, "enum MglZipFallbackResult\n", "file_io.h") if present else RESULT_ENUM
    production = f"#define HAVE_MGL_ZIP_HELPER {int(present)}\n"
    if "int FileResolveFullPath(" in cpp:
        production += "#include <new>\n"
        production += definition(cpp, "int FileResolveFullPath(", "file_io.cpp")
    if "static int make_fullpath_checked(" in cpp:
        production += definition(cpp, "static int make_fullpath_checked(", "file_io.cpp")
    for marker in (
        "static int OpenZipfileCached(", "static int FileIsZipped(", "static char* make_fullpath(",
        "static int get_stmode(", "static int isPathDirectory(", "static int isPathRegularFile(",
        "int FileClose(", "fileTYPE::fileTYPE()", "fileTYPE::~fileTYPE()", "int fileTYPE::opened()",
        "int FileOpenEx(", "int FileOpen(", "int FileReadAdv(", "int FileExists(", "int PathIsDir(",
        "int FileCanWrite(", "void create_path(", "void FileGenerateSavePath(",
        ("int " if checked_state else "void ") + "FileGenerateSavestatePath(",
    ):
        if marker == "static int isPathDirectory(" and "static int isPathDirectoryPrepared(" in cpp:
            production += definition(cpp, "static int isPathDirectoryPrepared(", "file_io.cpp")
        if checked_state and marker == "int FileGenerateSavestatePath(":
            production += "#include <new>\n"
        production += definition(cpp, marker, "file_io.cpp")
    # Rename only the resolver's call boundary for an exclusion/order spy.
    if present:
        production += "#define FileResolveMglZipFallback production_FileResolveMglZipFallback\n"
        production += definition(cpp, "MglZipFallbackResult FileResolveMglZipFallback(", "file_io.cpp")
        production += "#undef FileResolveMglZipFallback\n"
    else:
        # Historical menu/file behavior still runs: no helper existed to change
        # a missing literal path, and actual FileOpenEx fails the desired load.
        production += "MglZipFallbackResult production_FileResolveMglZipFallback(char *, size_t) { return MGL_ZIP_UNCHANGED; }\n"
    production += definition(user, "unsigned char user_io_ext_idx(", "user_io.cpp")
    production += definition(user, "void user_io_store_filename(", "user_io.cpp")
    start = menu.index("\n\tcase MENU_GENERIC_FILE_SELECTED:")
    end = menu.index("\n\tcase MENU_COMMON1:", start)
    dispatch = "void selected_case()\n{\n switch (menustate) {\n"
    dispatch += f'#line {menu.count(chr(10), 0, start) + 1} "menu.cpp"\n' + menu[start:end]
    dispatch += "\n default: break;\n }\n}\n"
    return types, production, dispatch


def fixtures(root, posix_root):
    def archive(name, members, method=zipfile.ZIP_DEFLATED):
        with warnings.catch_warnings():
            warnings.filterwarnings("ignore", message="Duplicate name:", category=UserWarning)
            with zipfile.ZipFile(root / name, "w", compression=method) as handle:
                for name, payload in members:
                    handle.writestr(name, payload)
    (root / "Present.gba").write_bytes(b"RAW_ORIGINAL")
    archive("Present.zip", [("Renamed.gba", b"ZIP_ALTERNATIVE")])
    archive("Missing.zip", [("Renamed.gba", b"RENAMED_PAYLOAD")])
    archive("Nested.zip", [("folder/", b""), ("folder/deep/Inner.GBA", b"NESTED_PAYLOAD")])
    archive("Mixed.zip", [("README.txt", b"README"), ("Only.gba", b"MIXED_PAYLOAD")])
    archive("Multi.zip", [("First.gba", b"A"), ("Second.GBA", b"B")])
    archive("Duplicate.zip", [("Same.gba", b"A"), ("Same.gba", b"B")])
    archive("Mismatch.zip", [("Inner.gb", b"WRONG_FORMAT")])
    archive("DirDot.zip", [("folder.gba/ROM", b"NO_EXTENSION")])
    archive("Empty.zip", [])
    archive("Dirs.zip", [("folder.gba/", b"")])
    (root / "Malformed.zip").write_bytes(b"NOT_A_ZIP")
    archive("BadLocal.zip", [("Inner.gba", b"SYNTHETIC_BYTES")])
    data = bytearray((root / "BadLocal.zip").read_bytes())
    # Keep supported member metadata and the central directory intact. Only
    # opening the member's extraction iterator should reject this archive.
    data[:4] = b"BAD!"
    (root / "BadLocal.zip").write_bytes(data)
    archive("Unsupported.zip", [("Inner.gba", b"UNSUPPORTED")], zipfile.ZIP_BZIP2)
    archive("Encrypted.zip", [("Inner.gba", b"SYNTHETIC_ENCRYPTION_FLAG")])
    data = bytearray((root / "Encrypted.zip").read_bytes())
    for sig, offset in ((b"PK\x03\x04", 6), (b"PK\x01\x02", 8)):
        first = data.index(sig) + offset
        data[first:first + 2] = (int.from_bytes(data[first:first + 2], "little") | 1).to_bytes(2, "little")
    (root / "Encrypted.zip").write_bytes(data)
    archive("SupportedAndUnsupported.zip", [("Supported.gba", b"SUPPORTED_PAYLOAD"),
                                             ("Unsupported.gba", b"UNSUPPORTED")])
    data = bytearray((root / "SupportedAndUnsupported.zip").read_bytes())
    offset = 0
    for sig, method_offset in ((b"PK\x03\x04", 8), (b"PK\x01\x02", 10)):
        offset = data.index(sig)
        offset = data.index(sig, offset + 1) + method_offset
        data[offset:offset + 2] = (12).to_bytes(2, "little")
    (root / "SupportedAndUnsupported.zip").write_bytes(data)
    archive("Nul.zip", [("Bad_x.gba", b"SYNTHETIC_BYTES")])
    data = bytearray((root / "Nul.zip").read_bytes())
    for sig, offset in ((b"PK\x03\x04", 30), (b"PK\x01\x02", 46)):
        data[data.index(sig) + offset + 3] = 0
    (root / "Nul.zip").write_bytes(data)
    archive("Sonic.Advance (USA).zip", [("Old.Name.GBA", b"MULTIDOT_PAYLOAD")])
    archive("LongNested.zip", [("folder/" * 80 + "Renamed.gba", b"LONG_NESTED")])
    for archive_name, total in (("Fit.zip", 1023), ("Over.zip", 1024)):
        count = total - len(posix_root + "/" + archive_name + "/") - len("Inner.gba")
        prefix = "a/" * (count // 2) if count % 2 == 0 else "ab/" + "a/" * ((count - 3) // 2)
        archive(archive_name, [(prefix + "Inner.gba", b"BOUNDARY_PAYLOAD")])
    archive("BaseFit.zip", [("a" * 251 + ".gba", b"BASENAME_255")])
    archive("BaseOver.zip", [("a" * 252 + ".gba", b"BASENAME_256")])
    (root / "Directory.gba").mkdir()
    archive("Directory.zip", [("Inner.gba", b"DIRECTORY_ALTERNATIVE")])
    archive("RawDisk.zip", [("Renamed.img", b"ZIP_DISK")])
    (root / "RawDisk.img").write_bytes(b"RAW_DISK")
    archive("MissingDisk.zip", [("Renamed.img", b"DISK_ALTERNATIVE")])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--source-ref")
    args = parser.parse_args()
    def read(name):
        if args.source_ref:
            return subprocess.check_output(["git", "-C", str(ROOT), "show", f"{args.source_ref}:{name}"], text=True, encoding="utf-8")
        return (ROOT / name).read_text(encoding="utf-8")
    types, production, dispatch = generate(read("file_io.cpp"), read("file_io.h"), read("user_io.cpp"),
                                            read("menu.cpp"), read("support/arcade/mra_loader.h"), read("user_io.h"))
    cc = [args.cc] if Path(args.cc).is_file() else shlex.split(args.cc)
    cxx = [args.cxx] if Path(args.cxx).is_file() else shlex.split(args.cxx)
    environment = os.environ.copy()
    if Path(cc[0]).is_file():
        environment["PATH"] = str(Path(cc[0]).parent) + os.pathsep + environment.get("PATH", "")
    target = subprocess.check_output(cc + ["-dumpmachine"], text=True, env=environment).strip()
    with tempfile.TemporaryDirectory(prefix="mister-mgl-regression-") as temporary:
        work = Path(temporary)
        root = work / "fixtures"
        root.mkdir()
        posix_root = root.as_posix()
        if "cygwin" in target:
            posix_root = subprocess.check_output([str(Path(cc[0]).with_name("cygpath.exe")), "-u", str(root)], text=True, env=environment).strip()
        fixtures(root, posix_root)
        (work / "mgl_zip_types.inc").write_text(types, encoding="utf-8")
        (work / "mgl_zip_production.inc").write_text(production, encoding="utf-8")
        (work / "mgl_zip_dispatch.inc").write_text(dispatch, encoding="utf-8")
        flags = ["-D_GNU_SOURCE", "-D_LARGEFILE64_SOURCE", "-I", str(ROOT / "lib/miniz")]
        if "cygwin" in target:
            flags += [f"-D{name}64={name}" for name in ("stat", "fstat", "fopen", "freopen", "ftello", "fseeko")]
        subprocess.run(cc + ["-std=gnu99", "-O2", *flags, "-c", str(ROOT / "lib/miniz/miniz.c"), "-o", str(work / "miniz.o")], check=True, env=environment)
        executable = work / ("mgl_zip.exe" if os.name == "nt" else "mgl_zip")
        subprocess.run(cxx + ["-std=gnu++14", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-stringop-truncation", *flags,
                              "-I", str(ROOT), "-I", str(work), str(ROOT / "tests/mgl_zip_regression.cpp"),
                              str(work / "miniz.o"), "-o", str(executable)], check=True, env=environment)
        run = subprocess.run([str(executable), posix_root], text=True, capture_output=True, env=environment)
        print(run.stdout, end="")
        print(run.stderr, end="", file=sys.stderr)
        raise SystemExit(run.returncode)


if __name__ == "__main__":
    main()
