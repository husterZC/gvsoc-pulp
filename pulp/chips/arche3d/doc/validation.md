# arche3d validation

Validated on 2026-09-26 using the default architecture and the conventional
GVSoC build/run flow in the [architecture guide](../README.md).
Both GVSoC and DRAMSys were built in `Release` mode. The host was an AMD Ryzen 7
5800X; wall times describe this machine and are not simulated hardware timing.

## Full software all-to-all benchmark

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
