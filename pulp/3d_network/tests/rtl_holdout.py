#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run fresh, independently scored native sparse cases in Questa (requires license)."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import sys
import time


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--rtl',type=Path,required=True)
    p.add_argument('--output-dir',type=Path,required=True)
    args=p.parse_args(); rtl=args.rtl.resolve(); out=args.output_dir.resolve()
    sys.path.insert(0,str(rtl/'scripts'))
    from check_rtl_topology import find_common_cells
    from check_soc import simulate
    from run_rtl_checks import real_sources as fat_sources
    from check_mesh import real_sources as mesh_sources
    from sparse_benchmark import prepare
    common=find_common_cells(None)
    report=dict(status='RUNNING',tests=[],source_sha256={})
    out.mkdir(parents=True,exist_ok=True)
    for fabric,sources in [('fattree',fat_sources(common)),('mesh',mesh_sources(common))]:
        top=f'tb_{fabric}_all_to_sparse'
        sources += [rtl/'target/src/benchmark/all_to_sparse_traffic.sv',rtl/f'target/src/benchmark/{top}.sv']
        generics=['-gGroupsPerSource=11','-gBeatsPerGroup=2','-gSeed=17']
        if fabric=='fattree': generics+=['-gRoutingMode=2']
        generated=out/fabric/'generated'; generated.mkdir(parents=True,exist_ok=True)
        generics=prepare(generics,fabric,generated)
        start=time.perf_counter()
        log=simulate(out/fabric,sources,top,'ALL_TO_SPARSE_PASS',common,generics,timeout=1800)
        paths=[f for f in (out/fabric).glob('*.csv') if not f.name.endswith('_destinations.csv')]
        if len(paths)!=1: raise RuntimeError('Expected one benchmark CSV')
        with paths[0].open() as f: rows=list(csv.DictReader(f))
        if len(rows)!=1 or rows[0]['status']!='PASS': raise RuntimeError('RTL scoreboard failed')
        report['tests'].append(dict(benchmark=rows[0],wall_seconds=time.perf_counter()-start,log=str(log)))
        for source in sources:
            report['source_sha256'][str(source.relative_to(rtl))]=hashlib.sha256(source.read_bytes()).hexdigest()
        (out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
        print(f'PASS fresh {fabric}: {rows[0]["runtime_cycles"]} cycles',flush=True)
    report['status']='PASS'
    (out/'results.json').write_text(json.dumps(report,indent=2)+'\n')


if __name__=='__main__': main()
