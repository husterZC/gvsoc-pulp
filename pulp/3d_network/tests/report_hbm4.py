#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Render the recorded HBM4 context sweep as Markdown and CSV."""
import argparse
import csv
import gzip
import hashlib
import json
import os
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
BUILD = Path(os.environ.get('NETWORK3D_BUILD_DIR', ROOT/'build/network3d_hbm4')).resolve()
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--input', type=Path, default=BUILD/'soc_context_b16_hbm4_results.json',
                    help='Completed sweep results (default: %(default)s)')
parser.add_argument('--output-dir', type=Path, default=BUILD/'report',
                    help='Generated report and artifacts (default: %(default)s)')
args = parser.parse_args()
OUTPUT = args.output_dir.resolve()
data = json.loads(args.input.read_text())
runs = data['runs']
assert [r['contexts'] for r in runs] == [8,16,32,64,128,256], 'Complete all six measurements first'
assert len({r['workload_checksum'] for r in runs}) == 1
logs = OUTPUT/'hbm4_logs'
logs.mkdir(parents=True, exist_ok=True)
archive = []
for run in runs:
    original = Path(run['log']).read_bytes()
    assert hashlib.sha256(original).hexdigest() == run['log_sha256']
    path = logs/f"x{run['contexts']}.log.gz"
    path.write_bytes(gzip.compress(original,mtime=0))
    config_bytes = (Path(run['log']).parent/'gvsoc_config.json').read_bytes()
    target = json.loads(config_bytes)['target']
    assert target['clock']['frequency'] == 1_000_000_000
    network = target['network']
    expected = dict(fabric=0,num_x=32,num_y=32,num_levels=3,routing_mode=1,
                    axi_addr_width=64,axi_data_width=512,axi_id_width=10,axi_len_width=8,
                    source_contexts=run['contexts'],memory_contexts=run['contexts'],
                    max_burst_beats=256,memory_base=0,interleave_bytes=4096,memory_bytes=4096)
    assert all(network[k] == v for k,v in expected.items())
    assert 'memory_read_slots' not in network
    for i in range(1024):
        endpoint = target[f'memory_{i}']
        assert endpoint['data_width'] == 512 and endpoint['burst_bytes'] == 32
        assert 'read_slots' not in endpoint
    config_path = logs/f"x{run['contexts']}.config.json.gz"
    config_path.write_bytes(gzip.compress(config_bytes,mtime=0))
    archive.append(dict(contexts=run['contexts'],path=path.name,
                        original_sha256=run['log_sha256'],
                        gzip_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                        generated_config=dict(path=config_path.name,
                            original_sha256=hashlib.sha256(config_bytes).hexdigest(),
                            gzip_sha256=hashlib.sha256(config_path.read_bytes()).hexdigest())))
(logs/'manifest.json').write_text(json.dumps(archive,indent=2)+'\n')
(OUTPUT/'soc_context_b16_hbm4_results.json').write_text(json.dumps(data,indent=2)+'\n')
best = min(runs, key=lambda r:r['runtime_cycles'])
smallest = min(r['contexts'] for r in runs if r['runtime_cycles'] <= best['runtime_cycles']*1.05)
fields = ['contexts','runtime_cycles','simulation_us','injection_cycles','drain_cycles',
          'aggregate_GBps','per_channel_GBps','channel_peak_utilization','peak_outstanding',
          'max_reads_at_one_endpoint','wall_seconds','process_wall_seconds','maximum_rss_kib']
with (OUTPUT/'soc_context_b16_hbm4_results.csv').open('w',newline='') as stream:
    writer = csv.DictWriter(stream,fieldnames=fields,extrasaction='ignore',lineterminator='\n')
    writer.writeheader()
    writer.writerows(runs)

rows = '\n'.join(f"| {r['contexts']} | {r['runtime_cycles']:,.1f} | {r['simulation_us']:.4f} | "
    f"{r['aggregate_GBps']/1000:.3f} | {r['per_channel_GBps']:.3f} | "
    f"{100*r['channel_peak_utilization']:.2f}% | {r['wall_seconds']:.2f} | "
    f"{r['process_wall_seconds']:.2f} | {r['maximum_rss_kib']/1024:.1f} |" for r in runs)
detail = '\n'.join(f"| {r['contexts']} | {r['injection_cycles']:,.1f} | {r['drain_cycles']:,.1f} | "
    f"{r['peak_outstanding']:,} | {r['max_reads_at_one_endpoint']:,} | "
    f"{r['endpoint_totals']['request_denials']:,} |" for r in runs)
parameters = data['parameters']
guide = Path(os.path.relpath(HERE.parent/'README.md', OUTPUT)).as_posix()
report = f'''# SoC B16 context sweep with DRAMSys HBM4 channels

All six configurations completed successfully.
**X={best['contexts']} has the lowest measured runtime: {best['runtime_cycles']:,.1f} cycles
({best['simulation_us']:.4f} µs), delivering {best['aggregate_GBps']/1000:.3f} TB/s across
1,024 channels.** X={smallest} is the smallest tested context count within 5% of
the best result. This conclusion applies to this all-to-all, repeated-address workload.

[Exact CSV](soc_context_b16_hbm4_results.csv) ·
[Results and source/configuration hashes](soc_context_b16_hbm4_results.json) ·
[Compressed logs and target configurations](hbm4_logs/manifest.json)

This report describes the inputs recorded in the results JSON. Endpoint source,
DRAMSys library and configuration hashes identify the measured implementation;
changing those inputs produces a new experiment. This formatter reads completed
measurements and writes generated output under `build/` by default.

## Configuration and workload

| Setting | Recorded value |
|---|---|
| Fabric / routing | {parameters['topology']} / {parameters['routing']} |
| Terminals / fat-tree levels | {parameters['num_x']} × {parameters['num_y']} / {parameters['num_levels']} |
| Clock | {parameters['frequency_hz']:,} Hz |
| AXI address / data width | {parameters['axi_addr_width']} / {parameters['axi_data_width']} bits |
| Burst length | {parameters['burst_beats']} beats |
| SourceContexts = MemoryContexts | 8, 16, 32, 64, 128, 256 |
| Additional memory read-slot limiter | None |
| Interleaving / mapped memory per endpoint | {parameters['memory_interleave_bytes']} / {parameters['mapped_bytes_per_endpoint']} bytes |
| Local start address | {parameters['start_offset']} |
| Total reads / useful bytes | {parameters['transactions']:,} / {parameters['read_bytes']:,} |

Each source reads every destination once, using the same local address at each
endpoint. These measurements describe that access pattern; they do not measure
random-address HBM bandwidth. Each endpoint is an independent DRAMSys channel.

## Measurements

Bandwidth counts useful read data, summed over all masters. GB/s and TB/s are
decimal. The per-channel figure includes NoC admission, memory service, response
delivery and final drain. Utilization is relative to the configured 64 GB/s
channel ceiling.

| X | Runtime (cycles) | Simulated time (µs) | Aggregate TB/s | Mean GB/s/channel | % of 64 GB/s | Simulation wall (s) | Process wall (s) | Peak host RSS (MiB) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
{rows}

Simulation wall time covers reset release through final completion. Process
wall time also includes target generation, simulator startup, channel
construction/preload and shutdown. It excludes compilation and post-run log
checks. Host load affects wall times; cycle counts describe simulated behavior.

| X | Request admission (cycles) | Response drain (cycles) | Peak total outstanding | Peak unfinished reads at one endpoint | Endpoint request denials |
|---:|---:|---:|---:|---:|---:|
{detail}

Admission ends at the last source request acceptance; responses also return
during admission. Unfinished endpoint reads include buffered data awaiting NoC
acceptance and are not the DRAM controller's native queue occupancy.

## Run another sweep

Follow the environment, DRAMSys, target build and run instructions in
[the module README](<{guide}>). The optional `tests/hbm4_sweep.py` tool runs
these six context configurations through the installed GVSoC target.
Generated data belongs under `gvsoc/build/`. This report does not establish
RTL-plus-DRAMSys cycle calibration.
'''
(OUTPUT/'soc_context_b16_hbm4.md').write_text(report)
print(OUTPUT/'soc_context_b16_hbm4.md')
