#!/usr/bin/env python3
"""Compile the current video.cpp HDMI/VRR functions against simulated I2C.

Requires Python 3 and a host C++ compiler. Example:
    python3 tests/video_vrr_regression.py --cxx g++

--source can point to an upstream video.cpp to demonstrate the regression.
No generated files are retained in the checkout.
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


def block(source, declaration):
    """Extract an intact top-level definition using this file's brace style."""
    match = re.search(
        r"^" + re.escape(declaration) + r"[ \t]*\n?[ \t]*\{.*?^\};?",
        source,
        re.MULTILINE | re.DOTALL,
    )
    if not match:
        raise ValueError("Production definition not found: " + declaration)
    line = source.count("\n", 0, match.start()) + 1
    return '#line %d "video.cpp"\n%s\n' % (line, match.group())


def declaration(source, name):
    match = re.search(r"^static [^\n;]*\b" + re.escape(name) + r"\b[^\n]*;$", source, re.MULTILINE)
    if not match:
        raise ValueError("Production state not found: " + name)
    return match.group() + "\n"


def generate(source):
    state = ""
    for macro in ("VRR_NONE", "VRR_FREESYNC", "VRR_VESA", "VRR_MISTER",
                  "FB_DV_LBRD", "FB_DV_RBRD", "FB_DV_UBRD", "FB_DV_BBRD",
                  "hdmi_spd_config", "hdmi_spare_config"):
        match = re.search(r"^#define " + macro + r"\b[^\n]*", source, re.MULTILINE)
        if not match:
            raise ValueError("Production macro not found: " + macro)
        state += match.group() + "\n"
    for definition in ("struct vrr_cap_t", "struct vmode_custom_param_t", "struct vmode_custom_t",
                       "static vrr_cap_t vrr_modes[] ="):
        state += block(source, definition)
    for name in ("hdmi_main_fd", "hdmi_edid_fd", "hdmi_spd_fd", "use_vrr", "use_freesync_spd",
                 "vrr_min_fr", "vrr_max_fr", "minimig_rtg_hdmi", "hdmi_power", "hdmi_need_init",
                 "last_vrr_mode", "last_vrr_rate", "last_vrr_vfp", "edid_version", "support_FHD",
                 "v_cur", "last_sync_invert", "last_pr_flags", "last_vic_mode"):
        state += declaration(source, name)
    functions = ""
    for definition in (
        "bool video_is_direct()",
        "static int video_vsync_adjust()",
        "static bool supports_vrr()",
        "static void hdmi_packet_enable(uint8_t mask, bool enable)",
        "static void hdmi_packet_set_data(uint8_t mask, uint8_t offset, uint8_t *data, int size)",
        "static void hdmi_config_init()",
        "static void hdmi_config_set_hdr()",
        "static void hdmi_invalidate_mode_cache()",
        "static void hdmi_config_set_mode(vmode_custom_t *vm)",
        "static void set_vrr_mode()",
        "static void video_set_mode(vmode_custom_t *v, double Fpix)",
        "static void spd_config_update()",
        "void video_reinit()",
    ):
        functions += block(source, definition)
    return state, functions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--source", type=Path, default=ROOT / "video.cpp")
    parser.add_argument("--verbose", action="store_true", help="Show production video logging")
    parser.add_argument("--only", choices=("freesync", "vesa"), help="Run one forced-mode case for baseline comparison")
    args = parser.parse_args()
    state, functions = generate(args.source.read_text(encoding="utf-8"))
    with tempfile.TemporaryDirectory(prefix="mister-video-vrr-") as output:
        output = Path(output)
        (output / "video_vrr_state.inc").write_text(state, encoding="utf-8")
        (output / "video_vrr_functions.inc").write_text(functions, encoding="utf-8")
        executable = output / ("video_vrr_regression.exe" if os.name == "nt" else "video_vrr_regression")
        command = shlex.split(args.cxx, posix=os.name != "nt") if not Path(args.cxx).is_file() else [args.cxx]
        environment = os.environ.copy()
        if Path(command[0]).is_file():
            # Portable Windows toolchains need their DLL directory for cc1plus
            # and for the generated executable. Keep this process-local.
            environment["PATH"] = str(Path(command[0]).resolve().parent) + os.pathsep + environment.get("PATH", "")
        command += ["-std=c++14", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable",
                    "-I" + str(ROOT), "-I" + str(output),
                    str(ROOT / "tests" / "video_vrr_regression.cpp"), "-o", str(executable)]
        subprocess.run(command, check=True, env=environment)
        test_command = [str(executable)] + ([args.only] if args.only else [])
        result = subprocess.run(test_command, capture_output=True, text=True, env=environment)
        if args.verbose or result.returncode:
            print(result.stdout, end="")
        else:
            for line in result.stdout.splitlines():
                if line.startswith(("PASS:", "All ")):
                    print(line)
        if result.stderr:
            print(result.stderr, end="", file=sys.stderr)
        result.check_returncode()


if __name__ == "__main__":
    main()
