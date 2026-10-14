#!/usr/bin/env python3
"""Test production Info/video/SPD paths using simulated SPI and I2C requests.

Run: python3 tests/video_spd_regression.py --cxx g++
Use --source-ref <commit> to demonstrate the pre-fix menu-only request counts.
No elapsed-time or hardware performance assertions are made.
"""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from video_vrr_regression import block, generate


ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "normal_popup", "normal_menu", "normal_quirk1", "normal_quirk2", "normal_quirk3",
    "direct_popup", "direct_quirk1", "direct_quirk2", "direct_quirk3",
    "rtg_popup", "rtg_quirk1", "rtg_quirk2", "rtg_quirk3",
    "freesync_popup", "vesa_popup", "normal_forced", "direct_forced", "rtg_forced",
    "normal_resolution", "direct_resolution", "rtg_resolution",
    "normal_reset", "direct_reset", "rtg_reset", "freesync_reset", "vesa_reset",
    "rtg_switch",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--source-ref", help="Read production definitions from this Git ref")
    parser.add_argument("--only", choices=CASES)
    parser.add_argument("--report", type=Path, help="Write request counts and case results as JSON")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    def read_source(name):
        if args.source_ref:
            return subprocess.check_output(["git", "-C", str(ROOT), "show", args.source_ref + ":" + name],
                                           text=True, encoding="utf-8")
        return (ROOT / name).read_text(encoding="utf-8")

    video = read_source("video.cpp")
    menu = read_source("menu.cpp")
    io = read_source("user_io.h")
    state, _ = generate(video)
    state += block(menu, "enum MENU").replace('"video.cpp"', '"menu.cpp"')
    for name in ("UIO_SET_VIDEO", "UIO_GET_VRES", "UIO_GET_FB_PAR"):
        state += re.search(r"^#define " + name + r"\b[^\n]*", io, re.MULTILINE).group() + "\n"
    functions = block(menu, "int menu_present()").replace('"video.cpp"', '"menu.cpp"')
    functions += block(menu, "void Info(const char *message, int timeout, int width, int height, int frame)").replace('"video.cpp"', '"menu.cpp"')
    for signature in (
        "bool video_is_direct()", "static int video_vsync_adjust()", "static bool supports_vrr()",
        "static void hdmi_packet_enable(uint8_t mask, bool enable)",
        "static void hdmi_packet_set_data(uint8_t mask, uint8_t offset, uint8_t *data, int size)",
        "static void hdmi_config_init()", "static void hdmi_config_set_hdr()",
        "static void hdmi_invalidate_mode_cache()", "static void hdmi_config_set_mode(vmode_custom_t *vm)",
        "static bool get_video_info(bool force, VideoInfo *video_info)",
        "static void set_vrr_mode()", "static void video_set_mode(vmode_custom_t *v, double Fpix)",
        "static void spd_config_update()",
    ):
        functions += block(video, signature)
    functions += re.search(r"^#define fr_constrain[^\n]*", video, re.MULTILINE).group() + "\n"
    functions += block(video, "void video_mode_adjust(bool force)")
    functions += block(video, "void video_reinit()")

    environment = os.environ.copy()
    compiler = Path(args.cxx)
    if compiler.is_file():
        environment["PATH"] = str(compiler.resolve().parent) + os.pathsep + environment.get("PATH", "")
    results = []
    with tempfile.TemporaryDirectory(prefix="mister-video-spd-") as temp:
        temp = Path(temp)
        (temp / "video_spd_state.inc").write_text(state, encoding="utf-8")
        (temp / "video_spd_functions.inc").write_text(functions, encoding="utf-8")
        executable = temp / ("video_spd_regression.exe" if os.name == "nt" else "video_spd_regression")
        subprocess.run([args.cxx, "-std=c++14", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable",
                        "-I" + str(ROOT), "-I" + str(temp), str(ROOT / "tests/video_spd_regression.cpp"),
                        "-o", str(executable)], check=True, env=environment)
        for case in ([args.only] if args.only else CASES):
            result = subprocess.run([str(executable), case], capture_output=True, text=True, env=environment)
            counts = []
            for line in result.stdout.splitlines():
                if line.startswith("REQUESTS "):
                    fields = line.split()
                    counts.append({"event": fields[1], **{key: int(value) for key, value in
                                   (field.split("=", 1) for field in fields[2:])}})
                if args.verbose or line.startswith(("REQUESTS ", "PASS:")):
                    print(line)
            if result.stderr:
                print(result.stderr, end="", file=sys.stderr)
            results.append({"case": case, "passed": result.returncode == 0, "counts": counts})
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps({"source": args.source_ref or "working tree", "cases": results}, indent=2) + "\n", encoding="utf-8")
    failures = sum(not result["passed"] for result in results)
    print("SPD regression: %d passed, %d failed; simulated requests, no hardware timing." % (len(results) - failures, failures))
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
