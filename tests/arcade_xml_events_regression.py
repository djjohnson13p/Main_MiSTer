#!/usr/bin/env python3
"""Exercise actual MRA file-SAX/ROM/DIP behavior across XML special nodes.

Tiny synthetic documents reach intact parser, callback, selector, buffer, ROM,
hex, MD5, DIP, config and core helper definitions. Only lower file/SPI endpoints
are spies. Each case runs in a fresh process. No user data or hardware is used.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
EXTRACTOR = ROOT / 'tests/arcade_nvram_regression.py'
ns = runpy.run_path(str(EXTRACTOR))
definition = ns['definition']
SOURCES = ns['SOURCES'] + ('lib/md5/md5.c', 'shmem.h')
SCENARIOS = ('inline-ordinary', 'inline-comment', 'inline-pi', 'inline-cdata',
    'inline-cdata-only', 'inline-comment-part', 'inline-comment-rom',
    'inline-pi-rom', 'inline-cdata-multiple', 'inline-empty-cdata',
    'inline-two-parts', 'inline-recovery', 'inline-empty-part', 'dip-ordinary',
    'dip-self', 'dip-comment-note', 'dip-comment-dip', 'dip-comment-switches',
    'dip-pi-dip', 'dip-pi-switches', 'dip-cdata-dip', 'dip-cdata-switches',
    'fresh-absent', 'fresh-open-error')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def generate(source):
    types, prod, parser, checked = ns['generate'](source)
    arcade, io = source['support/arcade/mra_loader.cpp'], source['file_io.cpp']
    types += 'int FileSaveConfig(const char *,void *,int);\n'
    for marker in ('static int      romblkl', 'static uint8_t* romdata', 'static uint8_t  romindex'):
        types += next(line for line in arcade.splitlines() if line.startswith(marker)) + '\n'
    types += next(line for line in source['shmem.h'].splitlines() if line.startswith('#define fpga_mem')) + '\n'
    config = ''
    if 'static int create_config_directory(' in io:
        config += '#define mkdir fixture_mkdir\n' + definition(io, 'static int create_config_directory(', 'file_io.cpp') + '#undef mkdir\n'
    config += '#define strcat observed_strcat\n' + definition(io, 'int FileSaveConfig(', 'file_io.cpp') + '#undef strcat\n'
    config += '__attribute__((unused)) static int observed_FileSaveConfig(const char *n,void *b,int z) { int r=FileSaveConfig(n,b,z); observe_file_return("FileSaveConfig",n,r); return r; }\n'
    where = prod.index('sw_struct *arcade_sw()')
    line = prod.rfind('#line', 0, where)
    prod = prod[:line] + config + prod[line:]
    sw_save = '#define FileSaveConfig observed_FileSaveConfig\n#define FileSave observed_FileSave\n' + definition(arcade, 'void arcade_sw_save()', 'support/arcade/mra_loader.cpp') + '#undef FileSave\n#undef FileSaveConfig\n'
    where = prod.index('void arcade_sw_load()')
    line = prod.rfind('#line', 0, where)
    prod = prod[:line] + sw_save + prod[line:]
    sw_load = definition(arcade, 'void arcade_sw_load()', 'support/arcade/mra_loader.cpp')
    if prod.count(sw_load) != 1:
        raise ValueError('DIP load observer point is ambiguous')
    prod = prod.replace(sw_load, '#define FileLoadConfig observed_FileLoadConfig\n' + sw_load + '#undef FileLoadConfig\n', 1)
    helpers = next(line for line in arcade.splitlines() if line.startswith('#define BLKL')) + '\n'
    for marker in ('static void rom_start(', 'static int rom_checksz(', 'static int rom_data(', 'static void rom_finish('):
        helpers += definition(arcade, marker, 'support/arcade/mra_loader.cpp')
    # Reject the inherited allocation-failure diagnostic endpoint. The selected
    # successful conversion and actual malloc/realloc bodies are unchanged.
    helpers += '#define printf fixture_hex_allocation_diagnostic\n' + definition(arcade, 'unsigned char* hexstr_to_char(', 'support/arcade/mra_loader.cpp') + '#undef printf\n'
    where = prod.index('static int xml_send_rom(')
    line = prod.rfind('#line', 0, where)
    prod = prod[:line] + helpers + prod[line:]
    return {'arcade_nvram_types.inc': types, 'arcade_nvram_production.inc': prod,
            'arcade_special_sxmlc.c': parser}, checked


def command(value):
    return [value] if Path(value).is_file() else shlex.split(value, posix=os.name != 'nt')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    parser.add_argument('--cc', default=os.environ.get('CC'))
    parser.add_argument('--source-ref')
    parser.add_argument('--report', '--output', dest='report', type=Path)
    args = parser.parse_args()
    raw = {p: subprocess.check_output(['git', '-C', str(ROOT), 'show', args.source_ref + ':' + p])
           if args.source_ref else (ROOT / p).read_bytes() for p in SOURCES}
    source = {p: b.decode('utf-8').replace('\r\n', '\n') for p, b in raw.items()}
    generated, checked = generate(source)
    cxx = command(args.cxx)
    cc = command(args.cc or str(Path(cxx[0]).with_name(Path(cxx[0]).name.replace('g++', 'gcc'))))
    env = os.environ.copy()
    if Path(cxx[0]).is_file():
        env['PATH'] = str(Path(cxx[0]).resolve().parent) + os.pathsep + env.get('PATH', '')
    pair = (Path(__file__), ROOT / 'tests/arcade_xml_events_regression.cpp', EXTRACTOR)
    pair_hashes = {p.name: digest(p.read_bytes()) for p in pair}
    builds, runs = [], []
    try:
        with tempfile.TemporaryDirectory(prefix='mister-arcade-xml-events-') as temp:
            work = Path(temp)
            for p in ('sxmlc.h', 'support/arcade/buffer.cpp', 'support/arcade/buffer.h', 'lib/md5/md5.h', 'lib/md5/md5.c'):
                target = work / p
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(source[p], encoding='utf-8', newline='\n')
            for p, text in generated.items():
                (work / p).write_text(text, encoding='utf-8', newline='\n')
            common = ['-O2', '-Wall', '-Wextra', '-Werror', '-I', str(work)]
            exe = work / ('probe.exe' if os.name == 'nt' else 'probe')
            commands = [cc + ['-std=gnu99'] + common + ['-Wno-error=format-security', '-c', str(work / 'arcade_special_sxmlc.c'), '-o', str(work / 'parser.o')],
                cc + ['-std=gnu99'] + common + ['-I', str(work / 'lib/md5'), '-c', str(work / 'lib/md5/md5.c'), '-o', str(work / 'md5.o')],
                cxx + ['-std=gnu++14', '-D_GNU_SOURCE', '-D_LARGEFILE64_SOURCE'] + common + ['-I', str(work / 'support/arcade'), str(pair[1]), str(work / 'support/arcade/buffer.cpp'), str(work / 'parser.o'), str(work / 'md5.o'), '-o', str(exe)]]
            for cmd in commands:
                result = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=60)
                builds.append(dict(command=cmd, returncode=result.returncode, stdout=result.stdout, stderr=result.stderr))
                if result.returncode:
                    raise RuntimeError('Compilation is a fixture error, not behavioral proof: ' + result.stdout + result.stderr)
                if result.stderr:
                    print(result.stderr, file=sys.stderr, end='')
            for case in SCENARIOS:
                result = subprocess.run([str(exe), case], capture_output=True, text=True, env=env, timeout=15)
                rows = [json.loads(line[5:]) for line in result.stdout.splitlines() if line.startswith('JSON ')]
                if len(rows) != 1:
                    raise RuntimeError('Missing case observation: ' + case + '\n' + result.stdout + result.stderr)
                row = rows[0]
                if row['case'] != case or not row['case_body_completed'] or row['trapped'] or row['excluded'] or not 0 <= row['failures'] <= row['checks'] or row['checks'] < 1 or result.returncode != int(bool(row['failures'])):
                    raise RuntimeError('Incomplete/crashed/invalid case is not proof: ' + case + '\n' + result.stdout + result.stderr)
                runs.append(dict(scenario=case, summary=row, returncode=result.returncode, stdout=result.stdout, stderr=result.stderr))
                print(('FAIL' if result.returncode else 'PASS') + ': ' + case)
    except Exception as error:
        if args.report:
            args.report.write_text(json.dumps(dict(fixture_error=str(error), builds=builds, completed_runs=runs), indent=2) + '\n', encoding='utf-8', newline='\n')
        raise
    if [r['scenario'] for r in runs] != list(SCENARIOS) or len(set(r['scenario'] for r in runs)) != len(SCENARIOS):
        raise RuntimeError('Incomplete or duplicate coverage')
    if not args.source_ref and {p: digest((ROOT / p).read_bytes()) for p in SOURCES} != {p: digest(b) for p, b in raw.items()}:
        raise RuntimeError('Production changed during regression run')
    if {p.name: digest(p.read_bytes()) for p in pair} != pair_hashes:
        raise RuntimeError('Fixture changed during regression run')
    summary = dict(cases=len(runs), checks=sum(r['summary']['checks'] for r in runs), failures=sum(r['summary']['failures'] for r in runs),
        failed_cases=sum(bool(r['summary']['failures']) for r in runs), excluded=0, prevented_boundaries=0, user_target_io=0, hardware_io=0)
    report = dict(source_ref=args.source_ref or 'working tree', checked_source=checked, source_raw_sha256={p: digest(b) for p, b in raw.items()},
        fixture_sha256=pair_hashes, extracted_sha256={p: digest(t.encode()) for p, t in generated.items()}, coverage_complete=True,
        scenario_order=list(SCENARIOS), builds=builds, runs=runs, summary=summary,
        scope='Actual file-SAX/full callback/full selector/buffer, private NVRAM helpers, DIP accessor/load/save/send, file/config helpers, ROM start/checksz/data/finish, hex conversion, MD5 provider, core helper bodies above SPI spies. Fresh process per case.',
        lower_spies='Only parser fopen redirects to a disposable seekable stream. FileOpen/Read/Close, save syscalls, directory mkdir/private prepared-directory query and legacy FileCreatePath are spies. Unselected external ROM/patch/cheats/button/game-ID/nonzero-address paths and hex allocation-failure diagnostic reject.',
        limits='Synthetic bounded inline MRA controls, no corpus prevalence claim, no hardware/restart/core execution, durable filesystem or runtime timing claim. No large payload/interleave/ROM zip/patch/allocation-failure evidence. Actual parser C has only format-security warning demotion for unused inherited serializers, with diagnostics visible; C++/MD5 retain strict Werror.')
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps(summary))
    return int(bool(summary['failures']))


if __name__ == '__main__':
    raise SystemExit(main())
