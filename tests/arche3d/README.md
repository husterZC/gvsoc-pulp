# arche3d DMA software fixture

This fixture instantiates one complete production logic tile, its I3D DMA,
a one-port crossbar, and one DRAMSys channel. It permits short software checks
without elaborating all 1,024 tiles. The production `arche3d` target still only
supports the supplied 32 × 32 geometry.

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
The full `alltoall` application must run on the production target, since its
workload covers all 1,024 endpoints.
