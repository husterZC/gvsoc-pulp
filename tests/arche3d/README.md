# arche3d software fixture

This fixture instantiates one, two, or four complete production logic tiles, their
I3D DMAs, a crossbar, DRAMSys channels, and the production data and sync NoCs.
It defaults to one tile and permits short software checks without elaborating
all 1,024 tiles. The production `arche3d` target still only supports the
supplied 32 × 32 geometry.

After the standard GVSoC environment setup, from the top-level repository:

```bash
make TARGETS=arche3d_dma_test MODULES="$PWD/pulp/tests/arche3d" build
make cfg=default app=smoke arche3d-sw
gvrun --target=arche3d_dma_test --target-dir=pulp/tests/arche3d \
    --binary=build/arche3d/sw/default/smoke/smoke.elf \
    --work-dir=build/runs/arche3d_dma_smoke run
```

Use `app=reject_collective` and its ELF for the negative test. It must exit
nonzero and print `Collective DMA is disabled for this iDMA instance`.
Use `app=queue` to stress full descriptor/burst queues and AXI ID reuse; its
final empty gather must retire only after all older copies complete.
`alltoall` reads the instantiated geometry: use four tiles for a short check,
or the production target for the full 1,024-endpoint benchmark. All apps use
the data stripe so DMA writes cannot overwrite the program image.

To check TCDM stacks and remote scalar L1 access, use four tiles (2 × 2):

```bash
make cfg=default app=memory arche3d-sw
gvrun --target=arche3d_dma_test --target-dir=pulp/tests/arche3d \
    --parameter=clusters=4 \
    --binary=build/arche3d/sw/default/memory/memory.elf \
    --work-dir=build/runs/arche3d_memory run
```

Every core checks its stack bounds and alignment, fills half its stack, and
allows core 0 to read that data through the shared L1 map. The test also checks
data/BSS initialization. Each tile then reads/writes a 2-KiB array in the next
tile's L1 and performs a remote atomic addition through the sync NoC. The local
pointer sees the same data. No DMA or separate synchronization memory is used.

Build `app=wakeup` and select its ELF with the same four-tile target to test
broadcasts, selected rows, a remote corner, reverse routing, two early
notifications, a blocked receiver, and simultaneous multicasts. A remote L1
write followed by a wakeup verifies data visibility when the receiver resumes.
The control-register barrier only separates test phases.

The full 32 × 32 sync NoC also has a lightweight test without software cores or
DRAMSys. It checks bit 31 of both destination masks, exact delivery counts,
multiple concurrent senders, remote reads/writes, and router queues of depth 1:

```bash
make TARGETS=arche3d_sync_test MODULES="$PWD/pulp/tests/arche3d" build
gvrun --target=arche3d_sync_test --target-dir=pulp/tests/arche3d \
    --work-dir=build/runs/arche3d_sync run
```

This ends with `ARCHE3D_SYNC_RESULT`. The synthetic endpoints exercise NoC
routing and backpressure; `memory` and `wakeup` separately test the real logic
tiles, L1 banks, and cluster wakeup controls.

## Floating-point format regression

The `fp_formats` application uses every configured Spatz core in the production
tile. It checks reset/readback of `fmode`, independent per-core selections,
FP16/BF16/E5M2/E4M3 addition, FMA, ordered reductions, widening addition, and
narrowing conversion. Raw expected encodings also cover cancellation to zero,
subnormal arithmetic, overflow, infinity and NaN. FP32/FP64 are checked in both
modes.

```bash
make TARGETS=arche3d_dma_test MODULES="$PWD/pulp/tests/arche3d" build
make cfg=default app=fp_formats arche3d-sw
gvrun --target=arche3d_dma_test --target-dir=pulp/tests/arche3d \
    --binary=build/arche3d/sw/default/fp_formats/fp_formats.elf \
    --work-dir=build/runs/arche3d_fp_formats run
```

Expected result: `ARCHE3D_RESULT` with `status: PASS`. The assembly uses standard
RVV mnemonics with `.option arch, +v`; no patched compiler or new arithmetic
encoding is required. It issues arithmetic immediately before all six CSR
write forms, checks the previous value returned by each CSR operation, and
changes formats without another `vsetvli`. The same raw inputs must produce
the old format's result before the switch and the new format's result after it.
VL and VTYPE must be preserved.

Validated on 2026-09-27 with the production `arche3d`, fixture, and ordinary
`spatz` targets built together. The one-tile and four-tile format runs pass;
the latter exercises 16 Spatz cores and completes in 17,029 total cycles.
SDK format/lint checks also pass. Companion regressions:

| Application | Tiles | Result |
| --- | ---: | --- |
| `fp_formats` | 4 | PASS; all four formats, CSR ordering, and FP32/FP64 |
| `smoke` | 1 | PASS; 38 DMA bursts, 19,463 bytes |
| `memory` | 4 | PASS; per-core stacks and remote L1 |
| `alltoall` | 4 | PASS; 16 bursts, 16,384 bytes |

These vector checks also caught an arche3d port-width unit mismatch: the
architecture's 32-bit VLSU width must become **4 bytes** in the VLSU/compute
model, while VLEN remains expressed in bits. Passing 32 bytes bypassed the
word-interleaved L1 bank mapping for the later words of a vector transfer.
The correction fixes those accesses and their modeled bandwidth; it does not
change floating-point instruction encodings. Other architectures' port
configuration is unchanged. Run logs and `verification.json` are under
`build/arche3d/validation/fp_formats/` and are not tracked.

## Special-function regression

Build `app=fp_special` and run its ELF with the same one/four-tile fixture.
It checks scalar and Spatz exp, sin, cos, sqrt and reciprocal for FP16, BF16,
E5M2 and E4M3, against independent 200-digit Decimal reference vectors.
Every FP8 bit pattern is covered, including NaNs; the 16-bit formats use
boundary cases and deterministic samples. The test also checks exception
flags, rounding modes, vector masks/tails, in-place and zero-length calls,
legacy exp decoding, and CSR ordering while vector work is queued.

The CSR ordering test originally failed: a pending vector operation could
observe a later `frm` write, and `fflags` could be read before completion.
Arche3d now drains older vector instructions when accessing `fflags`, `frm`
or `fcsr`, in addition to the existing drain on writes to `fmode`.

See [special functions](../../pulp/chips/arche3d/doc/special_functions.md)
for the API, timing/accuracy limits and complete build/run instructions.
`python pulp/tests/arche3d/test_special_functions.py` from the GVSoC root
checks opcode collisions and preservation of existing encodings.

## Cross-unit floating-point regression

Build `app=fp_alignment` and run on **four tiles**. It compares real scalar,
Spatz and logic-DMA/NoC sum/max results in all four formats and checks the
RedMule FP32 accumulator, including retained small contributions, BF16 range,
per-MAC rounding, and internal tile boundaries. The standalone
`test_float_math.py` provides an independent exact arithmetic oracle.
See [the arithmetic contract and complete commands](../../pulp/chips/arche3d/doc/float_math.md).

## Shared instruction-cache regression

The fixture uses the production single ELF loader, shared instruction caches,
I3D source arbitration, and direct cache initialization. Existing `memory` and `wakeup`
images fit in the cache and should report `icache_runtime_refills: 0`.

The `icache` application places six different functions beyond the first 32 KiB
and calls them from all cores while each cluster's DMA transfers data. It also
checks initialized L1 data copied from the single shared DRAM image:

```bash
make cfg=default app=icache arche3d-sw
gvrun --target=arche3d_dma_test --target-dir=pulp/tests/arche3d \
    --parameter=clusters=4 \
    --binary=build/arche3d/sw/default/icache/icache.elf \
    --work-dir=build/runs/arche3d_icache run
```

Use four tiles: the deliberately padded test ELF spans multiple 32-KiB program
stripes. This should report runtime refills and complete all DMA transfers.
`ARCHE3D_BOOT` must precede software execution and the final `ARCHE3D_RESULT`.
Preheating must report `preheat_mode: direct` and `preheat_cycles: 0`.
`icache_preloaded_lines` counts directly initialized lines; `icache_refills`
counts only actual downstream reads.

A focused cache test checks an immediate hit on a directly initialized line
with no downstream request, then supplies a 100-cycle refill delay and checks that resident
hits still complete immediately, that an evicted line cannot expose the incoming
line's data, that flush forces a refill, and that writes/straddling reads fail:

```bash
make TARGETS=arche3d_icache_test MODULES="$PWD/pulp/tests/arche3d" build
gvrun --target=arche3d_icache_test --target-dir=pulp/tests/arche3d \
    --work-dir=build/runs/arche3d_icache_ports run
```

Expected result: `ARCHE3D_ICACHE_RESULT` with `status: PASS`, one preloaded line,
four refills, two hits during refills, and 406 cycles.
