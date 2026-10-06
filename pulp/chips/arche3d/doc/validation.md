# Arche3D validation

This document describes checks for the current model. The
[architecture guide](../README.md) defines the configuration and memory map;
the [SDK programming guide](../../../../../arche3d_sdk/docs/bare_metal_programming.md)
defines software ownership, completion and synchronization. Implementation
changes are recorded in [CHANGELOG.md](../CHANGELOG.md).

Keep a result with its configuration, ELF/source hashes, simulator completion
and measurement boundaries. Replace superseded measurements when the design
changes. Generated logs and reports belong in the selected `build/` run
directory.

## Native collective regression

The [native test](../../../../tests/arche3d_collectives/README.md) instantiates
the production endpoints, routers and unicast bridges with synthetic L1
memories. It exercises:

- Row and column multicast and every integer/floating sum/max operation.
- X/Y coordinate-match masks, including sparse and two-dimensional groups.
- Bulk streams through 8193 bytes, exact tails and byte-aligned FP8 buffers.
- Sixteen concurrent streams, repeated epochs and intersecting routing trees.
- Delayed receive posting, source reuse and a delayed reduction contributor.
- Finite queues, ordinary unicast contention, nonzero local L1 bases and
  out-of-order PENDING/DENIED memory responses.

After environment setup, build and run from the GVSoC repository root:

```bash
make TARGETS='arche3d_dma_test arche3d_collective_test' \
    MODULES="$PWD/pulp/tests/arche3d;$PWD/pulp/tests/arche3d_collectives" build
COLLECTIVE_NI_CAPACITY=64 COLLECTIVE_ROUTER_CAPACITY=2 \
    gvrun --target=arche3d_collective_test --target-dir=pulp/tests/arche3d_collectives \
    --parameter=nx=32 --parameter=ny=32 \
    --work-dir=build/runs/collective_native_32x32 run
```

Expected: `ARCHE3D_NATIVE_COLLECTIVE_RESULT PASS nx=32 ny=32 phases=85`.
The target defaults to a 4×4 mesh, NI capacity 2 and router queue depth 1 for
backpressure tests. Run 32×1 and 1×32 to exercise single-axis meshes; use
`--parameter=l1_base=65536` to check local address translation.

With production queue capacities, the isolated bulk phases assert
`2 * max_hops + 3 + ceil(bytes / 128) - 1` cycles from first local NI capture
to final endpoint acceptance. The NI injection-to-arrival interval excludes
that first capture-to-injection cycle. Endpoint writes and software/barrier
costs are separate. These are architectural timing checks of the modeled
two-cycle pipeline.

## Software and performance checks

The [software fixture](../../../../tests/arche3d/README.md) uses complete
production logic tiles. Select `--parameter=clusters=4` for the following
collective checks:

| Application | Coverage |
| --- | --- |
| `collective` | Sixteen concurrent 512-byte streams, masked groups, scoped barriers, late posting, source reuse and ordinary DMA |
| `collective_latency` | One-byte row/column multicast and FP8 reduction, edge/middle roots, software and NI timing |
| `fp_alignment` | Scalar/vector/NoC/RedMule arithmetic agreement and FP32 RedMule accumulation |
| `reject_collective` | Out-of-range native root; expected failure before injection |

The [row size sweep](../../../../../arche3d_sdk/apps/collective_row_sweep/README.md)
checks all nine requested sizes from 1 to 8192 bytes, multicast and E4M3 sum,
and concurrent groups of 32, 16 or 8 clusters. One descriptor/epoch covers
each whole buffer. The reporter verifies all 54 cases and 126 group records,
exact beat offsets and lengths, selected sender/receiver sets, completion and
configuration. It reports setup-inclusive, prepared-launch and NI intervals
separately. Current measured tables and reproduction commands are maintained
in that benchmark's [current measurements](../../../../../arche3d_sdk/apps/collective_row_sweep/README.md#current-measurements).

Full 32×32 validation uses the production logic/NoC configuration with the
explicit RAM-backed configuration supplied by the benchmark. It retains the
128-byte link width, NI capacity 64, router queue depth 2 and two-cycle hops.
The 32×1 RAM fixture provides a smaller software check. RAM-backed results
are identified separately from a full DRAMSys run; the reporter requires
explicit `--allow-ram` and, for the row fixture, `--allow-fixture`.

## Other model checks

Use the software fixture instructions for `smoke`, `queue`, `memory`, `wakeup`,
`icache`, `fp_formats` and `fp_special`. These cover DMA/gather retirement,
local and remote memory, stacks, wakeup bitmaps, instruction-cache behavior,
floating-point format selection and special functions. The cache component
has an additional focused test for hits during a refill, victim-tag handling,
flushes and invalid requests.

[float_math.md](float_math.md) describes the independent arithmetic oracle and
cross-unit checks. [special_functions.md](special_functions.md) describes
reference-vector generation, rounding/exception checks and opcode validation.
The SDK [kernel benchmark guide](../../../../../arche3d_sdk/kernelbench/README.md)
contains workload-specific validation and reporting instructions.

The rotating `alltoall` application measures ordinary I3D reads. Its passive
checker verifies the expected traffic and completion. Record memory backend,
geometry, request/burst settings and cache activity with every measurement;
there is no hardcoded latency target.

Run `make -C arche3d_sdk lint` after SDK changes. Model/API changes and their
validation belong in the relevant changelog; update both changelogs when the
hardware/software contract changes.
