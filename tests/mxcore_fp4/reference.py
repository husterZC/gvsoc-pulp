"""Independent Python oracle and tiled, packed MXFP4 test vectors (stdlib only)."""
import math
import random
import struct

SHAPES = [(m, n, k) for m in (32, 64, 128) for n in (32, 64, 128)
          for k in (32, 64, 96, 128, 160, 192)]
VALUES = (0, .5, 1, 1.5, 2, 3, 4, 6)


def f32(value):
    try:
        return struct.unpack('<f', struct.pack('<f', value))[0]
    except OverflowError:
        return math.copysign(math.inf, value)


def pack(codes):
    return bytes(a | (b << 4) for a, b in zip(codes[::2], codes[1::2]))


def quantize(values):
    if any(not math.isfinite(x) for x in values):
        return bytes(16), 255
    maximum = max(abs(x) for x in values)
    exponent = max(-127, min(127, math.frexp(maximum)[1] - 3)) if maximum else 0
    result = []
    for value in values:
        scaled = math.ldexp(abs(value), -exponent)
        code = min(range(8), key=lambda q: (abs(VALUES[q] - scaled), q & 1))
        result.append(code | (8 if math.copysign(1, value) < 0 else 0))
    return pack(result), exponent + 127


def sizes(m, n, k):
    return [m*k//2, n*k//2, m*k//32, n*k//32, m*n//2, m*n//32]


def bases(m, n, k):
    result = [0x1000]
    for size in sizes(m, n, k)[:-1]:
        result.append(result[-1] + size)
    return result


def make_case(m, n, k, seed=1, pattern='random'):
    rng = random.Random(seed)
    a = [[rng.randrange(16) for _ in range(k)] for _ in range(m)]
    b = [[rng.randrange(16) for _ in range(k)] for _ in range(n)]
    sa = [[rng.randrange(-3, 4) for _ in range(k//32)] for _ in range(m)]
    sb = [[rng.randrange(-3, 4) for _ in range(k//32)] for _ in range(n)]
    if pattern == 'wide':
        sa = [[rng.randrange(-63, 64) for _ in range(k//32)] for _ in range(m)]
        sb = [[rng.randrange(-63, 64) for _ in range(k//32)] for _ in range(n)]
    if pattern in ('tiny', 'scale_nan'):
        sa = [[-70 if pattern == 'tiny' else 128]*(k//32) for _ in range(m)]
        sb = [[-70 if pattern == 'tiny' else 0]*(k//32) for _ in range(n)]
    if pattern == 'ties':
        a = [[1,2,2]+[0]*(k-3) for _ in range(m)]
        triples = [(1,0,0),(3,0,0),(1,2,0),(3,2,0),(2,4,0),(2,5,0),(0,4,5),(0,7,3)]
        b = []
        for j in range(n):
            triple = triples[j%8]
            b.append([x | (8 if j%16>=8 else 0) for x in triple]+[0]*(k-3))
        sa = [[0]*(k//32) for _ in range(m)]
        sb = [[0]*(k//32) for _ in range(n)]
    if pattern in ('zero', 'ones'):
        a = [[0 if pattern == 'zero' else 2]*k for _ in range(m)]
        b = [[2]*k for _ in range(n)]
        sa = [[0]*(k//32) for _ in range(m)]
        sb = [[0]*(k//32) for _ in range(n)]
    regions = []
    for matrix, dim in ((a, m), (b, n)):
        regions.append(b''.join(pack(matrix[i][kk:kk+32])
                       for t in range(0, dim, 32) for kk in range(0, k, 32)
                       for i in range(t, t+32)))
    for scales, dim in ((sa, m), (sb, n)):
        regions.append(bytes(scales[i][kk] + 127
                       for t in range(0, dim, 32) for kk in range(k//32)
                       for i in range(t, t+32)))
    result, scales_out = bytearray(), bytearray()
    # Convert once; a dot product of 32 E2M1 numbers is exact before scaling.
    av = [[(-1 if x & 8 else 1)*VALUES[x & 7] for x in row] for row in a]
    bv = [[(-1 if x & 8 else 1)*VALUES[x & 7] for x in row] for row in b]
    for mt in range(0, m, 32):
        for nt in range(0, n, 32):
            for i in range(mt, mt+32):
                row = []
                for j in range(nt, nt+32):
                    acc = 0.0
                    for kk in range(0, k, 32):
                        dot = sum(av[i][h]*bv[j][h] for h in range(kk, kk+32))
                        if sa[i][kk//32] == 128 or sb[j][kk//32] == 128:
                            acc = math.nan
                        else:
                            acc = f32(acc + math.ldexp(dot, sa[i][kk//32]+sb[j][kk//32]))
                    row.append(acc)
                data, scale = quantize(row)
                result.extend(data)
                scales_out.append(scale)
    ptrs = bases(m, n, k)
    memory = bytearray(ptrs[-1] + m*n//32 + 256)
    for ptr, data in zip(ptrs, regions):
        memory[ptr:ptr+len(data)] = data
    return bytes(memory), bytes(result + scales_out)
