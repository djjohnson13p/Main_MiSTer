#!/usr/bin/env python3
"""Exercise real generic file transfer and selected-case metadata transactions.

Production file/ZIP APIs, transfer, docs, cheats and full selected cases execute
intact. Synthetic fixtures replace core output, discovery, save mounts, GAMEID
output and savestate setup. No ROMs, saves, configuration or hardware are used.
--source-ref REF runs historical production against the current assertions.
"""
import argparse
import importlib.util
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


def replace_definition(text, marker, replacement):
    first = text.index(marker)
    opening = text.index("{", first)
    depth, last = 1, opening + 1
    while depth:
        depth += (text[last] == "{") - (text[last] == "}")
        last += 1
    return text[:first] + replacement + text[last:]


def generate(source, generator):
    definition = generator.definition
    cpp, header, user = source["file_io.cpp"], source["file_io.h"], source["user_io.cpp"]
    types, production, dispatch = generator.generate(cpp, header, user, source["menu.cpp"], source["support/arcade/mra_loader.h"], source["user_io.h"])
    if "strncpy(last_zip_fname" in cpp:
        # The pre-x5 cache warning is independently fixed and unrelated to
        # transfer counts. Historical fixtures use short container paths.
        cached = definition(cpp, "static int OpenZipfileCached(", "file_io.cpp")
        production = production.replace(cached,
            '#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored "-Wstringop-truncation"\n'
            + cached + '#pragma GCC diagnostic pop\n', 1)
    types += definition(header, "struct gameAssetValidator\n", "file_io.h")
    types += "\n".join(re.findall(r"^#define SCANF_[^\n]+", header, re.M)) + "\n"
    types += "\n".join(re.findall(r"^#define SNES_FILE_[^\n]+", source["support/snes/snes.h"], re.M)) + "\n"
    for marker in ("FileReadAdv", "FileSeek"):
        types += re.search(r"^int\s+" + marker + r"\([^\n]+", header, re.M).group() + "\n"
    types += re.search(r"^void FileGenerateSavePath\([^\n]+", header, re.M).group() + "\n"
    types += re.search(r"^#define MIN[^\n]+", cpp, re.M).group() + "\n"
    for marker in ("int FileReadSec(", "int FileSeek(", "int FileLoad(", "const char *getFullPath(",
                   "static int validAsset(", "static int findAssetByCrc(", "static int findAssetInSameDir(",
                   "static bool findPsxAsset(", "int findGameAsset("):
        # The ARM source's %lld diagnostic uses a different off64_t typedef on
        # POSIX hosts. Keep the function intact and isolate this host warning.
        if marker == "int FileSeek(":
            production += '#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored "-Wformat"\n'
        if marker == "int findGameAsset(":
            production += "#define findGameAsset production_findGameAsset\n"
        production += definition(cpp, marker, "file_io.cpp")
        if marker == "int findGameAsset(":
            production += "#undef findGameAsset\n"
        if marker == "int FileSeek(":
            production += '#pragma GCC diagnostic pop\n'
    # user_io.cpp includes limits.h; extracted transfer definitions retain that
    # dependency rather than relying on a host standard library's transitive includes.
    transfer = "#include <limits.h>\n#define FileReadAdv trace_FileReadAdv\n#define user_io_file_tx production_user_io_file_tx\n"
    transfer += "#define user_io_file_tx_checked production_user_io_file_tx_checked\n"
    if "UserIoFileTxResult user_io_file_tx_checked(" in user:
        transfer += definition(user, "UserIoFileTxResult user_io_file_tx_checked(", "user_io.cpp")
    transfer += definition(user, "int user_io_file_tx(", "user_io.cpp")
    if "UserIoFileTxResult user_io_file_tx_checked(" not in user:
        # Only adapt the historical per-call return; its production transfer
        # still executes and exposes old late-positive behavior to assertions.
        transfer += "UserIoFileTxResult production_user_io_file_tx_checked(const char *n, unsigned char i, char s, char m, char c, uint32_t a) { return production_user_io_file_tx(n,i,s,m,c,a) ? UserIoFileTxResult::CompletedHostPath : UserIoFileTxResult::RejectedBeforeDownload; }\n"
    transfer += "#undef user_io_file_tx_checked\n"
    transfer += "#undef user_io_file_tx\n#undef FileReadAdv\n"
    transfer += definition(user, "int user_io_use_cheats(", "user_io.cpp")
    transfer += "static char manual_path[1024] = {};\n#define game_docs_init production_game_docs_init\n"
    if "void game_docs_clear(" in source["game_docs.cpp"]:
        transfer += definition(source["game_docs.cpp"], "void game_docs_clear(", "game_docs.cpp")
    else:
        transfer += "void game_docs_clear() { manual_path[0] = 0; }\n"
    for marker in ("void game_docs_init(", "int game_docs_manual_available(", "const char *game_docs_get_manual("):
        transfer += definition(source["game_docs.cpp"], marker, "game_docs.cpp")
    transfer += "#undef game_docs_init\n"
    cheats = source["cheats.cpp"]
    transfer += definition(cheats, "struct cheat_rec_t\n", "cheats.cpp")
    transfer += definition(cheats, "struct CheatComp\n", "cheats.cpp")
    transfer += "typedef std::vector<cheat_rec_t> CheatVector; static CheatVector cheats;\n"
    transfer += "static int loaded=0, cheat_unit_size=16, cheat_max_active=128, iSelectedEntry=0, iFirstEntry=0;\nstatic char cheat_zip[1024] = {};\n"
    transfer += "int cheats_available(); void cheats_scan(int);\n"
    transfer += definition(cheats, "static int cheat_zip_validator(", "cheats.cpp")
    if "void cheats_clear(" in cheats:
        transfer += definition(cheats, "void cheats_clear(", "cheats.cpp")
    else:
        transfer += "void cheats_clear(bool) { cheats.clear(); loaded=0; cheat_zip[0]=0; }\n"
    transfer += "#define cheats_init production_cheats_init\n"
    transfer += definition(cheats, "void cheats_init(", "cheats.cpp")
    transfer += "#undef cheats_init\n"
    transfer += definition(cheats, "int cheats_available(", "cheats.cpp")
    transfer += definition(cheats, "void cheats_scan(", "cheats.cpp")
    fixture = (ROOT / "tests/mgl_zip_regression.cpp").read_text(encoding="utf-8")
    fixture = fixture[:fixture.index("\nstatic int failures = 0, checks = 0;")]
    fixture = fixture.replace("bool is_arcade() { return false; }", "bool is_arcade() { return false; }\nbool is_psx(); bool is_megacd(); bool pcecd_using_cd(); const char *psx_get_game_id();")
    for marker, replacement in {
        "UserIoFileTxResult user_io_file_tx_checked(": "UserIoFileTxResult user_io_file_tx_checked(const char *, unsigned char, char, char, char, uint32_t);",
        "void game_docs_clear(": "void game_docs_clear();",
        "void cheats_clear(": "void cheats_clear(bool);",
        "int user_io_file_tx(": "int user_io_file_tx(const char *, unsigned char, char, char, char, uint32_t);",
        "int user_io_file_mount(": "int user_io_file_mount(const char *, unsigned char, char pre=0, int pre_size=0);",
        "void user_io_set_index(": "void user_io_set_index(int);",
        "void game_docs_init(": "void game_docs_init(const char *, uint32_t);",
        "void cheats_init(": "void cheats_init(const char *, uint32_t);",
        "bool user_io_use_cheats(": "int user_io_use_cheats();",
        "uint32_t user_io_get_file_crc(": definition(user, "uint32_t user_io_get_file_crc(", "user_io.cpp"),
        "void process_addon(": "void process_addon(const char *, int);",
        "bool is_pce(": "bool is_pce();", "bool is_n64(": "bool is_n64();",
        "bool is_c64(": "bool is_c64();", "bool is_c128(": "bool is_c128();",
        "bool is_atari800(": "bool is_atari800();", "bool is_atari5200(": "bool is_atari5200();",
        "void pcecd_set_image(": "void pcecd_set_image(int, const char *);",
        "void pcecd_reset(": "void pcecd_reset();",
        "bool n64_rom_tx(": "bool n64_rom_tx(const char *, unsigned char, uint32_t, uint32_t &);",
        "void c64_open_file(": "void c64_open_file(const char *, int);",
        "void atari800_open_bios_file(": "void atari800_open_bios_file(const char *, int);",
        "int atari5200_check_cartridge_file(": "int atari5200_check_cartridge_file(const char *, int);",
        "int atari800_check_cartridge_file(": "int atari800_check_cartridge_file(const char *, int);",
        "void atari5200_open_cartridge_file(": "void atari5200_open_cartridge_file(const char *, int);",
        "void atari800_open_cartridge_file(": "void atari800_open_cartridge_file(const char *, int);",
    }.items():
        fixture = replace_definition(fixture, marker, replacement)
    fixture = fixture.replace('#include "mgl_zip_dispatch.inc"', '#include "file_load_metadata_regression.cpp"')
    return types, production, dispatch, transfer, fixture


def fixtures(root):
    old = bytes((i * 37 + 11) % 256 for i in range(4096))
    fresh = bytes((i * 13 + 29) % 256 for i in range(4096))
    for name in ("Old.gba", "Truncated.gba", "MapFailure.gba", "GoombaTest.gb", "Original.gba"):
        (root / name).write_bytes(old)
    (root / "New.gba").write_bytes(fresh)
    (root / "Empty.gba").write_bytes(b"")
    (root / "Disk.img").write_bytes(b"SYNTHETIC_WRITABLE_DISK")
    for name in ("Old", "Missing", "New", "Original", "Special", "MapFailure", "GoombaTest", "Empty", "Truncated"):
        (root / (name + ".pdf")).write_bytes(b"synthetic manual")
    for name in ("Old", "Missing", "New"):
        with zipfile.ZipFile(root / (name + ".zip"), "w", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr(name + " Cheat", bytes(16))
    for name, payload in (("Old", old), ("Zip", fresh)):
        crc = f"{zipfile.crc32(payload):08X}"
        manual = root / "docs/TestCore/Manuals" / (name + " Manual [" + crc + "].pdf")
        manual.parent.mkdir(parents=True, exist_ok=True)
        manual.write_bytes(b"synthetic CRC manual")
        cheat = root / "cheats/TestCore" / (name + " Cheats [" + crc + "].zip")
        cheat.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(cheat, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr(name + " CRC Cheat", bytes(16))
    for name, member in (("ZipGame.zip", "nested/Renamed.gba"), ("Original.zip", "Alternative.gba"), ("ZipDisk.zip", "Only.img")):
        with zipfile.ZipFile(root / name, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr(member, fresh)
    with zipfile.ZipFile(root / "Unsupported.zip", "w", compression=zipfile.ZIP_BZIP2) as archive:
        archive.writestr("Only.gba", fresh)
    (root / "Invalid.zip").write_bytes(b"NOT_A_ZIP")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--source-ref")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    names = ("file_io.cpp", "file_io.h", "user_io.cpp", "user_io.h", "menu.cpp", "game_docs.cpp", "cheats.cpp",
             "support/arcade/mra_loader.h", "support/snes/snes.h")
    source = {name: subprocess.check_output(["git", "-C", str(ROOT), "show", f"{args.source_ref}:{name}"], text=True, encoding="utf-8")
              if args.source_ref else (ROOT / name).read_text(encoding="utf-8") for name in names}
    spec = importlib.util.spec_from_file_location("mgl_generator", ROOT / "tests/mgl_zip_regression.py")
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    types, production, dispatch, transfer, fixture = generate(source, generator)
    command = [args.cxx] if Path(args.cxx).is_file() else shlex.split(args.cxx, posix=os.name != "nt")
    environment = os.environ.copy()
    if Path(command[0]).is_file():
        environment["PATH"] = str(Path(command[0]).resolve().parent) + os.pathsep + environment.get("PATH", "")
    with tempfile.TemporaryDirectory(prefix="mister-file-metadata-") as folder:
        work = Path(folder)
        root = work / "fixtures"
        root.mkdir()
        fixtures(root)
        for name, content in {"mgl_zip_types.inc": types, "mgl_zip_production.inc": production,
                              "mgl_zip_dispatch.inc": dispatch, "file_load_transfer.inc": transfer,
                              "fixture.cpp": fixture}.items():
            (work / name).write_text(content, encoding="utf-8")
        flags = ["-D_GNU_SOURCE", "-D_LARGEFILE64_SOURCE", "-I", str(ROOT / "lib/miniz")]
        target = subprocess.check_output(command + ["-dumpmachine"], text=True, env=environment).strip()
        if "cygwin" in target:
            flags += [f"-D{name}64={name}" for name in ("stat", "fstat", "fopen", "freopen", "ftello", "fseeko")]
        subprocess.run(command + ["-x", "c", "-std=gnu99", "-O2", *flags, "-c", str(ROOT / "lib/miniz/miniz.c"), "-o", str(work / "miniz.o")], check=True, env=environment)
        executable = work / ("metadata.exe" if os.name == "nt" else "metadata")
        subprocess.run(command + ["-std=gnu++14", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable", *flags,
                                  "-I", str(ROOT), "-I", str(ROOT / "tests"), "-I", str(work), str(work / "fixture.cpp"), str(work / "miniz.o"), "-o", str(executable)], check=True, env=environment)
        fixture_root = str(root)
        if "cygwin" in target:
            cygpath = Path(command[0]).with_name("cygpath.exe")
            converter = str(cygpath) if cygpath.is_file() else shutil.which("cygpath", path=environment["PATH"])
            if not converter:
                raise RuntimeError("cygpath is required for the MSYS/Cygwin compiler")
            fixture_root = subprocess.check_output([converter, "-u", fixture_root], text=True, env=environment).strip()
        result = subprocess.run([str(executable), fixture_root], text=True, capture_output=True, env=environment)
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        if args.output:
            rows = [json.loads(line[5:]) for line in result.stdout.splitlines() if line.startswith("JSON ")]
            args.output.write_text(json.dumps({"source_ref": args.source_ref, "returncode": result.returncode,
                                              "observations": rows, "stdout": result.stdout, "stderr": result.stderr,
                                              "scope": __doc__}, indent=2) + "\n", encoding="utf-8")
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
