#!/usr/bin/env python3
"""Exercise production remap, normal keyboard and Advanced UI handoff together.

Run on Linux/MSYS: python3 tests/input_remap_handoff_regression.py --cxx g++
--source-ref REF runs historical production behavior with the current assertions.
On a non-Linux host, --linux-codes supplies Linux input-event-codes.h (the local
ARM toolchain is detected when present). Discovery, hardware, drawing, runtime
map contents and persistence are fixtures. All generated files are temporary.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SCENARIOS = (
    "existing-suite", "before-ui", "after-handoff", "render-boundary",
    "normal-f12", "normal-unpaired-release", "later-normal-press",
    "foreign-release-ownership", "canonical-release", "normalized-release",
    "pending-repeat", "after-handoff-repeat", "selected-source-repeat", "second-before-ui",
    "repeat-does-not-rearm", "stale-fresh-recovery", "double-release", "wrong-code",
    "bound-alias", "normalized-alias", "foreign-repeat", "combo-bookkeeping",
    "default-finish", "dismiss-one", "dismiss-two", "invalid-menu-handoff", "new-capture",
    "invalid-dismiss-one", "invalid-dismiss-two", "invalid-double-handoff",
    "count-one-handoff", "timer-handoff", "ordinary-hooks", "soft-reset",
    "inventory-pending", "joycon-pending", "initialize-pending",
    "inventory-held", "joycon-held", "initialize-held",
)


def local_linux_codes(root):
    """Find the local toolchain header from either checkout or worktree depth."""
    for directory in (root, *root.parents):
        found = sorted((directory / "Tools/arm").glob(
            "*/arm-none-linux-gnueabihf/libc/usr/include/linux/input-event-codes.h"))
        if found:
            return found[0]
    return None


def generate(source, generator):
    block, region = generator.block, generator.region
    cpp, header, menu = source["input.cpp"], source["input.h"], source["menu.cpp"]
    types, capture = generator.generate(cpp, header, source["hardware.cpp"])
    for name in ("input.h", "user_io.h", "osd.h", "brightness.h"):
        for line in source[name].splitlines():
            if re.match(r"^#define (?:NONE|[LR](?:CTRL|SHIFT|ALT|GUI)|MODMASK|OSD\b|OSD_OPEN|CAPS_TOGGLE|EXT|EMU_SWITCH_[12]|UPSTROKE|NUMPLAYERS|UIO_KEYBOARD|CORE_TYPE_8BIT|EMU_(?:NONE|MOUSE|JOY0|JOY1)|REPEATDELAY|REPEATRATE|BRIGHTNESS_(?:UP|DOWN))\b", line):
                types += line + "\n"
    types += block(menu, "enum MENU\n", "menu.cpp")
    types += re.search(r"^#define BTN_NUM[^\n]+", cpp, re.M).group() + "\n"
    types += re.search(r"^#define BTN_TGL[^\n]+", cpp, re.M).group() + "\n"
    keys = "".join(block(cpp, marker, "input.cpp") for marker in
                   ("static const int ev2ps2[]", "static const int ev2ps2_set1[]", "static int ev2amiga[]"))
    keys += "uint8_t ps2_kbd_scan_set = 2;\n"
    keys += block(cpp, "uint32_t get_ps2_code(", "input.cpp")
    keys += block(cpp, "uint32_t get_amiga_code(", "input.cpp")
    keys += region(cpp, "static uint32_t modifier = 0;", "enum QUIRK\n", "input.cpp")
    # Add the complete normal keyboard branch inside the same real capture
    # callback, preserving its original keyboard-versus-joystick condition.
    callback_end = capture.rfind("\n}\n", 0, capture.index("\nint inventory_changed()") + 1)
    keyboard = block(cpp, "// keyboard\n\t\t\telse", "input.cpp")
    routing_start = cpp.index("\n\t\t\tif (ev->code >= 256", cpp.index("static void input_cb(struct input_event *ev, struct input_absinfo *absinfo, int dev, bool menu_event)\n{"))
    routing = cpp[routing_start + 1:cpp.index("\n", routing_start + 1)].strip()
    capture = (capture[:callback_end] + "\n bool menu_event = false;\n if (ev->type == EV_KEY) {\n"
               + " " + routing + " {}\n"
               + keyboard + "\n }\n" + capture[callback_end:])
    for marker in ("void get_map_source_name(", "advancedButtonMap *get_advanced_map_defs(",
                   "void input_switch(", "void input_notify_mode()", "int input_state()"):
        capture += block(cpp, marker, "input.cpp")
    # The unchanged reset helper compares signed host char with sizeof; ARM
    # uses unsigned char. Suppress that existing warning only for this helper.
    user_io = ("#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored \"-Wsign-compare\"\n"
               + "#define user_io_check_reset production_user_io_check_reset\n"
               + block(source["user_io.cpp"], "void user_io_check_reset(", "user_io.cpp")
               + "#undef user_io_check_reset\n#pragma GCC diagnostic pop\n"
               + block(source["user_io.cpp"], "void user_io_set_kbd_reset(", "user_io.cpp")
               + "#define user_io_kbd production_user_io_kbd\n"
               + block(source["user_io.cpp"], "void user_io_kbd(", "user_io.cpp")
               + "#undef user_io_kbd\n")
    ui = region(menu, "static uint32_t menu_key = 0;", "static char* getNet(", "menu.cpp")
    ascii_marker = re.search(r"^static[^\n]*keycode_table[^\n]*=", menu, re.M).group()
    ui = block(menu, ascii_marker, "menu.cpp") + block(menu, "static uint8_t GetASCIIKey(", "menu.cpp") + ui
    for marker in ("static bool menu_show_map_source(", "static void infowrite(", "void build_advanced_map_code_str(",
                   "void build_advanced_map_core_btn_str(", "void build_advanced_map_summary("):
        ui += block(menu, marker, "menu.cpp")
    prefix_start = menu.index("\t// decode and set events", menu.index("void HandleUI("))
    prefix_end = menu.index("\t\tcase KEY_F1:", prefix_start)
    ui += "static void tick_ui()\n{\n uint32_t c = menu_key_get(); ui_prefix_key = c;\n"
    ui += f'#line {menu.count(chr(10), 0, prefix_start) + 1} "menu.cpp"\n' + menu[prefix_start:prefix_end]
    ui += "\n default: break;\n }\n }\n switch (menustate) {\n"
    for first, last in (("\n\tcase MENU_JOYKBDMAP:", "\n\tcase MENU_ABOUT1:"),
                        ("\n\tcase MENU_ADVANCED_MAP_LIST1:", "\n\tcase MENU_ADVANCED_MAP_EDIT1:")):
        ui += region(menu, first, last, "menu.cpp")
    ui += "\n default: break;\n }\n}\n"
    return types, keys, capture, user_io, ui


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--source-ref")
    parser.add_argument("--linux-codes", type=Path)
    parser.add_argument("--output", type=Path, help="optional complete JSON run receipt")
    args = parser.parse_args()
    def read(name):
        if args.source_ref:
            return subprocess.check_output(["git", "-C", str(ROOT), "show", f"{args.source_ref}:{name}"], text=True, encoding="utf-8")
        return (ROOT / name).read_text(encoding="utf-8")
    source = {name: read(name) for name in ("input.cpp", "input.h", "hardware.cpp", "menu.cpp",
                                           "user_io.cpp", "user_io.h", "osd.h", "brightness.h")}
    spec = importlib.util.spec_from_file_location("event_generator", ROOT / "tests/input_remap_regression.py")
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    types, keys, capture, user_io, ui = generate(source, generator)
    command = [args.cxx] if Path(args.cxx).is_file() else shlex.split(args.cxx, posix=os.name != "nt")
    environment = os.environ.copy()
    if Path(command[0]).is_file():
        environment["PATH"] = str(Path(command[0]).resolve().parent) + os.pathsep + environment.get("PATH", "")
    target = subprocess.check_output(command + ["-dumpmachine"], text=True, env=environment).strip()
    fixture = (ROOT / "tests/input_remap_regression.cpp").read_text(encoding="utf-8")
    fixture = fixture.replace('#include "cfg.h"', '#include "cfg.h"\n#include "str_util.h"\n#include <utility>')
    linux_codes = None
    if "linux" not in target:
        codes = args.linux_codes or local_linux_codes(ROOT)
        if not codes:
            parser.error("Non-Linux compiler needs --linux-codes pointing to Linux input-event-codes.h")
        linux_codes = codes.read_text(encoding="utf-8")
        first, last = fixture.index("#ifdef __linux__"), fixture.index('#include "input_remap_types.inc"')
        fixture = (fixture[:first] + '#include "input-event-codes.h"\n'
                   + "struct input_event { uint16_t type; uint16_t code; int32_t value; };\nstruct input_absinfo { int value; };\n"
                   + fixture[last:])
    fixture = fixture.replace("struct DeviceFixture\n{", "struct DeviceFixture\n{\n uint16_t vid, pid;\n char name[128];")
    fixture = fixture.replace("struct DeviceFixtures\n{", "using devInput = DeviceFixture;\nstruct DeviceFixtures\n{")
    fixture = fixture.replace("static std::vector<int> cleared;", "static std::vector<int> cleared;\nstatic std::vector<int> normal_map_saves;")
    fixture = fixture.replace("void user_io_kbd(uint16_t key, int press) { if (key == KEY_ENTER && !press) released_enter++; }", "void user_io_kbd(uint16_t key, int press);")
    fixture = fixture.replace("bool do_delete)\n{\n\tif (!do_delete) std::abort();\n\tcleared.push_back(device);",
                              "bool do_delete = false)\n{\n if (do_delete) cleared.push_back(device); else normal_map_saves.push_back(device);")
    fixture = fixture.replace('#include "input_remap_production.inc"', '''#include "handoff_support.inc"
#include "keycode_production.inc"
#include "input_remap_production.inc"
#include "user_io_kbd_production.inc"
#include "menu_handoff_production.inc"
void user_io_kbd(uint16_t key, int press) {
 if (key == KEY_ENTER && !press) released_enter++;
 dispatched_keys.push_back({key, press});
 production_user_io_kbd(key, press);
}''')
    support, scenarios = (ROOT / "tests/input_remap_handoff_regression.cpp").read_text(encoding="utf-8").split("// Combined UI/event assertions.\n")
    fixture = fixture.replace("int main()\n{", "int existing_main()\n{", 1) + scenarios
    with tempfile.TemporaryDirectory(prefix="mister-remap-ui-") as directory:
        directory = Path(directory)
        files = {"input_remap_types.inc": types, "keycode_production.inc": keys,
                 "input_remap_production.inc": capture, "user_io_kbd_production.inc": user_io,
                 "menu_handoff_production.inc": ui, "handoff_support.inc": support, "proof.cpp": fixture}
        if linux_codes is not None:
            files["input-event-codes.h"] = linux_codes
        for name, value in files.items():
            (directory / name).write_text(value, encoding="utf-8")
        executable = directory / ("handoff.exe" if os.name == "nt" else "handoff")
        subprocess.run(command + ["-std=gnu++14", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable",
                                   "-Wno-unused-but-set-variable", "-I", str(ROOT), "-I", str(directory),
                                   str(directory / "proof.cpp"), "-o", str(executable)], check=True, env=environment)
        runs = []
        for scenario in SCENARIOS:
            result = subprocess.run([str(executable), scenario], capture_output=True, text=True, env=environment)
            observations = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
            runs.append({"scenario": scenario, "returncode": result.returncode, "stdout": result.stdout,
                         "stderr": result.stderr, "observations": observations})
            if scenario == "existing-suite":
                print(result.stdout, end="")
            else:
                summary = observations[-1] if observations else {"checks": 0, "failures": 1}
                print(f"{'FAIL' if result.returncode else 'PASS'}: {scenario} ({summary['checks']} checks, {summary['failures']} failures)")
                for observation in observations:
                    if observation.get("passed") is False:
                        print("FAIL: " + observation["label"], file=sys.stderr)
            print(result.stderr, end="", file=sys.stderr)
        if args.output:
            args.output.write_text(json.dumps({"source_ref": args.source_ref,
                "source_sha256": {name: hashlib.sha256(value.encode("utf-8")).hexdigest() for name, value in source.items()},
                "runs": runs, "compiler": args.cxx, "scope": __doc__}, indent=2) + "\n", encoding="utf-8")
        raise SystemExit(1 if any(run["returncode"] for run in runs) else 0)


if __name__ == "__main__":
    main()
