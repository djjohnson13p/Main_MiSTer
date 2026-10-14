#!/usr/bin/env python3
"""Exercise production advanced remap capture with synthetic input devices.

Run: python3 tests/input_remap_regression.py [--cxx g++]
Use --source-ref COMMIT to demonstrate failures against local Git history.
The real initialization, finish/clear functions, per-phase press ownership,
keyboard normalization, device binding, OSD-combo handling and type-3 event
branch are copied intact.
Device discovery, clocks, core output and persistence use host fixtures.
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


def block(source, marker, filename, line_offset=0):
    start = source.index(marker)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        # Selected definitions contain no unmatched braces in strings/comments.
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    if source[end:end + 1] == ";":
        end += 1
    line = source.count("\n", 0, start) + 1 + line_offset
    return f'#line {line} "{filename}"\n' + source[start:end] + "\n"


def region(source, start, end, filename, line_offset=0):
    first = source.index(start)
    last = source.index(end, first)
    line = source.count("\n", 0, first) + 1 + line_offset
    return f'#line {line} "{filename}"\n' + source[first:last] + "\n"


def generate(cpp, header, hardware):
    typedef_start = header.index("typedef struct {", header.index("#define ADVANCED_MAP_MAX"))
    typedef_end = header.index("} advancedButtonMap;", typedef_start) + len("} advancedButtonMap;")
    types = header[typedef_start:typedef_end] + "\n"
    types += "\n".join(re.findall(r"^#define (?:SYS_|NUMBUTTONS|ADVANCED_MAP_MAX)[^\n]*", header, re.MULTILINE)) + "\n"
    types += re.search(r"^#define NUMDEV [^\n]+", cpp, re.MULTILINE).group() + "\n"
    types += block(cpp, "enum QUIRK\n", "input.cpp")
    types += "\n".join(re.findall(r"^#define JOYCON_[^\n]+", cpp, re.MULTILINE)) + "\n"
    types += re.search(r"^#define KEY_EMU [^\n]+", header, re.MULTILINE).group() + "\n"

    generated = region(cpp, "static int mapping = 0;", "void start_map_setting(", "input.cpp")
    generated += block(hardware, "unsigned long CheckTimer(", "hardware.cpp")
    for marker in (
        "void start_map_setting(", "int get_map_count(", "int get_map_set(",
        "int get_map_button(", "int get_map_type(", "int get_map_clear(",
        "int get_map_finish(", "int get_map_dev(", "int get_map_cancel(",
        "int get_map_advance(", "void input_advanced_clear(", "void finish_map_setting(",
    ):
        generated += block(cpp, marker, "input.cpp")

    callback = cpp.index("static void input_cb(struct input_event *ev, struct input_absinfo *absinfo, int dev, bool menu_event)\n{")
    body_start = cpp.index("{", callback) + 1
    callback_body = cpp[body_start:]
    callback_line_offset = cpp.count("\n", 0, body_start)
    generated += "void capture_event(input_event *ev, int dev)\n{\n"
    # Keep event validation, device bindings, mouse interception and keyboard
    # remapping unchanged. Default maps/player assignment below this prefix
    # are peripheral: fixtures provide already initialized mmap records.
    generated += f'#line {callback_line_offset + 1} "input.cpp"\n'
    generated += callback_body[:callback_body.index("\n\tstatic int key_mapped = 0;")] + "\n"
    generated += region(callback_body, "\tint map_skip =", "\n\tif (!input[dev].has_mmap)", "input.cpp", callback_line_offset)
    generated += region(callback_body, "\tint old_combo =", "\n\t//mapping", "input.cpp", callback_line_offset)
    generated += block(callback_body, "if (mapping && mapping_type == 3 && ev->type == EV_KEY)", "input.cpp", callback_line_offset)
    generated += "}\n"
    return types, generated


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--source-ref", help="load production definitions from this local Git ref")
    args = parser.parse_args()

    def read_source(name):
        if args.source_ref:
            return subprocess.check_output(["git", "-C", str(ROOT), "show", f"{args.source_ref}:{name}"],
                                           text=True, encoding="utf-8")
        return (ROOT / name).read_text(encoding="utf-8")

    types, production = generate(read_source("input.cpp"), read_source("input.h"), read_source("hardware.cpp"))
    command = [args.cxx] if Path(args.cxx).is_file() else shlex.split(args.cxx, posix=os.name != "nt")
    environment = os.environ.copy()
    if Path(command[0]).is_file():
        environment["PATH"] = str(Path(command[0]).resolve().parent) + os.pathsep + environment.get("PATH", "")
    with tempfile.TemporaryDirectory(prefix="mister-input-remap-") as directory:
        directory = Path(directory)
        (directory / "input_remap_types.inc").write_text(types, encoding="utf-8")
        (directory / "input_remap_production.inc").write_text(production, encoding="utf-8")
        executable = directory / ("remap.exe" if os.name == "nt" else "remap")
        subprocess.run(command + ["-std=gnu++14", "-O2", "-Wall", "-Wextra", "-Werror",
                                   "-Wno-unused-variable", "-Wno-unused-but-set-variable",
                                   "-I", str(ROOT), "-I", str(directory),
                                   str(ROOT / "tests/input_remap_regression.cpp"), "-o", str(executable)],
                       check=True, env=environment)
        result = subprocess.run([str(executable)], text=True, capture_output=True, env=environment)
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
