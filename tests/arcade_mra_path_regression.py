"""Exercise complete MRA selection paths through actual helpers and file-SAX."""
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
HERE = ROOT/'tests'
IMPORTED_HELPER = (HERE/'arcade_scalar_metadata_regression.py').read_bytes()
EXTRACT = runpy.run_path(str(HERE/'arcade_scalar_metadata_regression.py'))['definition']
assert (HERE/'arcade_scalar_metadata_regression.py').read_bytes() == IMPORTED_HELPER
SOURCES = ('support/arcade/mra_loader.cpp', 'file_io.cpp', 'file_io.h', 'sxmlc.c', 'sxmlc.h', 'fpga_io.cpp')
CASES = ('ordinary-relative', 'ordinary-absolute', 'fit-relative1023', 'fit-absolute1023',
         'relative-joined1024', 'menu-absolute1024', 'nonarcade-parent-clip',
         'nonarcade-final-clip', 'leading-slash', 'usb-relative', 'literal-name', 'literal-rbf', 'root-copy1024',
         'builder-copy', 'builder-join', 'builder-parent', 'builder-resolve', 'builder-oom', 'builder-ui', 'builder-root-alias',
         'root-found', 'root-empty', 'root-cores-bounds', 'getter-preservation',
         'nonarcade-join1023', 'nonarcade-join1024', 'final1023', 'final1024', 'final1024-reversed',
         'selected-cores1023', 'selected-cores1024',
         'matcher-plain', 'matcher-plain-reversed', 'matcher-arcade', 'matcher-arcade-reversed',
         'no-match', 'no-rbf', 'invalid-scalar', 'sax-malformed', 'sax-error', 'sax-open-failure', 'directory-failure',
         'no-marker', 'marker-without-slash', 'first-marker', 'mixed-case-mra', 'captured-root', 'borrowed-input',
         'new-null', 'new-empty', 'new-invalid-root', 'new-null-root', 'new-overlong-root', 'new-recovery', 'new-cores-oom',
         'entry-resolve-oom', 'rbf-parent-oom', 'rbf-final-oom', 'rbf-cores-oom')

def sha(data):
    return hashlib.sha256(data).hexdigest()

def generate(source):
    mra, io = source[SOURCES[0]], source['file_io.cpp']
    paths = 'enum arcade_path_error {' in mra
    types = '#define HAVE_MRA_PATHS %d\n' % paths
    types += next(line for line in mra.splitlines() if line.startswith('#define kBigTextSize'))+'\n'
    if paths:
        types += EXTRACT(mra, 'enum arcade_path_error {', SOURCES[0])
        types += next(line for line in source['file_io.h'].splitlines() if line.startswith('int FileResolveFullPath('))+'\n'
    for marker, text in (('static char arcade_root', mra), ('static char mame_root', mra),
                         ('static char full_path', io), ('static int device =', io), ('static int usbnum =', io)):
        types += next(line for line in text.splitlines() if line.startswith(marker))+'\n'
    helper = ''
    if 'struct arcade_rbf_scan {' in mra:
        for marker in ('enum arcade_scalar_kind {', 'struct arcade_scalar_owner {', 'struct arcade_rbf_scan {'):
            types += EXTRACT(mra, marker, SOURCES[0])
        for marker in ('static arcade_scalar_kind arcade_scalar_type(', 'static bool arcade_scalar_start(',
                       'static bool arcade_scalar_close(', 'static bool arcade_scalar_append('):
            helper += EXTRACT(mra, marker, SOURCES[0])
    production = EXTRACT(io, 'const char *getStorageDir(', 'file_io.cpp')
    production += EXTRACT(io, 'const char *getRootDir(', 'file_io.cpp')
    production += 'static const char *observed_root() { const char *(*volatile call)()=getRootDir; return root_boundary(call()); }\n#define getRootDir observed_root\n'
    production += EXTRACT(io, 'static char* make_fullpath(', 'file_io.cpp')
    production += EXTRACT(io, 'const char *getFullPath(', 'file_io.cpp')
    if paths:
        production += EXTRACT(io, 'int FileResolveFullPath(', 'file_io.cpp')
        for marker in ('static int arcade_path_fail(', 'static int arcade_path_copy(',
                       'static int arcade_path_copy_root(', 'static int arcade_path_join(',
                       'static int arcade_path_parent(', 'static int arcade_path_resolve(',
                       'static void arcade_path_info(', 'static int set_arcade_root(',
                       'static const char *get_arcade_root(', 'static bool arcade_rbf_match('):
            production += EXTRACT(mra, marker, SOURCES[0])
            if marker == 'static int arcade_path_fail(':
                production += 'static int observed_path_failure(arcade_path_error *error,int code) { const int out=arcade_path_fail(error,code); observe_path_failure(code,error?int(*error):-1); return out; }\n#define arcade_path_fail observed_path_failure\n'
        production += 'static const char *observed_arcade_root(int rbf,arcade_path_error *error=nullptr) { const char *(*volatile call)(int,arcade_path_error*)=get_arcade_root; return call(rbf,error); }\n'
    else:
        production += '#define strcpy bounded_root_copy\n'+EXTRACT(mra, 'static void set_arcade_root(', SOURCES[0])+'#undef strcpy\n'
        production += '#define snprintf forwarded_path_format\n'+EXTRACT(mra, 'static const char *get_arcade_root(', SOURCES[0])+'#undef snprintf\n'
        production += 'static const char *observed_arcade_root(int rbf) { const char *(*volatile call)(int)=get_arcade_root; return call(rbf); }\n'
    production += '#define get_arcade_root observed_arcade_root\n'
    production += helper+EXTRACT(mra, 'static int xml_scan_rbf(', SOURCES[0])
    production += EXTRACT(mra, 'static const char *get_rbf_path(', SOURCES[0])
    if paths:
        production += 'static const char *observed_rbf_path(const char *name,const char *root=nullptr,arcade_path_error *error=nullptr) { const char *(*volatile call)(const char*,const char*,arcade_path_error*)=get_rbf_path; const char *out=call(name,root,error); observe_rbf_path(name,out); return out; }\n'
    else:
        production += 'static const char *observed_rbf_path(const char *name) { const char *(*volatile call)(const char *)=get_rbf_path; const char *out=call(name); observe_rbf_path(name,out); return out; }\n'
    production += '#define get_rbf_path observed_rbf_path\n'
    production += '#define XMLDoc_parse_file_SAX observed_parse\n#define opendir observed_opendir\n#define readdir observed_readdir\n#define closedir observed_closedir\n'
    production += EXTRACT(mra, 'static const char *get_rbf(', SOURCES[0])
    production += '#undef XMLDoc_parse_file_SAX\n#undef opendir\n#undef readdir\n#undef closedir\n#undef get_rbf_path\n'
    if paths:
        production += 'static const char *observed_get_rbf(const char *xml,int arcade,const char *root=nullptr,arcade_path_error *error=nullptr) { const char *out=get_rbf(xml,arcade,root,error); observe_get_rbf(out); return out; }\n'
    else:
        production += 'static const char *observed_get_rbf(const char *xml,int arcade) { const char *out=get_rbf(xml,arcade); observe_get_rbf(out); return out; }\n'
    production += '#define stat64 mra_path_stat64\n#define open path_core_open\n#define fstat64 path_core_fstat64\n#define read path_core_read\n#define close path_core_close\n'+EXTRACT(source['fpga_io.cpp'], 'int fpga_load_rbf(', 'fpga_io.cpp')+'#undef open\n#undef fstat64\n#undef read\n#undef close\n#undef stat64\n'
    production += '#define get_rbf observed_get_rbf\n#define fpga_load_rbf observed_core_handoff\n#define strcpy bounded_xml_copy\n'+EXTRACT(mra, 'int xml_load(', SOURCES[0])+'#undef strcpy\n#undef fpga_load_rbf\n#undef get_rbf\n#undef get_arcade_root\n#undef getRootDir\n#undef arcade_path_fail\n'
    parser = source['sxmlc.c']
    anchor = '#include "sxmlc.h"'
    assert parser.count(anchor) == 1
    parser = parser.replace(anchor, anchor+'\nextern FILE *mra_path_fopen(const SXML_CHAR *,const SXML_CHAR *);\n#undef sx_fopen\n#define sx_fopen mra_path_fopen\n', 1)
    return {'mra_path_types.inc': types, 'mra_path_production.inc': production, 'mra_path_sxmlc.c': parser}

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source-ref')
    ap.add_argument('--report', type=Path)
    ap.add_argument('--cxx', default=os.environ.get('CXX','g++'))
    ap.add_argument('--cc', default=os.environ.get('CC'))
    args = ap.parse_args()
    raw = {name: subprocess.check_output(['git', '-C', str(ROOT), 'show', args.source_ref+':'+name]) if args.source_ref else (ROOT/name).read_bytes() for name in SOURCES}
    source = {name: data.decode().replace('\r\n', '\n') for name, data in raw.items()}
    generated = generate(source)
    pair = {name: (HERE/name).read_bytes() for name in ('arcade_mra_path_regression.py', 'arcade_mra_path_regression.cpp')}
    imported_helper = IMPORTED_HELPER
    cxx = [args.cxx] if Path(args.cxx).is_file() else shlex.split(args.cxx,posix=os.name!='nt')
    cc_text = args.cc or str(Path(cxx[0]).with_name(Path(cxx[0]).name.replace('g++','gcc')))
    cc = [cc_text] if Path(cc_text).is_file() else shlex.split(cc_text,posix=os.name!='nt')
    env = os.environ.copy()
    if Path(cxx[0]).is_file():
        env['PATH'] = str(Path(cxx[0]).resolve().parent)+os.pathsep+env.get('PATH', '')
    builds, runs = [], []
    try:
        with tempfile.TemporaryDirectory(prefix='mister-arcade-mra-path-') as temporary:
            work = Path(temporary)
            (work/'sxmlc.h').write_text(source['sxmlc.h'], encoding='utf-8', newline='\n')
            for name, text in generated.items():
                (work/name).write_text(text, encoding='utf-8', newline='\n')
            exe = work/('probe.exe' if os.name=='nt' else 'probe')
            common = ['-O2', '-Wall', '-Wextra', '-Werror', '-I', str(work)]
            commands = [[*cc, '-std=gnu99', *common, '-Wno-error=format-security', '-c', str(work/'mra_path_sxmlc.c'), '-o', str(work/'parser.o')],
                        [*cxx, '-std=gnu++14', '-D_GNU_SOURCE', *common, str(HERE/'arcade_mra_path_regression.cpp'), str(work/'parser.o'), '-o', str(exe)]]
            for command in commands:
                result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=60)
                builds.append(dict(command=command, returncode=result.returncode, stdout=result.stdout, stderr=result.stderr))
                if result.returncode:
                    raise RuntimeError('Fixture compile failure is not behavioral proof: '+result.stdout+result.stderr)
                if result.stderr:
                    print(result.stderr, file=sys.stderr, end='')
            for case in CASES:
                result = subprocess.run([str(exe), case], capture_output=True, text=True, env=env, timeout=15)
                rows = [json.loads(line[5:]) for line in result.stdout.splitlines() if line.startswith('JSON ')]
                if len(rows)!=1 or rows[0]['case']!=case or not rows[0]['observation_case_completed'] or result.returncode != int(bool(rows[0]['failures'])):
                    raise RuntimeError('Incomplete regression is not proof: '+case+' '+result.stdout+result.stderr)
                historical = 'enum arcade_path_error {' not in source[SOURCES[0]]
                ended = historical and case in ('menu-absolute1024','root-copy1024')
                missing_api = historical and case.startswith(('builder-','root-found','root-empty','root-cores','getter-','new-','entry-resolve-oom','rbf-parent-oom','rbf-final-oom','rbf-cores-oom'))
                assert rows[0]['ended_before_copy'] == ended and rows[0]['excluded'] == (ended or missing_api)
                rows[0]['target_bytes'] = {key: dict(length=len(rows[0][key].encode('latin-1')), hex=rows[0][key].encode('latin-1').hex()) for key in ('wanted_xml','requested_xml','wanted_directory','actual_directory','wanted_core','actual_core','handed_xml','path_input','path_output','arcade_root','mame_root')}
                runs.append(dict(case=case, observation=rows[0], stdout=result.stdout, stderr=result.stderr, returncode=result.returncode))
                print(('EXCLUDED' if rows[0]['excluded'] else 'FAIL' if rows[0]['failures'] else 'PASS')+': '+case)
    except Exception as error:
        if args.report:
            args.report.write_text(json.dumps(dict(fixture_error=str(error), builds=builds, completed_runs=runs), indent=2)+'\n', encoding='utf-8', newline='\n')
        raise
    assert [row['case'] for row in runs] == list(CASES)
    assert {name: sha((HERE/name).read_bytes()) for name in pair} == {name: sha(data) for name, data in pair.items()}
    assert (HERE/'arcade_scalar_metadata_regression.py').read_bytes() == imported_helper
    if not args.source_ref:
        assert {name: sha((ROOT/name).read_bytes()) for name in SOURCES} == {name: sha(data) for name, data in raw.items()}
    report = dict(source_ref=args.source_ref or 'working tree', source_raw_sha256={name: sha(data) for name, data in raw.items()}, pair_sha256={name: sha(data) for name, data in pair.items()}, imported_helper_sha256=sha(imported_helper), extracted_sha256={name: sha(text.encode()) for name, text in generated.items()}, scenario_order=list(CASES), builds=builds, runs=runs,
        summary=dict(cases=len(runs), checks=sum(row['observation']['checks'] for row in runs), failures=sum(row['observation']['failures'] for row in runs), failed_cases=sum(bool(row['observation']['failures']) for row in runs), excluded=sum(row['observation']['excluded'] for row in runs), ended_before_copy=sum(row['observation']['ended_before_copy'] for row in runs), differing_complete_target_cases=[row['case'] for row in runs if row['observation']['target_differs']]),
        observation_only=False, scope='Actual full file-SAX and RBF consumer/matcher, xml_load/root/path functions, getFullPath/make_fullpath, storage/root providers and complete fpga_load_rbf definition. Lower disposable parser fopen, finite directory/findGamesDir metadata, root fault/ownership and menu/Info/syscall/FPGA endpoint spies. The actual core file-open failure branch returns -1 and emits its unchanged timed message; every later physical/restart endpoint rejects unexpected calls. Historical get_arcade_root snprintf is forwarded unchanged through a volatile libc function pointer; strict C++ diagnostics remain errors.',
        limits='Finite local selection/path and core-open-error evidence. Historical absolute copy boundaries end before write; later XML/helper/parser/core results remain explicitly unobserved and excluded. New private API, null/invalid-provider and allocation/recovery controls are explicitly excluded where the historical contract cannot express them. Root/provider mutation and long invalid roots are declared synthetic ownership/admission controls, not actual storage faults. Recovery follows actual core fopen failure, without a successful production restart. Core stat fixture carries only the signed64-bit size field; no successful core descriptor, allocation/read/FPGA/restart path is exercised. No real menu/command dispatch, target filesystem, physical core/FPGA, restart, durable storage, prevalence or performance evidence.')
    if args.report:
        args.report.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8', newline='\n')
    print(json.dumps(report['summary']))
    return int(bool(report['summary']['failures']))

if __name__ == '__main__':
    raise SystemExit(main())
