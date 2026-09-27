# arche3d data-NoC collective regression

This target instantiates the arche3d 4x4 data NoC, its production IO bridges,
and synthetic L1 endpoints. It requires no RISC-V software or DRAMSys.

After the normal GVSoC environment setup, from the GVSoC root:

```bash
make TARGETS=arche3d_collective_test MODULES="$PWD/pulp/tests/arche3d_collectives" build
gvrun --target=arche3d_collective_test --target-dir=pulp/tests/arche3d_collectives \
    --work-dir=build/runs/arche3d_collectives run
```

Expected: `ARCHE3D_COLLECTIVE_PASS jobs=59`, including integer and all four
floating-point sum/max formats, broadcast, match masks, byte-aligned FP8,
concurrent sources, delayed/denied targets, reordered replies and errors.
Default NI capacity is 2 and router queue depth is 1. Set
`COLLECTIVE_NI_CAPACITY` or `COLLECTIVE_ROUTER_CAPACITY` to override them.

This checks routing and completion under contention. Numerical coverage and
a software test using real cores, DMA and RedMule are described in
[float_math.md](../../pulp/chips/arche3d/doc/float_math.md).
