#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Independent rational-number oracle; no model arithmetic is imported."""
import struct
from fractions import Fraction as F
from functools import lru_cache
from pathlib import Path


@lru_cache(maxsize=1024)
def power(e):
    return F(2) ** e


def log2(v):
    e = v.numerator.bit_length() - v.denominator.bit_length()
    return e - (v < power(e))


def decode(bits, eb, fb):
    sign = -1 if bits >> (eb + fb) else 1
    exp, frac = (bits >> fb) & ((1 << eb) - 1), bits & ((1 << fb) - 1)
    if exp == (1 << eb) - 1:
        return None
    bias = (1 << (eb - 1)) - 1
    return sign * F(frac + ((1 << fb) if exp else 0)) * power((exp or 1) - bias - fb)


def encode(v, eb, fb, negative_zero=False):
    if v is None:
        return (((1 << eb) - 1) << fb) | (1 << (fb - 1))
    sign = int(v < 0 or negative_zero) << (eb + fb)
    v = abs(v)
    if not v:
        return sign
    bias = (1 << (eb - 1)) - 1
    e = max(log2(v), 1 - bias)
    mant = round(v / power(e - fb))
    if mant == 1 << (fb + 1):
        mant //= 2
        e += 1
    if e + bias >= (1 << eb) - 1:
        return sign | (((1 << eb) - 1) << fb)
    if mant < 1 << fb:
        return sign | mant
    return sign | ((e + bias) << fb) | (mant - (1 << fb))


MAG = [F(0), F(1, 2), F(1), F(3, 2), F(2), F(3), F(4), F(6)]


def case(mode, count):
    if mode in (1, 2, 5):
        data = bytes(((i * 2) % 16) | (((i * 2 + 1) % 16) << 4) for i in range(count // 2))
        scales = bytes(i % 256 for i in range(count // 32))
        eb, fb = {1: (5, 2), 2: (8, 7), 5: (4, 3)}[mode]
        out = bytearray()
        for i in range(count):
            code = (data[i // 2] >> (4 * (i % 2))) & 15
            scale = scales[i // 32]
            v = None if scale == 255 else (-1 if code & 8 else 1) * MAG[code & 7] * power(scale - 127)
            bits = encode(v, eb, fb, code == 8)
            out.extend(bits.to_bytes(2 if mode == 2 else 1, 'little'))
        return data, scales, bytes(out), b''
    eb, fb = (8, 7) if mode == 3 else (5, 10)
    # Sweep every 16-bit encoding for large cases, including signed zero,
    # subnormal blocks, ties, saturation, infinities and both signs of NaNs.
    codes = [i % 65536 for i in range(count)]
    data = struct.pack('<' + 'H' * count, *codes)
    out, scales = bytearray(), bytearray()
    for start in range(0, count, 32):
        values = [decode(x, eb, fb) for x in codes[start:start + 32]]
        if None in values:
            scales.append(255)
            out.extend(bytes(16))
            continue
        maximum = max(map(abs, values))
        e = max(-127, min(127, log2(maximum) - 2)) if maximum else 0
        scales.append(e + 127)
        nibbles = []
        for i, v in enumerate(values):
            distances = [abs(abs(v) / power(e) - q) for q in MAG]
            code = min(range(8), key=lambda c: (distances[c], c & 1))
            nibbles.append(code | ((codes[start + i] >> 15) << 3))
        out.extend(nibbles[i] | (nibbles[i + 1] << 4) for i in range(0, 32, 2))
    return data, b'', bytes(out), bytes(scales)


def matrix_blocks(rows, cols, column):
    """Enumerate logical coordinates in the documented MXCore storage order."""
    outer, inner = (cols, rows) if column else (rows, cols)
    for tile in range(0, outer, 32):
        for reduction in range(0, inner, 32):
            for lane in range(32):
                yield [(reduction + i, tile + lane) if column else
                       (tile + lane, reduction + i) for i in range(32)]


@lru_cache(maxsize=8192)
def requantize(block):
    if any(scale == 255 for _, scale in block):
        return bytes(16), 255
    values = [(-1 if code & 8 else 1) * MAG[code & 7] * power(scale - 127)
              for code, scale in block]
    maximum = max(map(abs, values))
    e = max(-127, min(127, log2(maximum) - 2)) if maximum else 0
    codes = []
    for (source, _), value in zip(block, values):
        scaled = abs(value) / power(e)
        closest = min(range(8), key=lambda code: (abs(scaled - MAG[code]), code & 1))
        codes.append(closest | (source & 8))
    return bytes(codes[i] | (codes[i + 1] << 4) for i in range(0, 32, 2)), e + 127


def reblock_case(mode, rows, cols, kind='sweep'):
    count = rows * cols
    codes = [(i + 3 * (i // 32)) % 16 for i in range(count)]
    scales = bytes(i % 256 for i in range(count // 32))
    if kind == 'zeros':
        codes = [8 * (i % 2) for i in range(count)]
        scales = bytes(254 * (i % 2) for i in range(count // 32))
    elif kind == 'wide':
        scales = bytes([254] * (count // 32))
    elif kind == 'ties':
        scales = bytes(126 + i % 4 for i in range(count // 32))
    elif kind == 'nan':
        scales = bytes([255] + [127] * (count // 32 - 1))
    data = bytes(codes[i] | (codes[i + 1] << 4) for i in range(0, count, 2))
    matrix = [None] * count
    for block, coordinates in enumerate(matrix_blocks(rows, cols, mode == 7)):
        for i, (r, c) in enumerate(coordinates):
            matrix[r * cols + c] = (codes[block * 32 + i], scales[block])
    out, out_scales = bytearray(), bytearray()
    for coordinates in matrix_blocks(rows, cols, mode == 6):
        payload, scale = requantize(tuple(matrix[r * cols + c] for r, c in coordinates))
        out.extend(payload)
        out_scales.append(scale)
    return data, scales, bytes(out), bytes(out_scales)


def generate(output):
    records = []
    for mode in range(1, 6):
        for count in (32, 1024, 8192, 32768, 65536):
            records.append((mode, 1, count, 0, *case(mode, count)))
    for width in (1, 2):
        rows, cols = 37, 65
        data = bytes(i % 256 for i in range(rows * cols * width))
        out = b''.join(data[(r*cols+c)*width:(r*cols+c+1)*width]
                       for c in range(cols) for r in range(rows))
        records.append((0, rows, cols, width, data, b'', out, b''))
    for mode in (6, 7):
        for rows, cols in ((32, 32), (32, 64), (64, 32), (64, 96), (96, 64),
                           (128, 128), (256, 256), (32, 2048), (2048, 32)):
            records.append((mode, rows, cols, 0, *reblock_case(mode, rows, cols)))
        for kind in ('zeros', 'wide', 'ties', 'nan'):
            records.append((mode, 32, 32, 0, *reblock_case(mode, 32, 32, kind)))
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open('wb') as f:
        f.write(struct.pack('<I', len(records)))
        for mode, rows, cols, width, *arrays in records:
            f.write(struct.pack('<8I', mode, rows, cols, width, *map(len, arrays)))
            for data in arrays:
                f.write(data)


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    generate(parser.parse_args().output)
