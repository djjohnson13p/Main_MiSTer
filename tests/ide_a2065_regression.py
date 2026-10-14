#!/usr/bin/env python3
"""Exercise production IDE waits and A2065 mailbox draining on a finite host fixture.

Run with a host C++ compiler; --source-ref demonstrates historical behavior.
SPI readiness depends on the CSR shadow produced by intact a2065_poll and
chip_wput definitions. A finite SPI spy budget reports the old mutual wait
as an observed-state failure, never as a test timeout. No hardware timing,
ARM fence effectiveness, or physical DDR3 atomicity is established here.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
HOST = "support/minimig/minimig_a2065.cpp"
REGISTERS = "support/minimig/minimig_a2065_registers.cpp"
SOURCES = ("ide.cpp", "ide.h", "user_io.h", HOST, REGISTERS,
           "support/minimig/minimig_a2065_types.h",
           "support/minimig/minimig_a2065_boardram.h",
           "support/minimig/minimig_a2065_ddr3.h")
EXPECTED_CASES = tuple(
    [prefix + "-misses-" + str(count) for prefix in ("read", "write") for count in (0, 15, 16, 32)] +
    [prefix + suffix for prefix in ("read", "write") for suffix in
     ("-port-mask", "-non5-byte-response", "-card-off", "-queued-csr0-ack")] +
    ["poll-pending-tint-index-" + str(index) for index in (11, 0xffffffff)] +
    ["packing-index-" + str(index) + "-flags-" + str(flags)
     for index in (0, 1, 0x80000000, 0xffffffff) for flags in range(4)]
)


def definition(source, signature, filename):
    """Extract a complete definition, including one-line accessor functions."""
    match = re.search(r"^" + re.escape(signature) + r"\s*\{", source, re.M)
    if not match:
        raise ValueError("Missing production definition: " + signature)
    start = match.end() - 1
    depth = 0
    token = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)
    for item in token.finditer(source, start):
        if item.group() == "{":
            depth += 1
        elif item.group() == "}":
            depth -= 1
            if not depth:
                line = source.count("\n", 0, match.start()) + 1
                return '#line %d "%s"\n%s\n' % (line, filename, source[match.start():item.end()])
    raise ValueError("Unclosed production definition: " + signature)


def declarations(source, names):
    output = []
    for name in names:
        match = re.search(r"^static [^\n;]*\b" + re.escape(name) + r"\b[^\n;]*;", source, re.M)
        if not match:
            raise ValueError("Missing production state: " + name)
        if match.group() not in output:
            output.append(match.group())
    return "\n".join(output) + "\n"


def generate(source):
    host, registers, ide = source[HOST], source[REGISTERS], source["ide.cpp"]
    config = definition(source["ide.h"], "struct ide_config", "ide.h")
    if not re.search(r"\buint32_t\s+bitoff\s*;", config):
        raise ValueError("Minimal fixture bitoff type differs from production ide_config")
    state = declarations(host, ("map", "card_up", "cmd_rd_idx", "mode", "prom_active", "rx_batching"))
    state += declarations(registers, ("csr", "rap", "am_initialized", "am_mode", "am_ladrf",
                                      "am_rdr_rlen", "am_tdr_tlen", "tdr_offset", "fakemac",
                                      "on_interrupt", "on_transmit"))
    state += re.search(r"^enum \{ MODE_DIRECT[^\n]*", host, re.M).group() + "\n"
    for macro in ("RX_BATCH_MAX", "CMD_DRAIN_MAX"):
        state += re.search(r"^#define " + macro + r"\b[^\n]*", host, re.M).group() + "\n"
    state += re.search(r"^#define UIO_DMA_SDIO\b[^\n]*", source["user_io.h"], re.M).group() + "\n"
    state += re.search(r"^#define RAM_MASK\b[^\n]*", source["support/minimig/minimig_a2065_boardram.h"], re.M).group() + "\n"
    production = ""
    for signature in ("uint16_t registers_csr0(void)", "uint16_t registers_csr(int n)",
                      "uint16_t registers_mode(void)", "static void chip_init_mask(void)",
                      "void rethink(void)", "void chip_wput(uint8_t reg_offset, uint16_t v)"):
        production += definition(registers, signature, REGISTERS)
    # Observe memcpy at the memory boundary, keeping rd64 and wr64 intact.
    production += "#define memcpy observed_memcpy\n"
    for signature in ("static uint64_t rd64(unsigned long off)",
                      "static void wr64(unsigned long off, uint64_t val)"):
        production += definition(host, signature, HOST)
    production += "#undef memcpy\n"
    for signature in ("static uint32_t rd_wptr(void)",
                      "static void ring_read_entry(uint32_t idx, uint8_t *rap_out, uint16_t *data_out)",
                      "static void push_csr_shadow(void)", "static void update_int_state(void)",
                      "static void on_interrupt_cb(void)", "static void a2065_drain_rx(void)",
                      "void a2065_poll(void)"):
        production += definition(host, signature, HOST)
    production += "static void traced_a2065_poll() { observe_poll(); a2065_poll(); }\n"
    production += definition(ide, "uint16_t ide_check()", "ide.cpp")
    helper_present = bool(re.search(r"^static uint16_t ide_wait_req\(", ide, re.M))
    if helper_present:
        production += "#define a2065_poll traced_a2065_poll\n"
        production += definition(ide, "static uint16_t ide_wait_req(ide_config *ide)", "ide.cpp")
        production += "#undef a2065_poll\n"
    else:
        # Referencing this observer also keeps the baseline strict-warning build
        # honest; this pointer is never called to emulate the missing fix.
        production += "void (*baseline_unused_poll_observer)() = traced_a2065_poll;\n"
    for name, signature in (("read", "static void process_read(ide_config *ide, int multi)"),
                            ("write", "static void process_write(ide_config *ide, int multi)")):
        function = definition(ide, signature, "ide.cpp")
        pattern = (r"ide_req = ide_wait_req\(ide\);" if helper_present else
                   r"ide_req = 0;\s*while \(!ide_req\) ide_req = \(ide_check\(\) >> ide->bitoff\) & 7;")
        statement = re.search(pattern, function)
        if not statement:
            raise ValueError("Production IDE wait call site changed: " + name)
        production += "static uint16_t wait_from_%s(ide_config *ide) {\nuint16_t ide_req;\n%s\nreturn ide_req;\n}\n" % (name, statement.group())
    return state, production, helper_present


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--source-ref")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    source = {name: subprocess.check_output(["git", "-C", str(ROOT), "show", args.source_ref + ":" + name],
                                           text=True, encoding="utf-8") if args.source_ref else
              (ROOT / name).read_text(encoding="utf-8") for name in SOURCES}
    state, production, helper_present = generate(source)
    environment = os.environ.copy()
    compiler = [args.cxx] if Path(args.cxx).is_file() else shlex.split(args.cxx, posix=os.name != "nt")
    if Path(compiler[0]).is_file():
        environment["PATH"] = str(Path(compiler[0]).resolve().parent) + os.pathsep + environment.get("PATH", "")
    with tempfile.TemporaryDirectory(prefix="mister-ide-a2065-") as directory:
        output = Path(directory)
        for filename in ("minimig_a2065_types.h", "minimig_a2065_ddr3.h"):
            (output / filename).write_text(source["support/minimig/" + filename], encoding="utf-8")
        (output / "ide_a2065_state.inc").write_text(state, encoding="utf-8")
        (output / "ide_a2065_production.inc").write_text(production, encoding="utf-8")
        executable = output / ("ide_a2065_regression.exe" if os.name == "nt" else "ide_a2065_regression")
        command = compiler + ["-std=c++14", "-Wall", "-Wextra", "-Werror", "-O2", "-I" + str(output),
                              str(ROOT / "tests/ide_a2065_regression.cpp"), "-o", str(executable)]
        subprocess.run(command, check=True, env=environment)
        result = subprocess.run([str(executable)], capture_output=True, text=True, env=environment, timeout=30)
        print(result.stdout, end="")
        if result.stderr:
            print(result.stderr, end="")
        cases = [json.loads(line[5:]) for line in result.stdout.splitlines() if line.startswith("JSON ")]
        case_names = [case["case"] for case in cases]
        passed = sum(case["passed"] is True for case in cases)
        failed = sum(case["passed"] is False for case in cases)
        summary = re.search(r"^IDE/A2065 regression: (\d+) passed, (\d+) failed;", result.stdout, re.M)
        coverage_complete = (len(cases) == len(EXPECTED_CASES) == 34 and
                             len(set(case_names)) == len(cases) and set(case_names) == set(EXPECTED_CASES) and
                             passed + failed == len(cases) and summary is not None and
                             tuple(map(int, summary.groups())) == (passed, failed))
        report = {
            "source_ref": args.source_ref or "working tree", "source_root": str(ROOT),
            "source_hash_basis": "UTF-8 source text with CRLF normalized to LF; test hashes use raw file bytes",
            "source_sha256": {name: hashlib.sha256(value.encode("utf-8")).hexdigest() for name, value in source.items()},
            "test_sha256": {name: hashlib.sha256((ROOT / "tests" / name).read_bytes()).hexdigest()
                            for name in ("ide_a2065_regression.py", "ide_a2065_regression.cpp")},
            "extracted_sha256": {"state": hashlib.sha256(state.encode()).hexdigest(),
                                 "production": hashlib.sha256(production.encode()).hexdigest()},
            "helper_present": helper_present,
            "minimal_ide_config_bitoff_type": "uint32_t; verified against selected production ide.h",
            "full_barrier_present": "__sync_synchronize();" in definition(source[HOST], "static void update_int_state(void)", HOST),
            "compiler_command": command, "exit_code": result.returncode,
            "coverage_complete": coverage_complete, "expected_case_count": len(EXPECTED_CASES),
            "passed_count": passed, "failed_count": failed,
            "all_passed": result.returncode == 0 and coverage_complete and failed == 0,
            "cases": cases, "hardware_tested": False,
            "limits": "Simulated SPI, DDR3 and network boundaries; no ARM execution, physical DDR3 ordering/atomicity, FPGA execution, network blocking, guest timing or performance measurements. Call-site wrappers contain extracted waits only, not complete IDE data-transfer functions."
        }
        if args.report:
            args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        return 0 if report["all_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
