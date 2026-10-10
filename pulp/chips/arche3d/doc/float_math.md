# Floating-point arithmetic across arche3d

Snitch scalar instructions, Spatz vector instructions, RedMule and data-NoC
reductions use GVSoC's existing `cpu/iss/flexfloat/flexfloat.c` backend.
`logic/float_math.hpp` adapts that library for the non-CPU engines; there is no
separate RedMule or NoC float-to-bit conversion implementation.

| Format | Exponent bits | Fraction bits | Positive infinity | Canonical NaN |
| --- | ---: | ---: | --- | --- |
| FP16 (IEEE binary16) | 5 | 10 | `0x7c00` | `0x7e00` |
| BF16 | 8 | 7 | `0x7f80` | `0x7fc0` |
| FP8 E5M2 | 5 | 2 | `0x7c` | `0x7e` |
| FP8 E4M3 | 4 | 3 | `0x78` | `0x7c` |
| FP32 (RedMule accumulator) | 8 | 23 | `0x7f800000` | `0x7fc00000` |

All formats include signed zeros, subnormals, infinities and NaNs. E4M3 has
maximum finite magnitude 240 and is **not E4M3FN**. CPU `fmode=0` selects
FP16/E5M2 and `fmode=3` selects BF16/E4M3. NoC and RedMule format selections
are explicit in each command and do not depend on a submitting core's CSRs.

Scalar/Spatz arithmetic uses the core's rounding mode and exception flags.
NoC and RedMule use round-to-nearest, ties-to-even (RNE), and do not update a
core's `fflags`. Their host floating-point environment is saved and restored
around computation, so operations cannot inherit or alter another unit's
rounding or exception state. Compare ordinary CPU arithmetic in `frm=0` for
bitwise agreement. Special functions use the implementation and accuracy limits in
[special_functions.md](special_functions.md).

## Data-NoC reductions

The native data-NoC collective endpoint supports element-wise sum and maximum
in all four formats. Its MMIO descriptor operation field uses these codes:

| Operation | Sum | Maximum | Element bytes |
| --- | ---: | ---: | ---: |
| Unsigned integer 16 | 2 | 5 | 2 |
| Signed integer 16 | 3 | 6 | 2 |
| FP16 | 4 | 7 | 2 |
| BF16 | 8 | 9 | 2 |
| FP8 E5M2 | 10 | 11 | 1 |
| FP8 E4M3 | 12 | 13 | 1 |

Code 1 selects multicast. Integer sums wrap at 16 bits. Floating sums round to
the selected element format at each combine; there is no wide accumulator in
the NoC. Maximum follows scalar RISC-V behavior: one NaN returns the numeric
operand, two NaNs return canonical NaN, and max(-0, +0) is +0.

The two-cycle router pipeline includes matching and reduction. No extra
join stage or outward request wave is modeled. Contributions are pushed
from every selected participant toward the root, which posts the result
receive. The model assumes a full-width datapath meeting that pipeline
latency; this is an architectural timing assumption, not RTL calibration.

Inputs combine in fixed local/right/left/up/down order, independently of
arrival timing. Floating sums are not associative: a differently ordered
serial sum can produce different bits with the same arithmetic library.

Each participant provides its own local L1 address. One descriptor streams
the whole buffer in wide flits (128 bytes by default), including an exact tail.
FP16/BF16 addresses and lengths must be even, and each beat must hold a complete
element; FP8 permits odd addresses and lengths. In X/Y coordinate-match masks,
zero is a wildcard and set bits must match the root.
Groups can be sparse or span both axes; the root always participates.
Unselected routers forward/combine selected branches without a local operand.
All members match slot, epoch, root, masks, operation and byte count.

The SDK uses `arche3d_collective_prepare_masked` (or the full-row/full-column
`arche3d_collective_prepare`), `arche3d_collective_post_receive`
and `arche3d_collective_send`. Send completion permits source reuse after
local capture; receive completion permits reading the destination after
all its L1 writes. Group synchronization uses `arche3d_masked_clusters_barrier`
with the collective's masks, or dedicated full-row/full-column barriers,
after these local completions. Ordinary DMA and native collectives have separate
command interfaces. The sync bus still handles scalar remote L1 and wakeup notifications.

The SDK [row size sweep](../../../../../arche3d_sdk/apps/collective_row_sweep/README.md)
uses E4M3 sum and midpoint roots for contiguous 32/16/8-member groups. Every
member contributes finite powers of two; the specified tree has exactly
representable partial sums and independent expected result encodings. Larger
buffers use one bulk command and epoch, with byte offsets matching successive
reduction beats. There are no per-beat software issue gaps; finite buffering
and L1/mesh contention may stall streaming. Router arithmetic remains part of
each two-cycle hop, with one result beat per cycle when unblocked.

## RedMule accumulation

All floating-point RedMule modes have an **IEEE binary32 (FP32) accumulator**:

1. Convert the initial Y element to FP32 using RNE.
2. For each term, fuse `X * W + accumulator` and round the result to FP32.
3. Keep those 32-bit accumulator bits across internal N-tile boundaries.
4. Convert to the selected output format only after the final term.

Finite FP16, BF16, and FP8 inputs and initial Y widen exactly to FP32. BF16 initial values
retain their range when widened to FP32. A fused MAC can still overflow or underflow in
FP32, and the final conversion is subject to the output format's range and
precision. For example, BF16 `max_finite * 2 - max_finite` remains finite even
though the intermediate product would overflow if rounded separately to FP32.

The adapter uses FlexFloat's host-double arithmetic to evaluate a fused result,
then stores only the rounded binary32 bits. No extra host-double precision is
retained between MACs or across tiles. `redmule_elem_size` describes initial
operand storage width, not accumulator precision; there is no accumulator
mode switch or change to the software command interface.

| Arithmetic mode | Input/output format | Accumulator |
| ---: | --- | --- |
| 3 | FP16 | FP32 |
| 7 | FP8 E5M2 | FP32 |
| 8 | BF16 | FP32 |
| 9 | FP8 E4M3 | FP32 |

Modes 8 and 9 select BF16 and E4M3. Integer modes use their own
accumulation rules. Only valid matrix terms are evaluated; padded entries must not
introduce zero-times-infinity NaNs or change signed zeros.

For a scalar reference, widen X/W and initial Y to FP32, execute the same
ordered sequence of FP32 FMAs in RNE, and narrow once at the end. A sequence
of scalar FP16/BF16/FP8 FMAs or NoC narrow sums has different intermediate
rounding. For example, starting at 1, adding `2^-11` twice produces
`1 + 2^-10` at FP16 output; an FP16 accumulator would discard both increments.
FP32 still rounds every MAC: adding `2^-24` to 1 ties back to 1 under RNE.

The SDK provides synchronous `arche3d_redmule_gemm_{fp16,bf16,e5m2,e4m3}`:
`Y[m,k] += X[m,n] * W[n,k]`. Buffers are row-major local L1 arrays in the named
format. One core owns RedMule at a time, and dimensions must be nonzero.

## Validation and commands

After the standard GVSoC environment setup and `make dramsys_preparation`,
run from the GVSoC repository root:

```bash
python pulp/tests/arche3d/test_float_math.py
make cfg=arche3d_redmule TARGETS='arche3d_dma_test arche3d_collective_test' \
    MODULES="$PWD/pulp/tests/arche3d;$PWD/pulp/tests/arche3d_collectives" build
make cfg=arche3d_redmule app=fp_alignment arche3d-sw
gvrun --target=arche3d_dma_test --target-dir=pulp/tests/arche3d \
    --parameter=clusters=4 \
    --parameter=config=arche3d_redmule \
    --binary=build/arche3d/sw/arche3d_redmule/fp_alignment/fp_alignment.elf \
    --work-dir=build/runs/arche3d_fp_alignment run
gvrun --target=arche3d_collective_test --target-dir=pulp/tests/arche3d_collectives \
    --work-dir=build/runs/arche3d_collectives run
make -C arche3d_sdk lint
```

The host test uses an independent exact `Fraction` oracle and requires a host
C/C++ compiler, but no Python packages. It checks all FP8 sum/max operand pairs,
random FP16/BF16 pairs, all narrow source encodings for conversion into and
back from the FP32 accumulator, arbitrary FP32-to-narrow output conversions,
and mixed-format fused FP32 MACs. Exact integer rounding handles the FP32
reference without enumerating its encoding space. Directed cases cover signed
zeros, subnormals, infinities, NaNs, and overflow; host rounding/exception
environment isolation is also checked.

`fp_alignment` runs on four production tiles. Select `arche3d_redmule` to exercise
RedMule; the default MXCore configuration reports that part skipped. Every scalar core computes sum,
max and FMA references; all configured Spatz cores compare vector results. The
native endpoint issues two-participant posted reductions and compares with the scalar
results. RedMule tests cover subnormals, fused rounding, BF16 input/initial-Y
range, FP16-vs-FP32 accumulation, per-MAC FP32-vs-host-double rounding, and
preservation of FP32 state across internal tiles.
The NoC-only test covers configurable meshes through 32x32, concurrent groups,
shallow queues, delayed receivers/contributors and denied local-memory targets.
