# arche3d validation

Validated on 2026-09-26 using the conventional GVSoC build/run flow in the
[architecture guide](../README.md). The recorded 32 × 32 software all-to-all
benchmark passes in **45,578.5 DMA cycles**, **0.07465%** above the recorded
45,544.5-cycle network-only reference. Caches are initialized directly at time
zero; cores start at cycle 62 and incur no runtime instruction-cache refills
in this application. The measured configuration used TCDM stacks, remote L1 and
multicast wakeup on the sync NoC, and 64 KiB per DRAM channel.

The default now uses `dram3d_vault_space=0x8000000` (128 MiB per vault)
and `dram3d_vault_interleave=0x8000` (32 KiB). Measurements below retain
their original capacities; they are not a new full-chip run with the larger map.

Both GVSoC and DRAMSys were built in `Release` mode. The host was an AMD Ryzen 7
5800X; wall times describe this machine and are not simulated hardware timing.

## Current full benchmark with direct cache initialization

The production target has 1,024 clusters / 6,144 cores, the level-3 Adaptive NCA
fat tree, eight source and memory contexts, and 1,024 HBM4 DRAMSys channels.
Every cluster reads one B16 burst from every endpoint, reusing its L1 buffer.
The clock is 1 GHz, AXI data width is 512 bits, and channel space/interleaving
is 64 KiB / 32 KiB. All reads use the second stripe, at endpoint-local offset
`0x8000`, so the program image in the first stripe remains intact.

| Metric | Current measurement |
| --- | ---: |
| DMA interval (`network_cycles`) | **45,578.5 cycles** |
| Difference from 45,544.5-cycle reference | **+34 cycles / +0.07465%** |
| DMA submission interval (`injection_cycles`) | 34,275.5 cycles |
| Software interval after start barrier | 45,628 cycles |
| Direct preheating | **0 cycles / 0 DRAM reads** |
| Program load completion / core release | 61 / 62 cycles |
| Total, including boot and runtime initialization | 46,524 cycles |
| Directly initialized cache lines | 9,216 (9 per cluster) |
| Runtime instruction-cache refills | **0** |
| Completed B16 reads / checked data | 1,048,576 / 1 GiB |
| Peak queued plus in-flight DMA requests | 262,102 |
| Simulation wall time | 923.565545 s (15 min 24 s) |
| Whole-command wall time | 1,277.01 s (21 min 17 s) |
| Maximum resident memory | 17.894 GiB |

**PASS, exit status 0.** Each of the 1,024 endpoints reports exactly 1,024 reads,
1 MiB returned, 32,768 native DRAM reads, and zero pending requests. All 1 GiB
of response data passed the passive checker. The single loader writes 524 bytes
to channel 0; there are no other writes. No cache-preheat reads appear in the
endpoint totals.

The DMA counters are measured at the DMA-facing side of the shared I3D source
adapter. They include queued requests, so the submission interval and peak count
are not directly comparable with the earlier direct-to-fabric source counters.
The physical source-context limit remains eight per cluster. The DMA interval
still covers completion of the entire all-to-all workload.

The DRAMSys simulation, memspec, controller, address-mapping and simulator-option
files are hash-identical to the reference. The new cache/data layout moves DMA
reads from endpoint-local offset zero to `0x8000`, and direct preheating does not
warm DRAM timing state. The comparison therefore measures the updated architecture,
not an identical initial DRAM state. No model parameter forces the cycle result.

After environment setup and `make dramsys_preparation`:

```bash
make cfg=default TARGETS=arche3d build
make cfg=default app=alltoall arche3d-sw
mkdir -p build/arche3d/validation/direct_preheat
/usr/bin/time -v gvrun --target=arche3d --parameter=config=default \
    --parameter=memory_init=pattern \
    --binary=build/arche3d/sw/default/alltoall/alltoall.elf \
    --work-dir=build/runs/arche3d_direct_preheat_alltoall_32x32 run \
    > build/arche3d/validation/direct_preheat/alltoall_32x32.log 2>&1
```

The observed result was:

```text
ARCHE3D_RESULT {"status":"PASS","clusters":1024,"transactions":1048576,"bytes":1073741824,"peak_outstanding":262102,"network_cycles":45578.5,"injection_cycles":34275.5,"software_cycles":45628,"boot_cycles":62,"total_cycles":46524,"icache_preloaded_lines":9216,"icache_refills":0,"icache_runtime_refills":0,"wall_seconds":923.565545}
```

The log, `alltoall_32x32_config_check.json`, and
`alltoall_32x32_verification.json` are under
`build/arche3d/validation/direct_preheat/`. The configuration audit checks all
1,024 preload inputs, the single system loader/snapshot, absence of timed-preheat
components, the production parameters, and unchanged DRAMSys configuration hashes.
Generated artifacts stay under `build/`; this directory contains only Markdown.

## Initial full software all-to-all benchmark (historical)

The following measurement predates TCDM stacks, the remote-L1/wakeup changes,
and shared instruction caches. It used separate stack and instruction memories
and 192-byte synchronization windows. It is retained as an earlier comparison.

The production target instantiates 1,024 SoftHier logic tiles, 6,144 cores,
1,024 dedicated I3D DMAs, the level-3 Adaptive NCA fat tree, and 1,024 DRAMSys
channels. Core 4 of each tile runs the compiled RISC-V application. There is no
synthetic traffic generator in this target.

| Setting | Value |
| --- | --- |
| Clock | 1 GHz |
| AXI address / data / ID / LEN widths | 64 / 512 / 10 / 8 bits |
| Source / memory contexts | 8 / 8 |
| DMA descriptor / burst capacity | 64 / 256 per tile |
| Burst | 16 beats = 1,024 bytes |
| DRAM base | `0x100000000` |
| Per-channel space / interleave | 32,768 / 32,768 bytes |
| DRAMSys configuration | `hbm4-emu-fast.json` |
| Extra endpoint read-slot limit | None |
| Workload | Each source reads each endpoint once, rotating from its own terminal |
| Total | 1,048,576 reads; 1,073,741,824 bytes |

After the environment and DRAMSys setup:

```bash
make cfg=default TARGETS=arche3d build
make cfg=default app=alltoall arche3d-sw
mkdir -p build/arche3d/validation
/usr/bin/time -v gvrun --target=arche3d --parameter=config=default \
    --parameter=memory_init=pattern \
    --binary=build/arche3d/sw/default/alltoall/alltoall.elf \
    --work-dir=build/runs/arche3d_alltoall run \
    > build/arche3d/validation/alltoall.log 2>&1
```

The control model checks the destination sequence and burst length for every
source, uniqueness of live AXI IDs, and completion of all expected reads before
software exits. `memory_init=pattern` additionally checks every returned byte
against the endpoint/address pattern used by `network3d_hbm4`. It is intended
for this read-only benchmark; applications that write DRAM use the default
zero initialization.

`network_cycles` spans first AXI acceptance to last AXI response using the
earlier benchmark's half-cycle convention. `software_cycles` spans release of
the global start barrier to the last cluster's exit, including DMA programming,
L1 completion, polling, and the final local-core barrier. `total_cycles` also
includes boot and BSS initialization. No model parameter forces a particular
benchmark duration.

The historical comparison is the recorded `network3d_hbm4` B16/X8/Adaptive-NCA
run: **45,544.5 cycles**, 8,192 peak outstanding reads, and 350.068 s of simulation
wall time. That run used a synthetic source and 4-KiB endpoint space/interleave.
Every read selected local offset zero, so the change to 32-KiB interleaving
preserves the per-endpoint DRAM address sequence. The full software run also
includes the cores, L1 accesses, and software synchronization.

### Measured result

**PASS.** The software-driven run took **45,534.5 network cycles**, which is
10 cycles (**0.02196%**) below the recorded 45,544.5-cycle comparison point.
The measured software interval is **45,569 cycles**, also close to that target.

| Metric | Recorded network-only benchmark | arche3d software benchmark |
| --- | ---: | ---: |
| Network cycles | 45,544.5 | 45,534.5 |
| Injection cycles | 45,427.5 | 45,375.5 |
| Drain cycles | 117 | 159 |
| Software cycles after start barrier | — | 45,569 |
| Total simulated cycles including boot | — | 46,445 |
| Completed B16 reads | 1,048,576 | 1,048,576 |
| Peak outstanding reads | 8,192 | 8,192 |
| Simulation wall time | 350.068 s | 986.223 s (16 min 26 s) |
| Whole-command wall time | 352.345 s | 1,329.220 s (22 min 9 s) |
| Maximum resident memory | 0.206 GiB | 14.577 GiB |

Every one of the 1,024 endpoints reported exactly **1,024 reads / 1 MiB**, with
32,768 native DRAM reads and zero pending requests at termination. In total,
33,554,432 native DRAM reads serviced the 1-GiB workload. All response bytes
passed the passive checker. The process exited with status zero.

The simulation, memspec, controller, address-mapping, and simulator-option
configuration files were hash-checked against the recorded baseline: all five
were identical. The full model has greater host overhead from its complete
logic-tile hierarchy, software execution, and DMA/L1 activity. Whole-command
timing additionally includes Python elaboration, configuration loading, and
shutdown. The close cycle result measures hardware traffic timing; it does
not equate the host costs of the two targets.

The observed result line was:

```text
ARCHE3D_RESULT {"status":"PASS","clusters":1024,"transactions":1048576,"bytes":1073741824,"peak_outstanding":8192,"network_cycles":45534.5,"injection_cycles":45375.5,"software_cycles":45569,"total_cycles":46445,"wall_seconds":986.222893}
```

Local artifacts are in `build/arche3d/validation/alltoall.log` and
`build/arche3d/validation/verification.json`. The latter records the endpoint
totals, functional results, configuration hashes, and ELF/log hashes. These
generated artifacts remain under `build/`; this documentation directory
contains only Markdown.

## Functional and regression checks

The [one-tile fixture](../../../../tests/arche3d/README.md) uses the production
tile, DMA, I3D model, and DRAMSys endpoint. Its reduced geometry is confined to
the test target; the production architecture remains 32 × 32.

| Test | Result | Coverage |
| --- | --- | --- |
| `smoke` | PASS; 38 bursts, 19,463 bytes | DRAM writes/readback above 4 GiB; sparse gather with 8/16/32/64-bit packed indices; byte-unaligned read crossing a 4-KiB boundary |
| `queue` | PASS; 513 bursts, 2,101,248 bytes | 512 queued reads, backpressure, AXI ID reuse, and ordered retirement of an empty gather |
| `reject_collective` | Expected failure | `Collective DMA is disabled for this iDMA instance` before any collective reaches I3D |
| Existing HBM4 suite | 12/12 PASS | AXI widths 8–1,024, B16/B256, 8×8 traffic, crossbar, slow clock, and small queues |

The queue trace recorded **100 frontend stalls and 100 matching grants**. This
confirms that the test actually exhausts capacity and exercises recovery.
Successful functional runs ended with no pending DRAMSys requests.

To repeat the functional checks, build the fixture once, then build each app
and select its ELF:

```bash
make TARGETS=arche3d_dma_test MODULES="$PWD/pulp/tests/arche3d" build
make cfg=default app=smoke arche3d-sw
gvrun --target=arche3d_dma_test --target-dir=pulp/tests/arche3d \
    --binary=build/arche3d/sw/default/smoke/smoke.elf \
    --work-dir=build/runs/arche3d_dma_smoke run
```

Use `app=queue` or `app=reject_collective` and the corresponding ELF/work
directory for the other tests. To observe the queue stalls, add
`--trace=chip/cluster_0/i3d_dma/local/fe/trace --trace-level=trace`.

The high DRAM address exposed a pre-existing 32-bit configuration read in the
I3D component. Address-map fields now retain all 64 bits. The existing HBM4
regressions were run after that fix:

```bash
make TARGETS='network3d network3d_hbm4 network3d_endpoint' build
NETWORK3D_BUILD_DIR="$PWD/build/arche3d/validation/hbm4" \
    python -B pulp/pulp/3d_network/tests/hbm4_checks.py
```

## Scope

These measurements validate the integrated GVSoC architecture and software;
they do not constitute a new RTL calibration of the added DMA or logic die.
The inherited logic-die collective masks are 16 bits wide. The I3D DMA rejects
all collectives and retains sparse gather support. The runtime's global
benchmark barrier is a control-register facility; it does not measure traffic
on the separately connected synchronization NoC.

The SDK is freestanding and currently supplies startup, memory layout, core
barriers, DMA access, and exit/trap reporting. High-level compute libraries and
an OS/C-library syscall layer are outside this initial runtime.

## Independent logic-model copy

The logic models now reside in [`../logic/`](../logic/README.md). Isolation was
checked after copying the models and moving the dedicated-DMA attachment out
of `soft_hier_old`:

- The production imports, a complete logic tile, and both 2D NoCs instantiated
  with an import guard rejecting every `pulp.chips.soft_hier_old` module.
- `make cfg=default TARGETS='arche3d arche3d_dma_test'
  MODULES="$PWD/pulp/tests/arche3d" build` passed, including full 32 × 32 target
  configuration generation.
- Both generated target configurations, the two arche3d ISA decoders, and
  **364 compiler dependency files for 25 components** across the `optim`,
  `debug`, `asserts`, and `profile` variants contained no legacy model paths.
- `smoke` and `queue` passed with exactly the same transaction counts, byte
  counts, peak outstanding requests, and cycle counters as before the copy.
  Their network intervals remained 63,893.5 and 46,863.5 cycles respectively.
- `reject_collective` still terminated with the expected diagnostic. Successful
  runs had no pending DRAMSys requests.

The L1 interleaver and optional sequencer register their local C++ sources using
`add_sources`; the models do not rely on SoftHier's CMake registrations. ISA
decoder names and generated register-header paths are specific to arche3d.
`soft_hier_old` itself has no remaining changes from the arche3d integration.

The full 1,024-cluster measurements above are from the initial integration;
that long benchmark was not rerun for this source-ownership change. Local
isolation logs and the comparison report are under
`build/arche3d/validation/isolation/`.

## TCDM stacks, remote L1, and multicast wakeup

The checks in this section used the then-default 384-KiB TCDM, reserving its
top 24 KiB for six 4-KiB stacks. The addresses below describe that configuration;
the current 432-KiB default layout is documented in [the README](../README.md#memory-and-numbering).
No dedicated stack component or `0x10000000` mapping remains.
The SDK links data/BSS/heap below `0x5a000`, leaves the stack reservation out of
loadable ELF segments and BSS initialization, and initializes SP from
`__stack_end` (`0x60000`) minus the core's slice offset.

There is no separate synchronization memory. Cores access another cluster's
actual L1 banks through `0x30000000 + cluster_id * 0x60000 + local_offset` on
the 32-bit sync NoC. Logic DMA accesses use the same addresses on the separate
data NoC. The special wakeup command is `0x50000000`, outside the full
`0x30000000`–`0x47ffffff` remote-L1 range. Its X/Y destination bitmaps cover all
32 coordinates in each dimension. Command completion waits for all selected
recipients, and early notifications remain pending until software consumes them.

| Check | Result |
| --- | --- |
| Production target and test targets build | PASS |
| `memory`, four clusters (2 × 2) | PASS; all 24 cores check their TCDM stacks; data/BSS initialization, shared L1 access, remote scalar reads/writes and atomic addition checked |
| `wakeup`, four clusters | PASS; broadcast, selected row/corner, reverse direction, two early notifications, blocked receiver, concurrent senders, empty masks, and remote-data visibility on wakeup |
| `arche3d_sync_test`, 32 × 32 | PASS; seven phases, 6,149 notifications, 3,079 request responses, 1,321 cycles |
| `smoke` | PASS; 38 bursts, 19,463 bytes, no pending requests |
| `queue` | PASS; 513 bursts, 2,101,248 bytes, no pending requests |
| `alltoall` software rebuild | PASS |

The full-size sync test uses the production NoC with synthetic endpoints,
not software cores. It exercises destination bit 31, selection excluding the
sender, 1,024 simultaneous senders converging on one receiver, four concurrent
broadcasts, remote L1 reads/writes, and router queues of depth 1. It also checks
that stale collective metadata cannot turn an ordinary L1 request into a
multicast. The real tile/bank/register behavior is covered separately by the
four-cluster software tests.

These checks exposed and fixed backpressure handling in the arche3d-local sync
router: multicast branches now release their input queue correctly, and grants
return to the actual predecessor instead of the next-hop direction. Multicast
children also have initialized status, propagate errors to their parent, and
release array buffers correctly. The changes are confined to arche3d's copies;
SoftHier, core, engine, and the shared I3D network models are unchanged.

The four-cluster memory and wakeup apps completed in 59,882 and 695 total
simulated cycles respectively, with no DMA transactions. Their control-register
barriers separate test phases; the tested remote L1 and notification traffic
uses the sync NoC. DMA smoke and queue cycle counts match the earlier results.

The preceding stack-layout checks also verified ELF sections, rejected data/BSS
that would overlap stacks at link time, and rejected invalid stack reservations.
The full 1,024-cluster all-to-all simulation was not rerun for these memory-map
changes, so the earlier 45,534.5-cycle measurement remains historical.
These new checks validate functionality rather than a new RTL timing calibration.

Repeatable commands are in the [fixture guide](../../../../tests/arche3d/README.md).
Current build/run logs and the verification report are under
`build/arche3d/validation/remote_l1_wakeup/`; the earlier stack-linker checks are
under `build/arche3d/validation/l1_sync/`.

## Shared instruction cache and direct initialization

The per-cluster instruction memories and ELF loaders are removed. One system
loader writes text/rodata and the initial `.data` image into the first DRAM
stripe. Each cluster has a shared 32-KiB cache with 64-byte lines and six
independent 256-bit fetch ports. Runtime refills share the 512-bit I3D source
with DMA; the source adapter arbitrates and assigns unique live AXI IDs.

The original timed-preheat implementation issued cache reads through I3D and
DRAMSys. Its full 32 × 32 run was stopped after the cycle-60,000 progress marker,
before any DMA benchmark traffic started. It produced no full-scale throughput
result. That implementation has been replaced by direct initialization.

At reset release, before any clocked transactions, the boot controller broadcasts
one ELF-derived snapshot to all caches. Each cache copies line bytes, valid tags,
and clean/ready state. This consumes zero simulated cycles and produces no
I3D or DRAMSys requests. The snapshot includes ELF zero-fill and preserves the
configured initial memory contents in gaps and partial-line padding. It appears
once in the system configuration, rather than once per cluster.

The single system loader still populates DRAM through I3D, and all cores start
on the cycle after its final completion. Core 0 in each cluster copies initialized
data into L1 and clears BSS. RV32 PCs use the `0x80000000` alias of physical
`0x100000000`. Channels expose 64 KiB with 32-KiB interleaving; the second stripe
holds application/DMA data. Runtime cache misses and `fence.i` invalidation
continue to use the normal timed path. Direct preheating represents an already
warm cache; it excludes the hardware cost of reaching that state.

### Functional checks with direct initialization

| Software check | Clusters | Directly loaded lines | Boot cycles | Total cycles | Runtime refills | Result |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `memory` | 4 | 80 | 125 | 54,783 | 0 | PASS |
| `alltoall` | 4 | 36 | 62 | 1,107 | 0 | PASS |
| `icache` | 4 | 2,048 | 3,890 | 74,423 | 48 | PASS |

All three runs report `preheat_mode: direct` and `preheat_cycles: 0`. Their
`icache_refills` counters equal their runtime-refill counters; direct loads
are recorded separately as `icache_preloaded_lines`.

`memory` checks initialized data/BSS, all core stacks, and remote L1 access on
24 real cores. DRAMSys records zero reads. Four-cluster `alltoall` records
exactly 16 DMA reads and no preheat reads. `icache` puts six functions beyond
the first 32 KiB and initialized data beyond 64 KiB. All 24 cores execute the
functions while DMAs transfer data. It passes with 48 actual cache refills,
516 DMA transactions, and 2,113,536 DMA bytes. Every endpoint in these tests
finishes with zero pending requests.

The focused `arche3d_icache_test` passes in 406 cycles. A line initialized at a
nonzero cache index hits immediately without a downstream request. The test
then checks hits during a 100-cycle refill, victim-tag invalidation, queued
misses, flush behavior, and rejection of writes and straddling reads. Four
runtime refills complete, including the refill forced by flush.

An ELF-layout check covers a nonzero entry/cache index, multiple load segments,
zero-filled segment tails, gaps and partial-line padding, both zero and pattern
DRAM initialization, and the no-binary hardware-build case.

Build and run through the conventional flow:

```bash
make cfg=default TARGETS='arche3d arche3d_dma_test arche3d_icache_test' \
    MODULES="$PWD/pulp/tests/arche3d" build
make cfg=default app=alltoall arche3d-sw
```

See the [fixture guide](../../../../tests/arche3d/README.md) for short-test run
commands. Logs and structured checks are under
`build/arche3d/validation/direct_preheat/`. The earlier timed-preheat logs remain
under `build/arche3d/validation/icache/`; they are historical measurements.
