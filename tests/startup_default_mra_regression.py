"""Finite production startup/default-MRA policy regressions; no hardware I/O.

Extracts whole actual main/parser/reader/getter/optional validator and labeled
init intervals. Lower services are passive, guarded or explicitly conditional.
Historical unsafe copies stop before writing; absent helper tests are excluded.
"""
import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
DEFAULT_SOURCE = HERE.parent if HERE.name == 'tests' else HERE.parent / 'Reviews/input-remap'
NAMES = ['main.cpp', 'user_io.cpp', 'user_io.h', 'file_io.cpp', 'file_io.h', 'input.cpp']
MAIN_NAMES = ['main-no-arguments', 'main-empty-path', 'main-short-path', 'main-explicit-mra', 'main-explicit-mgl', 'main-explicit-empty-xml', 'main-path1023-nullxml', 'main-path1023-shortxml', 'main-shortpath-xml1023', 'main-both1023', 'main-path1024-nullxml-first-guard', 'main-path1024-shortxml-second-guard', 'main-shortpath-xml1024-first-guard', 'main-both1024-first-guard', 'main-noargs-fpga-notready', 'main-path1024-fpga-notready', 'main-ready-then-loop-notready', 'main-extra-argument-ignored', 'main-percent-utf8-spelling']
CONTROL_NAMES = ['default-fat-relative-short', 'default-fat-leading-short', 'default-usb-relative-short', 'default-usb-leading-short', 'default-none', 'default-absent-file', 'default-explicit-xml-suppresses', 'default-explicit-emptyxml-suppresses', 'default-explicitxml-clipped-default-suppresses', 'default-index0-ignored', 'default-index1-ignored', 'default-lowercase-ignored', 'default-mixedcase-ignored', 'default-prefix-ignored', 'default-missingcomma-ignored', 'default-maskedprefix-ignored', 'default-DIP-prefix-ignored', 'default-X-prefix-ignored', 'default-valid-then-valid', 'default-valid-then-oversize', 'default-oversize-then-valid', 'default-valid-then-empty', 'default-empty-payload', 'default-empty-descriptor', 'default-empty-index2-stops-later', 'default-semicolon-field-order', 'default-percent-utf8-spelling', 'default-leading-double-slash']
BOUNDARY_NAMES = [f'default-{root}-{kind}-{length}-{presence}' for root in ('fat', 'usb0') for kind in ('relative', 'leading') for length in (1023, 1024) for presence in ('present', 'absent')]
DESCRIPTOR_NAMES = ['descriptor-item2048', 'descriptor-item2049', 'descriptor-item2049-next-valid', 'descriptor-spi10239', 'descriptor-spi10240', 'descriptor-default-after-spi-cutoff']
ORIGINAL = MAIN_NAMES + CONTROL_NAMES + BOUNDARY_NAMES + DESCRIPTOR_NAMES
EXTRA = ['formatter-negative-first', 'formatter-negative-later-retires-valid', 'formatter-negative-then-fitting-recovery', 'recovery-valid-oversize-then-fitting', 'recovery-oversize-then-exact1023']
API = ['api-nullpath-nullxml', 'api-nullpath-emptyxml', 'api-nullpath-fittingxml', 'api-empty-nullxml', 'api-empty-emptyxml', 'api-empty-fittingxml', 'api-path1023-nullxml', 'api-path1024-nullxml', 'api-path1023-emptyxml', 'api-path1024-emptyxml', 'api-fittingpath-xml1023', 'api-fittingpath-xml1024', 'api-both1023', 'api-path-unterminated1024', 'api-xml-unterminated1024', 'api-path-finalnul1023', 'api-xml-finalnul1023', 'api-percent-utf8']
assert len(ORIGINAL) == 69 and len(EXTRA) == 5 and len(API) == 18
assert len(set(ORIGINAL + EXTRA + API)) == 92

def sha(raw):
    return hashlib.sha256(raw).hexdigest()

def definition(text, marker):
    assert text.count(marker) == 1, marker
    start = text.index(marker)
    opening = text.index('{', start)
    depth, quote, comment, escape, i = 0, '', '', False, opening
    while i < len(text):
        c, nxt = text[i], text[i:i+2]
        if comment == '//':
            if c == '\n': comment = ''
        elif comment == '/*':
            if nxt == '*/': comment = ''; i += 1
        elif quote:
            if escape: escape = False
            elif c == '\\': escape = True
            elif c == quote: quote = ''
        elif nxt in ('//', '/*'):
            comment = nxt; i += 1
        elif c in ('"', "'"):
            quote = c
        elif c == '{': depth += 1
        elif c == '}':
            depth -= 1
            if not depth: return text[start:i+1]
        i += 1
    raise AssertionError('Unclosed definition: ' + marker)

def interval(text, begin, end, include_end=True):
    assert text.count(begin) == text.count(end) == 1
    start, stop = text.index(begin), text.index(end)
    assert start < stop
    return text[start:stop + (len(end) if include_end else 0)]

def generate(raw):
    text = {name: value.decode('utf-8') for name, value in raw.items()}
    user, header, extracts = text['user_io.cpp'], text['user_io.h'], {}
    def extract(name, value):
        assert name not in extracts
        extracts[name] = value
        return value
    actual_main = extract('main.cpp:whole main', definition(text['main.cpp'], 'int main('))
    actual_parse = extract('user_io.cpp:whole parse_config', definition(user, 'static void parse_config('))
    actual_read = extract('user_io.cpp:whole read_confstr', definition(user, 'void user_io_read_confstr('))
    actual_get = extract('user_io.cpp:whole get_confstr', definition(user, 'char *user_io_get_confstr('))
    actual_xml = extract('file_io.cpp:whole isXmlName', definition(text['file_io.cpp'], 'int isXmlName('))
    full_init = extract('user_io.cpp:whole init retained unexecuted', definition(user, 'void user_io_init('))
    entry = extract('user_io.cpp:init entry interval', interval(full_init, '\tcore_name[0] = 0;', '\tstrcpy(rbf_path, path);'))
    explicit = extract('user_io.cpp:init explicit XML interval', interval(full_init, '\tif (xml)\n\t{', '\t\tarcade_pre_parse(xml);\n\t}'))
    reader = extract('user_io.cpp:init descriptor calls interval', interval(full_init, '\tuser_io_read_confstr();', '\n\n\tif ((fpga_get_buttons', False))
    adoption = extract('user_io.cpp:init parse/adoption interval', interval(full_init, '\tparse_config();', '\t\tprintf("Using default MRA: %s\\n", xml);\n\t}'))
    version = extract('main.cpp:version declaration', re.findall(r'^const char \*version = .*;$', text['main.cpp'], re.M)[0])
    declarations = []
    for name in ('core_path', 'rbf_path', 'core_name', 'config_ver', 'is_arcade_type', 'disable_osd', 'cur_status', 'joy_force', 'joy_transl', 'use_cheats', 'ss_base', 'ss_size', 'uart_speeds', 'uart_speed_labels', 'midi_speeds', 'midi_speed_labels', 'defmra', 'boot0_loaded', 'boot0_mounted', 'cfgstr'):
        matches = re.findall(r'^static[^\n;]*\b' + re.escape(name) + r'\b[^\n]*;$', user, re.M)
        assert len(matches) == 1, name
        declarations.append(extract('user_io.cpp:declaration:' + name, matches[0]))
    declarations.append(extract('input.cpp:joy_bcount declaration', re.findall(r'^int\s+joy_bcount\s*=\s*0;$', text['input.cpp'], re.M)[0]))
    for name in ('UIO_GET_STRING', 'EMU_JOY0', 'DISKLED_OFF'):
        matches = re.findall(r'^#define\s+' + name + r'\s+[^\n]+$', header, re.M)
        assert len(matches) == 1, name
        declarations.append(extract('user_io.h:macro:' + name, matches[0]))
    exists = re.findall(r'^int\s+FileExists\([^\n]*;$', text['file_io.h'], re.M)
    assert len(exists) == 1 and re.fullmatch(r'int\s+FileExists\(const char \*name, int use_zip = 1\);', exists[0])
    declarations.append(extract('file_io.h:actual FileExists declaration and default argument', exists[0]))
    if 'enum class UserIoFileTxResult {' in header:
        declarations.append(extract('user_io.h:file-transfer enum', definition(header, 'enum class UserIoFileTxResult {') + ';'))
        declarations.append('#define FIXTURE_REAL_TX_ENUM 1')
    helper_marker = 'bool user_io_init_paths_valid('
    helper_present = helper_marker in user
    validator = '// The actual optional validator is absent in this historical source.\n'
    if helper_present:
        validator = extract('user_io.cpp:whole actual startup validator', definition(user, helper_marker)) + '\n'
        query_declarations = re.findall(r'^bool user_io_init_paths_valid\([^\n]*;$', header, re.M)
        assert len(query_declarations) == 1
        declarations.append(extract('user_io.h:actual startup validator declaration', query_declarations[0]))
        declarations.append('#define FIXTURE_REAL_INIT_VALIDATOR 1')
    else:
        assert helper_marker not in header and 'user_io_init_paths_valid(' not in actual_main
    globals_text = '#include "startup_build_identity.inc"\n\n' + '\n\n'.join(declarations + [version]) + '\n'
    production = '\n\n'.join([actual_read, actual_get, actual_xml, actual_parse, 'void fixture_selected_init_intervals(const char *path, const char *xml)\n{\n' + '\n\n'.join([entry, explicit, reader, adoption]) + '\n}', actual_main]) + '\n'
    files = {'startup_build_identity.inc': b'#define VDATE "261014-fixture"\n', 'startup_globals.inc': globals_text.encode(), 'startup_production.inc': production.encode(), 'startup_validator.inc': validator.encode()}
    return files, extracts, helper_present

@contextmanager
def generated_directory(requested, source):
    if requested:
        directory = requested.resolve()
        assert not directory.exists() and not directory.is_relative_to(source), 'Fresh output directory must be outside Source'
        directory.mkdir(parents=True)
        yield directory
    else:
        with tempfile.TemporaryDirectory(prefix='mister-startup-default-mra-') as temporary:
            directory = Path(temporary).resolve()
            assert not directory.is_relative_to(source)
            yield directory

def run_process(command, env, timeout):
    try:
        process = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', env=env, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        return {'command': command, 'returncode': None, 'timeout': True, 'stdout': str(error.stdout), 'stderr': str(error.stderr)}
    return {'command': command, 'returncode': process.returncode, 'stdout': process.stdout, 'stderr': process.stderr}

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, default=DEFAULT_SOURCE)
    ap.add_argument('--source-ref', help='immutable local Git ref; default reads current raw Source')
    ap.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    ap.add_argument('--generated-dir', type=Path, help='preserve generated files in a fresh external directory')
    ap.add_argument('--report', type=Path, help='write complete evidence to a fresh external JSON file')
    args = ap.parse_args()
    source, harness = args.source.resolve(), Path(__file__).with_suffix('.cpp')
    if args.report:
        assert not args.report.exists() and not args.report.resolve().is_relative_to(source)
    before = {name: sha((source / name).read_bytes()) for name in NAMES}
    pair_before = {'python': sha(Path(__file__).read_bytes()), 'cpp': sha(harness.read_bytes())}
    head = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    status_before = subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain'], text=True)
    raw = {name: subprocess.check_output(['git', '-C', str(source), 'show', args.source_ref + ':' + name]) if args.source_ref else (source / name).read_bytes() for name in NAMES}
    files, extracts, helper_present = generate(raw)
    catalog = ORIGINAL + EXTRA + (API if helper_present else [])
    compiler = [args.cxx] if Path(args.cxx).is_file() else shlex.split(args.cxx, posix=os.name != 'nt')
    env = os.environ.copy()
    if Path(compiler[0]).is_file(): env['PATH'] = str(Path(compiler[0]).resolve().parent) + os.pathsep + env.get('PATH', '')
    result = {'recorded_at': datetime.now(timezone.utc).isoformat(), 'source_ref': args.source_ref or 'working tree', 'head': head, 'source_sha256': {name: sha(value) for name, value in raw.items()}, 'working_source_before': before, 'pair_sha256': pair_before, 'generated_sha256': {name: sha(value) for name, value in files.items()}, 'extraction_sha256': {name: sha(value.encode()) for name, value in extracts.items()}, 'actual_validator_present': helper_present, 'new_private_API_cases': API if helper_present else [], 'absent_helper_case_exclusions': [] if helper_present else API, 'ordered_case_names': catalog, 'original_ordered69': ORIGINAL, 'formatter_recovery_cases': EXTRA, 'variants': [], 'git_status_before': status_before, 'scope': 'Whole actual main, parse_config, descriptor reader/getter, isXmlName and optional actual validator; exact labeled init entry/XML/read/adoption intervals, not whole init. Runtime endpoints/SPI bytes/regular-file existence/preparse/core-name are declared finite lower spies; original FileExists header supplies default1. Affinity is passive with scoped unique MSYS adapter; Linux native CPU declarations retained. Normal formatters forward libc; three named cases inject only declared -1 returns after normal libc writes. Historical unsafe strcpy stops before write and asserts no later continuation; scheduler/poll/exit frontier is distinct from observed main return. No actual file, archive decoder, mounted media, FPGA, hardware, restart, deferred512/256-byte producers, whole init or performance proof. Direct NULL/unterminated API buffers are synthetic query controls, not OS argv.'}
    with generated_directory(args.generated_dir, source) as directory:
        result['generated_directory'] = str(directory)
        for name, body in files.items(): (directory / name).write_bytes(body)
        for variant in ('loop', 'scheduler'):
            exe = directory / ('startup-' + variant + ('.exe' if os.name == 'nt' else ''))
            command = compiler + ['-std=gnu++14', '-D_GNU_SOURCE', '-O2', '-Wall', '-Wextra', '-Werror']
            if variant == 'scheduler': command += ['-DUSE_SCHEDULER']
            command += ['-I', str(directory), str(harness), '-o', str(exe)]
            build = run_process(command, env, 90)
            item = {'name': variant, 'build': build, 'run': None, 'observation_structure_valid': False}
            if build['returncode'] == 0:
                run = run_process([str(exe)], env, 30)
                item['run'] = run
                try:
                    rows = [json.loads(line) for line in run['stdout'].splitlines()]
                    assert len(rows) == len(catalog) + 1 and [row['case'] for row in rows[:-1]] == catalog
                    assert rows[-1]['summary']['cases'] == len(catalog) and rows[-1]['summary']['main_default_cases'] == 74 and rows[-1]['summary']['direct_helper_cases'] == (18 if helper_present else 0)
                    assert sum(row['assertions'] for row in rows[:-1]) + 1 == rows[-1]['summary']['assertions']
                    assert sum(row['failed_assertions'] for row in rows[:-1]) == rows[-1]['summary']['failed_assertions']
                except (json.JSONDecodeError, KeyError, AssertionError, TypeError) as error:
                    item['observation_error'] = repr(error)
                else:
                    item['observations'], item['summary'], item['observation_structure_valid'] = rows[:-1], rows[-1]['summary'], True
                    item['failed_case_names'] = [row['case'] for row in rows[:-1] if row['failed_assertions']]
                item['normal_exit'] = run['returncode'] in (0, 1)
                item['executable_sha256'] = sha(exe.read_bytes())
            result['variants'].append(item)
    after = {name: sha((source / name).read_bytes()) for name in NAMES}
    pair_after = {'python': sha(Path(__file__).read_bytes()), 'cpp': sha(harness.read_bytes())}
    result['working_source_after'], result['pair_sha256_after'] = after, pair_after
    result['git_status_after'] = subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain'], text=True)
    result['source_and_pair_stable'] = before == after and pair_before == pair_after and result['git_status_before'] == result['git_status_after']
    result['all_fixed_policy_assertions_passed'] = result['source_and_pair_stable'] and all(item['build']['returncode'] == 0 and item['build']['stdout'] == item['build']['stderr'] == '' and item['run'] is not None and item['run']['returncode'] == 0 and item['run']['stderr'] == '' and item['observation_structure_valid'] and item['summary']['failed_assertions'] == 0 for item in result['variants'])
    if args.report:
        args.report.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps({'source_ref': result['source_ref'], 'actual_validator_present': helper_present, 'cases_per_variant': len(catalog), 'original_inputs': 69, 'extra_formatter_recovery_inputs': 5, 'direct_helper_inputs': 18 if helper_present else 0, 'variants': [{'name': item['name'], 'build_return': item['build']['returncode'], 'run_return': item['run']['returncode'] if item['run'] else None, 'summary': item.get('summary'), 'strict_compiler_diagnostics_empty': item['build']['stdout'] == item['build']['stderr'] == ''} for item in result['variants']], 'all_fixed_policy_assertions_passed': result['all_fixed_policy_assertions_passed'], 'source_and_pair_stable': result['source_and_pair_stable'], 'report': str(args.report) if args.report else None}))
    if not result['all_fixed_policy_assertions_passed']: raise SystemExit(1)

if __name__ == '__main__':
    main()
