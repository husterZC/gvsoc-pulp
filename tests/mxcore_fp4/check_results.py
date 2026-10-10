#!/usr/bin/env python3
"""Validate saved GVSoC run logs and write a compact comparison with RTL."""
import argparse
import hashlib
import json
from pathlib import Path
import re

HERE=Path(__file__).resolve().parent
MODES=('sync','delayed','async','denied','error')


def check(logs):
    calibration=(HERE/'calibration.json').read_bytes()
    report=json.loads(calibration)
    rtl={(x['m'],x['n'],x['k']):x['rtl_cycles'] for x in report['records']}
    results={}
    for mode in MODES:
        data=(logs/f'{mode}.log').read_text()
        if f'MXCoreFP4 PASS: 54 shapes, mode={mode}' not in data:
            raise RuntimeError(f'{mode}: missing complete regression pass')
        rows=re.findall(rf'MXCORE_GVSOC,(\d+),(\d+),(\d+),(\d+),{mode}\b',data)
        values={(int(m),int(n),int(k)):int(c) for m,n,k,c in rows}
        if len(rows)!=54 or values.keys()!=rtl.keys():
            raise RuntimeError(f'{mode}: incomplete/duplicate shape coverage')
        if mode=='sync' and values!=rtl:
            raise RuntimeError('GVSoC cycles differ from measured RTL')
        results[mode]=dict(shapes_passed=54,log_sha256=hashlib.sha256(data.encode()).hexdigest())
    return dict(calibration_sha256=hashlib.sha256(calibration).hexdigest(),
                description='Nominal GVSoC trace replay vs RTL; delayed/async/denied/error modes test model protocol behavior only.',
                total_gvsoc_cases=270,rtl_random_cases=len(report['records']),
                rtl_directed_cases=len(report['directed']),max_nominal_cycle_error=0,
                nominal_output_mismatches=0,modes=results,
                cycles=[dict(m=m,n=n,k=k,rtl=c,gvsoc=c) for (m,n,k),c in sorted(rtl.items())])


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs',type=Path,help='Directory containing sync.log, delayed.log, async.log, denied.log, error.log')
    parser.add_argument('--output',type=Path,default=HERE/'validation.json')
    args=parser.parse_args()
    args.output.write_text(json.dumps(check(args.logs),indent=2)+'\n')
