# Native arche3d collective regression

This target instantiates production `floonoc_v2` routers, native two-sided
collective endpoints, unicast IO bridges and synthetic L1 memories. It runs
without RISC-V cores or DRAMSys. Dimensions are configurable from 1 to 32 on
each axis; the default is 4 x 4.

From the GVSoC root after environment setup:

```bash
make TARGETS=arche3d_collective_test MODULES="$PWD/pulp/tests/arche3d_collectives" build
gvrun --target=arche3d_collective_test --target-dir=pulp/tests/arche3d_collectives \
    --parameter=nx=32 --parameter=ny=32 \
    --work-dir=build/runs/collective_native_32x32 run
```

Expected: `ARCHE3D_NATIVE_COLLECTIVE_RESULT PASS nx=32 ny=32 phases=85`.
The 85 phases cover edge/middle multicast and reduction on both axes,
all integer/floating sum/max formats, odd FP8 addresses, full-flit payloads,
16 concurrent slots on intersecting rows/columns, repeated epochs,
late receive posting, reuse of captured send buffers, a late contributor,
ordinary unicast contention, and asynchronously denied local-memory accesses.
Masked phases check X/Y coordinate-bit selection semantics and
check sparse rows/columns, sparse two-dimensional groups, blocks, root-only and
whole-mesh operations. Excluded clusters never submit descriptors. Sixteen
concurrent masked groups also exercise overlapping routing trees.
Every destination and surrounding guard bytes are checked against independent
goldens. Isolated latency is asserted, not merely printed.

Bulk phases add 128/256/129/2049/8192/8193-byte streams on both axes, exact
partial tails, all reduction formats across multiple beats, 16 simultaneous
streams, intersecting masked streams, delayed receive posting beyond buffer
capacity, source reuse after whole-send completion, and a delayed contributor.
Both PENDING and DENIED asynchronous L1 responses are exercised, including
responses out of order and concurrent ordinary unicast traffic. All 85 phases
run with shallow queues as well as production capacities.

Default NI capacity is 2 and router input queue depth is 1 to stress backpressure.
Use `COLLECTIVE_NI_CAPACITY=64 COLLECTIVE_ROUTER_CAPACITY=2` for production
capacities; this also asserts first-capture-to-last-acceptance latency of
`2 * max_hops + 3 + ceil(bytes / 128) - 1` for the isolated bulk phases.
Both configurations retain the two-cycle router pipeline.
Set `--parameter=l1_base=65536` to also check a relocated local L1 address range.
For a 32-member row/column, the middle-root multicast and reduction each take
35 cycles from local capture to receiver acceptance: one NI injection cycle,
two local-router cycles, and 32 cycles across the 16-hop distance. Edge roots
take 65 cycles over 31 hops. This excludes endpoint memory writes and software.

The SDK `collective` app checks independent hardware row/column barriers and
16 simultaneous 512-byte streams on complete logic tiles, followed by ordinary remote
XDMA reads and writes to check the retained instruction frontend. `collective_latency` reports
software completion, scoped-barrier completion and optional NI timing.
The SDK [row size sweep](../../../arche3d_sdk/apps/collective_row_sweep/README.md)
extends software measurement to 1–8192 bytes and concurrent 32/16/8-member
groups, using one bulk descriptor per buffer. Its traces check every beat
and retain per-sender injection intervals. It runs on
the default production system or the explicitly labelled 32x1 software fixture.
`fp_alignment` checks the native reductions against scalar/vector arithmetic;
see [float_math.md](../../pulp/chips/arche3d/doc/float_math.md).
