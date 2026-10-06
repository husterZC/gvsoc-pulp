# Scalar and Spatz special functions

Arche3d supports **exp, sin, cos, sqrt and reciprocal** on its scalar Snitch
cores and attached Spatz units, for FP16, BF16, FP8 E5M2 and FP8 E4M3.
Each core's `fmode` CSR selects the formats:

| `fmode` | 16-bit elements | 8-bit elements |
| --- | --- | --- |
| 0 | FP16 E5M10 | FP8 E5M2 |
| 3 | BF16 E8M7 | FP8 E4M3 |

E4M3 has bias 7, subnormals, infinities and NaNs; it is **not E4M3FN**.
Its maximum finite magnitude is 240. The format switch does not convert bits
already in registers or memory.

## Operations and numerical behavior

| Function | Meaning | Scalar instruction | Vector instruction |
| --- | --- | --- | --- |
| exp | e raised to x | `fexp.h`, `fexp.b` | `vfexp.v` |
| sin | Sine of x in radians | `fsin.h`, `fsin.b` | `vfsin.v` |
| cos | Cosine of x in radians | `fcos.h`, `fcos.b` | `vfcos.v` |
| sqrt | Nonnegative square root | Existing `fsqrt.h`, `fsqrt.b` | Existing `vfsqrt.v` |
| reciprocal | Rounded 1/x | `frecip.h`, `frecip.b` | `vfrecip.v` |

The `.h`/`.b` suffix selects the operand width; `fmode` chooses the format at
that width. Vector functions use SEW=8 or SEW=16. The new vector instructions
reject other SEWs. Existing FP32/FP64 square root and the legacy `vfexp.vv`
continue to work through their previous handlers.

Reciprocal computes 1/x at the selected precision. It is a custom operation,
distinct from the standard RVV
[`vfrec7.v` seven-bit estimate](https://docs.riscv.org/reference/isa/unpriv/v-st-ext).
Square-root encodings and the legacy `vfexp.vv` encoding are preserved.
The legacy exp source is still in bits 19:15; the new unary vector encodings
place the source in bits 24:20.

Narrow operations implement RNE, RTZ, RDN, RUP and RMM destination rounding,
gradual underflow, and directed overflow. Scalar instructions have an `rm`
field; `rm=7` selects `frm`, as used by the SDK. Vector operations use `frm`.
Reserved rounding modes raise an illegal-instruction exception. Exception
flags accumulate in `fflags`: NX=1, UF=2, OF=4, DZ=8 and NV=16. Underflow is
detected after rounding and requires an inexact result.

Special cases include:

- exp(-infinity)=+0; exp(+infinity)=+infinity, without overflow flags.
- sin preserves signed zero; cos(±0)=1; sin/cos of infinity raise NV.
- sqrt preserves signed zero; negative nonzero inputs raise NV.
- Reciprocal of ±0 returns ±infinity and raises DZ; reciprocal of ±infinity
  returns ±0.
- Quiet NaNs return a canonical quiet NaN; signaling NaNs additionally raise NV.
- Masked-off vector elements and elements outside the active range neither
  execute nor raise exceptions; their destination bits are preserved.

Transcendentals use host binary64 `libm`, then round to the selected narrow
format. They are functional simulator operations, not a particular RTL
polynomial or lookup-table approximation. The regression compares results
against an independent 200-digit Decimal reference. This is not a guarantee
of correctly rounded transcendental results for every possible FP16/BF16
input or a match to an unspecified hardware SFU.

## Instruction allocation and timing

New operations use the RISC-V **custom-2 major opcode (`0x5b`)**, following
the [standard opcode allocation guidance](https://docs.riscv.org/reference/isa/unpriv/rv-32-64g.html).
Their names are GVSoC decoder labels; the stock assembler does not recognize
the new mnemonics. SDK intrinsics and assembly macros emit the encodings.

For scalar operations, `rd` and `rs1` name floating-point registers:

| Field | Value |
| --- | --- |
| 31:25 | `2 * operation + width`, where exp=0, sin=1, cos=2, reciprocal=3; width=0 for 8 bits, 1 for 16 bits |
| 24:20 | 0 |
| 19:15 | Source FPR |
| 14:12 | Rounding mode |
| 11:7 | Destination FPR |
| 6:0 | `0x5b` |

For vector operations:

| Field | Value |
| --- | --- |
| 31:26 | `32 + operation`, using the same operation numbers |
| 25 | `vm`: 1=unmasked, 0=use v0 mask |
| 24:20 | Source vector register |
| 19:15 | 0 |
| 14:12 | 1 |
| 11:7 | Destination vector register |
| 6:0 | `0x5b` |

Scalar and vector operations use these **base instruction latency assumptions**:

| Function | Cycles |
| --- | ---: |
| exp | 5 |
| sin | 11 |
| cos | 11 |
| sqrt | 5 |
| reciprocal | 5 |

The table is defined by `SPECIAL_FUNCTION_LATENCIES` in
`logic/snitch/special_functions.py`. Scalar destination readiness uses the
same latency, including FP16, BF16 and FP8 sqrt. Legacy vector exp also uses
5 cycles. Spatz adds its existing element-count/compute-width duration and
queue/dependency scheduling. The shared `vfsqrt.v` opcode uses this base
latency at every SEW; its FP32/FP64 arithmetic handlers are unchanged.
These latencies are not calibrated against an RTL SFU and should not be used
as a measured SFU performance prediction.

The implementation stays under `pulp/chips/arche3d/logic/snitch/`. It adds no
global instruction extension to other targets. Reads/writes of `fflags`,
`frm` and `fcsr` wait for older Spatz work, as do writes to `fmode`; a CSR
change cannot reinterpret a queued instruction or clear its flags too early.

## Software and regression

Include `arche3d.h`. Scalar calls take and return raw `uint8_t`/`uint16_t`
encodings, avoiding compiler-specific FP8/BF16 types. For example:

```c
arche3d_fp_format_set(ARCHE3D_FP_FORMAT_BF16_E4M3);
uint16_t result = arche3d_scalar_exp_fp16(0x0000); // BF16 1.0: 0x3f80

// On a core with Spatz:
uint8_t input[] = {0x00, 0x38}; // E4M3 0.0, 1.0
uint8_t output[2];
arche3d_vector_exp_fp8(input, output, 2);
```

Replace `exp` with `sin`, `cos`, `sqrt` or `reciprocal`. Vector calls strip
mine arbitrary element counts, support in-place operation, and wait for
their stores before returning. Zero count does nothing. For assembly kernels,
include `arche3d_fp_asm.inc` and use `ARCHE3D_VFEXP`, `ARCHE3D_VFSIN`,
`ARCHE3D_VFCOS` or `ARCHE3D_VFRECIP` with destination/source register numbers
and optional `vm`. Use standard `vfsqrt.v` for square root.

After the normal GVSoC environment and DRAMSys setup, from the GVSoC root:

```bash
make TARGETS=arche3d_dma_test MODULES="$PWD/pulp/tests/arche3d" build
make cfg=default app=fp_special arche3d-sw
gvrun --target=arche3d_dma_test --target-dir=pulp/tests/arche3d \
    --parameter=clusters=4 \
    --binary=build/arche3d/sw/default/fp_special/fp_special.elf \
    --work-dir=build/runs/arche3d_fp_special run
python pulp/tests/arche3d/test_special_functions.py
```

Expected: `ARCHE3D_RESULT` with `status: PASS`, followed by a passing opcode
test. The software runs scalar checks on all six cores of each cluster and
vector checks on every configured Spatz core. Each format is visited in a
different order per core. It covers all 256 encodings of both FP8 formats,
128 boundary/sample cases each for FP16/BF16, all five functions, raw result
bits and exception flags, all five reciprocal rounding modes, overflow and
underflow rounding, sticky flags, vector masks/tails, in-place calls,
zero-length calls, the legacy exp encoding, and CSR ordering under queued
vector work. The opcode test checks all new encodings against the complete
arche3d scalar/vector ISA and checks that existing encodings were preserved.

Reference vectors are checked in with the SDK. Regenerate without optional
Python dependencies using:

```bash
python arche3d_sdk/utilities/generate_fp_special_vectors.py
make -C arche3d_sdk lint
```
