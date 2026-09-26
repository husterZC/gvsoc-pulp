#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare real GVSoC IO_v2 runs with the RTL repository's recorded benchmarks.

No fitted latencies or golden cycle counts enter the C++ models. Historical
source relocation is resolved by content hashes. Current production RTL and
current benchmark sources must match the latest recorded source manifests.
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
        add(f, '/runs/'+str(i), m, soc=0, sparse=0, fabric=0,
            mode=int(m['routing_mode']), repeats=int(m['beats_per_destination']))
    f = Path('docs/mesh_results.json')
    j = json.loads((rtl/f).read_text())
    add(f, '/all_to_all/runs/1/metrics', j['all_to_all']['runs'][1]['metrics'],
        soc=0, sparse=0, fabric=1, mode=0, repeats=1)
    for f in map(Path, ['docs/all_to_sparse_results.json', 'docs/all_to_sparse_group_results.json']):
        for path, m in metrics(json.loads((rtl/f).read_text())):
            add(f, path, m, soc=0, sparse=1, fabric=int(m['topology']=='mesh'),
                mode=int(m['routing_mode']), groups=int(m.get('groups_per_source', m['beats_per_source'])),
                repeats=int(m.get('beats_per_group', 1)), seed=int(m['seed']))
    if native_reference:
        for path, m in metrics(json.loads(native_reference.read_text())):
            add(native_reference, path, m, soc=0, sparse=1, fabric=int(m['topology']=='mesh'),
                mode=int(m['routing_mode']), groups=int(m['groups_per_source']),
                repeats=int(m['beats_per_group']), seed=int(m['seed']))
    for f in map(Path, ['docs/soc_results.json', 'docs/soc_context_b16_results.json',
                       'target/checks/gvsoc_3d_network/benchmark_results.json']):
        if not (rtl/f).exists():
            continue
        for path, m in metrics(json.loads((rtl/f).read_text())):
            if 'fabric' not in m:
                continue
            add(f, path, m, soc=1, fabric=int(m['fabric']), mode=int(m['routing_mode']),
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
    # all-to-all was refactored to a common generator; record its current hash
    # separately, without claiming its old wrapper hash matches after refactoring.
    for name, expected in hashes.items():
        if digest(rtl/name) != expected:
            raise RuntimeError(f'Stale RTL references: source hash changed: {name}')
    current = {str(p.relative_to(rtl)): digest(p)
               for p in (rtl/'target/src/benchmark').rglob('*.sv')}
    return dict(verified_rtl_sha256=hashes, current_benchmark_sha256=current,
                historical_all_to_all_note='Old wrappers were refactored; cycle references retained. '
                'Current production RTL hashes verified; current workload generator hashed separately.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rtl', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--quick', action='store_true', help='skip 1024-terminal SoC B16 sweeps')
    parser.add_argument('--native-reference', type=Path, help='results.json from rtl_holdout.py')
    args = parser.parse_args()
    rtl = args.rtl.resolve()
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
    for record in cases(rtl, args.native_reference):
        p = record['parameters']
        if args.quick and p.get('soc') and p.get('nx')==32 and p.get('burst')==16:
            continue
        command = ['bash', str(HERE/'run.sh')]
        for key, value in p.items():
            command += ['--parameter', f'{key}={value}']
        start = time.perf_counter()
        result = subprocess.run(command, capture_output=True, text=True, timeout=1800)
        lines = [line.removeprefix('NETWORK3D_RESULT ') for line in result.stdout.splitlines()
                 if line.startswith('NETWORK3D_RESULT ')]
        if result.returncode or len(lines)!=1 or 'NETWORK3D_FAIL' in result.stderr:
            raise RuntimeError(f'{p}: {result.stdout}\n{result.stderr}')
        measured = json.loads(lines[0])
        measured['process_wall_seconds'] = time.perf_counter()-start
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
    protocol_cases = [dict(soc=0,fabric=0,mode=mode,stress=1,groups=11,repeats=2,seed=17)
                      for mode in range(3)]
    protocol_cases += [dict(soc=0,fabric=1,nx=3,ny=2,stress=1,groups=11,spill=spill)
                       for spill in (1,2,3)]
    protocol_cases += [dict(soc=1,fabric=1,nx=3,ny=2,functional=1,stress=1,datawidth=width)
                       for width in (8,32,64,128)]
    protocol_cases += [dict(soc=1,fabric=1,nx=3,ny=2,functional=1,stress=1,datawidth=width,backing=0)
                       for width in (8,32,64,128)]
    protocol_cases += [dict(soc=1,fabric=1,nx=3,ny=2,sc=16,mc=16,readslots=slots,
                           burst=16,stress=1,backing=backing)
                       for slots in (1,7) for backing in (0,1)]
    protocol_cases += [dict(soc=1,fabric=1,nx=3,ny=2,sc=16,mc=16,burst=16,stress=1,endpoint=endpoint)
                       for endpoint in (0,2)]
    protocol_cases += [dict(soc=1,fabric=1,nx=3,ny=2,functional=1,endpoint=2)]
    for p in protocol_cases:
        command=['bash',str(HERE/'run.sh')]
        for key,value in p.items(): command+=['--parameter',f'{key}={value}']
        result=subprocess.run(command,capture_output=True,text=True,timeout=180)
        lines=[line.removeprefix('NETWORK3D_RESULT ') for line in result.stdout.splitlines()
               if line.startswith('NETWORK3D_RESULT ')]
        if result.returncode or len(lines)!=1 or 'NETWORK3D_FAIL' in result.stderr:
            raise RuntimeError(f'Protocol case {p}: {result.stdout}\n{result.stderr}')
        report['protocol_tests'].append(dict(parameters=p,gvsoc=json.loads(lines[0]),status='PASS'))
        print(f'PASS protocol {p}',flush=True)
    report['max_deviation_percent'] = max(r['deviation_percent'] for r in report['tests'])
    report['status'] = 'PASS' if all(r['status']=='PASS' for r in report['tests']) else 'FAIL'
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(f'{report["status"]}: {len(report["tests"])} cases; '
          f'maximum cycle error {report["max_deviation_percent"]:.4f}%', flush=True)
    return 0 if report['status']=='PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
