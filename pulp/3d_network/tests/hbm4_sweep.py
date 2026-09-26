#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the full 1024-channel B16 context sweep, checking every returned byte."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run_inputs(report):
    return dict(source_sha256=report['source_sha256'],
                library_sha256=report['dramsys_library']['sha256'],
                config_sha256={k:v['sha256'] for k,v in report['resolved_config_manifest']['resolved'].items()},
                parameters=report['parameters'])


def simulation_inputs(inputs):
    # Formatting/collection changes to this runner do not change simulation.
    # All model, target, driver, build and launch inputs must still match.
    return dict(inputs,source_sha256={k:v for k,v in inputs['source_sha256'].items()
                                    if not k.endswith('/hbm4_sweep.py')})


def collect(log, contexts):
    text = log.read_text()
    results = [json.loads(s) for s in re.findall(r'^NETWORK3D_RESULT (.*)$', text, re.M)]
    channels = [json.loads(s) for s in re.findall(r'^DRAMSYS_ENDPOINT_RESULT (.*)$', text, re.M)]
    if 'NETWORK3D_FAIL' in text or len(results) != 1 or results[0]['status'] != 'PASS':
        raise RuntimeError(f'Benchmark failed: {log}')
    result = results[0]
    assert result['transactions'] == 1024**2
    assert len(channels) == 1024 and {c['channel'] for c in channels} == set(range(1024))
    for c in channels:
        assert c['read_requests'] == 1024 and c['native_reads'] == 32768
        assert c['read_bytes'] == 1024**2 and c['pending'] == 0
        assert c['native_writes'] == c['write_requests'] == c['write_bytes'] == 0
    maximums = re.findall(r'MAX BW:.*?\|\s*([0-9.]+) GB/s', text)
    assert len(maximums) == 1024 and all(float(x) == 64 for x in maximums)
    result.update(contexts=contexts, simulation_us=result['runtime_cycles']/1000,
                  read_bytes=1024**3, aggregate_GBps=1024**3/result['runtime_cycles'],
                  per_channel_GBps=1024**2/result['runtime_cycles'],
                  channel_peak_utilization=1024**2/result['runtime_cycles']/64,
                  endpoint_totals={k: sum(c[k] for c in channels) for k in channels[0] if k != 'channel'},
                  max_reads_at_one_endpoint=max(c['peak_reads'] for c in channels),
                  dramsys_max_GBps_per_channel=64,
                  log=str(log), log_sha256=digest(log))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--contexts', nargs='+', type=int, default=[8,16,32,64,128,256])
    parser.add_argument('--output', type=Path, default=HERE.parent/'doc/soc_context_b16_hbm4_results.json')
    parser.add_argument('--reuse', action='store_true', help='Verify and reuse existing complete logs')
    args = parser.parse_args()
    build = Path(os.environ.get('NETWORK3D_BUILD_DIR', ROOT/'build/network3d_hbm4')).resolve()
    config = Path(os.environ.get('DRAMSYS_PATH', build))/'dramsys_configs/hbm4-emu-example.json'
    # Match hbm4.sh's loader search order and record the actual selected input.
    systemc = Path(os.environ.get('SYSTEMC_HOME', ROOT/'third_party/systemc_install'))
    search = [systemc/'lib64', systemc/'lib']
    search += [Path(p) for p in os.environ.get('LD_LIBRARY_PATH', '').split(':') if p]
    search += [ROOT/'third_party/DRAMSys']
    library = next((p/'libDRAMSys_Simulator.so' for p in search
                    if (p/'libDRAMSys_Simulator.so').is_file()), None)
    if library is None:
        raise FileNotFoundError('libDRAMSys_Simulator.so not found; follow DRAMSys.md '
                                'and add its installation directory to LD_LIBRARY_PATH')
    library = library.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    source_files = sorted(HERE.parent.glob('*.cpp')) + sorted(HERE.parent.glob('*.hpp')) + \
        sorted(HERE.parent.glob('*.py')) + [HERE/p for p in ('driver.cpp','benchmark.py',
        'hbm4_benchmark.py','prepare_hbm4.py','hbm4.sh','hbm4_sweep.py','build.sh','run.sh')]
    report = dict(timestamp_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        host=dict(hostname=platform.node(), platform=platform.platform(), python=platform.python_version()),
        parameters=dict(topology='fattree', routing='NCA_HASH', fabric=0, mode=1, num_x=32, num_y=32,
            num_levels=3, frequency_hz=1_000_000_000, axi_addr_width=64, axi_data_width=512,
            axi_id_width=10, axi_len_width=8, burst_beats=16, max_burst_beats=256,
            memory_interleave_bytes=4096, mapped_bytes_per_endpoint=4096, start_offset=0,
            memory_read_slots=None, channels=1024, io_spill=2, workload='all-to-all reads',
            transactions=1024**2, read_bytes=1024**3, repeats=1),
        dramsys_library=dict(path=str(library), sha256=digest(library)),
        source_sha256={str(p.relative_to(ROOT)):digest(p) for p in source_files},
        resolved_config_manifest=json.loads((config.parent/'manifest.json').read_text()), runs=[])
    for item in report['resolved_config_manifest']['resolved'].values():
        if digest(config.parent/item['path']) != item['sha256']:
            raise RuntimeError('Prepared DRAMSys configuration changed; regenerate it and its manifest')
    for contexts in args.contexts:
        run = build/'sweep'/f'x{contexts}'
        run.mkdir(parents=True, exist_ok=True)
        log, stats = run/'simulation.log', run/'process_time.txt'
        snapshot = run/'inputs.json'
        expected_inputs = run_inputs(report)
        if args.reuse and log.exists() and stats.exists():
            if not snapshot.exists() or simulation_inputs(json.loads(snapshot.read_text())) != simulation_inputs(expected_inputs):
                raise RuntimeError(f'Cannot reuse results with missing/changed simulation inputs: {run}')
        else:
            snapshot.write_text(json.dumps(expected_inputs,indent=2)+'\n')
            env = dict(os.environ, NETWORK3D_RUN_DIR=str(run))
            command = ['/usr/bin/time','-v','-o',str(stats),'bash',str(HERE/'hbm4.sh'),'run',
                       f'--parameter=sc={contexts}',f'--parameter=mc={contexts}']
            print(f'Start X={contexts}: {log}', flush=True)
            start = time.perf_counter()
            with log.open('w') as stream:
                completed = subprocess.run(command, env=env, stdout=stream, stderr=subprocess.STDOUT)
            (run/'elapsed_seconds.txt').write_text(str(time.perf_counter()-start)+'\n')
            if completed.returncode:
                raise RuntimeError(f'Process exited {completed.returncode}: {log}')
        result = collect(log, contexts)
        result['execution_source_sha256'] = json.loads(snapshot.read_text())['source_sha256']
        result['process_wall_seconds'] = float((run/'elapsed_seconds.txt').read_text())
        result['maximum_rss_kib'] = int(re.search(r'Maximum resident set size \(kbytes\): (\d+)', stats.read_text())[1])
        report['runs'].append(result)
        args.output.write_text(json.dumps(report, indent=2)+'\n')
        print(json.dumps({k: result[k] for k in ('contexts','runtime_cycles','aggregate_GBps',
                                                'wall_seconds','process_wall_seconds','maximum_rss_kib')}), flush=True)


if __name__ == '__main__':
    main()
