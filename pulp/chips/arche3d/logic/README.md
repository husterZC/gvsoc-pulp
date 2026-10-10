# arche3d logic die

This directory owns the logic-die models used by arche3d. They were copied from
`pulp/chips/soft_hier_old` at pulp commit
`6725ffaaea2b89f22a7c1f268791ef6036a564ec`, together with the dedicated-DMA
attachment added for arche3d. Original copyright and license notices are kept.
The attachment now lives entirely in this copy.

These are ordinary source files maintained independently. Python imports,
C++ source lists/includes, component names, and register-map generation paths
refer to `pulp.chips.arche3d.logic` / `pulp/chips/arche3d/logic`. Generated core
ISA names start with `arche3d_snitch_` so another target's decoder generation
cannot replace them. There is no copy or synchronization step during a build;
porting a future SoftHier change requires an explicit edit and validation here.

| Path | Models |
| --- | --- |
| `cluster_unit.py` | Logic tile, banked L1 composition, and dedicated-DMA attachment |
| `icache.cpp` | Shared IO_v2 instruction cache adapted from `pulp/snitch/snitch_icache.cpp` |
| `cluster_registers.*` | Local barriers, boot, and cluster registers |
| `memory.*`, `l1_fabric.*` | Storage banks and queued L1 arbitration with shared HWPE bandwidth |
| `snitch/` | Core wrappers, XDMA/RedMule/RVV instruction extensions, zero memory, sequencer, and register schema |
| `idma/` | Logic-die DMA and the frontend/sparse middle-end shared with the I3D DMA |
| `offload_decoder.*`, `light_redmule.*`, `matrix_bridge*`, `layout_engine.*`, `util_dumpper.*` | Selectable matrix engines, streaming layout conversion, tile peripherals and instruction offload |
| `flex_mesh_noc.py`, `floonoc/` | Scalar sync NoC and bitmap-selected wakeup notifications |
| `flex_mesh_noc_v2.py`, `floonoc_v2/`, `noc_bridge*` | Data NoC, ordinary DMA bridges and native masked collective endpoints/routers |

Shared dependencies are the GVSoC framework, generic ISS/ISA services,
routers/ELF loader, and PULP's common Spatz/Snitch support. The I3D interconnect
and DRAMSys endpoints are composed by `../system.py` from `pulp/3d_network`.

Build and run through the [arche3d target](../README.md). Model C++ sources are
registered by the Python components through GVSoC's `add_sources` convention.

Data collectives use local MMIO send/receive slots. Packets carry
X/Y coordinate-bit match masks, explicit epochs, total bytes and beat offsets.
One descriptor streams an L1 buffer through bounded eight-beat read/write
windows; local memory timing and mesh backpressure remain active. Each router
matches reduction beats by offset and can forward one result per cycle.
Multicast replicates outward; reduction combines selected contributions inward
in the two-cycle router pipeline. Send completion is local capture, with no receiver-acknowledgement
tree. The DMA bridges carry ordinary unicast requests separately.
Numerical reduction helpers remain shared by the native router implementation.
Full-row, full-column and masked barriers reside in `../control.cpp`, outside
both NoCs. See the [architecture guide](../README.md) for the SDK contract and
the separate meanings of collective match masks and sync-wakeup bitmaps.

The instruction-cache implementation is a source copy of the PULP Snitch cache.
Its arche3d constructor reads ordinary component properties, supporting both
compiled platform trees and GVSoC's JSON fallback when the ELF or test geometry
changes. It uses direct mapping, one demand refill slot, and no speculative
prefetching. A host-side initialization port copies the ELF entry window into
cache data and tags at time zero; it generates no IO traffic. The single shared
snapshot is prepared in `../instructions.py` and broadcast by the boot controller.
The arche3d copy also serves resident hits during an unrelated refill and
invalidates the victim before its buffer is refilled. A focused regression
checks both behaviors, cache flush, and invalid accesses. No change to the
shared cache or framework is required.
