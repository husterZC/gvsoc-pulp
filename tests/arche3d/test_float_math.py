#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exact Fraction oracle for the arche3d FlexFloat adapters; no extra packages.

Checks every FP8 operand pair for sum/max, random FP16/BF16 pairs, all source
encodings for FP16 accumulator conversion, and fused FP16 MACs in all formats.
"""
import bisect
import ctypes
from fractions import Fraction
import math
from pathlib import Path
import random
import subprocess
import tempfile


class Format:
    def __init__(self, exponent, mantissa):
        self.e, self.m = exponent, mantissa
        self.sign = 1 << (exponent + mantissa)
        self.inf = ((1 << exponent) - 1) << mantissa
        self.nan = self.inf | (1 << (mantissa - 1))
        self.values = [self.decode(bits) for bits in range(self.sign * 2)]
        self.positive = self.values[:self.inf]
        self.overflow = self.positive[-1] + (self.positive[-1] - self.positive[-2]) / 2

    def decode(self, bits):
        negative = bool(bits & self.sign)
        exponent = (bits & (self.sign - 1)) >> self.m
        fraction = bits & ((1 << self.m) - 1)
        if exponent == (1 << self.e) - 1:
            return math.nan if fraction else -math.inf if negative else math.inf
        significand = fraction + ((1 << self.m) if exponent else 0)
        power = max(exponent, 1) - ((1 << (self.e - 1)) - 1) - self.m
        value = Fraction(significand) * Fraction(2) ** power
        return -value if negative else value

    def rounded(self, value, negative_zero=False):
        if isinstance(value, float):
            if math.isnan(value):
                return self.nan
            return self.inf | (self.sign if value < 0 else 0)
        sign = self.sign if value < 0 or (value == 0 and negative_zero) else 0
        value = abs(value)
        if value >= self.overflow:
            return sign | self.inf
        upper = bisect.bisect_left(self.positive, value)
        if upper == len(self.positive):
            return sign | (upper - 1)
        if self.positive[upper] == value:
            return sign | upper
        lower = upper - 1
        dl, du = value - self.positive[lower], self.positive[upper] - value
        return sign | (lower if dl < du or (dl == du and lower % 2 == 0) else upper)

    def sum(self, a, b):
        value = self.values[a] + self.values[b]
        return self.rounded(value, a == self.sign and b == self.sign)

    def maximum(self, a, b):
        av, bv = self.values[a], self.values[b]
        an = isinstance(av, float) and math.isnan(av)
        bn = isinstance(bv, float) and math.isnan(bv)
        if an:
            return self.nan if bn else b
        if bn:
            return a
        if av == bv == 0:
            return (a & b) & self.sign
        return a if av >= bv else b


def main():
    root = Path(__file__).resolve().parents[3]
    formats = [Format(5, 10), Format(8, 7), Format(5, 2), Format(4, 3)]
    random_source = random.Random(0x3d)
    checks = 0
    with tempfile.TemporaryDirectory(prefix="arche3d-float-") as temp:
        library = Path(temp) / "math.so"
        obj = Path(temp) / "flexfloat.o"
        flags = ["-O2", "-fPIC", "-frounding-math", "-fno-strict-aliasing"]
        subprocess.run(["cc", *flags, "-c", str(root / "core/models/cpu/iss/flexfloat/flexfloat.c"),
                        "-o", str(obj)], check=True)
        subprocess.run(["c++", *flags, "-shared", "-std=c++17", "-I" + str(root / "core/models"),
                        "-I" + str(root / "pulp"), str(Path(__file__).with_name("float_math_probe.cpp")),
                        str(obj), "-lm", "-o", str(library)], check=True)
        lib = ctypes.CDLL(str(library))
        for function, count in [("pair", 4), ("mac", 4), ("convert", 3)]:
            getattr(lib, function).argtypes = [ctypes.c_uint] * count
            getattr(lib, function).restype = ctypes.c_uint
        assert lib.environment(), "Host rounding/exception environment leaked"
        for index, fmt in enumerate(formats):
            pairs = ((a, b) for a in range(256) for b in range(256)) if index >= 2 else (
                (random_source.randrange(65536), random_source.randrange(65536)) for _ in range(20000))
            for a, b in pairs:
                for maximum in (0, 1):
                    want = fmt.maximum(a, b) if maximum else fmt.sum(a, b)
                    got = lib.pair(index, maximum, a, b)
                    assert got == want, ("pair", index, maximum, hex(a), hex(b), hex(got), hex(want))
                    checks += 1
            for bits in range(fmt.sign * 2):
                for destination in (0, index):
                    value = fmt.values[bits]
                    # A same-format bit conversion preserves the NaN payload.
                    want = bits if index == destination else formats[destination].rounded(
                        value, bits == fmt.sign)
                    got = lib.convert(index, destination, bits)
                    assert got == want, ("convert", index, destination, hex(bits), hex(got), hex(want))
                    checks += 1
            for _ in range(20000):
                a, b = (random_source.randrange(fmt.sign * 2) for _ in range(2))
                c = random_source.randrange(65536)
                av, bv, cv = fmt.values[a], fmt.values[b], formats[0].values[c]
                product = av * bv
                value = product + cv
                negative_zero = product == cv == 0 and bool((a ^ b) & fmt.sign) and c == 0x8000
                want = formats[0].rounded(value, negative_zero)
                got = lib.mac(index, a, b, c)
                assert got == want, ("mac", index, hex(a), hex(b), hex(c), hex(got), hex(want))
                checks += 1
    print(f"ARCHE3D_FLOAT_MATH_PASS checks={checks} exhaustive_fp8_pairs=1 fp16_accumulator=1")


if __name__ == "__main__":
    main()
