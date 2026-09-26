#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare the installed network3d target with RTL benchmark references.

Reference source hashes are checked before running comparisons. No fitted
latencies or golden cycle counts enter the C++ models.
"""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]


def metrics(node, path=''):
    if isinstance(node, dict):
        if node.get('status') == 'PASS' and 'runtime_cycles' in node:
            yield path, node
        else:
            for key, value in node.items():
                yield from metrics(value, path + '/' + key)
    elif isinstance(node, list):
        for key, value in enumerate(node):
            yield from metrics(value, path + '/' + str(key))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def cases(rtl, native_reference=None):
    records = []
    def add(filename, path, m, **p):
        records.append(dict(reference=str(filename), reference_path=path,
                            parameters=p, rtl=m))
    f = Path('docs/benchmark_results.json')
    j = json.loads((rtl/f).read_text())
    for i, m in enumerate(j['runs']):
        add(f, '/runs/'+str(i), m, i3d=0, sparse=0, fabric=0,
            mode=int(m['routing_mode']), repeats=int(m['beats_per_destination']))
    f = Path('docs/mesh_results.json')
    j = json.loads((rtl/f).read_text())
    add(f, '/all_to_all/runs/1/metrics', j['all_to_all']['runs'][1]['metrics'],
        i3d=0, sparse=0, fabric=1, mode=0, repeats=1)
    for f in map(Path, ['docs/all_to_sparse_results.json', 'docs/all_to_sparse_group_results.json']):
        for path, m in metrics(json.loads((rtl/f).read_text())):
            add(f, path, m, i3d=0, sparse=1, fabric=int(m['topology']=='mesh'),
                mode=int(m['routing_mode']), groups=int(m.get('groups_per_source', m['beats_per_source'])),
                repeats=int(m.get('beats_per_group', 1)), seed=int(m['seed']))
    if native_reference:
        for path, m in metrics(json.loads(native_reference.read_text())):
            add(native_reference, path, m, i3d=0, sparse=1, fabric=int(m['topology']=='mesh'),
                mode=int(m['routing_mode']), groups=int(m['groups_per_source']),
                repeats=int(m['beats_per_group']), seed=int(m['seed']))
    # These reference filenames belong to the separate RTL repository.
    for f in map(Path, ['docs/soc_results.json', 'docs/soc_context_b16_results.json',
                       'target/checks/gvsoc_3d_network/benchmark_results.json']):
        if not (rtl/f).exists():
            continue
        for path, m in metrics(json.loads((rtl/f).read_text())):
            if 'fabric' not in m:
                continue
            add(f, path, m, i3d=1, fabric=int(m['fabric']), mode=int(m['routing_mode']),
                nx=int(m['num_x']), ny=int(m['num_y']), burst=int(m['burst_beats']),
                sc=int(m['source_contexts']), mc=int(m['memory_contexts']),
                readslots=int(m['memory_read_slots']), offset=int(m['start_offset']),
                datawidth=int(m['data_width']))
    unique = {}
    for r in records:
        key = tuple(sorted(r['parameters'].items()))
        if key not in unique:
            unique[key] = r
    return list(unique.values())


def provenance(rtl):
    hashes = {}
    j = json.loads((rtl/'docs/soc_context_b16_results.json').read_text())
    hashes.update({k: v for k, v in j['manifest']['input_sha256'].items()
                   if k.endswith(('.sv', '.svh'))})
    j = json.loads((rtl/'docs/all_to_sparse_group_results.json').read_text())
    hashes.update({k: v for k, v in j['benchmark_files_sha256'].items() if k.endswith('.sv')})
    # Check the sources covered by the reference manifests. Record the current
    # workload sources separately; recording alone does not verify a reference.
    for name, expected in hashes.items():
        if digest(rtl/name) != expected:
            raise RuntimeError(f'Stale RTL references: source hash changed: {name}')
    current = {str(p.relative_to(rtl)): digest(p)
               for p in (rtl/'target/src/benchmark').rglob('*.sv')}
    return dict(verified_rtl_sha256=hashes, current_benchmark_sha256=current,
                reference_scope='Only source hashes present in the reference manifests are verified; '
                'the remaining current benchmark sources are recorded separately.')


def run_gvsoc(folder, parameters, timeout):
    folder.mkdir(parents=True, exist_ok=True)
    command = ['gvrun', '--target=network3d', f'--work-dir={folder}']
    command += [f'--parameter={key}={value}' for key, value in parameters.items()]
    command += ['run']
    log = folder/'simulation.log'
    start = time.perf_counter()
    with log.open('w') as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, timeout=timeout)
    elapsed = time.perf_counter()-start
    text = log.read_text()
    lines = [line.removeprefix('NETWORK3D_RESULT ') for line in text.splitlines()
             if line.startswith('NETWORK3D_RESULT ')]
    if result.returncode or len(lines)!=1 or 'NETWORK3D_FAIL' in text:
        raise RuntimeError(f'Benchmark failed for {parameters}; see {log}')
    measured = json.loads(lines[0])
    if measured['status'] != 'PASS':
        raise RuntimeError(f'Benchmark failed for {parameters}; see {log}')
    measured['process_wall_seconds'] = elapsed
    return measured


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rtl', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--work-dir', type=Path, default=ROOT/'build/network3d/validation',
                        help='Simulation logs and configurations (default: %(default)s)')
    parser.add_argument('--quick', action='store_true', help='skip 1024-terminal I3D B16 sweeps')
    parser.add_argument('--native-reference', type=Path, help='results.json from rtl_holdout.py')
    args = parser.parse_args()
    rtl = args.rtl.resolve()
    work_dir = args.work_dir.resolve()
    report = dict(status='RUNNING', date_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  host=platform.node(), threshold_percent=5.0, quick=args.quick,
                  provenance=provenance(rtl), tests=[], protocol_tests=[])
    if args.native_reference:
        fresh=json.loads(args.native_reference.read_text())
        if fresh['status']!='PASS': raise RuntimeError('Fresh RTL run has not passed')
        for filename, expected in fresh['source_sha256'].items():
            if digest(rtl/filename)!=expected: raise RuntimeError(f'Stale holdout: {filename}')
        report['provenance']['fresh_native_reference_sha256']=digest(args.native_reference)
        report['provenance']['fresh_native_rtl_sha256']=fresh['source_sha256']
    report['model_sha256'] = {str(p.relative_to(HERE.parent)): digest(p)
                             for p in HERE.parent.rglob('*') if p.suffix in ('.hpp', '.cpp', '.py', '.sh')}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    for index, record in enumerate(cases(rtl, args.native_reference)):
        p = record['parameters']
        if args.quick and p.get('i3d') and p.get('nx')==32 and p.get('burst')==16:
            continue
        measured = run_gvsoc(work_dir/f'rtl_{index:02}', p, timeout=1800)
        gold = record['rtl']
        deviation = 100*abs(measured['runtime_cycles']/float(gold['runtime_cycles'])-1)
        count = int(gold.get('transactions', gold.get('total_beats')))
        passed = deviation<=5 and measured['transactions']==count
        if 'workload_checksum' in gold:
            passed &= measured['workload_checksum']==gold['workload_checksum']
        record.update(gvsoc=measured, deviation_percent=deviation, status='PASS' if passed else 'FAIL')
        report['tests'].append(record)
        args.output.write_text(json.dumps(report, indent=2)+'\n')
        print(f'{record["status"]} {p}: RTL={gold["runtime_cycles"]}, '
              f'GVSoC={measured["runtime_cycles"]}, error={deviation:.4f}%', flush=True)
    protocol_cases = [dict(i3d=0,fabric=0,mode=mode,stress=1,groups=11,repeats=2,seed=17)
                      for mode in range(3)]
    protocol_cases += [dict(i3d=0,fabric=1,nx=3,ny=2,stress=1,groups=11,spill=spill)
                       for spill in (1,2,3)]
    protocol_cases += [dict(i3d=1,fabric=1,nx=3,ny=2,functional=1,stress=1,datawidth=width)
                       for width in (8,32,64,128)]
    protocol_cases += [dict(i3d=1,fabric=1,nx=3,ny=2,functional=1,stress=1,datawidth=width,backing=0)
                       for width in (8,32,64,128)]
    protocol_cases += [dict(i3d=1,fabric=1,nx=3,ny=2,sc=16,mc=16,readslots=slots,
                           burst=16,stress=1,backing=backing)
                       for slots in (1,7) for backing in (0,1)]
    protocol_cases += [dict(i3d=1,fabric=1,nx=3,ny=2,sc=16,mc=16,burst=16,stress=1,endpoint=endpoint)
                       for endpoint in (0,2)]
    protocol_cases += [dict(i3d=1,fabric=1,nx=3,ny=2,functional=1,endpoint=2)]
    protocol_cases += [dict(i3d=0,fabric=2,nx=nx,ny=ny,stress=1,groups=11,spill=spill)
                       for nx,ny in ((1,1),(3,2),(9,7),(32,32)) for spill in (1,2,3)]
    protocol_cases += [dict(i3d=0,fabric=2,nx=3,ny=2,sparse=0,repeats=3)]
    protocol_cases += [dict(i3d=1,fabric=2,nx=3,ny=2,functional=1,stress=1,
                           datawidth=width,backing=backing)
                       for width in (8,64,512,1024) for backing in (0,1)]
    protocol_cases += [dict(i3d=1,fabric=2,nx=3,ny=2,sc=8,mc=8,burst=16,
                           readslots=1,stress=1,backing=0,mode=mode) for mode in (0,1,2)]
    protocol_cases += [dict(i3d=1,fabric=2,nx=3,ny=2,burst=256,stress=1,backing=0)]
    protocol_cases += [dict(i3d=1,fabric=2,nx=3,ny=2,functional=1,endpoint=2)]
    for index, p in enumerate(protocol_cases):
        measured = run_gvsoc(work_dir/f'protocol_{index:02}', p, timeout=180)
        report['protocol_tests'].append(dict(parameters=p,gvsoc=measured,status='PASS'))
        print(f'PASS protocol {p}',flush=True)
    report['max_deviation_percent'] = max(r['deviation_percent'] for r in report['tests'])
    report['status'] = 'PASS' if all(r['status']=='PASS' for r in report['tests']) else 'FAIL'
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(f'{report["status"]}: {len(report["tests"])} cases; '
          f'maximum cycle error {report["max_deviation_percent"]:.4f}%', flush=True)
    return 0 if report['status']=='PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
