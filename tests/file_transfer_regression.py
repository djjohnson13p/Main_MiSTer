#!/usr/bin/env python3
"""Require truthful transfer results, cleanup and unchanged valid delegates.

Production backend, checked/legacy transfer, SNES and UEF helpers execute on
synthetic plain/ZIP/gzip fixtures. Hardware is spied; real miniz/zlib are linked.
--source-ref runs historical code with only an int-to-result compatibility
adapter; legacy late-positive paths must fail desired assertions behaviorally.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import shlex
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / "tests"
SCENARIOS = ("valid", "empty", "missing", "zero-first", "short-first", "zero-second", "partial-second", "mapped-zero", "mapped-short", "zip-error", "aux-zero", "helpers")


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--source-ref", default=None)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    names = ("file_io.cpp", "file_io.h", "user_io.cpp", "user_io.h", "menu.cpp", "game_docs.cpp", "cheats.cpp", "support/arcade/mra_loader.h", "support/snes/snes.h", "support/snes/snes.cpp", "support/uef/uef_reader.cpp")
    source = {name: subprocess.check_output(["git", "-C", str(ROOT), "show", f"{args.source_ref}:{name}"], text=True, encoding="utf-8") if args.source_ref else (ROOT / name).read_text(encoding="utf-8") for name in names}
    generator = module("mgl_generator", ROOT / "tests/mgl_zip_regression.py")
    metadata = module("metadata_generator", ROOT / "tests/file_load_metadata_regression.py")
    types, production, dispatch, transfer, fixture = metadata.generate(source, generator)
    types += "\n".join(re.findall(r"^#define FIO_FILE_(?:TX|TX_DAT|INDEX|INFO)\s+[^\n]+", source["user_io.h"], re.M)) + "\n"
    original_support = (ROOT / "tests/file_load_metadata_regression.cpp").read_text(encoding="utf-8")
    support = original_support[:original_support.index('#include "file_load_transfer.inc"')]
    prefix, scenarios = (TESTS / "file_transfer_regression.cpp").read_text(encoding="utf-8").split("// LATE_TRANSFER_SCENARIOS\n")
    support = prefix + support
    for marker, replacement in {
        "void user_io_set_download(": '''void user_io_set_download(unsigned char enable, int address = 0) {
 download_calls++; production_user_io_set_download(enable, address);
 if (spi_index != 255) { download_active = enable; download_states.push_back(enable); ordered_events.push_back(enable ? "download-on" : "download-off"); }
 if (enable && spi_index != 255 && fault_active && !fault_after_first && fault_mode != "aux-zero") apply_fault(load_path);
}''',
        "void user_io_file_tx_data(": '''void user_io_file_tx_data(const uint8_t *data, uint32_t count) {
 production_user_io_file_tx_data(data, count);
 if (spi_index == 255) { cheat_reset_calls++; return; }
 data_calls++; transfer_bytes.insert(transfer_bytes.end(), data, data + count); ordered_events.push_back("payload");
 if (fault_after_first && data_calls == expected_data_calls + 1) apply_fault(load_path);
}''',
        "bool is_snes(": "bool is_snes() { return helper_core == 1; }",
        "bool is_electron(": "bool is_electron() { return helper_core == 2; }",
        "uint8_t *snes_get_header(": "uint8_t *snes_get_header(fileTYPE *);",
        "uint8_t *snes_get_mirrored_rom(": "uint8_t *snes_get_mirrored_rom(fileTYPE *, uint32_t *);",
        "void snes_patch_bs_header(": "void snes_patch_bs_header(fileTYPE *, uint8_t *);",
        "void snes_msu_init(": "void snes_msu_init(const char *);",
        "int UEF_FileSend(": "int UEF_FileSend(fileTYPE *, int);",
        "void mdplus_init(": "void mdplus_init(const char *) { optional_calls++; }",
        "void check_status_change(": "void check_status_change() { status_calls++; }",
        "void *shmem_map(": '''void *shmem_map(uint32_t, uint32_t size) { if(map_failure) return nullptr; mapped_bytes.assign(size, 0xD3); return mapped_bytes.data(); }''',
        "void shmem_unmap(": '''void shmem_unmap(void *, uint32_t) { unmap_calls++; ordered_events.push_back("unmap"); }''',
        "int trace_FileReadAdv(": '''int trace_FileReadAdv(fileTYPE *file, void *data, int count, int failres = 0) {
 requested_counts.push_back(count); before_crcs.push_back(mz_crc32(0, static_cast<const unsigned char *>(data), count));
 const int read_count = FileReadAdv(file, data, count, failres);
 read_counts.push_back(read_count); after_crcs.push_back(mz_crc32(0, static_cast<const unsigned char *>(data), count)); ordered_events.push_back("read");
 if (truncate_aux_after_header && std::string(file->name)=="bsx_bios.rom" && read_count==count && count==32768) {
     if (truncate(path_for_aux.c_str(),0)) std::abort();
     truncate_aux_after_header=false;
 }
 if (target_active) std::printf("JSON {\\"kind\\":\\"read\\",\\"file\\":\\"%s\\",\\"requested\\":%d,\\"actual\\":%d,\\"before_crc\\":%u,\\"after_crc\\":%u}\\n", file->name, count, read_count, before_crcs.back(), after_crcs.back());
 return read_count;
}'''
    }.items():
        support = metadata.replace_definition(support, marker, replacement)
    support += '#include "file_load_transfer.inc"\n#include "mgl_zip_dispatch.inc"\n'
    support += "static std::string case_context;\nstatic std::string path(const char *name) { return storage_root + \"/\" + name; }\n"
    support += generator.definition(original_support, "static void prepare(", "file_load_metadata_regression.cpp") + scenarios
    fixture = fixture.replace('#include "file_load_metadata_regression.cpp"', '#include "late-transfer-support.inc"')
    transfer = transfer.replace("#define FileReadAdv trace_FileReadAdv\n", "#define FileReadAdv trace_FileReadAdv\n#define FileReadSec trace_FileReadSec\n#define FileSeek trace_FileSeek\n#define FileOpen trace_FileOpen\n#define FileClose trace_FileClose\n")
    transfer = transfer.replace("#undef FileReadAdv\n", "#undef FileReadAdv\n#undef FileReadSec\n#undef FileSeek\n#undef FileOpen\n#undef FileClose\n")
    spi = ""
    for marker, name in (("void user_io_set_download(", "user_io_set_download"), ("void user_io_file_tx_data(", "user_io_file_tx_data")):
        spi += f"#define {name} production_{name}\n" + generator.definition(source["user_io.cpp"], marker, "user_io.cpp") + f"#undef {name}\n"
    helper = source["support/snes/snes.cpp"]
    helper = helper[helper.index("static uint8_t hdr[512];"):helper.index("////////////// MSU")]
    msu = "static char snes_romFileName[1024]={}, SelectedPath[1024]={}; static char has_cd=0; static fileTYPE f_audio={};\n"
    msu += "#define MSU_CD_SET 1\n#define MSU_DATA_BASE 3\nstatic void msu_send_command(uint64_t value) { msu_commands.push_back(value); }\n"
    msu += '#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored "-Wstringop-truncation"\n' + generator.definition(source["support/snes/snes.cpp"], "void snes_msu_init(", "support/snes/snes.cpp") + '#pragma GCC diagnostic pop\n'
    uef = source["support/uef/uef_reader.cpp"]
    uef = uef[uef.index("#define UEF_ChunkHeaderSize"):]
    # Instrument actual helpers around real backend and libc calls. No helper
    # success stubs; macro boundaries solely inject controlled I/O failures.
    helpers = "#line 12 \"support/snes/snes.cpp\"\n#define FileReadAdv trace_FileReadAdv\n#define FileSeekLBA trace_FileSeekLBA\n#define malloc trace_malloc\n" + helper
    helpers += "#undef FileReadAdv\n#undef FileSeekLBA\n#undef malloc\n" + msu
    helpers += "#line 40 \"support/uef/uef_reader.cpp\"\n#define FileReadAdv trace_FileReadAdv\n#define tmpfile trace_tmpfile\n#define fread trace_fread\n#define fwrite trace_fwrite\n#define fseek trace_fseek\n" + uef
    helpers += "#undef FileReadAdv\n#undef tmpfile\n#undef fread\n#undef fwrite\n#undef fseek\n"
    checked_present = "UserIoFileTxResult user_io_file_tx_checked(" in source["user_io.cpp"]
    start = source["user_io.cpp"].index("\t\t\t\t\t\tidx = p[idx] - '0';")
    end = source["user_io.cpp"].index("\n\t\t\t\t\t}", start)
    boot = "static bool fixture_cpc=false; static int configured_attempts=0;\nstatic bool is_cpc() { return fixture_cpc; }\nstatic void StoreIdx_F(int,const char *) { configured_attempts++; }\n"
    boot += "static bool configured_boot(const char *name,int slot,bool cpc,bool ready=false) { fixture_cpc=cpc; int idx=0,boot0_loaded=ready; char p[1]={char('0'+slot)}, str[2100]; char ext[]=\"ROM\"; char opensave=0; uint32_t load_addr=0; std::snprintf(str,sizeof(str),\"%s\",name);\n"
    boot += source["user_io.cpp"][start:end] + "\n return boot0_loaded; }\n"
    transfer += boot
    transfer = f"#define HAVE_CHECKED_TRANSFER {int(checked_present)}\n" + spi + helpers + transfer
    fixture = "#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES\n#include <zlib.h>\n#include <climits>\n#include <cassert>\n" + fixture
    command = [args.cxx] if Path(args.cxx).is_file() else shlex.split(args.cxx, posix=os.name != "nt")
    environment = os.environ.copy()
    if Path(command[0]).is_file():
        environment["PATH"] = str(Path(command[0]).resolve().parent) + os.pathsep + environment.get("PATH", "")
    with tempfile.TemporaryDirectory(prefix="mister-late-transfer-") as temporary:
        work = Path(temporary); root = work / "fixtures"; root.mkdir()
        metadata.fixtures(root)
        for name, size in (("Short.gba", 4096), ("Partial.gba", 8192), ("Mapped.gba", 4096), ("goomba.rom", 4096)):
            (root / name).write_bytes(bytes((i * 19 + 3 + (i // 4096) * 41) % 256 for i in range(size)))
        import gzip, struct
        header = b"UEF File!\0" + bytes((0, 0))
        tape = header + struct.pack("<HI", 0x100, 1) + b"A"
        (root / "Tape.uef").write_bytes(tape)
        (root / "TapeGzip.uef").write_bytes(gzip.compress(tape))
        with zipfile.ZipFile(root / "TapeArchive.zip", "w", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("Raw.uef", tape)
            archive.writestr("Gzip.uef", gzip.compress(tape))
        (root / "HeaderOnly.uef").write_bytes(header)
        (root / "BadHeader.uef").write_bytes(b"NOT UEF!" + bytes(8))
        (root / "BadChunk.uef").write_bytes(header + struct.pack("<HI", 0x100, 9) + b"A")
        (root / "BadCarrier.uef").write_bytes(header + struct.pack("<HI", 0x110, 1) + b"A")
        (root / "BadGzip.uef").write_bytes(gzip.compress(tape)[:-4])
        mixed = header + struct.pack("<HI", 0, 4) + b"INFO" + struct.pack("<HI", 0x777, 2) + b"??"
        mixed += struct.pack("<HIH", 0x110, 2, 4) + struct.pack("<HIH", 0x112, 2, 4)
        mixed += struct.pack("<HIHH", 0x111, 4, 0, 0)
        mixed += struct.pack("<HIf", 0x113, 4, 2400.0) + struct.pack("<HIf", 0x116, 4, 0.5)
        (root / "Mixed.uef").write_bytes(mixed)
        for name, size in (("Rom.msu",4096), ("GameCopier.bs",33280), ("ShortMusic.spc",65800), ("Rom.eZ0",4096), ("Rom.eZZ",4096), ("Rom.sfc",32768), ("Mirror.sfc",49152), ("Copier.smc",33280), ("Small.sfc",64), ("EmptyCopier.smc",512), ("bsx_bios.rom",32768), ("Game.bs",32768), ("Music.spc",66048)):
            (root / name).write_bytes(bytes((i * 13 + 7) % 256 for i in range(size)))
        sufami = bytearray(2 * 1024 * 1024)
        for offset in (0, 1024 * 1024):
            sufami[offset:offset + 15] = b"BANDAI SFC-ADX\0"
        sufami[0x10:0x10 + 15] = b"SFC-ADX BACKUP\0"
        sufami[1024 * 1024 + 0x36] = 4
        sufami[1024 * 1024 + 0x37] = 3
        (root / "Sufami.sfc").write_bytes(sufami)
        for name, extra in (("Composite.rom",0), ("BadOffset.rom",0xFFFFFFFF)):
            (root / name).write_bytes(b"MiSTer" + bytes(6) + struct.pack("<I",extra) + b"DATA")
        (root / "SmallComposite.rom").write_bytes(b"MiSTer")
        # Allocate only sparse temporary extents: a large container can have
        # a tiny valid payload after composite preflight. Never allocate GiB.
        sparse_supported = True
        sparse_path = root / "LargeComposite.rom"
        fd = os.open(sparse_path, os.O_CREAT | os.O_RDWR, 0o600)
        try:
            if os.name == "nt":
                import ctypes, msvcrt
                returned = ctypes.c_ulong()
                sparse_supported = bool(ctypes.windll.kernel32.DeviceIoControl(
                    ctypes.c_void_p(msvcrt.get_osfhandle(fd)), 0x900C4, None, 0, None, 0,
                    ctypes.byref(returned), None))
            if sparse_supported:
                offset = (1 << 31) + 1024
                os.write(fd, b"MiSTer" + bytes(6) + struct.pack("<I", offset - 16))
                os.lseek(fd, offset, os.SEEK_SET)
                os.write(fd, b"DATA")
        finally:
            os.close(fd)
        (root / "sparse-supported.txt").write_text("1" if sparse_supported else "0")
        archive_path = root / "Broken.zip"
        with zipfile.ZipFile(archive_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("Failed.gba", bytes((i * 23 + 5) % 256 for i in range(4096)))
        with zipfile.ZipFile(archive_path) as archive:
            info = archive.infolist()[0]
            offset = info.header_offset + 30 + len(info.filename.encode()) + len(info.extra)
        archive_bytes = bytearray(archive_path.read_bytes()); archive_bytes[offset] = 0xFF; archive_path.write_bytes(archive_bytes)
        for name, content in {"mgl_zip_types.inc": types, "mgl_zip_production.inc": production, "mgl_zip_dispatch.inc": dispatch,
                              "file_load_transfer.inc": transfer, "late-transfer-support.inc": support, "proof.cpp": fixture}.items():
            (work / name).write_text(content, encoding="utf-8")
        flags = ["-D_GNU_SOURCE", "-D_LARGEFILE64_SOURCE", "-I", str(ROOT / "lib/miniz")]
        target = subprocess.check_output(command + ["-dumpmachine"], text=True, env=environment).strip()
        if "cygwin" in target:
            flags += [f"-D{name}64={name}" for name in ("stat", "fstat", "fopen", "freopen", "ftello", "fseeko")]
        subprocess.run(command + ["-x", "c", "-std=gnu99", "-O2", *flags, "-c", str(ROOT / "lib/miniz/miniz.c"), "-o", str(work / "miniz.o")], check=True, env=environment)
        executable = work / ("late.exe" if os.name == "nt" else "late")
        subprocess.run(command + ["-std=gnu++14", "-funsigned-char", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable", *flags, "-I", str(ROOT), "-I", str(work),
                                  str(work / "proof.cpp"), str(work / "miniz.o"), "-lz", "-o", str(executable)], check=True, env=environment)
        fixture_root = str(root)
        if "cygwin" in target:
            converter = Path(command[0]).with_name("cygpath.exe")
            fixture_root = subprocess.check_output([str(converter), "-u", fixture_root], text=True, env=environment).strip()
        runs = []
        for scenario in SCENARIOS:
            # Every fresh process receives fresh file contents too; otherwise
            # an earlier deliberate truncation changes a later scenario's size.
            for name, size in (("Short.gba", 4096), ("Partial.gba", 8192), ("Mapped.gba", 4096), ("goomba.rom", 4096)):
                (root / name).write_bytes(bytes((i * 19 + 3 + (i // 4096) * 41) % 256 for i in range(size)))
            try:
                result = subprocess.run([str(executable), fixture_root, scenario], capture_output=True, text=True, env=environment, timeout=15)
            except subprocess.TimeoutExpired as error:
                result = subprocess.CompletedProcess([], 124, error.stdout if isinstance(error.stdout,str) else (error.stdout or b"").decode("utf-8",errors="replace"), (error.stderr if isinstance(error.stderr,str) else (error.stderr or b"").decode("utf-8",errors="replace")) + "\nTimed out after15 seconds")
            observations = [json.loads(line[5:]) for line in result.stdout.splitlines() if line.startswith("JSON ")]
            summary = [row for row in observations if "checks" in row and "failures" in row]
            if not summary:
                result.returncode = result.returncode or 1
            runs.append({"scenario": scenario, "returncode": result.returncode, "observations": observations, "stdout": result.stdout, "stderr": result.stderr})
            print(f"{'FAIL' if result.returncode else 'PASS'}: {scenario}")
            for row in observations:
                if row.get("passed") is False: print("FAIL: " + row["label"])
        if args.output:
            args.output.write_text(json.dumps({"source_ref": args.source_ref, "scope": __doc__, "runs": runs,
                "source_sha256": {name: hashlib.sha256(text.encode()).hexdigest() for name, text in source.items()}}, indent=2) + "\n", encoding="utf-8")
        raise SystemExit(1 if any(run["returncode"] for run in runs) else 0)


if __name__ == "__main__":
    main()
