#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run installed production models and SDK regressions; keep all artifacts in build/."""
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
WORK = ROOT / 'build/arche3d_work'
ENV = os.environ.copy()
ENV['PATH'] = str(ROOT / 'install/bin') + os.pathsep + ENV.get('PATH', '')
ENV['PYTHONPATH'] = os.pathsep.join([str(ROOT / 'pulp'), str(ROOT / 'install/python'), ENV.get('PYTHONPATH', '')])
ENV['USE_GVRUN'] = '1'


def run(command, name):
    result = subprocess.run(list(map(str, command)), cwd=ROOT, env=ENV, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
    log = WORK / 'logs' / (name + '.log')
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text(result.stdout)
    if result.returncode:
        raise RuntimeError(f'{name} failed ({result.returncode}); see {log}\n{result.stdout[-2000:]}')
    return result.stdout


def main():
    records = []
    for config, path in [('default', ROOT / 'arche3d_sdk/apps/collective_row_sweep/ram_config.py'),
                         ('arche3d_redmule', ROOT / 'pulp/tests/arche3d/ram_redmule.py')]:
        for app, clusters in [('accelerators', 1), ('fp_formats', 1), ('fp_alignment', 4),
                              ('memory', 4), ('smoke', 1), ('collective', 4)]:
            label = f'{config}_{app}'
            directory = WORK / 'sw' / label
            run(['make', '-C', ROOT / 'arche3d_sdk', f'PYTHON={sys.executable}',
                 f'cfg={config}', f'app={app}', f'BUILD={directory}'], label + '_build')
            output = run(['gvrun', '--target=arche3d_dma_test', '--target-dir=pulp/tests/arche3d',
                          f'--parameter=config={path}', f'--parameter=clusters={clusters}',
                          '--parameter=progress_cycles=0', f'--binary={directory / (app + ".elf")}',
                          f'--work-dir={WORK / "runs" / label}', 'run'], label)
            result = [json.loads(line.split(' ', 1)[1]) for line in output.splitlines()
                      if line.startswith('ARCHE3D_RESULT ')]
            if len(result) != 1 or result[0]['status'] != 'PASS':
                raise RuntimeError(f'Missing PASS: {label}')
            if app == 'accelerators' and config == 'default' and 'shapes=54 irq=4 gram=18 overlap=36 reblock_gemm=1' not in output:
                raise RuntimeError('Missing MXCore shape/IRQ/shared-input/output-overlap/reblock coverage')
            if app == 'fp_alignment' and config == 'default' and 'redmule=SKIP(unavailable)' not in output:
                raise RuntimeError('Absent RedMule was not skipped')
            records.append(dict(config=config, app=app, **result[0]))
            print(f'PASS {label} ({clusters} clusters)', flush=True)
    output = run(['gvrun', '--target=arche3d_layout_test', '--target-dir=pulp/tests/arche3d',
                  f'--work-dir={WORK / "runs/layout"}', 'run'], 'layout')
    layout = [json.loads(line.split(' ', 1)[1]) for line in output.splitlines()
              if line.startswith('ARCHE3D_LAYOUT_RESULT ')]
    if len(layout) != 53 or sum(record['mode'] in (6, 7) for record in layout) != 26 or 'ARCHE3D_L1_PRIORITY_PASS' not in output or 'ARCHE3D_LAYOUT_INVALID_PASS' not in output:
        raise RuntimeError('Incomplete layout/priority coverage')
    report = dict(status='PASS', memory_backend='memory', full_chip=False,
                  m=[32,64,128], n=[32,64,128], k=[32,64,96,128,160,192],
                  shared_input_gram_shapes=18, rejected_output_overlaps=36,
                  reblock_matmul=True,
                  sdk=records, layout=layout, hwpe_bytes_per_cycle=512, conversion_latency=5)
    (WORK / 'validation.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS 53 layout cases, exclusive acquisition, invalid descriptors, priority/data ordering\n{WORK / "validation.json"}')


if __name__ == '__main__':
    main()
