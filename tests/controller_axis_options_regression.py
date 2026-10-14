#!/usr/bin/env python3
"""Normal-input production-body baseline; prepare/read before first execution.

This baseline does not implement controller options. It does not compile legacy
deadzone setup, configuration parsing or device enumeration. Configured ordinary
deadzone values enter at an explicit fixture boundary. Each named case runs in a
fresh process, resetting the original function-local paired-axis position.
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
NAMES = ('input.cpp', 'input.h', 'cfg.h', 'user_io.cpp', 'user_io.h',
         'spi.h', 'fpga_io.h', 'str_util.h',
         'support/n64/n64_joy_emu.cpp', 'support/n64/n64_joy_emu.h')
CASES = [
    'math_deadzone_0', 'math_deadzone_1', 'math_deadzone_2',
    'math_deadzone_10', 'math_deadzone_64',
    'generic_left_byte', 'generic_left_word', 'generic_right_byte', 'generic_right_word',
    'psx_left_word', 'psx_right_byte', 'ordinary_center_deadband',
    'raw_duplicate_suppression', 'bound_subdevice', 'multiple_devices_players',
    'grab_disabled', 'osd_visible', 'unassigned_player', 'non_8bit_output',
    'mouse_x_route', 'mouse_y_route', 'paddle_byte', 'paddle_word', 'spinner_route',
    'lightgun_positional', 'lightgun_other_mapped_stick', 'wheel_steering',
    'wheel_pedal', 'wheel_pedal_inverted', 'wheel_shared_combo',
    'n64_cardinal_learning', 'n64_diagonal_learning',
    'n64_tv_0', 'n64_tv_1', 'n64_tv_2', 'n64_tv_3',
    'n64_tv_4', 'n64_tv_5', 'n64_tv_6', 'n64_tv_7',
    'identity_default_mapping', 'identity_configured_unique',
    'identity_unmatched_unique', 'identity_utf8_empty_force',
    'n64_p3_reverse_player', 'n64_p2_reverse_player',
    'joyswap_left', 'joyswap_right', 'generic_deadzone10_sequence',
    'generic_deadzone64_sequence', 'psx_deadzone10_sequence', 'psx_deadzone64_sequence',
]


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def definition(text, marker, filename):
    if text.count(marker) != 1:
        raise ValueError('Expected one actual definition: ' + filename + ':' + marker)
    start = text.index(marker)
    opening = text.index('{', start)
    depth, end, state = 1, opening + 1, 'code'
    while depth:
        if end >= len(text):
            raise ValueError('Unclosed actual definition')
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

    def add(filename, marker):
        body = definition(texts[filename], marker, filename)
        key = filename + ':' + marker
        extracted[key] = dict(scope='whole actual definition', sha256=digest(body.encode()), source=body)
        line = texts[filename].count('\n', 0, texts[filename].index(marker)) + 1
        return '#line ' + str(line) + ' "' + filename + '"\n' + body + '\n'

    def typedef(filename, typename):
        text = texts[filename]
        end = text.index('} ' + typename + ';') + len('} ' + typename + ';')
        start = text.rfind('typedef struct', 0, end)
        body = text[start:end]
        extracted[filename + ':' + typename] = dict(scope='whole actual declaration', sha256=digest(body.encode()), source=body)
        return body + '\n'

    header = texts['input.h']
    macros = '\n'.join(re.findall(r'^#define (?:NUMPLAYERS|NUMBUTTONS|ADVANCED_MAP_MAX|SYS_[A-Z0-9_]+)\s+[^\n]+', header, re.MULTILINE)) + '\n'
    if not macros or not re.search(r'^#define NUMDEV\s+30\s*$', texts['input.cpp'], re.MULTILINE):
        raise ValueError('Actual array declarations/constants missing')
    types = macros + '#define NUMDEV 30\n' + typedef('input.h', 'advancedButtonMap') + typedef('input.h', 'advancedButtonState')
    types += add('input.cpp', 'enum QUIRK\n')
    types += '#ifndef __linux__\n#define ff_effect FixtureOpaqueFFEffect\n#endif\n'
    types += typedef('input.cpp', 'devInput')
    types += '#ifndef __linux__\n#undef ff_effect\n#endif\n'
    declarations = []
    for pattern in (r'^static devInput input\[NUMDEV\] = \{\};$', r'^static int grabbed = 1;$',
                    r'^static int mouse_emu = 0;$', r'^static int mouse_emu_x = 0;$', r'^static int mouse_emu_y = 0;$'):
        matches = re.findall(pattern, texts['input.cpp'], re.MULTILINE)
        if len(matches) != 1:
            raise ValueError('Actual scalar declaration missing: ' + pattern)
        declarations += matches
    for pattern in (r'^static unsigned char core_type = CORE_TYPE_UNKNOWN;$',
                    r'^static int io_ver = 0;$', r'^static int joyswap = 0;$'):
        matches = re.findall(pattern, texts['user_io.cpp'], re.MULTILINE)
        if len(matches) != 1:
            raise ValueError('Actual output declaration missing')
        declarations += matches
    constants = '\n'.join(re.findall(r'^#define (?:CORE_TYPE_[A-Z0-9_]+|UIO_ASTICK(?:_2)?)\s+[^\n]+', texts['user_io.h'], re.MULTILINE)) + '\n'
    types += constants + '\n'.join(declarations) + '\n'
    extracted['actual globals/constants'] = dict(scope='actual declarations/macros', source=types, sha256=digest(types.encode()))
    interfaces = []
    for name in ('user_io_status_get', 'user_io_osd_is_visible', 'is_psx', 'is_n64',
                 'user_io_l_analog_joystick', 'user_io_r_analog_joystick'):
        matches = re.findall(r'^[^\n{}]*\b' + name + r'\([^\n]*\);$', texts['user_io.h'], re.MULTILINE)
        if len(matches) != 1:
            raise ValueError('Actual interface missing: ' + name)
        interfaces += matches
    types += '\n'.join(interfaces) + '\n'
    extracted['used user_io header declarations'] = dict(scope='exact used public declarations/defaults', source='\n'.join(interfaces), sha256=digest('\n'.join(interfaces).encode()))
    output = ''
    for side in ('l', 'r'):
        name = 'user_io_' + side + '_analog_joystick'
        output += '#define ' + name + ' production_' + name + '\n'
        output += add('user_io.cpp', 'void ' + name + '(')
        output += '#undef ' + name + '\n'
        output += 'void ' + name + '(unsigned char num, char x, char y) { record_api("' + side + '", num, x, y); production_' + name + '(num, x, y); }\n'
    math = add('input.cpp', 'static inline void joy_clamp(') + add('input.cpp', 'static inline float boxradf(')
    math += '#define joy_apply_deadzone production_joy_apply_deadzone\n'
    math += add('input.cpp', 'static void joy_apply_deadzone(')
    math += '#undef joy_apply_deadzone\n'
    math += 'static void joy_apply_deadzone(int *x,int *y,const devInput *d,const int stick) { record_deadzone(x,y,d,stick); production_joy_apply_deadzone(x,y,d,stick); finish_deadzone(*x,*y); }\n'
    n64 = texts['support/n64/n64_joy_emu.cpp']
    constants = n64[n64.index('static constexpr int MAX_DIAG'):n64.index('void stick_swap(')]
    extracted['N64 actual constants'] = dict(scope='actual whole constants', source=constants, sha256=digest(constants.encode()))
    math += constants + add('support/n64/n64_joy_emu.cpp', 'void stick_swap(')
    math += '#define n64_joy_emu production_n64_joy_emu\n' + add('support/n64/n64_joy_emu.cpp', 'void n64_joy_emu(')
    math += '#undef n64_joy_emu\n'
    math += 'void n64_joy_emu(const int x,const int y,int *xo,int *yo,int cardinal,float range) { record_n64(x,y,cardinal,range); production_n64_joy_emu(x,y,xo,yo,cardinal,range); finish_n64(*xo,*yo); }\n'
    math += add('input.cpp', 'static bool joy_dir_is_diagonal(') + add('input.cpp', 'static void joy_analog(')
    math += '#define uint unsigned int\n' + add('input.cpp', 'static char *get_unique_mapping(') + '#undef uint\n'
    cpp = texts['input.cpp']
    start = cpp.index('\t\t//analog joystick\n\t\tcase EV_ABS:')
    end = cpp.index('\n\t\t}\n\t}\n}', start)
    dispatch = cpp[start:end]
    if dispatch.count('case EV_ABS:') != 1 or dispatch.count('case EV_REL:') != 1:
        raise ValueError('Expected complete original normal EV_ABS/EV_REL switch intervals')
    extracted['input_cb normal EV_ABS/EV_REL interval'] = dict(scope='selected complete switch cases, not whole input_cb/input_test', first_line=cpp.count('\n', 0, start) + 1, source=dispatch, sha256=digest(dispatch.encode()))
    entry = 'int sub_dev = dev;\n\n\t//check if device is a part of multifunctional device\n\tif (!JOYCON_COMBINED(dev) && input[dev].bind >= 0) dev = input[dev].bind;'
    if cpp.count(entry) != 1:
        raise ValueError('Actual binding interval changed')
    extracted['input_cb actual binding interval'] = dict(scope='selected entry interval', source=entry, sha256=digest(entry.encode()))
    joycon_macros = '\n'.join(re.findall(r'^#define JOYCON_[^\n]+', cpp, re.MULTILINE)) + '\n'
    dispatch = joycon_macros + 'static void selected_normal_event(FixtureEvent *ev, FixtureAbsInfo *absinfo, int dev) {\n' + entry + '\n switch(ev->type) {\n' + dispatch + '\n }\n}\n'
    files = {'axis_types.inc': types.encode(), 'axis_output.inc': output.encode(),
             'axis_math.inc': math.encode(), 'axis_dispatch.inc': dispatch.encode(),
             'axis_cases.inc': ('static const char *case_names[] = {' + ','.join(json.dumps(x) for x in CASES) + '};\n').encode()}
    for name in ('cfg.h', 'spi.h', 'str_util.h'):
        files['axis_fixture_' + name] = sources[name]
    files['fpga_io.h'] = sources['fpga_io.h']
    return files, extracted


@contextmanager
def build_directory(requested, root):
    if requested:
        path = requested.resolve()
        if path == root or root in path.parents:
            raise ValueError('Generated files must stay outside Source')
        path.mkdir(parents=True, exist_ok=False)
        yield path
    else:
        with tempfile.TemporaryDirectory(prefix='mister-normal-axis-') as temporary:
            path = Path(temporary).resolve()
            if path == root or root in path.parents:
                raise ValueError('Temporary build must stay outside Source')
            yield path


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
        raise ValueError('Evidence report must stay outside Source')
    harness = Path(__file__).with_suffix('.cpp')
    pair = {str(path): digest(path.read_bytes()) for path in (Path(__file__), harness)}
    raw_before = {name: digest((root / name).read_bytes()) for name in NAMES}
    git = lambda *command: subprocess.check_output(['git', '-C', str(root), *command], timeout=30)
    head, status = git('rev-parse', 'HEAD').decode().strip(), git('status', '--porcelain').decode('utf-8')
    sources = {name: git('show', args.source_ref + ':' + name) if args.source_ref else (root / name).read_bytes() for name in NAMES}
    generated, extracted = generate(sources)
    generated['axis_harness.cpp'] = harness.read_bytes()
    compiler = [args.cxx] if Path(args.cxx).is_file() else shlex.split(args.cxx, posix=os.name != 'nt')
    env = os.environ.copy()
    if Path(compiler[0]).is_file():
        env['PATH'] = str(Path(compiler[0]).resolve().parent) + os.pathsep + env.get('PATH', '')
    receipt = dict(source_ref=args.source_ref or 'working tree', source_HEAD=head,
                   pair_sha256=pair, source_raw_before=raw_before,
                   selected_source_sha256={name: digest(raw) for name, raw in sources.items()},
                   extractions=extracted, catalog=CASES, build=None, runs=[], observations=[])
    with build_directory(args.generated_dir, root) as directory:
        for name, raw in generated.items():
            (directory / name).write_bytes(raw)
        executable = directory / ('axis.exe' if os.name == 'nt' else 'axis')
        command = compiler + ['-std=gnu++14', '-D_GNU_SOURCE', '-O2', '-Wall', '-Wextra', '-Werror', '-funsigned-char', '-I', '.', 'axis_harness.cpp', '-o', executable.name]
        build = subprocess.run(command, cwd=directory, capture_output=True, encoding='utf-8', errors='strict', env=env, timeout=120)
        receipt['build'] = dict(command=command, cwd=str(directory), returncode=build.returncode, stdout=build.stdout, stderr=build.stderr)
        receipt['generated_sha256'] = {name: digest(raw) for name, raw in generated.items()}
        if build.returncode == 0 and not build.stdout and not build.stderr:
            for index, name in enumerate(CASES):
                run = subprocess.run([str(executable), str(index)], capture_output=True, encoding='utf-8', errors='strict', env=env, timeout=15)
                receipt['runs'].append(dict(name=name, returncode=run.returncode, stdout=run.stdout, stderr=run.stderr))
                if run.stdout.startswith('CASE_JSON='):
                    item = json.loads(run.stdout[len('CASE_JSON='):])
                    if item['name'] != name:
                        raise ValueError('Wrong named-case runtime observation')
                    receipt['observations'].append(item)
        receipt['generated_directory'] = str(directory)
    receipt['source_raw_after'] = {name: digest((root / name).read_bytes()) for name in NAMES}
    receipt['pair_sha256_after'] = {str(path): digest(path.read_bytes()) for path in (Path(__file__), harness)}
    receipt['source_and_pair_stable'] = receipt['source_raw_after'] == raw_before and receipt['pair_sha256_after'] == pair and git('status', '--porcelain').decode('utf-8') == status
    observations = receipt['observations']
    receipt['summary'] = dict(cases=len(observations), assertions=sum(x['assertions'] for x in observations), failed_assertions=sum(x['failed_assertions'] for x in observations))
    receipt['strict_compiler_diagnostics_empty'] = not build.stdout and not build.stderr
    receipt['exclusions'] = ['new controller option module/APIs and explicit new settings absent from historical Source', 'legacy setup_deadzone/config parser/device enumeration', 'whole input_cb/input_test/input_poll', 'real evdev/SPI/FPGA/controller feel/latency and user media']
    receipt['ABI_scope'] = 'Native Linux event/FF types; non-Linux unique field-only event/absinfo and untouched opaque FF stand-ins. -funsigned-char explicitly models byte-oriented consumer chars; not a verified platform-default claim or warning suppression.'
    passed = build.returncode == 0 and receipt['strict_compiler_diagnostics_empty'] and len(observations) == len(CASES) == 52 and len(set(CASES)) == 52 and all(x['returncode'] == 0 and not x['stderr'] for x in receipt['runs']) and receipt['summary']['failed_assertions'] == 0 and receipt['source_and_pair_stable']
    receipt['all_baseline_assertions_passed'] = passed
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        with args.report.open('x', encoding='utf-8', newline='\n') as output:
            json.dump(receipt, output, indent=2)
            output.write('\n')
    print(json.dumps(dict(source_ref=receipt['source_ref'], build_return=build.returncode,
                         summary=receipt['summary'], all_baseline_assertions_passed=passed,
                         source_and_pair_stable=receipt['source_and_pair_stable'], report=str(args.report) if args.report else None)))
    return 0 if passed else (build.returncode or 1)


if __name__ == '__main__':
    result = main()
    if result:
        raise SystemExit(result)
