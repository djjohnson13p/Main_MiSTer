#!/usr/bin/env python3
"""Ordinary PC88 module and separately labeled generic-write branch boundary.

Preparation requires review before execution. No real image/SPI/device calls.
Historical refs without the API explicitly exclude direct-module observations.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
NAMES = ("support/pc88/pc88.cpp", "support/pc88/pc88.h", "user_io.cpp", "user_io.h",
         "file_io.cpp", "file_io.h", "spi.h", "fpga_io.h", "support.h")
SIZES = (512, 1024, 1536, 16384)
CATALOG = ([f"{group}_{disk}_{wide}_{size}" for group in ("write", "read")
            for disk in (0, 1) for wide in (0, 1) for size in SIZES]
           + [f"{group}_{disk}_{wide}" for group in ("growth", "partial", "lifecycle")
              for disk in (0, 1) for wide in (0, 1)]
           + ["independent_0", "independent_1", "non-pc88", "original-name-decline",
              "slot2", "slot15", "unmounted", "op0", "original-casefold",
              "packet_0_0", "packet_1_1", "shorter_0_1", "shorter_1_0"])


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def inside(path, root):
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except ValueError:
        return False


def require(value, message):
    if not value:
        raise RuntimeError(message)


def git(root, *args, optional=False):
    run = subprocess.run(["git", "-C", str(root), *args], capture_output=True, timeout=30)
    if optional and run.returncode and b"does not exist" in run.stderr:
        return None
    require(run.returncode == 0, "Git read failed: " + run.stderr.decode("utf-8", "replace"))
    return run.stdout


def read(root, ref, name):
    if ref:
        return git(root, "show", ref + ":" + name, optional=name.startswith("support/pc88/"))
    path = root / name
    return path.read_bytes() if path.exists() else None


def end_brace(text, start):
    depth = 0
    token = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)
    for match in token.finditer(text, start):
        if match.group() == "{":
            depth += 1
        elif match.group() == "}":
            depth -= 1
            if depth == 0:
                return match.end()
    raise RuntimeError("Unclosed extraction")


def definition(text, marker):
    require(text.count(marker) == 1, "Unique actual definition: " + marker)
    start = text.index(marker)
    return text[start:end_brace(text, text.index("{", start))]


def declaration(text, marker):
    require(text.count(marker) == 1, "Unique actual declaration: " + marker)
    start = text.index(marker)
    return text[start:text.index(";", start) + 1]


def line(text, pattern):
    rows = re.findall(pattern, text, re.M)
    require(len(rows) == 1, "Unique actual interface/constant: " + pattern)
    return rows[0]


def generate(selected, directory, parent):
    text = {name: raw.decode("utf-8") for name, raw in selected.items() if raw is not None}
    uio, fio, header, spi = (text[name] for name in ("user_io.cpp", "file_io.cpp", "file_io.h", "spi.h"))
    present = "support/pc88/pc88.cpp" in text
    require(present == ("support/pc88/pc88.h" in text), "Module/header presence must agree")
    extracts = {}
    def save(label, body):
        extracts[label] = {"sha256": sha(body.encode()), "source": body}
        return body
    interfaces = [line(header, r'^(int FileClose\(fileTYPE \*file\);)$'),
                  line(header, r'^(int FileSeek\(fileTYPE \*file, __off64_t offset, int origin\);)$'),
                  line(header, r'^(int FileReadAdv\(fileTYPE \*file, void \*pBuffer, int length, int failres = 0\);)$'),
                  line(header, r'^(int FileWriteAdv\(fileTYPE \*file, void \*pBuffer, int length, int failres = 0\);)$'),
                  line(header, r'^(int  FileOpenEx\(.*\);)$')]
    for pattern in (r'^(void EnableIO\(\);)$', r'^(void DisableIO\(\);)$',
                    r'^(void spi_block_read\(uint8_t \*addr, int wide, int sz = 512\);)$',
                    r'^(void spi_block_write\(const uint8_t \*addr, int wide, int sz = 512\);)$'):
        interfaces.append(line(spi, pattern))
    interfaces += [line(text["fpga_io.h"], r'^(uint16_t fpga_spi\(uint16_t word\);)$')]
    for pattern in (r'^(char \*user_io_get_core_name\(int orig = 0\);)$', r'^(int user_io_get_width\(\);)$', r'^(void diskled_on\(\);)$'):
        interfaces.append(line(text["user_io.h"], pattern))
    actual_type = definition(header, "struct fileTYPE\n") + ";"
    require("__off64_t       size;" in actual_type and "__off64_t       offset;" in actual_type, "Actual size/offset field types")
    constants = [line(text["user_io.h"], r'^(#define UIO_' + name + r'\s+.*)$') for name in ("BUFFER_SIZE", "SECTOR_RD", "SECTOR_WR")]
    types = "struct fileZipArchive;\n" + actual_type + "\n" + "\n".join(interfaces + constants) + "\n"
    types += definition(spi, "uint16_t inline spi_w(") + "\n"
    files = {"pc88_types.inc": save("actual full fileTYPE/interfaces/constants/spi_w", types),
             "pc88_presence.h": "#define PC88_PRESENT " + str(int(present)) + "\n",
             "pc88_api.h": selected["support/pc88/pc88.h"].decode() if present else "// Actual PC88 API absent in this selected ref.\n"}
    declarations = [declaration(uio, marker) for marker in ("static fileTYPE sd_image[16]", "static int      sd_image_cangrow[16]", "static uint64_t buffer_lba[16]", "static int use_save", "static int fio_size", "static char core_name[32]", "static char orig_name[32]")]
    declarations.append(declaration(uio, "static uint8_t buffer[16][UIO_BUFFER_SIZE]"))
    files["pc88_globals.inc"] = save("actual selected enclosing declarations", "\n".join(declarations) + "\n")
    files["pc88_providers.inc"] = save("whole actual original-name and width getters", definition(uio, "char *user_io_get_core_name(int orig)") + "\n" + definition(uio, "int user_io_get_width()") + "\n")
    files["pc88_lifecycle.inc"] = save("whole actual fileTYPE lifecycle", "\n".join(definition(fio, marker) for marker in ("fileTYPE::fileTYPE()", "fileTYPE::~fileTYPE()", "int fileTYPE::opened()")) + "\n")
    for marker in ("int FileSeek(fileTYPE", "int FileReadAdv(fileTYPE", "int FileWriteAdv(fileTYPE"):
        save("source-only lower API: " + marker, definition(fio, marker))
    poll = save("source-only whole user_io_poll", definition(uio, "void user_io_poll()"))
    save("source-only whole user_io_file_mount", definition(uio, "int user_io_file_mount("))
    after_n64 = poll.index("else if (is_n64() && n64_process_save(")
    start = poll.index("else if (op == 2)", after_n64)
    branch = save("entire actual generic op2 branch after N64", poll[start:end_brace(poll, poll.index("{", start))])
    files["pc88_generic.inc"] = "// Branch-only wrapper; no whole-poll execution.\nstatic void generic_write_branch(int disk, int op, uint64_t lba, uint32_t sz, uint32_t blksz, int ack)\n{\nif (false) {}\n" + branch + "\n}\n"
    static_checks = {"whole_mount_and_poll_runtime": False, "module_API_absent": not present, "whole_parent_recovery_performed": False}
    if present:
        module = text["support/pc88/pc88.cpp"]
        includes = [row for row in module.splitlines(keepends=True) if re.match(r"\s*#include\b", row)]
        require({row.strip() for row in includes} == {'#include <string.h>', '#include <strings.h>', '#include "../../file_io.h"', '#include "../../user_io.h"', '#include "../../spi.h"', '#include "pc88.h"'} and len(includes) == 6, "Only six known includes removed")
        files["pc88_module.inc"] = save("whole module with include-lines only removed", "".join(row for row in module.splitlines(keepends=True) if not re.match(r"\s*#include\b", row)))
        save("whole actual PC88 module", module)
        mount = "\tpc88_mount(index);\n"
        hook = "\t\t\telse if (int pc88op = pc88_sd_service(disk, &sd_image[disk], op, lba, sz, ack))\n\t\t\t{\n\t\t\t\tif (pc88op < 0) break;\n\t\t\t}\n"
        support = '// PC-8801\n#include "support/pc88/pc88.h"\n\n'
        require(uio.count(mount) == uio.count(hook) == text["support.h"].count(support) == 1, "Exact official insertion sites")
        require("buffer_lba[index] = -1;\n" + mount + "\tif (!index" in uio, "Mount reset position")
        require(uio.index("else if (int a3op") < uio.index(hook) < uio.index("else if (int falop"), "Dispatch position")
        static_checks.update({"exact_mount_position": True, "exact_dispatch_position": True})
        if parent is not None:
            recovered = uio.replace(mount, "", 1).replace(hook, "", 1).encode()
            require(recovered == parent["user_io.cpp"] and text["support.h"].replace(support, "", 1).encode() == parent["support.h"], "Whole parent existing-file recovery")
            static_checks.update({"whole_parent_recovery_performed": True, "whole_parent_existing_files_recovered": True})
    else:
        require("pc88_mount(" not in uio and "pc88_sd_service(" not in uio, "Absent module must not have orphan callers")
        files["pc88_module.inc"] = "// No historical implementation fabricated.\n"
    files["pc88_harness.cpp"] = (HERE / "pc88_sd_service_regression.cpp").read_text(encoding="utf-8")
    for name, body in files.items():
        (directory / name).write_text(body, encoding="utf-8", newline="\n")
    return present, extracts, static_checks, {name: sha(body.encode()) for name, body in files.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=HERE.parent if HERE.name == "tests" else HERE.parent / "Main_MiSTer")
    parser.add_argument("--source-ref")
    parser.add_argument("--recovery-ref", help="Optional explicit pre-import ref for whole existing-file recovery; never inferred")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--generated-dir", type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    root = args.source_root.resolve()
    require(args.generated_dir is None or not inside(args.generated_dir, root), "Generated files must be outside selected Source")
    require(args.report is None or not inside(args.report, root), "Report must be outside selected Source")
    require(args.generated_dir is not None or not inside(Path(tempfile.gettempdir()), root), "Temporary artifacts must be outside selected Source")
    compiler = shutil.which(args.cxx) or args.cxx
    env = os.environ.copy(); env["PATH"] = str(Path(compiler).resolve().parent) + os.pathsep + env.get("PATH", "")
    temporary = args.generated_dir is None
    directory = Path(tempfile.mkdtemp(prefix="mister-pc88-service-") if temporary else args.generated_dir).resolve()
    report = {"source_root": str(root), "source_ref": args.source_ref or "working tree", "recovery_ref": args.recovery_ref,
              "catalog": CATALOG, "generated_directory": str(directory), "build": None, "runs": [], "observations": [],
              "coverage_complete": False, "passed": False, "failure": None,
              "limits": ["Whole module or complete generic write branch only; no whole mount/poll/request decoder execution.",
                         "Historical absent module remains excluded with passed=null; generic void branch has no fabricated service return.",
                         "Actual full fileTYPE text/lifecycle/interfaces with native non-Linux __off64_t spelling adapter; not ARM ABI/layout proof.",
                         "File API behavior is a stateful in-memory ordinary-success model with normal type0 and no real filp. It is not actual libc/archive/file execution. No fault injection, malformed requests, hardware, disk or user data access.",
                         "Whole parent existing-file recovery is performed only when an explicit recovery ref is supplied; ordinary CI does not infer or freeze a parent."]}
    phase = "read-source"
    try:
        selected = {name: read(root, args.source_ref, name) for name in NAMES}
        report["selected_source_sha256"] = {name: sha(raw) if raw is not None else None for name, raw in selected.items()}
        report["source_HEAD"] = git(root, "rev-parse", "HEAD").decode().strip()
        report["pair_sha256"] = {str(HERE / name): sha((HERE / name).read_bytes()) for name in ("pc88_sd_service_regression.py", "pc88_sd_service_regression.cpp")}
        report["source_raw_before"] = {name: sha((root / name).read_bytes()) if (root / name).exists() else None for name in NAMES}
        parent = None
        if args.recovery_ref:
            report["recovery_commit"] = git(root, "rev-parse", args.recovery_ref + "^{commit}").decode().strip()
            parent = {name: git(root, "show", args.recovery_ref + ":" + name) for name in ("user_io.cpp", "support.h")}
            report["recovery_source_sha256"] = {name: sha(raw) for name, raw in parent.items()}
        directory.mkdir(parents=True, exist_ok=True)
        require(not any(directory.iterdir()), "Generated directory must be fresh and empty")
        phase = "generate"
        present, extracts, static_checks, generated = generate(selected, directory, parent)
        report.update({"module_API_present": present, "static_caller_checks": static_checks, "extractions": extracts, "generated_sha256": generated})
        command = [compiler, "-std=gnu++14", "-D_GNU_SOURCE", "-O2", "-Wall", "-Wextra", "-Werror", "pc88_harness.cpp", "-o", "pc88.exe"]
        def capture(command, timeout):
            try:
                run = subprocess.run(command, cwd=directory, env=env, capture_output=True, text=True, encoding="utf-8", errors="strict", timeout=timeout)
                return {"command": command, "returncode": run.returncode, "stdout": run.stdout, "stderr": run.stderr, "timed_out": False}
            except subprocess.TimeoutExpired as error:
                def decoded(value):
                    return value.decode("utf-8", "replace") if isinstance(value, bytes) else value or ""
                return {"command": command, "returncode": None, "stdout": decoded(error.stdout), "stderr": decoded(error.stderr), "timed_out": True}
        phase = "compile"
        report["build"] = capture(command, 90)
        require(report["build"]["returncode"] == 0 and not report["build"]["timed_out"], "Strict compile failed or timed out; captured record retained")
        require(not report["build"]["stdout"] and not report["build"]["stderr"], "Strict successful compiler diagnostics must be empty")
        report["executable_sha256_before"] = sha((directory / "pc88.exe").read_bytes())
        for name in CATALOG:
            phase = "case:" + name
            run = capture([str(directory / "pc88.exe"), name], 10)
            report["runs"].append({"name": name, **run})
            require(run["returncode"] in (0, 1) and not run["stderr"] and not run["timed_out"], "Fixture error/crash/timeout is not behavioral evidence")
            row = json.loads(run["stdout"])
            require(row["name"] == name and row["module_present"] == present, "Case/presence mismatch")
            require((run["returncode"] == 0) == (row["failed_assertions"] == 0), "Case status mismatch")
            report["observations"].append(row)
        observations = report["observations"]
        require(len(CATALOG) == len(set(CATALOG)) == len(observations) == 57, "Exact complete57 catalog")
        for row in observations:
            excluded = row["module_excluded"] and not row["generic_branch_ran"]
            require(row["case_status"] == ("excluded-api-absent" if excluded else "completed"), "Explicit exclusion status")
            require((row["passed"] is None and row["assertions"] == 0 and row["stages"] == []) if excluded else (row["assertions"] > 0 and row["passed"] == (row["failed_assertions"] == 0)), "No excluded zero-assertion case counted as passed")
        require(all(sha((directory / name).read_bytes()) == expected for name, expected in generated.items()), "Generated files changed during run")
        require(sha((directory / "pc88.exe").read_bytes()) == report["executable_sha256_before"], "Executable changed during run")
        require([row["name"] for row in observations] == CATALOG, "Ordered complete catalog")
        report["coverage_complete"] = True
    except Exception as error:
        report["failure"] = {"phase": phase, "type": type(error).__name__, "message": str(error)}
    finally:
        report["source_raw_after"] = {name: sha((root / name).read_bytes()) if (root / name).exists() else None for name in NAMES}
        report["pair_sha256_after"] = {name: sha(Path(name).read_bytes()) for name in report.get("pair_sha256", {})}
        stable = report.get("source_raw_before") == report["source_raw_after"] and report.get("pair_sha256") == report["pair_sha256_after"]
        report["source_and_pair_stable"] = stable
        report["executable_sha256_after"] = sha((directory / "pc88.exe").read_bytes()) if (directory / "pc88.exe").exists() else None
        report["generated_sha256_after"] = {name: sha((directory / name).read_bytes()) for name in report.get("generated_sha256", {}) if (directory / name).exists()}
    observations = report["observations"]
    summary = {"catalog_cases": len(CATALOG), "observed_cases": len(observations), "attempted_processes": len(report["runs"]),
               "completed_cases": sum(row["case_status"] == "completed" for row in observations), "assertions": sum(row["assertions"] for row in observations),
               "failed_assertions": sum(row["failed_assertions"] for row in observations), "generic_branch_cases": sum(row["generic_branch_ran"] for row in observations),
               "module_cases_excluded": sum(row["module_excluded"] for row in observations), "fully_excluded_cases": sum(row["module_excluded"] and not row["generic_branch_ran"] for row in observations)}
    report["summary"] = summary
    report["passed"] = report["coverage_complete"] and report["failure"] is None and stable and summary["failed_assertions"] == 0
    report["generated_directory_retained"] = not temporary or not report["passed"]
    output = args.report
    if not report["passed"] and output is None:
        output = directory / "failed-report.json"
    if output:
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open("x", encoding="utf-8", newline="\n") as handle:
            json.dump(report, handle, indent=2, ensure_ascii=False); handle.write("\n")
    print(json.dumps({"source_ref": report["source_ref"], "module_API_present": report.get("module_API_present"), "summary": summary,
                      "coverage_complete": report["coverage_complete"], "passed": report["passed"], "report": str(output) if output else None}))
    if temporary and report["passed"]:
        shutil.rmtree(directory)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    result = main()
    if result:
        raise SystemExit(result)
