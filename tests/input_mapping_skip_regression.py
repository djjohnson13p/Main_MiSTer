#!/usr/bin/env python3
"""Exercise intact production normal-mapping skip/capture and adjacent controls.

Synthetic devices supply loaded maps, discovery, hardware and persistence;
actual initialization, prefix, binding/normalization, combo, all mapping branches,
skip loop and hardware skip dispatcher execute. Every scenario has a fresh
process, so the real static duplicate-key latch is not reset by test magic.
--source-ref REF runs historical production with the current assertions.
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


def generate(cpp, header, hardware, user_header, generator):
    types, production = generator.generate(cpp, header, hardware)
    types += re.search(r"^void start_map_setting\([^\n]+", header, re.M).group() + "\n"
    types += re.search(r"^#define BUTTON_DPAD_COUNT[^\n]+", header, re.M).group() + "\n"
    types += re.search(r"^#define UIO_KEYBOARD[^\n]+", user_header, re.M).group() + "\n"
    types += "using uint = unsigned int;\n#ifndef KEY_MENU\n#define KEY_MENU 139\n#define KEY_HOMEPAGE 172\n#endif\n"
    types += re.search(r"^static[^\n]*\bosdbtn\s*=[^\n]+", cpp, re.M).group() + "\n"
    declaration = re.search(r"^\s*static int key_mapped[^\n]+", cpp, re.M).group().strip()
    production = production.replace("void capture_event(input_event *ev, int dev)\n{\n",
        "void capture_event(input_event *ev, int dev)\n{\ninput_absinfo *absinfo = &fixture_absinfo;\n" + declaration + "\n")
    closing = production.rfind("\n}\n", 0, production.index("\nint inventory_changed()") + 1)
    production = (production[:closing] + "\n"
        + generator.block(cpp, "if (mapping && (mapping_dev >= 0 || ev->value)", "input.cpp") + "\n" + production[closing:])
    production += generator.block(cpp, "void send_map_cmd(", "input.cpp")
    return types, production


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--source-ref")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    def read(name):
        if args.source_ref:
            return subprocess.check_output(["git", "-C", str(ROOT), "show", f"{args.source_ref}:{name}"], text=True, encoding="utf-8")
        return (ROOT / name).read_text(encoding="utf-8")
    spec = importlib.util.spec_from_file_location("event_generator", ROOT / "tests/input_remap_regression.py")
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    cpp, header = read("input.cpp"), read("input.h")
    types, production = generate(cpp, header, read("hardware.cpp"), read("user_io.h"), generator)
    fixture = (ROOT / "tests/input_remap_regression.cpp").read_text(encoding="utf-8")
    fixture = fixture.replace("struct input_absinfo { int value; };", "struct input_absinfo { int value, minimum, maximum; };")
    fixture = fixture.replace('#include "input_remap_production.inc"', '''char joy_bnames[NUMBUTTONS][32] = {};
static input_absinfo fixture_absinfo = {};
void spi_uio_cmd(int) {}
void capture_event(input_event *, int);
void input_cb(input_event *event, input_absinfo *, int dev) { capture_event(event, dev); }
#include "input_remap_production.inc"''')
    fixture = fixture.replace("int main()\n{", "int existing_main()\n{", 1)
    fixture += (ROOT / "tests/input_mapping_skip_regression.cpp").read_text(encoding="utf-8")
    command = [args.cxx] if Path(args.cxx).is_file() else shlex.split(args.cxx, posix=os.name != "nt")
    environment = os.environ.copy()
    if Path(command[0]).is_file():
        environment["PATH"] = str(Path(command[0]).resolve().parent) + os.pathsep + environment.get("PATH", "")
    scenarios = []
    for alternative in (0, 1):
        for slot in (0, 4):
            for control, reverse in (("menu", 0), ("menu", 1), ("single-menu", 0), ("space", 0), ("queued-space", 0), ("queued-space-repeat", 0)):
                if control.startswith("queued-") and slot != 4:
                    continue
                if control == "space" and slot == 0 and not alternative:
                    continue  # No selected controller exists at first base slot.
                scenarios.append((control, alternative, slot, reverse))
    scenarios += [(control, alternative, 0, 0) for control, alternative in
        (("placeholder", 0), ("placeholder", 1), ("hardware-skip", 0), ("hardware-skip", 1),
         ("core-definition", 0), ("negative-skip", 0), ("core-calibration", 0), ("keyboard-remap", 0), ("existing-suite", 0))]
    with tempfile.TemporaryDirectory(prefix="mister-mapping-skip-") as folder:
        work = Path(folder)
        for name, text in {"input_remap_types.inc": types, "input_remap_production.inc": production, "fixture.cpp": fixture}.items():
            (work / name).write_text(text, encoding="utf-8")
        executable = work / ("mapping.exe" if os.name == "nt" else "mapping")
        subprocess.run(command + ["-std=gnu++14", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable", "-Wno-unused-but-set-variable",
            "-I", str(ROOT), "-I", str(work), str(work / "fixture.cpp"), "-o", str(executable)], env=environment, check=True)
        runs, total, failed, fixture_total, fixture_failed = [], 0, 0, 0, 0
        for control, alternative, slot, reverse in scenarios:
            result = subprocess.run([str(executable), control, str(alternative), str(slot), str(reverse)], capture_output=True, text=True, env=environment)
            observations = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
            if control != "existing-suite":
                summary = observations[-1]
                total += summary["checks"]; failed += summary["failures"]
                fixture_total += summary["fixture_checks"]; fixture_failed += summary["fixture_failures"]
                print(f"{'FAIL' if result.returncode else 'PASS'}: {control} alt{alternative} slot{slot} reverse{reverse} ({summary['checks']} focused checks, {summary['failures']} failures; {summary['fixture_checks']} fixture checks, {summary['fixture_failures']} failures)")
                for observation in observations:
                    if observation.get("passed") is False:
                        print("FAIL: " + observation["label"], file=sys.stderr)
            else:
                print(result.stdout, end="")
            print(result.stderr, end="", file=sys.stderr)
            runs.append({"control": control, "alternative": bool(alternative), "slot": slot, "reverse_release": bool(reverse),
                "returncode": result.returncode, "observations": observations, "stdout": result.stdout, "stderr": result.stderr})
        print(f"{'FAIL' if any(run['returncode'] for run in runs) else 'PASS'}: production normal mapping skip ({total} focused checks, {failed} failures; {fixture_total} fixture checks, {fixture_failed} failures)")
        if args.output:
            args.output.write_text(json.dumps({"source_ref": args.source_ref, "input_sha256": hashlib.sha256(cpp.encode()).hexdigest(),
                "checks": total, "failures": failed, "fixture_checks": fixture_total, "fixture_failures": fixture_failed,
                "runs": runs, "scope": __doc__}, indent=2) + "\n", encoding="utf-8")
        raise SystemExit(1 if any(run["returncode"] for run in runs) else 0)


if __name__ == "__main__":
    main()
