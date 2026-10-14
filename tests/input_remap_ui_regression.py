#!/usr/bin/env python3
"""Exercise production remap source feedback and MENU_JOYKBDMAP rendering.

Run: python3 tests/input_remap_ui_regression.py [--cxx g++]
--source-ref REF loads the intact input/menu definitions from local Git history.
The historical screen still executes when it has no source-name accessor, so
the regression reports missing rendered feedback rather than a compile error.
OSD, mapping event state, device enumeration and button labels are fixtures.
No FPGA hardware, input events, filesystem persistence or deployment is tested.
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
    """Copy an intact definition with its original diagnostic line numbers."""
    start = source.index(marker)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        # These selected definitions have no braces in strings or comments.
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    line = source.count("\n", 0, start) + 1
    return f'#line {line} "{filename}"\n' + source[start:end] + "\n"


def generate(input_source, header, menu):
    mapping_struct = re.search(r"typedef struct \{.*?\} advancedButtonMap;", header, re.DOTALL)
    device_limit = re.search(r"^#define NUMDEV\s+\d+", input_source, re.MULTILINE)
    name_field = re.search(r"^\s*char\s+name\[\d+\];", input_source, re.MULTILINE)
    if not (mapping_struct and device_limit and name_field):
        raise ValueError("Unable to extract production mapping/device declarations")
    declarations = mapping_struct.group(0) + "\n" + device_limit.group(0) + "\n"
    declarations += "struct devInput { uint16_t vid, pid; " + name_field.group(0).strip() + " char guard[128]; };\n"
    declarations += "static devInput input[NUMDEV] = {};\n"

    accessor = "void get_map_source_name(" in input_source
    helper_marker = "static bool menu_show_map_source("
    helper = helper_marker in menu
    production = f"#define HAVE_SOURCE_ACCESSOR {int(accessor)}\n#define HAVE_SOURCE_HELPER {int(helper)}\n"
    production += definition(input_source, "int get_map_dev()", "input.cpp")
    if accessor:
        production += definition(input_source, "void get_map_source_name(", "input.cpp")
    if helper:
        production += definition(menu, helper_marker, "menu.cpp")

    # Instrument the call boundary to check raw boxed labels before the intact
    # renderer clamps them to 27 columns. Its definition is only macro-renamed.
    production += "#define infowrite production_infowrite\n"
    production += definition(menu, "static void infowrite(", "menu.cpp")
    production += "#undef infowrite\n"
    production += definition(menu, "void build_advanced_map_code_str(", "menu.cpp")
    begin = menu.index("\n\tcase MENU_JOYKBDMAP:")
    end = menu.index("\n\tcase MENU_ABOUT1:", begin)
    line = menu.count("\n", 0, begin) + 1
    production += "static void render_remap()\n{\n switch (menustate) {\n"
    production += f'#line {line} "menu.cpp"\n' + menu[begin:end]
    production += "\n default: break;\n }\n}\n"
    return declarations, production


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--source-ref", help="historical production source from local Git history")
    args = parser.parse_args()

    def read_source(name):
        if args.source_ref:
            return subprocess.check_output(["git", "-C", str(ROOT), "show", f"{args.source_ref}:{name}"],
                                           text=True, encoding="utf-8")
        return (ROOT / name).read_text(encoding="utf-8")

    declarations, production = generate(read_source("input.cpp"), read_source("input.h"), read_source("menu.cpp"))
    command = [args.cxx] if Path(args.cxx).is_file() else shlex.split(args.cxx, posix=os.name != "nt")
    environment = os.environ.copy()
    if Path(command[0]).is_file():
        environment["PATH"] = str(Path(command[0]).resolve().parent) + os.pathsep + environment.get("PATH", "")

    with tempfile.TemporaryDirectory(prefix="mister-remap-ui-") as temporary:
        directory = Path(temporary)
        (directory / "input_remap_ui_declarations.inc").write_text(declarations, encoding="utf-8")
        (directory / "input_remap_ui_production.inc").write_text(production, encoding="utf-8")
        executable = directory / ("remap_ui.exe" if os.name == "nt" else "remap_ui")
        subprocess.run(command + ["-std=gnu++14", "-O2", "-Wall", "-Wextra", "-Werror",
                                  "-I", str(ROOT), "-I", str(directory),
                                  str(ROOT / "tests/input_remap_ui_regression.cpp"), "-o", str(executable)],
                       check=True, env=environment)
        result = subprocess.run([str(executable)], text=True, capture_output=True, env=environment)
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
