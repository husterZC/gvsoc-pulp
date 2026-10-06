# arche3d software fixture

This fixture instantiates one, two, four (2x2), or 32 (32x1) complete production logic tiles, their
I3D DMAs, a crossbar, configurable memory endpoints, and the production data and sync NoCs.
It defaults to one tile and permits short software checks without elaborating
all 1,024 tiles. The production `arche3d` target still only supports the
supplied 32 × 32 geometry.

Memory defaults to DRAMSys. To select the built-in RAM endpoint, set
`dram3d_backend='memory'` in the selected architecture configuration and adjust
`dram3d_ram_slots` as needed. Select the same configuration with `cfg` when
building and the `config` parameter when running, as in the production target. The fixture
uses the configured memory parameters while retaining its small crossbar geometry.

After the standard GVSoC environment setup, from the top-level repository:

```bash
make TARGETS=arche3d_dma_test MODULES="$PWD/pulp/tests/arche3d" build
make cfg=default app=smoke arche3d-sw
gvrun --target=arche3d_dma_test --target-dir=pulp/tests/arche3d \
    --binary=build/arche3d/sw/default/smoke/smoke.elf \
    --work-dir=build/runs/arche3d_dma_smoke run
```

The supplied [RAM configuration](../../../arche3d_sdk/apps/collective_row_sweep/ram_config.py)
inherits the default architecture and changes only the memory backend. The
equivalent RAM check needs no DRAMSys/SystemC setup and uses the same default
software layout:

```bash
ARCHE3D_RAM_CONFIG="$PWD/arche3d_sdk/apps/collective_row_sweep/ram_config.py"
make cfg="$ARCHE3D_RAM_CONFIG" TARGETS=arche3d_dma_test MODULES="$PWD/pulp/tests/arche3d" build
make cfg=default app=smoke arche3d-sw
gvrun --target=arche3d_dma_test --target-dir=pulp/tests/arche3d \
    --parameter=config="$ARCHE3D_RAM_CONFIG" \
    --binary=build/arche3d/sw/default/smoke/smoke.elf \
    --work-dir=build/runs/arche3d_ram_smoke run
```

Use the same RAM configuration for `queue`, `memory`, `wakeup`, and `icache`;
select four tiles for the multi-tile tests below. The `icache` check exercises
timed refills from preloaded RAM while DMA traffic is active. Report the memory
backend with timing results. Backend selection and legacy
configuration compatibility can also be checked without a simulator installation:

```bash
python pulp/tests/arche3d/test_memory_backend.py
```

Use `app=reject_collective` and its ELF for the negative test. It must exit
nonzero and print `Invalid collective descriptor` for the out-of-range root.
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

Select four tiles to exercise 16 Spatz cores. The architecture's 32-bit VLSU
width is expressed as **4 bytes** in the VLSU/compute model, while VLEN remains
in bits. Vector accesses must respect the word-interleaved L1 bank mapping.
Keep run logs and timing results with the selected configuration in `build/`.

## Special-function regression

Build `app=fp_special` and run its ELF with the same one/four-tile fixture.
It checks scalar and Spatz exp, sin, cos, sqrt and reciprocal for FP16, BF16,
E5M2 and E4M3, against independent 200-digit Decimal reference vectors.
Every FP8 bit pattern is covered, including NaNs; the 16-bit formats use
boundary cases and deterministic samples. The test also checks exception
flags, rounding modes, vector masks/tails, in-place and zero-length calls,
legacy exp decoding, and CSR ordering while vector work is queued.

Arche3d drains older vector instructions when accessing `fflags`, `frm` or
`fcsr`, and when writing `fmode`. The ordering checks ensure pending operations
use the intended rounding mode and that flags are read after completion.

See [special functions](../../pulp/chips/arche3d/doc/special_functions.md)
for the API, timing/accuracy limits and complete build/run instructions.
`python pulp/tests/arche3d/test_special_functions.py` from the GVSoC root
checks opcode collisions and preservation of existing encodings.

## Cross-unit floating-point regression

Build `app=fp_alignment` and run on **four tiles**. It compares real scalar,
Spatz and native data-NoC sum/max results in all four formats and checks the
RedMule FP32 accumulator, including retained small contributions, BF16 range,
per-MAC rounding, and internal tile boundaries. The standalone
`test_float_math.py` provides an independent exact arithmetic oracle.
See [the arithmetic contract and complete commands](../../pulp/chips/arche3d/doc/float_math.md).

## Shared instruction-cache regression

The fixture uses production direct ELF population of HBM, shared instruction
caches, I3D source arbitration, and direct cache initialization. Existing `memory` and `wakeup`
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
Loading must report `image_load_mode: direct`, `image_loaded_cycle: 0`, and
`cores_start_cycle: 1`. Preheating must report `preheat_mode: direct` and
`preheat_cycles: 0`.
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
four refills and two hits during refills. The test checks each access against
its configured latency; total run time belongs with the run artifacts.

## Native collectives and scoped barriers

Build `app=collective` and run the four-tile fixture. It checks independent row
and column barriers (including groups that never enter other groups' barriers),
unequal generation counts across groups, 16 concurrent 512-byte streams, late multicast
receives, source reuse after local capture and repeated epochs. It also checks
X/Y coordinate-match masks, barriers entered only by selected members, and
ordinary logic-die DMA reads/writes after collective activity. Expected:
`ARCHE3D_POSTED_COLLECTIVE_PASS` followed by `ARCHE3D_RESULT` PASS.

`app=collective_latency` checks one-byte multicast and E4M3 reduction. Use
`--parameter=progress_cycles=0` when collecting its JSON records. The SDK
benchmark README describes NI traces and reporting. The NoC-only
`arche3d_collective_test` checks the full 32x32 fabric and two-cycle-hop timing.

`app=collective_row_sweep` uses `--parameter=clusters=32` to test the complete
size/partition sweep on one full row: 1–8192-byte midpoint multicast/E4M3 sum
with one 32-member, two concurrent 16-member, or four concurrent 8-member groups.
The reporter requires `--allow-fixture` and labels this 32x1 result explicitly.
Use the production `arche3d` target for default 32x32 measurements. See the
[benchmark instructions](../../../arche3d_sdk/apps/collective_row_sweep/README.md)
for bulk descriptors, timing boundaries, full-size commands and validation.
