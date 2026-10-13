#!/usr/bin/env python3
"""Compile and test the actual Menu datetime formatter, without FPGA hardware."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    args = parser.parse_args()
    child_env = dict(os.environ)
    compiler_dir = Path(args.cxx).resolve().parent
    child_env["PATH"] = str(compiler_dir) + os.pathsep + child_env.get("PATH", "")
    root = Path(__file__).resolve().parents[1]
    source = (root / "menu.cpp").read_text(encoding="utf-8")
    system_format = re.search(r'if \(fread\(str, 6, 1, f\)\) sprintf\(s, "([^"]+)"', source).group(1)
    main_source = (root / "main.cpp").read_text(encoding="utf-8")
    suffix = re.search(r'const char \*version = "\$VER:" VDATE "([^"]+)";', main_source).group(1)
    system_line = system_format.replace("%s", "261013" + suffix, 1).replace("%s", "261013", 1)
    assert len(system_line) <= 29, "Main and OS versions must fit the OSD sidestripe row"
    start = source.index("static void menu_format_datetime(")
    end = source.index("\nvoid HandleUI(void)", start)
    production_function = source[start:end]
    harness = r'''
#include <cassert>
#include <clocale>
#include <cstdio>
#include <cstring>
#include <ctime>
#include "cfg.h"
cfg_t cfg = {};
'''
    cases = r'''
int main() {
    std::setlocale(LC_TIME, "C");
    std::tm clock = {};
    clock.tm_year = 126; clock.tm_mon = 9; clock.tm_mday = 13;
    clock.tm_wday = 2; clock.tm_min = 5; clock.tm_sec = 9;
    struct Case { int hour; const char *normal; const char *twelve; };
    const Case cases[] = {
        {0, "Oct 13 Tu 00:05:09", "Oct 13 12:05:09 AM"},
        {11, "Oct 13 Tu 11:05:09", "Oct 13 11:05:09 AM"},
        {12, "Oct 13 Tu 12:05:09", "Oct 13 12:05:09 PM"},
        {23, "Oct 13 Tu 23:05:09", "Oct 13 11:05:09 PM"},
    };
    for (const auto &c : cases) {
        clock.tm_hour = c.hour;
        for (int mode = 0; mode <= 1; ++mode) {
            cfg.osd_clock_12h = mode;
            char output[32] = " MiSTer      ";
            menu_format_datetime(output + std::strlen(output),
                                 sizeof(output) - std::strlen(output), &clock);
            assert(std::strcmp(output + 13, mode ? c.twelve : c.normal) == 0);
            assert(std::strlen(output) == 31);
            // Keep all four icon positions before the date and fit a 32-column OSD.
            output[8] = 0x1d; output[9] = 0x1c; output[10] = 4; output[11] = 0x95;
            assert(std::strcmp(output + 13, mode ? c.twelve : c.normal) == 0);
        }
    }
    cfg.osd_clock_12h = 0;
    char tiny[2] = {'x', 'x'};
    menu_format_datetime(tiny, sizeof(tiny), &clock);
    std::puts("PASS: Menu clock midnight/noon/AM/PM, original layout, icon space and small buffer");
}
'''
    with tempfile.TemporaryDirectory(prefix="mister-clock-") as directory:
        test_source = Path(directory) / "clock.cpp"
        executable = Path(directory) / ("clock.exe" if os.name == "nt" else "clock")
        test_source.write_text(harness + production_function + cases, encoding="utf-8")
        subprocess.run([args.cxx, "-std=gnu++14", "-Wall", "-Wextra", "-Werror",
                        "-I", str(root), str(test_source), "-o", str(executable)], check=True, env=child_env)
        subprocess.run([str(executable)], check=True, env=child_env)


if __name__ == "__main__":
    main()
