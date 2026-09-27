#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exact Fraction oracle for the arche3d FlexFloat adapters; no extra packages.

Checks every FP8 operand pair for sum/max, random FP16/BF16 pairs, all source
encodings for FP32 accumulator conversion, and fused FP32 MACs in all formats.
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
        # Cache the narrow formats only; binary32 has too many encodings to enumerate.
        self.values = ([self.decode(bits) for bits in range(self.sign * 2)]
                       if exponent + mantissa < 16 else None)
        self.positive = self.values[:self.inf] if self.values is not None else None
        self.overflow = self.decode(self.inf - 1) + (
            self.decode(self.inf - 1) - self.decode(self.inf - 2)) / 2

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
        if self.positive is None:
            return sign | self.rounded_wide(value)
        upper = bisect.bisect_left(self.positive, value)
        if upper == len(self.positive):
            return sign | (upper - 1)
        if self.positive[upper] == value:
            return sign | upper
        lower = upper - 1
        dl, du = value - self.positive[lower], self.positive[upper] - value
        return sign | (lower if dl < du or (dl == du and lower % 2 == 0) else upper)

    def rounded_wide(self, value):
        """Round an exact positive rational using integer quotient/remainder only."""
        if not value:
            return 0
        numerator, denominator = value.numerator, value.denominator
        exponent = numerator.bit_length() - denominator.bit_length()
        if (numerator < denominator << exponent if exponent >= 0
                else numerator << -exponent < denominator):
            exponent -= 1
        bias = (1 << (self.e - 1)) - 1
        # Subnormal numbers all have the same quantum as the smallest normal binade.
        exponent = max(exponent, 1 - bias)
        quantum = exponent - self.m
        if quantum >= 0:
            denominator <<= quantum
        else:
            numerator <<= -quantum
        significand, remainder = divmod(numerator, denominator)
        if 2 * remainder > denominator or (2 * remainder == denominator and significand & 1):
            significand += 1
        if significand == 1 << (self.m + 1):
            significand >>= 1
            exponent += 1
        if significand < 1 << self.m:
            return significand
        return ((exponent + bias) << self.m) | (significand - (1 << self.m))

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
    fp32 = Format(8, 23)
    fp32_index = len(formats)
    accumulator_edges = [0, 0x80000000, 1, 0x80000001, 0x007fffff, 0x00800000,
                         0x3f7fffff, 0x3f800000, 0x3f800001, 0x3f800002, 0xbf800000,
                         0x7f7fffff, 0xff7fffff, 0x7f800000, 0xff800000, 0x7fc00000,
                         0x7f800001]
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
                for destination in (fp32_index, index):
                    value = fmt.values[bits]
                    # A same-format bit conversion preserves the NaN payload.
                    want = bits if index == destination else fp32.rounded(value, bits == fmt.sign)
                    got = lib.convert(index, destination, bits)
                    assert got == want, ("convert", index, destination, hex(bits), hex(got), hex(want))
                    checks += 1
                wide = lib.convert(index, fp32_index, bits)
                got = lib.convert(fp32_index, index, wide)
                want = fmt.rounded(fmt.values[bits], bits == fmt.sign)
                assert got == want, ("roundtrip", index, hex(bits), hex(got), hex(want))
                checks += 1

            # Output conversion covers arbitrary FP32 state, not only widened narrow inputs.
            for bits in accumulator_edges + [random_source.getrandbits(32) for _ in range(20000)]:
                want = fmt.rounded(fp32.decode(bits), bits == fp32.sign)
                got = lib.convert(fp32_index, index, bits)
                assert got == want, ("output", index, hex(bits), hex(got), hex(want))
                checks += 1

            def check_mac(a, b, c):
                av, bv, cv = fmt.values[a], fmt.values[b], fp32.decode(c)
                product = av * bv
                value = product + cv
                negative_zero = product == cv == 0 and bool((a ^ b) & fmt.sign) and c == fp32.sign
                want = fp32.rounded(value, negative_zero)
                got = lib.mac(index, a, b, c)
                assert got == want, ("mac", index, hex(a), hex(b), hex(c), hex(got), hex(want))

            one = ((1 << (fmt.e - 1)) - 1) << fmt.m
            edges = [0, fmt.sign, 1, fmt.sign | 1, (1 << fmt.m) - 1, 1 << fmt.m,
                     one - 1, one, one + 1, fmt.sign | one, fmt.inf - 1,
                     fmt.sign | (fmt.inf - 1), fmt.inf, fmt.sign | fmt.inf, fmt.nan,
                     fmt.inf | 1]
            for a in edges:
                for b in edges:
                    for c in accumulator_edges:
                        check_mac(a, b, c)
                        checks += 1
            for _ in range(20000):
                a, b = (random_source.randrange(fmt.sign * 2) for _ in range(2))
                c = random_source.getrandbits(32)
                check_mac(a, b, c)
                checks += 1
    print(f"ARCHE3D_FLOAT_MATH_PASS checks={checks} exhaustive_fp8_pairs=1 fp32_accumulator=1")


if __name__ == "__main__":
    main()
