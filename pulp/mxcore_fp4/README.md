# MXCoreFP4

`pulp.mxcore_fp4.MXCoreFP4` models `C[M,N] = A[M,K] × B[K,N]` with packed
MXFP4 E2M1 inputs, E8M0 input scales, and FP32 accumulation. Output storage is
selectable: MXFP4, FP32, BF16, FP16, FP8 E4M3, or FP8 E5M2. It accepts these
calibrated shapes for every output format:

- M and N: 32, 64, 128.
- K (the shared dimension): 32, 64, 96, 128, 160, 192.
- Additionally, (M,N,K)=(32,32,576) and (32,192,64) (56 shapes total).

The model is a functional accelerator with **measured transaction schedules**.
It computes results from the memory contents supplied by the simulated system;
the profiles contain only timing, addresses relative to the six buffer bases,
and transfer directions. They contain no operand or result data. This deliberately
limits timing claims to the requested shapes and the configuration below. Other
shapes, formats, and hardware configurations are rejected rather than extrapolated.

## Hardware and calibration

The source is [pulp-platform/MXCore at edf45a65](https://github.com/pulp-platform/MXCore/tree/edf45a65a9c19dcb901eed2148ab9621573a6621).
Upstream quantizes results to MXFP8 and shares each input FP4 scale across 64
values. The calibration variant makes these explicit changes:

1. Adds the combinational FP32 → MXFP4 output quantizer, including packed output
   streaming. Rounding is nearest, ties to even, with finite saturation.
2. Uses a scale per **32 FP4 values** on inputs, matching the output block size.
3. Fixes a signed comparison in FPnew that otherwise fails to recognize E8M0
   `0xff` as NaN. The scale comparison now uses an 8-bit all-ones constant.
4. Retains the upstream native FP32 output path and adds combinational RNE
   converters for BF16, FP16, E4M3, and E5M2, each feeding the existing style of
   two-entry output FIFO and 256-bit streamer. These casts add no pipeline stage;
   their buffering, serialization, and completion timing is measured in RTL.

The upstream arithmetic datapath, controller, streamer, and FIFOs are simulated
in Verilator. The shared FP8 hardware paths remain enabled because the upstream
FP4 dot product uses them. Only FP4 inputs are exposed by this GVSoC model.

| Parameter | Value |
| --- | --- |
| Physical `VectorSize` | 16 packed lanes = 32 FP4 values |
| `NPE` | 32 |
| `Reuse` | 32 |
| `NumPipeRegs` | 4, upstream BEFORE configuration |
| Peak FP4 throughput | 1,024 MACs/cycle |
| TCDM | 256 bits, eight 32-bit banks |
| Calibration memory | Always granted, one-cycle responses |
| Input/MXFP4 output scale block | 32 elements; native outputs have no scale |
| Accumulator initialization | Zero; no preload |

Latency is measured from the trigger acceptance edge to the completion event.
MMIO setup is excluded; all accelerator memory traffic and output quantization
are included. All 54 cases passed two random seeds with **zero mismatched output
bytes**, including scales. The GVSoC regression checks every transfer address,
direction, issue cycle, result byte, and completion cycle against these profiles.
The nominal cycle error is zero by construction of the measured schedules.

| M | N | K=32 | K=64 | K=96 | K=128 | K=160 | K=192 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 32 | 32 | 72 | 121 | 156 | 189 | 222 | 273 |
| 32 | 64 | 121 | 201 | 271 | 341 | 425 | 507 |
| 32 | 128 | 236 | 373 | 511 | 645 | 811 | 975 |
| 64 | 32 | 121 | 201 | 271 | 341 | 425 | 507 |
| 64 | 64 | 236 | 373 | 511 | 645 | 811 | 975 |
| 64 | 128 | 442 | 715 | 989 | 1256 | 1583 | 1911 |
| 128 | 32 | 236 | 373 | 511 | 645 | 811 | 975 |
| 128 | 64 | 442 | 715 | 989 | 1256 | 1583 | 1911 |
| 128 | 128 | 854 | 1399 | 1945 | 2480 | 3127 | 3783 |

See [calibration.json](../../tests/mxcore_fp4/calibration.json) for the RTL
revision, tool versions, configuration, seeds, and input/output/trace hashes.
The original [calibration test directory](../../tests/mxcore_fp4) contains the
MXFP4 baseline. The output extension, reproducible RTL preparation, arithmetic
oracle, calibration records, and Arche3D benchmark are in the enclosing GVSoC
repository's `scripts/mxcore_fp4/`. `output_profiles.inc` contains the measured
schedules for all six outputs; the legacy MXFP4 schedules remain unchanged.

For 32×32×576, nominal unstalled RTL cycles are: MXFP4 729, FP32 834,
BF16/FP16 770, and E4M3/E5M2 738. These exclude MMIO setup and do not include
Arche3D L1/interconnect service delays.

The two additional benchmark conditions have the following unstalled timings.
Both shapes perform 393,216 MACs; N=192 produces three times as many output
elements as N=64. Input traffic includes A rereads for successive N tiles.

| M×N×K | MXFP4 | FP32 | BF16/FP16 | E4M3/E5M2 |
| --- | ---: | ---: | ---: | ---: |
| 32×64×192 | 507 | 718 | 594 | 532 |
| 32×192×64 | 544 | 1200 | 816 | 624 |

Delayed, asynchronous, denied/retried, and failed memory requests are supported
and regression-tested. Each extra memory-service cycle stretches the remaining
schedule by one cycle. **Contention timing is an additive approximation**, not an
RTL calibration of stalls or FIFO overlap. The 256-bit port represents an
aggregate of eight banks; individual-bank contention is not modeled internally.

## Integration

```python
from pulp.mxcore_fp4 import MXCoreFP4

mx = MXCoreFP4(self, 'mxcore_fp4')
clock.o_CLOCK(mx.i_CLOCK())
mx.o_OUT(memory.i_INPUT())
# Map mx.i_INPUT() into the software MMIO space and bind mx.o_IRQ(...).
```

`input` and `out` use the `io_v2` **single-request** protocol. An output request
is one 32-byte transfer. Use a single-request-to-beat adapter when connecting to
a beat-protocol interconnect. `irq` is a `wire<bool>` output. This component is
available for composition; no existing chip's address map is changed.

## Memory layout and arithmetic

All pointers must be aligned to 32 bytes. Buffers have separate bases and need
not be adjacent. Output buffers must not overlap any other buffer. Input
buffers may alias. Matrices are tightly packed in the RTL's tiled traversal:

| Buffer | Order, from outermost to innermost | Bytes |
| --- | --- | --- |
| A | M tile, K block, row within tile, 32 K elements | M×K/2 |
| B | N tile, K block, column within tile, 32 K elements | N×K/2 |
| Scale A | M tile, K block, row within tile | M×K/32 |
| Scale B | N tile, K block, column within tile | N×K/32 |
| C | M tile, N tile, row within tile, 32 columns | M×N×output_bits/8 |
| Scale C (MXFP4 only) | M tile, N tile, row within tile | M×N/32 |

Every tile dimension and K block has 32 elements. Consecutive elements occupy
the low then high nibble of each byte. B is packed by columns, not as a plain
row-major array. [reference.py](../../tests/mxcore_fp4/reference.py) provides an
executable packing example.

E2M1 magnitudes are `{0, 0.5, 1, 1.5, 2, 3, 4, 6}`; bit 3 carries the sign.
An E8M0 byte `s` in 0–254 multiplies the values by `2^(s-127)`; 255 denotes a
NaN block. Each 32-term dot product is accumulated with one FP32 rounding,
then the next K block is accumulated in order. For an output block, the shared
exponent for MXFP4 is `floor(log2(max_abs))-2`, clamped to the E8M0 finite range. All-zero
blocks use scale 127. A block containing a nonfinite accumulator uses scale
255 and zero element payloads. The output quantizer preserves signs on finite
values rounded to zero. Directed RTL checks cover zero, ones, rounding ties,
saturation, wide exponents, subnormal accumulation, and NaN scales.

Native outputs store little-endian IEEE values in the same tile order, with no
scale buffer. FP32 stores the accumulator directly. Narrow outputs round once
from FP32, with ties to even, gradual underflow, signed zero, infinity on
overflow, and canonical quiet NaNs. **E4M3 is the IEEE-style encoding with
infinities/NaNs, not E4M3FN**; this matches the Arche3D SDK's existing FP8 API.
Native output NaNs affect individual elements; MXFP4 poisons the whole block.
RESULT_SCALE is ignored for native outputs and may be zero.

## Programming

[regs.h](regs.h) defines offsets and `mxcore_fp4_program()`. Registers use
32-bit little-endian accesses. The compute register layout follows MXCore;
the single-context job/IRQ interface below is the model's software interface.

1. Read ACQUIRE (0x04). Zero reserves the sole job; `0xffffffff` means unavailable.
2. Program the six pointers and M/N/K using `mxcore_fp4_program()`.
3. Make input writes visible, then write TRIGGER (0x00).
4. Wait for IRQ or FINISHED (0x08); check ERROR (0x64).
5. Write FINISHED to acknowledge completion and lower IRQ.

`mxcore_fp4_program()` retains MXFP4 output. To select another format after that
helper, set CTRL_ENGINE to `MXCORE_FP4_CONTROL_FORMAT(format)` and C_TILE_BITS
to `32*32*output_bits` before triggering. Format IDs in CTRL_ENGINE[25:23] are
0 MXFP4, 1 FP32, 2 BF16, 3 FP16, 4 E4M3, 5 E5M2. Bit 21 is set only for MXFP4;
the FP32 accumulation selector remains unchanged. All six pointers must still
be programmed, although RESULT_SCALE is unused for native outputs.

STATUS (0x0c) is busy. CYCLES (0x60) reports elapsed accelerator cycles, including
extra memory delay. ERROR is 0 for success, 1 for invalid configuration, and 2
for memory failure. CYCLES and ERROR are GVSoC diagnostics, absent in upstream
RTL. Unsupported accesses return `IO_RESP_INVALID` without starting a job.
Configuration writes and a second trigger are rejected while busy.

Additional read-only GVSoC diagnostics, stable after completion:

| Offset | Value |
| --- | --- |
| 0x68 | Nominal calibrated RTL cycles for the last job |
| 0x6c | Extra memory-service cycles added to the schedule |
| 0x70, 0x74 | Completed read bytes, completed write bytes |
| 0x78, 0x7c | Low/high words of trigger acceptance cycle in this clock domain |

For successful jobs, CYCLES = nominal cycles + memory-service cycles. These
counters do not measure PE active cycles. Useful-MAC utilization is
`M*N*K / (1024 * elapsed_cycles)`. Cluster utilization uses the sum of useful
MACs divided by the physical engine count times 1024 times the interval from
the earliest trigger to the latest completion, including idle engines.

SOFT_CLEAR (0x14) clears registers, completion, IRQ, and scheduled work. An
outstanding memory request is drained before another job can be acquired;
already-issued memory writes are not rolled back.
