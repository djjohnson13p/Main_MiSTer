#!/usr/bin/env python3
"""Production module plus normal-input routing regression, with pinned baseline.

No configuration parser, enumeration, legacy setup_deadzone or hardware executes.
The two fixture hooks only supply admitted coefficients before byte snapshots and
dispatch additional named cases. Whole production definitions remain unchanged.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess

HERE = Path(__file__).resolve().parent
BASELINE_PY_SHA = 'e8b8142a460cda3fb6101cb297fa39b57e8bbd5e7ba32e1789658c0aef15cd89'
BASELINE_CPP_SHA = '72f52950ee9a0f65b5602707b8775dabcadbed1270f1e96e25a9489833dd1c73'
MODULE_CHECKS_SHA = '7c4c21398c2532d6b6bf61e58182e62bde05f7b29d571500fa67bdf7da31f65b'
MODULE_NAMES = ('controller_axis.h', 'controller_axis.cpp')
ACTIVE_CASES = [
    'active_left_gain_half', 'active_right_gain_half',
    'active_left_saturation10', 'active_right_curve2',
    'active_bound_device', 'active_lightgun_aux', 'active_psx_saturation10',
    'active_n64_gain_half', 'active_excluded_wheel', 'active_excluded_paddle',
    'active_excluded_spinner', 'active_excluded_positional_gun',
    'active_excluded_mouse', 'active_raw_learning', 'active_after_deadzone10',
    'active_explicit_identity_fields',
]
REPRESENTATION_ONLY = (
    'same_build_input_signature_before', 'same_build_input_signature_after',
    'same_build_cfg_signature',
)


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def pinned_baseline():
    path = HERE / 'controller_axis_options_regression.py'
    harness = path.with_suffix('.cpp')
    if digest(path.read_bytes()) != BASELINE_PY_SHA or digest(harness.read_bytes()) != BASELINE_CPP_SHA:
        raise ValueError('Frozen sibling baseline pair differs; review before changing pins')
    spec = importlib.util.spec_from_file_location('axis_frozen_baseline', path)
    module = importlib.util.module_from_spec(spec)
    # No __main__ execution and no import-time bytecode writes into Source/tests.
    code = compile(path.read_bytes(), str(path), 'exec')
    exec(code, module.__dict__)
    if len(module.CASES) != 52 or len(set(module.CASES)) != 52:
        raise ValueError('Frozen baseline catalog mismatch')
    return module, harness


def canonical_observation(item):
    result = dict(item)
    for key in REPRESENTATION_ONLY:
        if key not in result:
            raise ValueError('Missing explicitly excluded representation field: ' + key)
        del result[key]
    return result


def compatibility_comparison(path, observations, catalog):
    previous = json.loads(path.read_text(encoding='utf-8'))
    old = previous['observations']
    if not previous.get('all_baseline_assertions_passed', previous.get('passed', False)):
        raise ValueError('Comparison reference must be an actually passing retained report')
    if len(old) < 52 or [x['name'] for x in old[:52]] != catalog:
        raise ValueError('Comparison report lacks exact ordered original52 catalog')
    if len(observations) < 52:
        return dict(reference=str(path), sha256=digest(path.read_bytes()), passed=False,
                    reason='Incomplete current observations', ignored_fields=list(REPRESENTATION_ONLY))
    differences = [catalog[i] for i in range(52)
                   if canonical_observation(observations[i]) != canonical_observation(old[i])]
    return dict(reference=str(path), sha256=digest(path.read_bytes()), cases=52,
                ignored_fields=list(REPRESENTATION_ONLY), differences=differences,
                passed=not differences)


def main():
    baseline, old_harness = pinned_baseline()
    default = HERE.parent if (HERE.parent / 'input.cpp').exists() else HERE.parent / 'Main_MiSTer'
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', '--root', dest='source', type=Path, default=default)
    parser.add_argument('--source-ref')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    parser.add_argument('--generated-dir', type=Path)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--baseline-report', type=Path, help='Optional original52 full observation comparison')
    args = parser.parse_args()
    root = args.source.resolve()
    if args.report and (args.report.resolve() == root or root in args.report.resolve().parents):
        raise ValueError('Evidence report must stay outside Source')
    harness = Path(__file__).with_suffix('.cpp')
    checks = HERE / 'controller_axis_module_checks.cpp'
    dependencies = (Path(__file__), harness, HERE / 'controller_axis_options_regression.py', old_harness, checks)
    if digest(checks.read_bytes()) != MODULE_CHECKS_SHA:
        raise ValueError('Reviewed sibling module checks differ')
    dependency_before = {str(path): digest(path.read_bytes()) for path in dependencies}
    git = lambda *command: subprocess.check_output(['git', '-C', str(root), *command], timeout=30)
    head = git('rev-parse', 'HEAD').decode('utf-8').strip()
    status = git('status', '--porcelain').decode('utf-8')
    selected_files = set(git('ls-tree', '-r', '--name-only', args.source_ref).decode('utf-8').splitlines()) if args.source_ref else None
    present = [(name in selected_files) if selected_files is not None else (root / name).is_file() for name in MODULE_NAMES]
    if any(present) != all(present):
        raise ValueError('Actual module header/implementation must be jointly present or absent')
    feature = all(present)
    names = tuple(baseline.NAMES) + (MODULE_NAMES if feature else ())
    # Observe optional current module paths even on historical selections.
    observed_names = tuple(baseline.NAMES) + MODULE_NAMES + ('cfg.cpp',)
    current_pins = lambda: {name: digest((root / name).read_bytes()) if (root / name).is_file() else None for name in observed_names}
    raw_before = current_pins()
    sources = {name: git('show', args.source_ref + ':' + name) if args.source_ref else (root / name).read_bytes() for name in names}
    input_text = sources['input.cpp'].decode('utf-8')
    if feature != ('controller_axis::Coefficients axis_options;' in input_text):
        raise ValueError('Actual device coefficient declaration and module presence disagree')
    if feature and 'bool apply_axis_options = false' not in input_text:
        raise ValueError('Actual private default-false eligibility interface missing')
    generated, extracted = baseline.generate({name: sources[name] for name in baseline.NAMES})
    copied_harness = old_harness.read_text(encoding='utf-8')
    hooks = [
        ('    setup(index);\n', '    setup(index);\n    candidate_setup(index);\n'),
        ('    default: check(false,"unknown finite case"); break;', '    default: candidate_active_case(index); break;'),
    ]
    for old, new in hooks:
        if copied_harness.count(old) != 1:
            raise ValueError('Frozen harness hook is not unique: ' + old)
        copied_harness = copied_harness.replace(old, new, 1)
    catalog = list(baseline.CASES) + (ACTIVE_CASES if feature else [])
    if len(set(catalog)) != len(catalog) or len(ACTIVE_CASES) != 16:
        raise ValueError('Candidate catalog must be unique and explicit')
    generated['axis_baseline_harness.cpp'] = copied_harness.encode('utf-8')
    generated['axis_harness.cpp'] = harness.read_bytes()
    generated['axis_candidate_presence.h'] = ('#define AXIS_CANDIDATE_PRESENT ' + str(int(feature)) + '\n').encode()
    generated['axis_cases.inc'] = ('static const char *case_names[] = {' + ','.join(json.dumps(x) for x in catalog) + '};\n').encode()
    if feature:
        for name in MODULE_NAMES:
            generated[name] = sources[name]
            extracted[name] = dict(scope='whole actual production file compiled unchanged', sha256=digest(sources[name]), source=sources[name].decode('utf-8'))
        generated['axis_module_checks.cpp'] = checks.read_bytes()
    compiler = [args.cxx] if Path(args.cxx).is_file() else shlex.split(args.cxx, posix=os.name != 'nt')
    if not compiler:
        raise ValueError('Empty compiler command')
    env = os.environ.copy()
    if Path(compiler[0]).is_file():
        env['PATH'] = str(Path(compiler[0]).resolve().parent) + os.pathsep + env.get('PATH', '')
    receipt = dict(source_ref=args.source_ref or 'working tree', source_HEAD=head,
                   source_status_before=status, actual_module_present=feature,
                   dependency_sha256=dependency_before, source_raw_before=raw_before,
                   selected_source_sha256={name: digest(raw) for name, raw in sources.items()},
                   extractions=extracted, catalog=catalog, baseline_catalog=list(baseline.CASES),
                   active_catalog=ACTIVE_CASES, fixture_only_hooks=[dict(before=a, after=b) for a, b in hooks],
                   build=None, runs=[], observations=[], module_checks=None)
    flags = ['-std=gnu++14', '-D_GNU_SOURCE', '-O2', '-Wall', '-Wextra', '-Werror', '-funsigned-char', '-I', '.']
    with baseline.build_directory(args.generated_dir, root) as directory:
        for name, raw in generated.items():
            (directory / name).write_bytes(raw)
        executable = directory / ('axis.exe' if os.name == 'nt' else 'axis')
        command = compiler + flags + ['axis_harness.cpp'] + (['controller_axis.cpp'] if feature else []) + ['-o', executable.name]
        build = subprocess.run(command, cwd=directory, capture_output=True, encoding='utf-8', errors='strict', env=env, timeout=120)
        receipt['build'] = dict(command=command, cwd=str(directory), returncode=build.returncode, stdout=build.stdout, stderr=build.stderr)
        receipt['generated_sha256'] = {name: digest(raw) for name, raw in generated.items()}
        if build.returncode == 0 and not build.stdout and not build.stderr:
            for index, name in enumerate(catalog):
                run = subprocess.run([str(executable), str(index)], capture_output=True, encoding='utf-8', errors='strict', env=env, timeout=15)
                receipt['runs'].append(dict(name=name, returncode=run.returncode, stdout=run.stdout, stderr=run.stderr))
                if run.stdout.startswith('CASE_JSON='):
                    item = json.loads(run.stdout[len('CASE_JSON='):])
                    if item['name'] != name:
                        raise ValueError('Wrong named-case runtime observation')
                    receipt['observations'].append(item)
        if feature:
            module_exe = directory / ('axis_module.exe' if os.name == 'nt' else 'axis_module')
            module_command = compiler + flags + ['axis_module_checks.cpp', 'controller_axis.cpp', '-o', module_exe.name]
            module_build = subprocess.run(module_command, cwd=directory, capture_output=True, encoding='utf-8', errors='strict', env=env, timeout=120)
            module_record = dict(command=module_command, cwd=str(directory), returncode=module_build.returncode, stdout=module_build.stdout, stderr=module_build.stderr)
            module_run = None
            if module_build.returncode == 0 and not module_build.stdout and not module_build.stderr:
                run = subprocess.run([str(module_exe)], capture_output=True, encoding='utf-8', errors='strict', env=env, timeout=30)
                module_run = dict(returncode=run.returncode, stdout=run.stdout, stderr=run.stderr)
            module_passed = module_run is not None and module_run['returncode'] == 0 and not module_run['stderr'] and module_run['stdout'].splitlines()[-1:] == ['PASS controller_axis_module_checks checks=16656 failures=0']
            receipt['module_checks'] = dict(build=module_record, run=module_run, required_checks=16656, passed=module_passed)
        receipt['generated_directory'] = str(directory)
    receipt['source_raw_after'] = current_pins()
    receipt['dependency_sha256_after'] = {str(path): digest(path.read_bytes()) for path in dependencies}
    receipt['source_and_dependencies_stable'] = receipt['source_raw_after'] == raw_before and receipt['dependency_sha256_after'] == dependency_before and git('status', '--porcelain').decode('utf-8') == status
    observations = receipt['observations']
    receipt['summary'] = dict(cases=len(observations), compatibility_cases=min(len(observations), 52),
                            active_cases=max(len(observations)-52, 0),
                            assertions=sum(x['assertions'] for x in observations),
                            failed_assertions=sum(x['failed_assertions'] for x in observations))
    receipt['strict_compiler_diagnostics_empty'] = not build.stdout and not build.stderr and (not feature or (not receipt['module_checks']['build']['stdout'] and not receipt['module_checks']['build']['stderr']))
    receipt['comparison'] = compatibility_comparison(args.baseline_report, observations, list(baseline.CASES)) if args.baseline_report else None
    receipt['historical_exclusions'] = [] if feature else ACTIVE_CASES + ['actual new module API checks: module absent; no synthetic replacement']
    receipt['exclusions'] = ['cfg.cpp/INI lexer and new list/open configuration integration', 'legacy setup_deadzone and device enumeration', 'whole input_cb/input_test/input_poll and live reopen', 'touchgun producer and processing before the selected normal event interval', 'real evdev/SPI/FPGA, physical controller feel/latency, user media', 'all RBF/security-focused work']
    receipt['ABI_scope'] = 'Frozen baseline adapters and explicit -funsigned-char byte model retained. Native Linux event/FF types, unique non-Linux field-only stand-ins; no native ARM execution or pointer-width parity claim.'
    receipt['compatibility_policy'] = 'Original52 case names, assertion counts and every exported meaningful observation must compare exactly when --baseline-report is supplied. Only three explicit same_build representation fingerprints are omitted; no trace, math, raw state, packet or output filtering.'
    passed = build.returncode == 0 and receipt['strict_compiler_diagnostics_empty'] and len(observations) == len(catalog) and all(x['returncode'] == 0 and not x['stderr'] for x in receipt['runs']) and receipt['summary']['failed_assertions'] == 0 and receipt['source_and_dependencies_stable'] and (not feature or receipt['module_checks']['passed']) and (receipt['comparison'] is None or receipt['comparison']['passed'])
    receipt['passed'] = passed
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        with args.report.open('x', encoding='utf-8', newline='\n') as output:
            json.dump(receipt, output, indent=2)
            output.write('\n')
    print(json.dumps(dict(source_ref=receipt['source_ref'], actual_module_present=feature,
                         build_return=build.returncode, summary=receipt['summary'],
                         module_checks_passed=receipt['module_checks']['passed'] if feature else None,
                         compatibility_comparison=receipt['comparison'], passed=passed,
                         source_and_dependencies_stable=receipt['source_and_dependencies_stable'],
                         report=str(args.report) if args.report else None)))
    return 0 if passed else (build.returncode or 1)


if __name__ == '__main__':
    result = main()
    if result:
        raise SystemExit(result)
