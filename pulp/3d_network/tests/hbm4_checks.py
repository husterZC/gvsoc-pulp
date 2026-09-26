#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Functional, backpressure and timing sensitivity checks using real DRAMSys."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
from dramsys_config import read_json

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]


def main():
    build = Path(os.environ.get('NETWORK3D_BUILD_DIR', ROOT/'build/network3d_hbm4')).resolve()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=build/'hbm4_checks.json',
                        help='Results JSON (default: %(default)s)')
    args = parser.parse_args()
    original = Path(os.environ.get('DRAMSYS_PATH', ROOT/'core/models/memory'))/'dramsys_configs/hbm4-emu-example.json'
    results = []

    def run(name, config=original, **parameters):
        folder = build/'checks'/name
        folder.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, DRAMSYS_PATH=str(config.parent.parent))
        params = dict(fabric=1, nx=2, ny=2, sc=32, mc=32)
        params.update(parameters)
        command = ['gvrun', '--target=network3d_hbm4', f'--work-dir={folder}'] + \
            [f'--parameter={k}={v}' for k,v in params.items()] + ['run']
        log = folder/'simulation.log'
        with log.open('w') as stream:
            subprocess.run(command,env=env,stdout=stream,stderr=subprocess.STDOUT,check=True)
        text = log.read_text()
        records = [json.loads(s) for s in re.findall(r'^NETWORK3D_RESULT (.*)$',text,re.M)]
        channels = [json.loads(s) for s in re.findall(r'^DRAMSYS_ENDPOINT_RESULT (.*)$',text,re.M)]
        assert 'NETWORK3D_FAIL' not in text and len(records)==1 and records[0]['status']=='PASS', log
        assert len(channels)==params['nx']*params['ny'] and all(c['pending']==0 for c in channels), log
        result = dict(name=name, parameters=params, config=str(config), gvsoc=records[0],
                      channels=channels, log_sha256=hashlib.sha256(log.read_bytes()).hexdigest())
        results.append(result)
        print(name, records[0]['runtime_cycles'], 'PASS', flush=True)
        return result

    for width in (8,64,128,512,1024):
        run(f'functional_width{width}', functional=1, stress=1, datawidth=width)
    base = run('b16_default')
    run('b16_stress_8x8', nx=8, ny=8, stress=1)
    run('b16_xbar_8x8', fabric=2, nx=8, ny=8, stress=1)
    run('functional_xbar', fabric=2, nx=3, ny=2, functional=1, stress=1, datawidth=512)
    run('b256_xbar', fabric=2, burst=256, stress=1,
        interleave_bytes=32768, memory_bytes=32768)
    for name in ('slow_clock','small_queues'):
        dest = build/'checks'/name/'dramsys_configs'
        shutil.copytree(original.parent,dest,dirs_exist_ok=True)
        top = read_json(dest/original.name)['simulation']
        if name=='slow_clock':
            path = dest/'memspec'/Path(top['memspec']).name
            doc = read_json(path)
            timing = doc['memspec']['memtimingspec']
            timing['tCK'] *= 2
        else:
            path = dest/'mcconfig'/Path(top['mcconfig']).name
            doc = read_json(path)
            doc['mcconfig']['RequestBufferSize'] = 1
            doc['mcconfig']['MaxActiveTransactions'] = 1
        path.write_text(json.dumps(doc,indent=2)+'\n')
        measured = run(name,config=dest/original.name)
        assert measured['gvsoc']['runtime_cycles'] > base['gvsoc']['runtime_cycles']
        if name=='small_queues':
            assert sum(c['request_denials'] for c in measured['channels']) > 0
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(dict(status='PASS',tests=results),indent=2)+'\n')


if __name__=='__main__':
    main()
