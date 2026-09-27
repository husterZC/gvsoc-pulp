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

All formats include signed zeros, subnormals, infinities and NaNs. E4M3 has
maximum finite magnitude 240 and is **not E4M3FN**. CPU `fmode=0` selects
FP16/E5M2 and `fmode=3` selects BF16/E4M3. NoC and RedMule format selections
are explicit in each command and do not depend on a submitting core's CSRs.

Scalar/Spatz arithmetic uses the core's rounding mode and exception flags.
NoC and RedMule use round-to-nearest, ties-to-even (RNE), and do not update a
core's `fflags`. Their host floating-point environment is saved and restored
around computation, so operations cannot inherit or alter another unit's
rounding or exception state. Compare ordinary CPU arithmetic in `frm=0` for
bitwise agreement. This change does not extend the CPU backend's rounding-mode
support. Special functions retain the implementation and accuracy limits in
[special_functions.md](special_functions.md).

## Data-NoC reductions

The logic-die DMA supports element-wise sum and maximum in all four formats.
The legacy five-bit XDMA operation field and bridge sideband use these codes:

| Operation | Sum | Maximum | Element bytes |
| --- | ---: | ---: | ---: |
| Unsigned integer 16 | 2 | 5 | 2 |
| Signed integer 16 | 3 | 6 | 2 |
| FP16 | 4 | 7 | 2 |
| BF16 | 8 | 9 | 2 |
| FP8 E5M2 | 10 | 11 | 1 |
| FP8 E4M3 | 12 | 13 | 1 |

Code 1 remains broadcast. Integer sums wrap at 16 bits. Floating sums round to
the selected element format at each combine; there is no wide accumulator in
the NoC. Maximum follows scalar RISC-V behavior: one NaN returns the numeric
operand, two NaNs return canonical NaN, and max(-0, +0) is +0.

A router's reduction/join stage takes **one network cycle** for any of these
operations. Link traversal, response routing, arbitration and backpressure
still take their normal time. This is a timing assumption, not an RTL
calibration, and is not a claim that the whole collective completes in one cycle.

Replies combine in fixed local/right/left/up/down tree order, independently
of target completion order. Floating sums are not associative: a serial sum
with a different order can give different bits even using the same library.

The source addresses the initiating cluster's remote-L1 alias; participants
access the same local offset. Each fragment is limited to a wide-network beat.
The bridge splits longer DMA transfers. FP16/BF16 addresses and lengths must
be even; FP8 accepts byte alignment and odd lengths. Match-mask bits constrain
coordinate bits, not individual destinations. For a 32x32 grid, zero selects
all coordinates and `0x1f` selects the initiating coordinate in that dimension.

Use one core to own the logic-die DMA's command registers, normally core n-1.
The SDK exposes `arche3d_noc_{sum,max}_{fp16,bf16,e5m2,e4m3}`; each returns a
DMA descriptor ID. Wait with `arche3d_dma_wait(id)` before reading the result
or modifying contributions. The I3D DMA still rejects all collective commands.
The sync bus remains for remote scalar L1 access and wakeup multicast.

## RedMule accumulation

All floating-point RedMule modes now have an **IEEE binary16 accumulator**:

1. Convert the initial Y element to FP16 using RNE.
2. For each term, fuse `X * W + accumulator` and round the result to FP16.
3. Keep those 16-bit accumulator bits across internal N-tile boundaries.
4. Convert to the selected output format only after the final term.

X/W retain their input format's range until the fused result is rounded. For
example, BF16 `2^20 * 2^-20` produces FP16 1; the operands are not first
converted to half. BF16 initial Y can overflow or underflow when converted
to the FP16 accumulator. The numerical contract deliberately does not retain
an FP32 accumulator. Internal host double values are FlexFloat's arithmetic
implementation, not architectural accumulator state.

| Arithmetic mode | Input/output format | Accumulator |
| ---: | --- | --- |
| 3 | FP16 | FP16 |
| 7 | FP8 E5M2 | FP16 |
| 8 | BF16 | FP16 |
| 9 | FP8 E4M3 | FP16 |

Modes 0–7 preserve their encodings. Mode 8 adds BF16 and mode 9 makes E4M3
available. Integer modes and the model's transfer/timing state machine are
unchanged. Only valid matrix terms are evaluated; padded entries must not
introduce zero-times-infinity NaNs or change signed zeros.

For FP16 inputs, a dot product matches the same ordered sequence of scalar
FP16 FMAs. For BF16/FP8, use the FP16-accumulator reference above: a sequence
of BF16/FP8 FMAs rounds at a different precision. For example, starting at 1,
adding `2^-11` twice stays 1 in an FP16 accumulator, while an FP32 accumulator
retains the contributions.

The SDK provides synchronous `arche3d_redmule_gemm_{fp16,bf16,e5m2,e4m3}`:
`Y[m,k] += X[m,n] * W[n,k]`. Buffers are row-major local L1 arrays in the named
format. One core owns RedMule at a time, and dimensions must be nonzero.

## Validation and commands

After the standard GVSoC environment setup and `make dramsys_preparation`,
run from the GVSoC repository root:

```bash
python pulp/tests/arche3d/test_float_math.py
make TARGETS='arche3d_dma_test arche3d_collective_test' \
    MODULES="$PWD/pulp/tests/arche3d;$PWD/pulp/tests/arche3d_collectives" build
make cfg=default app=fp_alignment arche3d-sw
gvrun --target=arche3d_dma_test --target-dir=pulp/tests/arche3d \
    --parameter=clusters=4 \
    --binary=build/arche3d/sw/default/fp_alignment/fp_alignment.elf \
    --work-dir=build/runs/arche3d_fp_alignment run
gvrun --target=arche3d_collective_test --target-dir=pulp/tests/arche3d_collectives \
    --work-dir=build/runs/arche3d_collectives run
make -C arche3d_sdk lint
```

The host test uses an independent exact `Fraction` oracle and requires a host
C/C++ compiler, but no Python packages. It checks all FP8 sum/max operand pairs,
random FP16/BF16 pairs, all source encodings for conversion into the accumulator,
and mixed-format fused FP16 MACs. It also tests host environment isolation.

`fp_alignment` runs on four production tiles. Every scalar core computes sum,
max and FMA references; all configured Spatz cores compare vector results. The
logic DMA issues real two-participant reductions and compares with the scalar
results. RedMule tests cover subnormals, fused rounding, BF16 input range,
FP16-vs-FP32 accumulation, and preservation of FP16 state across internal tiles.
The NoC-only test uses a 4x4 mesh, concurrent sources, shallow queues, delayed
and denied targets, reordered responses, match masks and invalid alignment.

Validated on 2026-09-27:

| Check | Result |
| --- | --- |
| Exact arithmetic oracle | PASS, 685,312 checks |
| `fp_alignment`, four tiles | PASS, 24 scalar cores and 16 Spatz units; 660,425 cycles |
| 4x4 data-NoC stress | PASS, 59 jobs, 9,486 target beats, 19,884 cycles |
| `fp_formats`, `fp_special` | PASS on four tiles |
| `smoke`, `memory`, `wakeup`, `alltoall` | PASS on four tiles |
| `reject_collective` | Expected failure on the I3D DMA |
| SDK formatting and lint | PASS |

The cross-unit software test includes 147,456 scalar reference operations,
98,304 scalar/vector comparisons, 16,256 scalar/NoC comparisons and 64 RedMule
GEMMs across four tiles. RedMule cases also cross M/K tile boundaries and use
incomplete edge tiles. NoC/RedMule commands are tested with deliberately
mismatched CPU format and rounding CSRs. Logs and machine-readable results
are saved under `build/arche3d/validation/fp_alignment/` (not tracked).
These are numerical/functional checks; the full 32x32 all-to-all benchmark
was not rerun for this arithmetic change.
