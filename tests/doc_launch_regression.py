#!/usr/bin/env python3
"""Run actual document selected-case/script generation with Bash command spies.

The production helper and complete selected case execute with video, FileSave,
fork/agetty and viewer boundaries replaced by spies. No viewer, user document,
hardware, actual fork or /tmp/script is touched. --source-ref REF demonstrates
historical quoting and buffer-size failures with capped diagnostic sprintf.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import posixpath
import re
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def definition(source, marker):
    first = source.index(marker)
    opening = source.index("{", first)
    depth, last = 1, opening + 1
    while depth:
        depth += (source[last] == "{") - (source[last] == "}")
        last += 1
    if source[last:last + 1] == ";":
        last += 1
    return f'#line {source.count(chr(10), 0, first) + 1} "menu.cpp"\n' + source[first:last] + "\n"


SPIES = r'''
function record() { builtin printf '%s\0' "$@" END >&3; }
function cd() {
 record CD "$@"
 local literal=0
 if [[ "$1" == -- ]]; then shift; literal=1; fi
 if [[ "$#" != 1 || ( "$literal" == 0 && "$1" == -* ) ]]; then record CDERROR; return 64; fi
}
function environment() { record ENV "$LC_ALL" "$HOME" "$LESSKEY"; }
function /media/fat/linux/pdfviewer() { record PDF "$@"; environment; return 23; }
function /media/fat/linux/glow() { record GLOW "$@"; environment; builtin printf 'MARKDOWN_OUTPUT\n'; return 42; }
function less() {
 record LESS "$@"; environment
 if [[ "$1" == -R ]]; then local line; IFS= read -r line; record PIPE "$line"; return 17; fi
 return 29
}
function echo() { record ECHO "$@"; return 0; }
function injected() { record INJECTED; builtin printf 'SUBSTITUTED'; }
'''


def cases():
    result = [("ordinary_pdf", "/media/fat/docs/Manual.pdf", "pdf"),
              ("ordinary_txt", "/media/fat/docs/Read Me.txt", "txt"),
              ("ordinary_md", "/media/fat/docs/Read Me.md", "md")]
    directories = ["Game Manuals", "Quote' folder", 'Double" folder', "Price$HOME", "$(injected)",
                   "`injected`", "folder\n", "tab\tfolder", "[glob]*?; &|<>!", "Mañual 日本語"]
    names = ["Manual'quote", 'Manual"quote', "Price$HOME", "Manual$(injected)", "Manual`injected`",
             "New\nline", "Tab\tname", "[glob]*?; &|<>!", "Mañual 日本語"]
    for kind in ("pdf", "txt", "md"):
        result += [(f"dir_{kind}_{i}", f"/media/fat/{directory}/Manual.{kind}", kind) for i, directory in enumerate(directories)]
        result += [(f"name_{kind}_{i}", f"/media/fat/docs/{name}.{kind}", kind) for i, name in enumerate(names)]
    result += [("uppercase_pdf", "/media/fat/docs/Manual.PDF", "pdf"),
               ("uppercase_txt", "/media/fat/docs/Manual.TXT", "txt"),
               ("uppercase_md", "/media/fat/docs/Manual.MD", "md"),
               ("dotted_directory", "/media/fat/folder.md/Manual.pdf", "pdf"),
               ("unknown_in_dotted_directory", "/media/fat/folder.md/Manual", "default"),
               ("unknown_extension", "/media/fat/docs/Manual.bin", "default"),
               ("root_pdf", "/Manual.pdf", "pdf"), ("root_md", "/.md", "md"),
               ("relative_txt", "folder/Read Me.txt", "txt"), ("relative_dash", "-folder/Manual.pdf", "pdf"),
               ("relative_filename", "Manual.pdf", "pdf"), ("short_pdf", ".pdf", "pdf"),
               ("short_txt", ".txt", "txt"), ("short_md", ".md", "md"),
               ("short_unknown", "a", "default"), ("empty", "", "default"),
               ("root_only", "/", "default"), ("trailing_slash", "/media/fat/docs/", "default"),
               ("all_slashes", "///", "default")]
    for length in (1021, 1023):
        tail = "Manual.pdf"
        prefix = "/media/fat/" + "docs/" * ((length - 10 - len(tail)) // 5)
        prefix += "x" * (length - len(prefix) - len(tail))
        result.append((f"long_{length}", prefix + tail, "pdf"))
    result.append(("long_quoted", "/media/fat/" + "d' x/" * 200 + "Manual.pdf", "pdf"))
    result.append(("copied_shared_path", "/media/fat/docs/Manual.pdf", "pdf"))
    return result


def directory(path):
    return posixpath.dirname(path.rstrip("/")) or ("/" if path.startswith("/") else ".")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--bash", default=os.environ.get("BASH", "bash"))
    parser.add_argument("--source-ref")
    parser.add_argument("--case", help="run one named path fixture for focused diagnosis")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    source = subprocess.check_output(["git", "-C", str(ROOT), "show", f"{args.source_ref}:menu.cpp"], text=True, encoding="utf-8") if args.source_ref else (ROOT / "menu.cpp").read_text(encoding="utf-8")
    types = definition(source, "enum MENU\n")
    production = ""
    present = "static std::string menu_document_script(" in source
    if present:
        production += definition(source, "static std::string menu_shell_quote(")
        production += definition(source, "static std::string menu_document_script(")
    first = source.index("\n\tcase MENU_DOC_FILE_SELECTED:")
    last = source.index("\n\tcase MENU_DOC_FILE_SELECTED_2:", first)
    selected = source[first:last]
    if not present:
        # Old ext+1 is outside the one-byte empty literal on short paths. The
        # same empty string in two bytes prevents that undefined diagnostic
        # read, retaining the historical fallback for a meaningful comparison.
        selected = selected.replace('const char *ext = "";', 'const char *ext = padded_empty_extension;')
    production += "void selected_case()\n{\n switch (menustate) {\n"
    production += f'#line {source.count(chr(10), 0, first) + 1} "menu.cpp"\n' + selected + "\n default: break;\n }\n}\n"
    command = [args.cxx] if Path(args.cxx).is_file() else shlex.split(args.cxx, posix=os.name != "nt")
    bash = [args.bash] if Path(args.bash).is_file() else shlex.split(args.bash, posix=os.name != "nt")
    environment = os.environ.copy()
    if Path(command[0]).is_file():
        environment["PATH"] = str(Path(command[0]).resolve().parent) + os.pathsep + environment.get("PATH", "")
    observations, failures, checks = [], [], 0
    def check(ok, label):
        nonlocal checks
        checks += 1
        if not ok:
            failures.append(label)
            print("FAIL: " + label, file=sys.stderr)
    with tempfile.TemporaryDirectory(prefix="mister-doc-launch-") as temporary:
        work = Path(temporary)
        (work / "doc_launch_types.inc").write_text(types, encoding="utf-8")
        (work / "doc_launch_production.inc").write_text(production, encoding="utf-8")
        executable = work / ("doc.exe" if os.name == "nt" else "doc")
        subprocess.run(command + ["-std=gnu++14", "-D_GNU_SOURCE", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable",
                                   "-I", str(ROOT), "-I", str(work), str(ROOT / "tests/doc_launch_regression.cpp"), "-o", str(executable)], check=True, env=environment)
        for label, path, kind in cases():
            if args.case and args.case != label:
                continue
            input_path, script_path, call_path = work / "path.bin", work / "script.sh", work / "calls.bin"
            input_path.write_bytes(path.encode("utf-8"))
            run = subprocess.run([str(executable), str(input_path), str(script_path), "1", str(int(label == "copied_shared_path")), "1", "0", str(call_path)], capture_output=True, env=environment)
            # Legacy Markdown's four-byte suffix can begin at a UTF8
            # continuation byte. Only the fixture's ASCII report is parsed.
            report = next(line for line in run.stdout.splitlines() if line.startswith(b"REPORT ")).decode("ascii")
            try:
                run.stdout.decode("utf-8")
                diagnostic_hex = None
            except UnicodeDecodeError:
                diagnostic_hex = run.stdout.hex()
            size, saved, fullpath, saves, forks, state, copied, exited, status, pid = map(int, report.split()[1:])
            script = script_path.read_bytes()
            calls = call_path.read_bytes().decode("utf-8").split("\0")[:-1]
            check(run.returncode == 0 and saves == forks == fullpath == 1 and state and copied and exited == -1 and status == 0 and pid == 123,
                  label + ": full selected parent flow and saved selection")
            check(calls == ["video", "fb", "nag", "unlink", "save", "fork", "ARGS"], label + ": selected flow ordering")
            check(saved == len(script) and size < 2048, label + ": script saved completely without fixed-buffer overflow")
            syntax = subprocess.run(bash + ["--noprofile", "--norc", "-n"], input=SPIES.encode() + script, capture_output=True, env=environment)
            check(syntax.returncode == 0, label + ": generated script Bash syntax")
            records = []
            execution = None
            if syntax.returncode == 0 and size < 2048:
                execution = subprocess.run(bash + ["--noprofile", "--norc", "-c", "exec 3>&1\n" + SPIES + script.decode("utf-8")], capture_output=True, env=environment)
                parts = execution.stdout.decode("utf-8").split("\0")
                current = []
                for part in parts:
                    if part == "END": records.append(current); current = []
                    elif part: current.append(part)
            cdirs = [record[1:] for record in records if record[0] == "CD"]
            cdirs = [record[1:] if record[:1] == ["--"] else record for record in cdirs]
            check(cdirs == [[directory(path)]] and not any(record[0] == "CDERROR" for record in records), label + ": literal working directory")
            expected = {"pdf": ["PDF", "--zoom_to_fit", "--cache_size=1", path], "txt": ["LESS", path],
                        "md": ["GLOW", "--style", "dark", path], "default": ["PDF", "--cache_size=1"]}[kind]
            viewers = [record for record in records if record[0] in ("PDF", "GLOW", "LESS")]
            # Pipeline children may emit spy records in either order; the
            # actual stdin stream assertion below verifies their connection.
            check(sorted(viewers) == sorted([expected, ["LESS", "-R"]] if kind == "md" else [expected]), label + ": literal viewer arguments and pipeline")
            check([record for record in records if record[0] == "ENV"] == [["ENV", "en_US.UTF-8", "/root", "/media/fat/linux/lesskey"]] * (2 if kind == "md" else 1), label + ": script environment")
            check([record for record in records if record[0] == "ECHO"] == [["ECHO", "Press any key to continue"]]
                  and not any(record[0] == "INJECTED" for record in records), label + ": prompt and no filename evaluation")
            check(execution is not None and execution.returncode == {"pdf": 23, "default": 23, "txt": 29, "md": 17}[kind], label + ": viewer exit status retained after prompt")
            if kind == "md":
                check([record for record in records if record[0] == "PIPE"] == [["PIPE", "MARKDOWN_OUTPUT"]], label + ": actual glow-to-less stream")
            observations.append({"case": label, "path": path, "script_bytes": len(script), "legacy_sprintf_required": size,
                                 "syntax": syntax.returncode, "records": records, "returncode": execution.returncode if execution else None,
                                 "non_utf8_diagnostic_hex": diagnostic_hex})
        # Complete child branch executes only harmless syscall/agetty/exit spies.
        for terminal, saved_ok, child in ((1, 1, 1), (0, 1, 0), (1, 0, 0)):
            input_path.write_bytes(b"/media/fat/docs/Manual.pdf")
            run = subprocess.run([str(executable), str(input_path), str(script_path), str(terminal), "0", str(saved_ok), str(child), str(call_path)], capture_output=True, env=environment)
            fields = list(map(int, next(line for line in run.stdout.decode().splitlines() if line.startswith("REPORT ")).split()[1:]))
            calls = call_path.read_bytes().decode().split("\0")[:-1]
            if child:
                check(fields[5] and fields[7] == 1 and fields[9] == 0 and calls[:10] == ["video", "fb", "nag", "unlink", "save", "fork", "affinity", "setsid", "execl", "exit"], "child: affinity/session/agetty failure exit flow")
                check(calls[11:] == ["/sbin/agetty", "/sbin/agetty", "-a", "root", "-l", "/tmp/script", "--nohostname", "-L", "tty2", "linux"], "child: original agetty arguments")
            elif not terminal:
                check(fields[2:5] == [0, 0, 0] and fields[5] and fields[8] == -1 and fields[9] == 0
                      and script_path.read_bytes() == b"" and calls == ["ARGS"], "disabled fb terminal: correct screen with no generation/video/save/fork side effects")
            else:
                check(fields[3:5] == [1, 1], "failed save: existing parent fork behavior retained")
    print(f"{'FAIL' if failures else 'PASS'}: production document script/selected flow/Bash spies ({checks} checks, {len(failures)} failures)")
    if args.output:
        args.output.write_text(json.dumps({"source_ref": args.source_ref, "menu_sha256": hashlib.sha256(source.encode()).hexdigest(),
            "checks": checks, "failures": failures, "observations": observations, "scope": __doc__}, indent=2) + "\n", encoding="utf-8")
    raise SystemExit(1 if failures else 0)


if __name__ == "__main__":
    main()
