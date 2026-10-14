#!/usr/bin/env python3
"""Exercise production physical Menu guard and Advanced capture completion.

No real Bluetooth/device calls. hci_get_route records a query and reports no
adapter, so the complete production no-Bluetooth branch executes for normal
pairing. Capture must remain protected; each scenario uses a fresh process.
--source-ref REF runs historical production with the current desired behavior.
Native Linux headers are used on Linux; otherwise --linux-codes or the local
ARM toolchain supplies the real Linux event-code header.
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
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SCENARIOS = ("protected-remap", "protected-keyboard", "advanced-input", "advanced-output",
             "advanced-held-input", "advanced-held-output", "advanced-mask", "normal-outside", "capture-short",
             "escape-clear-input", "escape-clear-output", "controller-clear", "scripts-hold")


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--source-ref")
    parser.add_argument("--linux-codes", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    def read(name):
        if args.source_ref:
            return subprocess.check_output(["git", "-C", str(ROOT), "show", f"{args.source_ref}:{name}"], text=True, encoding="utf-8")
        return (ROOT / name).read_text(encoding="utf-8")

    source = {name: read(name) for name in ("input.cpp", "input.h", "hardware.cpp", "menu.cpp", "user_io.cpp", "user_io.h", "osd.h", "brightness.h")}
    event_generator = module("event_generator", ROOT / "tests/input_remap_regression.py")
    handoff = module("handoff_generator", ROOT / "tests/input_remap_handoff_regression.py")
    intact_block = event_generator.block
    def optional_feedback_block(text, marker, filename, *extra):
        # Historical production predates source feedback; its actual renderer
        # never calls these optional definitions. No substitute logic is added.
        if marker in ("void get_map_source_name(", "static bool menu_show_map_source(") and marker not in text:
            return ""
        return intact_block(text, marker, filename, *extra)
    event_generator.block = optional_feedback_block
    types, keys, capture, user_io, ui = handoff.generate(source, event_generator)
    types += re.search(r"^void start_map_setting\([^\n]+", source["input.h"], re.M).group() + "\n"
    menu = source["menu.cpp"]
    ui = ui[:ui.index("static void tick_ui()")]
    prefix_start = menu.index("\t// decode and set events", menu.index("void HandleUI("))
    prefix_end = menu.index("\n\tif (select_ini)", prefix_start)
    ui += "static void tick_ui()\n{\n bool select = false;\n uint32_t c = menu_key_get(); ui_prefix_key = c;\n"
    ui += f'#line {menu.count(chr(10), 0, prefix_start) + 1} "menu.cpp"\n' + menu[prefix_start:prefix_end]
    ui += "\n switch (menustate) {\n"
    for first, last in (("\n\tcase MENU_JOYKBDMAP:", "\n\tcase MENU_ABOUT1:"),
                        ("\n\tcase MENU_ADVANCED_MAP_LIST1:", "\n\tcase MENU_ATARI8BIT_CART1:"),
                        ("\n\tcase MENU_BTPAIR2:", "\n\tcase MENU_SCRIPTS:")):
        ui += event_generator.region(menu, first, last, "menu.cpp")
    ui += "\n default: break;\n }\n}\n"
    for text, marker in ((menu, "#define STD_EXIT"), (source["osd.h"], "#define DISABLE_KEYBOARD")):
        types += next(line for line in text.splitlines() if line.startswith(marker)) + "\n"
    command = [args.cxx] if Path(args.cxx).is_file() else shlex.split(args.cxx, posix=os.name != "nt")
    environment = os.environ.copy()
    if Path(command[0]).is_file():
        environment["PATH"] = str(Path(command[0]).resolve().parent) + os.pathsep + environment.get("PATH", "")
    target = subprocess.check_output(command + ["-dumpmachine"], text=True, env=environment).strip()
    fixture = (ROOT / "tests/input_remap_regression.cpp").read_text(encoding="utf-8")
    fixture = fixture.replace('#include "cfg.h"', '#include "cfg.h"\n#include "str_util.h"\n#include <utility>')
    linux_codes = None
    if "linux" not in target:
        codes = args.linux_codes or handoff.local_linux_codes(ROOT)
        if not codes:
            raise ValueError("Non-Linux host requires locally discovered Linux event header")
        linux_codes = codes.read_text(encoding="utf-8")
        first, last = fixture.index("#ifdef __linux__"), fixture.index('#include "input_remap_types.inc"')
        fixture = fixture[:first] + '#include "input-event-codes.h"\nstruct input_event { uint16_t type, code; int32_t value; };\nstruct input_absinfo { int value; };\n' + fixture[last:]
    fixture = fixture.replace("struct DeviceFixture\n{", "struct DeviceFixture\n{\n uint16_t vid, pid;\n char name[128];")
    fixture = fixture.replace("struct DeviceFixtures\n{", "using devInput = DeviceFixture;\nstruct DeviceFixtures\n{")
    fixture = fixture.replace("static std::vector<int> cleared;", "static std::vector<int> cleared;\nstatic std::vector<int> normal_map_saves;")
    fixture = fixture.replace("void user_io_kbd(uint16_t key, int press) { if (key == KEY_ENTER && !press) released_enter++; }", "void user_io_kbd(uint16_t, int);")
    fixture = fixture.replace("bool do_delete)\n{\n\tif (!do_delete) std::abort();\n\tcleared.push_back(device);", "bool do_delete = false)\n{\n if (do_delete) cleared.push_back(device); else normal_map_saves.push_back(device);")
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
    support = (ROOT / "tests/input_remap_handoff_regression.cpp").read_text(encoding="utf-8").split("// Combined UI/event assertions.\n")[0]
    extra, scenarios = (ROOT / "tests/advanced_bt_guard_regression.cpp").read_text(encoding="utf-8").split("// PROBE_SCENARIOS\n")
    support = support.replace("int user_io_menu_button() { return 0; }", "int user_io_menu_button() { return physical_menu_button; }")
    support = "static int physical_menu_button = 0;\n" + support + extra.replace("static int physical_menu_button = 0;\n", "")
    fixture = fixture.replace("int main()\n{", "int existing_main()\n{", 1) + scenarios
    with tempfile.TemporaryDirectory(prefix="mister-advanced-bt-") as temporary:
        work = Path(temporary)
        files = {"input_remap_types.inc": types, "keycode_production.inc": keys, "input_remap_production.inc": capture,
                 "user_io_kbd_production.inc": user_io, "menu_handoff_production.inc": ui, "handoff_support.inc": support, "proof.cpp": fixture}
        if linux_codes is not None:
            files["input-event-codes.h"] = linux_codes
        for name, text in files.items():
            (work / name).write_text(text, encoding="utf-8")
        executable = work / ("bt.exe" if os.name == "nt" else "bt")
        subprocess.run(command + ["-std=gnu++14", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable", "-Wno-unused-but-set-variable",
            "-I", str(ROOT), "-I", str(work), str(work / "proof.cpp"), "-o", str(executable)], check=True, env=environment)
        runs = []
        for scenario in SCENARIOS:
            result = subprocess.run([str(executable), scenario], capture_output=True, text=True, env=environment)
            observations = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
            runs.append({"scenario": scenario, "returncode": result.returncode, "observations": observations, "stdout": result.stdout, "stderr": result.stderr})
            print(f"{'FAIL' if result.returncode else 'PASS'}: {scenario}")
            print(result.stderr, end="")
            for row in observations:
                if row.get("passed") is False:
                    print("FAIL: " + row["label"])
        report = {"source_ref": args.source_ref, "source_sha256": {name: hashlib.sha256(text.encode()).hexdigest() for name, text in source.items()},
                  "scope": __doc__, "compiler": args.cxx, "runs": runs}
        report["focused_checks"] = sum(run["observations"][-1]["checks"] for run in runs)
        report["fixture_checks"] = sum(run["observations"][-1]["fixture_checks"] for run in runs)
        report["focused_failures"] = sum(run["observations"][-1]["failures"] for run in runs)
        report["fixture_failures"] = sum(run["observations"][-1]["fixture_failures"] for run in runs)
        print(f"{'FAIL' if any(run['returncode'] for run in runs) else 'PASS'}: production Advanced Bluetooth guard ({report['focused_checks']} focused checks, {report['focused_failures']} failures; {report['fixture_checks']} fixture checks, {report['fixture_failures']} failures)")
        if args.output:
            args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        raise SystemExit(1 if any(run["returncode"] for run in runs) else 0)


if __name__ == "__main__":
    main()
