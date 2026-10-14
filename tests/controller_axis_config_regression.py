#!/usr/bin/env python3
"""Prepared actual-source controller configuration/reopen integration fixture.

Requires the implemented Source module, new config cases and actual setter.
It never substitutes implementations when they are absent. Run only after
Root review. No INI lexer, full cfg_parse, legacy deadzone setup or devices run.
"""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
NAMES = ('cfg.cpp', 'cfg.h', 'input.cpp', 'input.h', 'str_util.h',
         'debug.h', 'controller_axis.h', 'controller_axis.cpp')
CASES = (
    'delivered_three_options', 'delivered_whitespace_case',
    'invalid_first_resets_only_own', 'invalid_later_preserves_rows',
    'rejected_rows_no_slots', 'capacity_32_diagnostic',
    'selected_section_replaces', 'independent_first_match',
    'empty_lists_defaults', 'explicit_identity',
    'numeric_selector_families', 'literal_id_sysfs_unique',
    'unique_unconfigured', 'unique_configured',
    'unique_force_nonmatching_config', 'two_devices_independent',
    'delivered_cfg_update_without_reopen', 'reopen_refresh',
    'reopen_absent_defaults', 'setter_preserves_legacy_fields',
)


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def block(text, marker, filename):
    if text.count(marker) != 1:
        raise ValueError('Expected one actual definition: ' + filename + ':' + marker)
    start = text.index(marker)
    opening = text.index('{', start)
    depth, end, state = 1, opening + 1, 'code'
    while depth:
        if end >= len(text):
            raise ValueError('Unclosed actual Source definition')
        char, pair = text[end], text[end:end + 2]
        if state == 'line':
            if char == '\n':
                state = 'code'
        elif state == 'comment':
            if pair == '*/':
                state = 'code'
                end += 1
        elif state in ('"', "'"):
            if char == '\\':
                end += 1
            elif char == state:
                state = 'code'
        elif pair == '//':
            state = 'line'
            end += 1
        elif pair == '/*':
            state = 'comment'
            end += 1
        elif char in ('"', "'"):
            state = char
        else:
            depth += (char == '{') - (char == '}')
        end += 1
    if text[end:end + 1] == ';':
        end += 1
    return text[start:end]


def generate(sources):
    texts = {name: raw.decode('utf-8') for name, raw in sources.items()}
    extracted = {}

    def pin(label, body, scope):
        extracted[label] = dict(scope=scope, source=body, sha256=digest(body.encode()))
        return body + '\n'

    def add(filename, marker):
        body = block(texts[filename], marker, filename)
        line = texts[filename].count('\n', 0, texts[filename].index(marker)) + 1
        return '#line ' + str(line) + ' "' + filename + '"\n' + pin(filename + ':' + marker, body, 'whole actual definition')

    def typedef(filename, typename):
        text = texts[filename]
        ending = '} ' + typename + ';'
        if text.count(ending) != 1:
            raise ValueError('Actual declaration unavailable: ' + typename)
        end = text.index(ending) + len(ending)
        # The nearest typedef before this declaration's opening is its owner.
        start = text.rfind('typedef ', 0, text.index(ending))
        body = text[start:end]
        return pin(filename + ':' + typename, body, 'whole actual declaration')

    def one(filename, pattern, label):
        rows = re.findall(pattern, texts[filename], re.MULTILINE)
        if len(rows) != 1:
            raise ValueError('Actual declaration missing: ' + label)
        return pin(label, rows[0], 'actual declaration')

    if '#include "controller_axis.h"' not in texts['cfg.h']:
        raise ValueError('Actual cfg.h module inclusion not implemented')
    for tag in ('AXIS_SATURATION', 'AXIS_MULTIPLIER', 'AXIS_NONLINEARITY'):
        if not re.search(r'\b' + tag + r'\b', texts['cfg.cpp']):
            raise ValueError('New actual config tag absent: ' + tag)
    for name, member, tag in (
            ('SATURATION', 'controller_saturation', 'AXIS_SATURATION'),
            ('MULTIPLIER', 'controller_multiplier', 'AXIS_MULTIPLIER'),
            ('NONLINEARITY', 'controller_nonlinearity', 'AXIS_NONLINEARITY')):
        rows = [row for row in texts['cfg.cpp'].splitlines() if '"' + name + '"' in row and tag in row and member in row]
        if len(rows) != 1:
            raise ValueError('Actual new option/member/tag mapping absent: ' + name)
    if not re.search(r'controller_axis::Coefficients\s+axis_options\s*;', texts['input.cpp']):
        raise ValueError('Actual per-device coefficient declaration absent')

    types = '#include "cfg.h"\n#include "str_util.h"\n'
    for macro in ('NUMPLAYERS', 'NUMBUTTONS', 'ADVANCED_MAP_MAX'):
        types += one('input.h', r'^#define ' + macro + r'\s+[^\n]+$', 'input.h:' + macro)
    types += one('input.cpp', r'^#define NUMDEV\s+[^\n]+$', 'input.cpp:NUMDEV')
    types += typedef('input.h', 'advancedButtonMap') + typedef('input.h', 'advancedButtonState')
    types += '#ifndef __linux__\n#define ff_effect FixtureOpaqueFFEffect\n#endif\n'
    types += typedef('input.cpp', 'devInput')
    types += '#ifndef __linux__\n#undef ff_effect\n#endif\n'
    types += one('input.cpp', r'^static devInput input\[NUMDEV\] = \{\};$', 'input.cpp:input array')
    types += one('cfg.cpp', r'^cfg_t cfg;$', 'cfg.cpp:cfg global')

    cfg = typedef('cfg.cpp', 'ini_vartypes_t') + typedef('cfg.cpp', 'ini_var_t')
    cfg += add('cfg.cpp', 'static const ini_var_t ini_vars[] =')
    cfg += one('cfg.cpp', r'^static bool var_array_append\[[^\n]+$', 'cfg.cpp:var_array_append')
    for pattern, label in (
            (r'^static FILE \*orig_stdout = NULL;$', 'orig_stdout'),
            (r'^static FILE \*dev_null = NULL;$', 'dev_null'),
            (r'^static FILE \*debug_file = NULL;$', 'debug_file'),
            (r'^static constexpr int CFG_ERRORS_MAX = [^\n]+$', 'CFG_ERRORS_MAX'),
            (r'^static constexpr int CFG_ERRORS_STRLEN = [^\n]+$', 'CFG_ERRORS_STRLEN'),
            (r'^static char cfg_errors\[[^\n]+$', 'cfg_errors'),
            (r'^static int cfg_error_count = 0;$', 'cfg_error_count')):
        cfg += one('cfg.cpp', pattern, 'cfg.cpp:' + label)
    for macro in ('CHAR_IS_SPACE',):
        cfg += one('cfg.cpp', r'^#define ' + macro + r'\([^\n]+$', 'cfg.cpp:' + macro)
    cfg += one('debug.h', r'^#define ini_parser_debugf\(a, [^\n]+$', 'debug.h:ini_parser_debugf')
    cfg += add('cfg.cpp', 'void cfg_error(')
    cfg += ('// Explicit forbidden lower boundary: no legacy numeric option runs.\n'
            'static void ini_parse_numeric(const ini_var_t *, const char *, void *) { throw std::runtime_error("unexpected legacy numeric dispatch"); }\n')
    cfg += add('cfg.cpp', 'static void ini_parse_var(')
    resets = re.findall(r'^[ \t]*memset\(var_array_append, 0, sizeof\(var_array_append\)\);$', texts['cfg.cpp'], re.MULTILINE)
    if len(resets) != 2 or len({row.strip() for row in resets}) != 1:
        raise ValueError('Actual selected-section reset statement changed')
    statement = resets[0].strip()
    pin('cfg.cpp:selected-section reset', statement, 'actual statement from both selected-section branches; declarative boundary, not full ini_parse execution')
    cfg += 'static void fixture_selected_section_boundary() {\n' + statement + '\n}\n'

    setter = '#define uint unsigned int\n#define get_unique_mapping production_get_unique_mapping\n'
    setter += add('input.cpp', 'static char *get_unique_mapping(')
    setter += '#undef get_unique_mapping\n#undef uint\n'
    setter += ('static char *get_unique_mapping(int dev, int force_unique = 0) {\n'
               ' char *value = production_get_unique_mapping(dev, force_unique);\n'
               ' getter_observations.push_back(GetterObservation{dev, force_unique, value}); return value;\n}\n')
    setter += add('input.cpp', 'static void setup_controller_axes(')
    call = re.findall(r'^[ \t]*setup_controller_axes\(i\);$', texts['input.cpp'], re.MULTILINE)
    if len(call) != 1:
        raise ValueError('Actual reopen setter invocation absent or ambiguous')
    pin('input.cpp:reopen setter invocation', call[0], 'actual call placement is source evidence only; fixture directly invokes setter without enumeration/legacy setup')

    files = {'axis_config_types.inc': types.encode(), 'axis_config_cfg.inc': cfg.encode(),
             'axis_config_setter.inc': setter.encode(),
             'axis_config_cases.inc': ('static const char *case_names[] = {' + ','.join(json.dumps(x) for x in CASES) + '};\n').encode()}
    for name in ('cfg.h', 'str_util.h', 'controller_axis.h', 'controller_axis.cpp'):
        files[name] = sources[name]
    return files, extracted


@contextmanager
def build_directory(requested, root):
    if requested:
        directory = requested.resolve()
        if directory == root or root in directory.parents:
            raise ValueError('Generated files must stay outside Source')
        directory.mkdir(parents=True, exist_ok=False)
        yield directory
    else:
        with tempfile.TemporaryDirectory(prefix='mister-axis-config-') as temporary:
            directory = Path(temporary).resolve()
            if directory == root or root in directory.parents:
                raise ValueError('Generated files must stay outside Source')
            yield directory


def main():
    default = HERE.parent if (HERE.parent / 'input.cpp').exists() else HERE.parent / 'Main_MiSTer'
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', '--root', dest='source', type=Path, default=default)
    parser.add_argument('--source-ref')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    parser.add_argument('--generated-dir', type=Path)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    root = args.source.resolve()
    if args.report and (args.report.resolve() == root or root in args.report.resolve().parents):
        raise ValueError('Report must stay outside Source')
    harness = Path(__file__).with_suffix('.cpp')
    pair = {str(path): digest(path.read_bytes()) for path in (Path(__file__), harness)}
    git = lambda *command: subprocess.check_output(['git', '-C', str(root), *command], timeout=30)
    head = git('rev-parse', 'HEAD').decode('ascii').strip()
    status = git('status', '--porcelain').decode('utf-8')
    before = {name: digest((root / name).read_bytes()) for name in NAMES if (root / name).is_file()}
    sources, missing = {}, []
    for name in NAMES:
        if args.source_ref:
            read = subprocess.run(['git', '-C', str(root), 'show', args.source_ref + ':' + name], capture_output=True, timeout=30)
            if read.returncode:
                missing.append(name)
            else:
                sources[name] = read.stdout
        elif (root / name).is_file():
            sources[name] = (root / name).read_bytes()
        else:
            missing.append(name)
    receipt = dict(source_ref=args.source_ref or 'working tree', source_HEAD=head,
                   pair_sha256=pair, source_raw_before=before,
                   selected_source_sha256={name: digest(raw) for name, raw in sources.items()},
                   catalog=list(CASES), build=None, runs=[], observations=[],
                   API_absent=bool(missing), missing_source_files=missing)
    passed = False
    if not missing:
        generated, extracted = generate(sources)
        generated['axis_config_harness.cpp'] = harness.read_bytes()
        receipt['extractions'] = extracted
        compiler = [args.cxx] if Path(args.cxx).is_file() else shlex.split(args.cxx, posix=os.name != 'nt')
        env = os.environ.copy()
        if Path(compiler[0]).is_file():
            env['PATH'] = str(Path(compiler[0]).resolve().parent) + os.pathsep + env.get('PATH', '')
        with build_directory(args.generated_dir, root) as directory:
            for name, raw in generated.items():
                (directory / name).write_bytes(raw)
            executable = directory / ('axis-config.exe' if os.name == 'nt' else 'axis-config')
            command = compiler + ['-std=gnu++14', '-D_GNU_SOURCE', '-O2', '-Wall', '-Wextra', '-Werror', '-pedantic', '-I', '.', 'controller_axis.cpp', 'axis_config_harness.cpp', '-o', executable.name]
            build = subprocess.run(command, cwd=directory, capture_output=True, encoding='utf-8', errors='strict', env=env, timeout=120)
            receipt['build'] = dict(command=command, cwd=str(directory), returncode=build.returncode, stdout=build.stdout, stderr=build.stderr)
            receipt['generated_sha256'] = {name: digest(raw) for name, raw in generated.items()}
            if build.returncode == 0 and not build.stdout and not build.stderr:
                for index, name in enumerate(CASES):
                    run = subprocess.run([str(executable), str(index)], capture_output=True, encoding='utf-8', errors='strict', env=env, timeout=15)
                    receipt['runs'].append(dict(name=name, returncode=run.returncode, stdout=run.stdout, stderr=run.stderr))
                    lines = [line[len('CASE_JSON='):] for line in run.stdout.splitlines() if line.startswith('CASE_JSON=')]
                    if len(lines) == 1:
                        item = json.loads(lines[0])
                        if item['name'] != name:
                            raise ValueError('Wrong named runtime observation')
                        receipt['observations'].append(item)
            receipt['generated_directory'] = str(directory)
    else:
        receipt['exclusions'] = [dict(name=name, reason='actual new Source module/API absent; no substitute or historical execution') for name in CASES]
    after = {name: digest((root / name).read_bytes()) for name in NAMES if (root / name).is_file()}
    receipt['source_raw_after'] = after
    receipt['pair_sha256_after'] = {str(path): digest(path.read_bytes()) for path in (Path(__file__), harness)}
    receipt['source_and_pair_stable'] = before == after and pair == receipt['pair_sha256_after'] and git('status', '--porcelain').decode('utf-8') == status
    observations = receipt['observations']
    receipt['summary'] = dict(cases=len(observations), assertions=sum(x['assertions'] for x in observations), failed_assertions=sum(x['failed_assertions'] for x in observations), excluded_cases=len(CASES) if missing else 0)
    receipt['boundaries'] = ['complete actual ini enum/table/new ini_parse_var and cfg_error', 'declarative actual selected-section flag-reset statement, not full file/section parser', 'actual setup_controller_axes/get_unique_mapping and actual device/config declarations', 'no legacy setup_deadzone/ini_parse_numeric execution, full cfg_parse, file lexer or hardware']
    if receipt['build']:
        build = receipt['build']
        passed = build['returncode'] == 0 and not build['stdout'] and not build['stderr'] and len(observations) == len(CASES) == 20 and len(set(CASES)) == 20 and all(run['returncode'] == 0 and not run['stderr'] for run in receipt['runs']) and receipt['summary']['failed_assertions'] == 0 and receipt['source_and_pair_stable']
    receipt['integration_assertions_passed'] = passed
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        with args.report.open('x', encoding='utf-8', newline='\n') as output:
            json.dump(receipt, output, indent=2)
            output.write('\n')
    print(json.dumps(dict(source_ref=receipt['source_ref'], summary=receipt['summary'], API_absent=bool(missing), integration_assertions_passed=passed, report=str(args.report) if args.report else None)))
    return 0 if passed else 2 if missing else 1


if __name__ == '__main__':
    result = main()
    if result:
        raise SystemExit(result)
