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
