#!/usr/bin/env python3
"""Exercise production MGL XML parsing, action recovery and selected-file loading.

Actual sxmlc, MGL callbacks, file/ZIP APIs and transfer/selected cases execute.
Synthetic temporary files and peripheral spies replace hardware and persistence.
--source-ref runs historical production against the current desired behavior.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--source-ref")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    def read(name):
        if args.source_ref:
            return subprocess.check_output(["git", "-C", str(ROOT), "show", f"{args.source_ref}:{name}"],
                                           text=True, encoding="utf-8")
        return (ROOT / name).read_text(encoding="utf-8")

    names = ("file_io.cpp", "file_io.h", "user_io.cpp", "menu.cpp", "game_docs.cpp", "cheats.cpp",
             "support/arcade/mra_loader.h", "support/arcade/mra_loader.cpp", "support/snes/snes.h",
             "sxmlc.c", "sxmlc.h", "lib/miniz/miniz.c", "lib/miniz/miniz.h")
    source = {name: read(name) for name in names}
    metadata = module("metadata_generator", ROOT / "tests/file_load_metadata_regression.py")
    generator = module("mgl_generator", ROOT / "tests/mgl_zip_regression.py")
    types, production, dispatch, transfer, fixture = metadata.generate(source, generator)
    # Historical ZIP-cache strncpy is an independently fixed x5 warning. Its
    # container path here is short. Isolate that known diagnostic to its actual
    # unchanged function; do not count a historical compile warning as proof.
    if "strncpy(last_zip_fname" in source["file_io.cpp"]:
        definition = generator.definition(source["file_io.cpp"], "static int OpenZipfileCached(", "file_io.cpp")
        assert definition in production
        production = production.replace(definition,
            '#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored "-Wstringop-truncation"\n'
            + definition + '#pragma GCC diagnostic pop\n', 1)
    support = (ROOT / "tests/file_load_metadata_regression.cpp").read_text(encoding="utf-8")
    support = support[:support.index("\nstatic int failures = 0, checks = 0;")]
    fixture = fixture.replace('#include "file_load_metadata_regression.cpp"',
        '#include "mgl_attribute_support.inc"\n#include "mgl_attribute_parse.inc"\n#include "mgl_attribute_regression.cpp"')
    definitions = '#include "sxmlc.h"\n#define mgl parsed_mgl\nstatic mgl_struct mgl = {};\n'
    definitions += "static size_t decoded_path_bytes = 0;\nstatic int xml_errors = 0;\n#define scan_mgl production_scan_mgl\n"
    definitions += generator.definition(source["support/arcade/mra_loader.cpp"], "static int scan_mgl(", "support/arcade/mra_loader.cpp")
    definitions += """#undef scan_mgl
static int scan_mgl(XMLEvent event, const XMLNode *node, SXML_CHAR *text, int n, SAX_Data *data)
{
    if (event == XML_EVENT_START_DOC) { decoded_path_bytes = 0; xml_errors = 0; }
    if (event == XML_EVENT_START_NODE && !strcasecmp(node->tag, "file"))
        for (int i = 0; i < node->n_attributes; i++)
            if (!strcasecmp(node->attributes[i].name, "path")) decoded_path_bytes = strlen(node->attributes[i].value);
    if (event == XML_EVENT_ERROR) xml_errors++;
    return production_scan_mgl(event, node, text, n, data);
}
"""
    for marker in ("mgl_struct* mgl_parse(", "mgl_struct* mgl_get("):
        definitions += generator.definition(source["support/arcade/mra_loader.cpp"], marker, "support/arcade/mra_loader.cpp")
    definitions += "#undef mgl\n"
    # Execute the complete real post-core script decision. Peripheral core
    # identity and clock are fixtures; an empty parsed script must become done.
    start = source["user_io.cpp"].index("\tif (!mgl_get()->count || is_menu() || is_st() || is_archie())")
    end = source["user_io.cpp"].index("\n}\n", start)
    definitions += """
static bool fixture_menu_core = false, fixture_st_core = false, fixture_archie_core = false;
static unsigned long timer_delay = 0;
static unsigned long GetTimer(unsigned long delay) { timer_delay = delay; return 123 + delay; }
static bool mgl_fixture_is_menu() { return fixture_menu_core; }
static bool mgl_fixture_is_st() { return fixture_st_core; }
static bool mgl_fixture_is_archie() { return fixture_archie_core; }
#define is_menu mgl_fixture_is_menu
#define is_st mgl_fixture_is_st
#define is_archie mgl_fixture_is_archie
static void initialize_script()
{
"""
    definitions += source["user_io.cpp"][start:end] + "\n}\n#undef is_menu\n#undef is_st\n#undef is_archie\n"
    command = [args.cxx] if Path(args.cxx).is_file() else shlex.split(args.cxx, posix=os.name != "nt")
    environment = os.environ.copy()
    if Path(command[0]).is_file():
        environment["PATH"] = str(Path(command[0]).resolve().parent) + os.pathsep + environment.get("PATH", "")
    target = subprocess.check_output(command + ["-dumpmachine"], text=True, env=environment).strip()
    with tempfile.TemporaryDirectory(prefix="mister-mgl-attributes-") as folder:
        work = Path(folder)
        root = work / "fixtures"
        root.mkdir()
        fixture_root = str(root)
        if "cygwin" in target:
            cygpath = Path(command[0]).with_name("cygpath.exe")
            converter = str(cygpath) if cygpath.is_file() else shutil.which("cygpath", path=environment["PATH"])
            if not converter:
                raise RuntimeError("cygpath is required for the MSYS/Cygwin compiler")
            fixture_root = subprocess.check_output([converter, "-u", fixture_root], text=True, env=environment).strip()
        prefix = fixture_root + "/Alias.zip/"
        needed = 1023 - len(prefix.encode("utf-8")) - len("Target.gba")
        assert needed >= 3
        nested = "a/" * (needed // 2) if needed % 2 == 0 else "ab/" + "a/" * ((needed - 3) // 2)
        member = nested + "Target.gba"
        payload = b"SYNTHETIC_PREFIX_MEMBER_PAYLOAD"
        with zipfile.ZipFile(root / "Alias.zip", "w", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr(member, payload)
        complete = prefix + member
        assert len(complete.encode("utf-8")) == 1023
        (root / "prefix.txt").write_bytes(complete.encode("utf-8"))
        (root / "ordinary.gba").write_bytes(b"ORDINARY_SYNTHETIC_ROM")
        (root / "Disk.img").write_bytes(b"SYNTHETIC_WRITABLE_DISK")
        files = {"mgl_zip_types.inc": types, "mgl_zip_production.inc": production,
                 "mgl_zip_dispatch.inc": dispatch, "file_load_transfer.inc": transfer,
                 "mgl_attribute_support.inc": support, "mgl_attribute_parse.inc": definitions,
                 "fixture.cpp": fixture, "sxmlc.c": source["sxmlc.c"], "sxmlc.h": source["sxmlc.h"],
                 "miniz.c": source["lib/miniz/miniz.c"], "miniz.h": source["lib/miniz/miniz.h"]}
        for name, content in files.items():
            (work / name).write_text(content, encoding="utf-8")
        flags = ["-D_GNU_SOURCE", "-D_LARGEFILE64_SOURCE", "-I", str(work), "-I", str(ROOT / "lib/miniz")]
        if "cygwin" in target:
            flags += [f"-D{name}64={name}" for name in ("stat", "fstat", "fopen", "freopen", "ftello", "fseeko")]
        for name in ("miniz", "sxmlc"):
            # sxmlc defines _GNU_SOURCE itself before including libc headers.
            c_flags = [flag for flag in flags if name != "sxmlc" or flag != "-D_GNU_SOURCE"]
            subprocess.run(command + ["-x", "c", "-std=gnu99", "-O2", *c_flags,
                "-c", str(work / (name + ".c")), "-o", str(work / (name + ".o"))], check=True, env=environment)
        executable = work / ("attributes.exe" if os.name == "nt" else "attributes")
        subprocess.run(command + ["-std=gnu++14", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable",
            *flags, "-I", str(ROOT), "-I", str(ROOT / "tests"), str(work / "fixture.cpp"),
            str(work / "miniz.o"), str(work / "sxmlc.o"), "-o", str(executable)], check=True, env=environment)
        result = subprocess.run([str(executable), fixture_root], capture_output=True, env=environment)
        stdout = result.stdout.decode("utf-8", errors="backslashreplace")
        stderr = result.stderr.decode("utf-8", errors="backslashreplace")
        rows = [json.loads(line[5:]) for line in stdout.splitlines() if line.startswith("JSON ")]
        if args.output:
            args.output.write_text(json.dumps({"source_ref": args.source_ref,
                "source_sha256": {name: hashlib.sha256(text.encode()).hexdigest() for name, text in source.items()},
                "returncode": result.returncode, "observations": rows, "stdout": stdout, "stderr": stderr,
                "scope": __doc__}, indent=2) + "\n", encoding="utf-8")
        print(stderr, end="", file=sys.stderr)
        for row in rows:
            print(json.dumps(row))
        if not rows or "checks" not in rows[-1] or "failures" not in rows[-1]:
            print("FAIL: MGL fixture did not produce its final check summary", file=sys.stderr)
            raise SystemExit(result.returncode or 1)
        print(f"{'PASS' if result.returncode == 0 else 'FAIL'}: production MGL attributes "
              f"({rows[-1]['checks']} checks, {rows[-1]['failures']} failures)")
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
