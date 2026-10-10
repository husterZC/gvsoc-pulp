#!/usr/bin/env python3
"""Recreate regression fixtures and verify their hashes against real RTL runs."""
import hashlib
import json
from pathlib import Path
from reference import make_case

HERE=Path(__file__).resolve().parent


def generate():
    report=json.loads((HERE/'calibration.json').read_text())
    work=HERE/'build/calibration'
    work.mkdir(parents=True,exist_ok=True)
    last={}
    for record in report['records']:
        last[record['m'],record['n'],record['k']]=record
    for (m,n,k),record in last.items():
        name=f'm{m}_n{n}_k{k}'
        inp=work/f'{name}.bin'; out=work/f'{name}.expected'
        ih=record['input_sha256']; oh=record['output_sha256']
        if inp.exists() and out.exists() and hashlib.sha256(inp.read_bytes()).hexdigest()==ih and hashlib.sha256(out.read_bytes()).hexdigest()==oh:
            continue
        memory,golden=make_case(m,n,k,record['seed'])
        if hashlib.sha256(memory).hexdigest()!=ih or hashlib.sha256(golden).hexdigest()!=oh:
            raise RuntimeError(f'{name}: fixture no longer matches the recorded RTL run')
        inp.write_bytes(memory); out.write_bytes(golden)


if __name__=='__main__':
    generate()
