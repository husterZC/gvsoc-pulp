# Changelog

Record model behavior and interface changes here with their implementation
date. Keep architecture documentation aligned with the current design and
replace superseded measurements instead of retaining comparison tables.
Changes to the shared hardware/software contract also belong in the
[SDK changelog](../../../../arche3d_sdk/CHANGELOG.md).

## 2026-10-06 — Native bulk collectives

### Added

- Per-cluster native collective endpoints at system register offset `0x1000`,
  with 16 independent operation slots and local send/receive descriptors.
- X/Y coordinate-bit match masks for full, sparse and two-dimensional groups;
  excluded endpoints forward traffic without contributing or receiving data.
- Outward multicast replication and inward reduction in `floonoc_v2`, using
  a two-cycle forwarding/reduction pipeline and fixed operand order.
- Whole-buffer streaming under one slot/epoch. Packets carry total length and
  byte offset; reduction contexts match successive beats independently.
- Bounded eight-beat read and write windows that honor local L1 latency,
  asynchronous completion and contention. NI queues and mesh credits provide
  backpressure, with one beat per cycle when unblocked.
- Independent row, column and masked barriers in the controller, releasing
  one control cycle after the final selected arrival without NoC packets.
- Whole-buffer completion and first/last beat timestamp registers. Invalid
  descriptors, local bounds/alignment and elements wider than a beat are rejected.
- Native regression coverage for bulk tails, all reduction formats, masks,
  concurrent streams, source reuse, delayed participants, asynchronous L1
  responses, ordinary unicast contention and relocated L1 addresses.

### Changed

- Collective sends complete on local capture without a receiver ACK tree;
  reductions use contributions pushed by every participant. Receives finish
  only after destination L1 writes complete.
- Endpoint processing is organized into explicit completion, send, read,
  write and scheduling stages. Collective NI traces share one formatter.
- Architecture and SDK documentation describe the current design, with
  reproducible validation instructions and no superseded benchmark tables.

### Removed

- Collective-only XDMA encodings/handlers, DMA descriptor sidebands and the
  DMA-bridge collective path. Native endpoints own collective traffic.

### Validation

- Model builds and the 85-phase native regression cover 4×4 and 32×32 meshes,
  shallow queues, single-axis meshes and relocated L1 addresses. Invalid
  descriptors whose elements exceed the beat width are rejected.
- Complete logic-tile tests cover collective correctness and FP agreement.
  The 54-case RAM-backed row sweep retains its software and NI timings after
  cleanup; full external DRAMSys timing is outside this validation scope.
