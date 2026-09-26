#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prepare a reproducible DRAMSys-5-compatible copy of the HBM4 config.

Original files are never edited. The manifest records every compatibility
conversion and instrumentation override, with source and resolved hashes.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


def read_json(path):
    # Preserve comment markers inside JSON strings.
    text = re.sub(r'("(?:\\.|[^"\\])*"|//[^\n]*|/\*[\s\S]*?\*/)',
                  lambda m: m[0] if m[0].startswith('"') else '', path.read_text())
    return json.loads(text)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(source, output):
    source, output = Path(source).resolve(), Path(output).resolve()
    if source.parent == output:
        raise ValueError('Output must differ from the source configuration directory')
    top = read_json(source)
    config = top['simulation']
    docs, files = {}, {'simulation': source}
    changes = []
    for key in ('addressmapping', 'mcconfig', 'memspec', 'simconfig'):
        name = Path(config[key]).name
        path = source.parent / key / name
        files[key], docs[key] = path, read_json(path)
        config[key] = name
    changes.append('Use basename references required by the bundled DRAMSys 5 loader.')
    config.pop('tracesetup', None)
    changes.append('Remove standalone traffic generators; all traffic comes from GVSoC.')
    spec = docs['memspec']['memspec']
    arch, timing = spec['memarchitecturespec'], spec['memtimingspec']
    timing['clkMhz'] = round(1e-6 / timing['tCK'], 9)
    changes.append('Derive clkMhz from tCK; preserve every original memspec timing value.')
    mapping = docs['addressmapping']['addressmapping']
    byte_bits = (arch['width'] * arch.get('nbrOfDevices', 1) // 8).bit_length() - 1
    old_bits = len(mapping['BYTE_BIT'])
    if mapping['BYTE_BIT'] != list(range(old_bits)) or old_bits < byte_bits:
        raise ValueError('Unsupported source byte mapping')
    delta = old_bits - byte_bits
    if delta:
        mapping['BYTE_BIT'] = list(range(byte_bits))
        for key, bits in mapping.items():
            if key != 'BYTE_BIT' and key.endswith('_BIT'):
                if any(not isinstance(bit, int) or bit < old_bits for bit in bits):
                    raise ValueError('Unsupported noncontiguous/XOR address map')
                mapping[key] = [bit - delta for bit in bits]
        changes.append(f'Convert {1 << old_bits}-byte BYTE_BIT addressing to {1 << byte_bits}-byte '
                       'words matching memspec.width; shift all other mapping bits equally.')
    sim = docs['simconfig']['simconfig']
    for key in ('DatabaseRecording', 'ProgressBar', 'EnableWindowing'):
        sim[key] = False
    changes.append('Disable database recording, progress bar and window instrumentation; retain '
                   'storage, refresh, scheduler, queue capacities and all memory timings.')
    output.mkdir(parents=True, exist_ok=True)
    resolved = {'simulation': output / source.name}
    resolved['simulation'].write_text(json.dumps(top, indent=2) + '\n')
    for key, doc in docs.items():
        dest = output / key / files[key].name
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(json.dumps(doc, indent=2) + '\n')
        resolved[key] = dest
    manifest = dict(changes=changes, source={k: dict(path=str(v), sha256=sha256(v))
                                           for k, v in files.items()},
                    resolved={k: dict(path=str(v.relative_to(output)), sha256=sha256(v))
                              for k, v in resolved.items()})
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return resolved['simulation']


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[4] /
                        'add_dramsyslib_patches/dramsys_configs/hbm4-emu-example.json')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(prepare(args.source, args.output))
