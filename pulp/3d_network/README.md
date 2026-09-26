# 3D network models

Cycle models of `3D-Fattree-Impl/src/fattree/fattree_interconnect.sv`,
`src/mesh/mesh_interconnect.sv`, and `src/soc/soc_interconnect.sv`, for the
`chi/3darch` IO_v2 engine. All three use the same C++ fabric kernel. There are
no benchmark-specific timing adjustments or lookup tables in the models.

The model retains the RTL's two-entry spill registers, registered links,
binary-tree fair arbitration and stalled-request locks. Fat-tree routing
supports LCA (0), NCA hash (1), and occupancy-adaptive NCA (2); mesh routing
is non-wrapping XY. The SoC model adds source/memory context allocation,
per-peer reservation/grant exchanges, terminal arbitration, AW/AR/W/B/R
packets, write reassembly, and ordered read-beat retirement.

## Use in a target

The requested folder name starts with a digit, so import it with `importlib`:

```python
import importlib
network3d = importlib.import_module('pulp.3d_network.interconnect')
MemoryEndpoint = importlib.import_module('pulp.3d_network.memory_endpoint').MemoryEndpoint

noc = network3d.SocInterconnect(
    self, 'noc', fabric=0, num_x=32, num_y=32, num_levels=3,
    routing_mode=1, source_contexts=4, memory_contexts=4,
    axi_data_width=64, max_burst_beats=256,
    memory_base=0, interleave_bytes=4096, memory_bytes=4096)
memory = MemoryEndpoint(self, 'memory_0', data_width=64, size=4096, read_slots=4)

clock.o_CLOCK(noc.i_CLOCK())
clock.o_CLOCK(memory.i_CLOCK())
master.o_OUTPUT(noc.i_INPUT(0))
noc.o_OUTPUT(0, memory.i_INPUT())
```

Bind each needed terminal in the same way. Terminal number is `x * num_y + y`.
An unbound output returns `IO_RESP_INVALID`. `FatTreeInterconnect` derives
dimensions from `num_levels`; `MeshInterconnect` takes `num_x`, `num_y`, and
`io_spill` explicitly. See [tests/benchmark.py](tests/benchmark.py) for a
complete target with masters and memories at every terminal.

`MemoryEndpoint` is a separate component. It owns storage and memory service;
`SocInterconnect` owns the NoC and its source/sink network interfaces. To use
external backing storage, bind `memory.o_OUTPUT(storage.i_INPUT())`; otherwise
the endpoint uses its internal RAM. You can also connect the NoC directly to
another IO_v2 beat endpoint, which then supplies its own timing and capacity.
Clock the NoC and this endpoint together; insert the engine's clock bridge
when connecting components in different clock domains.

### DRAMSys HBM4 endpoints

`DramsysEndpoint` connects one DRAMSys channel directly to each SoC memory
port. It uses the repository's HBM4 emulation configuration and edited memspec;
it has no `read_slots` parameter. Configure source/memory NI contexts on the
NoC, and memory capacity/scheduling in DRAMSys's controller JSON.

```python
DramsysEndpoint = importlib.import_module('pulp.3d_network.dramsys_endpoint').DramsysEndpoint
memory = DramsysEndpoint(self, 'hbm_channel_0',
    dram_type='hbm4-emu-example.json', data_width=512)
noc.o_OUTPUT(0, memory.i_INPUT())
clock.o_CLOCK(memory.i_CLOCK())  # Same 1 GHz clock as the NoC.
```

Use `SocInterconnect(axi_addr_width=64, axi_data_width=512, ...)` with this
example. Configuration follows GVSoC's [DRAMSys integration](../../../DRAMSys.md):
`dram_type` selects a JSON in `$DRAMSYS_PATH/dramsys_configs`. Without that
override, the model searches `memory/dramsys_configs` on GVSoC's module paths,
including `core/models/memory`, populated by `make build-configs`.
`libDRAMSys_Simulator.so` is loaded through `LD_LIBRARY_PATH`, normally from
`third_party/DRAMSys` after sourcing `sourceme.sh`. Explicit `config=` and
`library=` constructor arguments remain available for specialized targets.

These measurements use the repository's **bundled** DRAMSys library with
SystemC **2.3.3 built with C++17**. That library has an older configuration
schema and a two-argument `add_dram` API; the newer library build recipe and
the core wrapper use a different API. Matching configuration paths does not
make those library versions interchangeable. The focused HBM4 tests keep the
validated library and perform the required configuration conversion at build
time, preserving the source memspec and recording all changes and hashes.

```bash
# From the gvsoc repository root, after installing the compatible library
# in third_party/DRAMSys as described in DRAMSys.md.
source sourceme.sh
export SYSTEMC_HOME=/path/to/systemc-2.3.3-install
export PYTHON=/path/to/python-with-gvsoc-dependencies
bash pulp/pulp/3d_network/tests/hbm4.sh build
"$PYTHON" pulp/pulp/3d_network/tests/hbm4_checks.py
"$PYTHON" pulp/pulp/3d_network/tests/hbm4_sweep.py
```

The build is isolated in `gvsoc/build/network3d_hbm4`. `hbm4.sh run` accepts the
same `--parameter=name=value` overrides as the normal benchmark, with HBM4,
1 GHz, 64-bit AXI addresses, 512-bit data, B16 and X=8 as defaults. For example,
`--parameter=sc=32 --parameter=mc=32` selects X=32. By default the test launcher
sets `DRAMSYS_PATH` to the build directory and prepares its `dramsys_configs`
from `add_dramsyslib_patches/dramsys_configs/hbm4-emu-example.json`. Set
`DRAMSYS_PATH` yourself to use an existing prepared configuration directory;
the build then leaves those files untouched. `--parameter=dram_type=<file.json>`
selects another simulation JSON. Select a compatible library using
`LD_LIBRARY_PATH`, as for other GVSoC targets.

To prepare another configuration, run
`prepare_hbm4.py --source ... --output "$DRAMSYS_PATH/dramsys_configs"`.
The older `NETWORK3D_DRAMSYS_CONFIG` and `NETWORK3D_DRAMSYS_LIBRARY` test
variables have been replaced by these standard conventions.
[doc/hbm4_config](doc/hbm4_config/README.md) is only a frozen snapshot for the
historical measurements; neither the model nor the test runner reads it.

The bridge serializes a burst's native DRAM requests, waiting for each
DRAMSys `END_REQ` before the next `BEGIN_REQ`; multiple bursts remain in flight
under DRAMSys's own capacity control. It assembles native 32-byte completions
into 64-byte AXI R beats and emits at most one R beat per NoC cycle. Denied
responses are retained until a synchronous retry. Partial writes use native
byte enables and receive a single B response after all native writes complete.
Native completion data is buffered until the NoC accepts it. There is no
additional response-buffer capacity setting. The bundled C ABI orders read
completions as a FIFO. After assembling that data, the bridge interleaves AXI
responses round robin across reads whose next beat is ready. It advances after
each accepted beat, preserves beat order within each transaction, and holds a
denied beat unchanged until retry. This scheduling adds no read-slot limit.
Reset during active DRAM traffic is rejected.

See [the HBM4 context-sweep report](doc/soc_context_b16_hbm4.md) for measurements,
the exact configuration conversions, and the scope of validation.
See [the bandwidth diagnosis](doc/soc_context_b16_hbm4_diagnosis.md) for isolated
channel measurements and the effect of AXI read-response scheduling on the NoC.

### Parameters

| Python argument | RTL parameter / meaning | Default |
|---|---|---|
| `fabric` | `Fabric`: 0 fat tree, 1 mesh (SoC only) | 0 |
| `num_x`, `num_y` | `NumX`, `NumY` | 32, 32 |
| `num_levels` | `NumLevels`, radix-16 fat tree | 3 |
| `routing_mode` | `RoutingMode`: LCA, hash, adaptive | 1 |
| `io_spill` | `NumIOSpill` / `MeshIOSpill`; fat tree fixes this at 2 | 2 |
| `addr_width`, `data_width` | Native packet address/data widths, bits | 32, 64 |
| `axi_addr_width`, `axi_data_width` | `AXIAddrWidth`, `AXIDataWidth`, bits | 32, 64 |
| `axi_id_width`, `axi_len_width` | `AXIIdWidth`, `AXILenWidth`, bits | 10, 8 |
| `source_contexts`, `memory_contexts` | `SourceContexts`, `MemoryContexts` | 4, 4 |
| `max_burst_beats` | `MaxBurstBeats` | 256 |
| `memory_base`, `interleave_bytes`, `memory_bytes` | `MemoryBase`, `InterleaveBytes`, per-endpoint `MemoryBytes` | 0, 4096, 4096 |

Memory endpoint parameters:

| Python argument | Meaning | Default |
|---|---|---|
| `data_width` | Memory bus width in bits; match `SocInterconnect.axi_data_width` | 64 |
| `size` | Local RAM capacity in bytes | 4096 |
| `read_slots` | Concurrent memory read bursts; `axi_sim_mem.ReadSlots` | 4 |
| `benchmark_init`, `endpoint_id` | Initialize RAM with the RTL benchmark's byte pattern | `False`, 0 |

Migration: remove `memory_read_slots` from `SocInterconnect` and set
`MemoryEndpoint(read_slots=...)`. `memory_contexts` remains in the NoC:
it limits destination NI transaction contexts, independently of the memory's
read slots. `memory_bytes` describes address decoding, not integrated storage.

Positive mesh dimensions and 1–32 spill stages are supported. The fat tree
supports levels 3–6, with dimensions derived from the RTL expansion rules;
the practical allocation limit is one million terminals. Large SoC context
counts and terminal counts consume correspondingly more host memory. These
limits are checked at construction. AXI data widths are powers of two from
8 to 1024 bits; burst capacity is at most 256 beats and must fit the LEN width.

NoC header fields and payload padding are represented by typed metadata;
padding the RTL `NocAddrWidth`/`NocDataWidth` has no timing effect. Request
identity replaces physical AXI ID encoding; distinct live IO_v2 objects are
distinct transactions. The AXI ID-width setting does not limit those object
identities or impose same-ID ordering.

### IO_v2 contract

Source ports and both sides of the native models declare
**`IoV2SingleReq(width=0)`**: a request keeps its identity and receives one
completion. Native models accept one packet of at most
`ceil(data_width/8)` bytes. Its destination occupies the low
`ceil(log2(num_x*num_y))` address bits; the complete address reaches the
ejection endpoint unchanged. Completion acknowledges delivery to that
endpoint; the native RTL fabric has no separate response network.

The SoC model accepts a complete read or write transaction in one IO_v2
object. It models `ceil(size / (axi_data_width/8))` full-width AXI beats, with
a possibly partial final beat. Requests to one source share the finite
source contexts. For `a = address - memory_base` and `G = interleave_bytes`:

```
destination = (a / G) % number_of_terminals
local_address = (a / (G * number_of_terminals)) * G + a % G
```

The SoC memory ports declare **`IoV2Beat(axi_data_width // 8)`** and carry
translated local addresses. The sink NI issues one read descriptor or a
sequence of write beats after full W reassembly. It forms NoC response packets
only as the endpoint offers R beats or a B acknowledgement. It contains no
memory read-slot limit, read scheduler, or assumed memory completion latency.
The engine inserts protocol adapters for compatible SingleReq/Sync memories;
those adapters have their own timing, so use `MemoryEndpoint` to reproduce
the RTL benchmark endpoint policy.

`MemoryEndpoint` models `axi_sim_mem`: finite read slots, round-robin service
per R beat, a held selection under backpressure, registered AW-to-W readiness,
one W beat per cycle, and B after the last W. A slot freed on an edge cannot
be reused on that edge. Its optional SingleReq backing port receives one
complete transaction per burst; backing completion gates read availability
or B. Backend latency annotations and asynchronous responses are consumed
in scheduled cycles, not counted again in the returned request's latency.
An untimed backing RAM avoids adding a second memory service model.

`DENIED` retains ownership at the sender, and request retries are serviced
synchronously. Responses can be denied and are held until `resp_retry()`;
the wrapper also services that callback synchronously. Address, parent, and
initiator identity on the original request are preserved. Payloads and byte
strobes have independent downstream storage, including across cancellation.
Reset flushes the fabric and contexts; reset the connected masters and
memories together. Late responses to canceled children are discarded.

On source ports, unsupported atomics, zero-size or data-less requests, and
beat-stream fragments are rejected. SoC requests outside the memory map, crossing an
interleave stripe, or exceeding burst capacity return `IO_RESP_INVALID`.
Connect a beat-stream master through the engine's explicit protocol bridge
when needed; direct AXI pin behavior such as W-before-AW, independent AW/W
admission, AXI IDs, and per-R-beat source backpressure is outside this
transaction interface. Memory-side beat backpressure is modeled explicitly.
Timing validation below concerns the traffic
benchmarks' full-width INCR requests, not every possible AXI pin trace.

## Build and run

From the GVSoC `pulp` submodule root:

```bash
bash pulp/3d_network/tests/build.sh
bash pulp/3d_network/tests/run.sh
bash pulp/3d_network/tests/run.sh \
  --parameter soc=1 --parameter fabric=1 \
  --parameter nx=3 --parameter ny=2 --parameter burst=7
python3 pulp/3d_network/tests/validate.py \
  --rtl ../../3D-Fattree-Impl \
  --native-reference pulp/3d_network/tests/rtl_holdout_results.json \
  --output ../build/network3d/validation.json
```

The focused build needs CMake >=3.16, a C++17 compiler, the initialized
`engine`, `core`, `pulp`, `gvrun`, and `config_tree` submodules, and a modern
Python with the engine/gvrun dependencies. Set `PYTHON` to your interpreter.
The scripts automatically use `gvsoc/build/network3d/venv/bin/python` if that
local environment exists. `NETWORK3D_BUILD_DIR` changes the build/install
location; `NETWORK3D_RUN_DIR` isolates concurrent runs. No existing SDK
installation is overwritten. A platform-tree warning when changing
parameters is normal: GVSoC loads the generated JSON configuration instead.

To rerun the fresh native RTL comparisons, initialize the RTL repository's
`common_cells` dependency with Bender, then run:

```bash
python3 -B pulp/3d_network/tests/rtl_holdout.py \
  --rtl ../../3D-Fattree-Impl --output-dir ../build/network3d/rtl_holdout
```

Pass that directory's `results.json` to `validate.py --native-reference`.
The RTL command needs Questa and an available license. The saved reference
also verifies the exact common-cells source hashes used for its simulation.

The benchmark supports `soc`, `fabric`, `mode`, `nx`, `ny`, `sparse`,
`groups`, `repeats`, `seed`, `burst`, `sc`, `mc`, `readslots`, `offset`,
`datawidth`, and `spill` target parameters. `stress=1` enables downstream
request denial, delayed asynchronous responses, annotated latency, and
upstream response denial. `functional=1` with `soc=1` selects masked writes,
readback, stripe boundaries, invalid requests, and partial reads.
`backing=0` exercises the endpoint's internal RAM; the default `backing=1`
uses the test driver's storage scoreboard and optional delay/denial injection.
`readslots` configures each separate endpoint, not the interconnect.
Protocol probes use `endpoint=0` for an inline whole-read responder and
`endpoint=2` for unbound memory ports; normal simulations use `endpoint=1`.

`tests/native.cpp` and `tests/soc.cpp` also expose the same kernels as small
standalone executables for debugging and sanitizer runs. Accuracy claims
use the **GVSoC IO_v2 benchmark**, not just these kernel executables.

## Validation and performance

The recorded run passes **33 RTL comparisons and 21 IO_v2 protocol tests**.
All recorded native and SoC comparisons are **cycle-exact**, including the
1024-terminal B16 context sweep. Moving memory service out also makes the NI
hold its AR selection throughout endpoint backpressure, as the RTL does.

| Benchmark | RTL cycles | GVSoC cycles | Absolute error |
|---|---:|---:|---:|
| Fat-tree hash all-to-all, 1 beat/destination | 2,209.5 | 2,209.5 | 0% |
| Mesh all-to-all, 1 beat/destination | 21,076.5 | 21,076.5 | 0% |
| Fresh fat-tree adaptive sparse, N=11, X=2, seed=17 | 82.5 | 82.5 | 0% |
| Fresh mesh sparse, same workload | 445.5 | 445.5 | 0% |
| SoC fat tree, 1-beat reads, 4 source/memory contexts | 23,392.5 | 23,392.5 | 0% |
| SoC fat tree, 16-beat reads, 8 contexts | 57,035.5 | 57,035.5 | 0% |
| SoC fat tree, 16-beat reads, 32 contexts | 55,973.5 | 55,973.5 | 0% |
| SoC fat tree, 16-beat reads, 128 contexts | 57,312.5 | 57,312.5 | 0% |

The million-transaction SoC 1-beat case took **5.16 seconds** in the GVSoC
simulation loop, **5.79 seconds** including launch/configuration. Its saved
RTL run reports 3,815.65 seconds. The million-packet native hash all-to-all
took 1.23 seconds in the simulation loop.

The B16 tests each contain 1,048,576 transactions and 16,777,216 returned beats:

| Source/memory contexts | GVSoC simulation loop | GVSoC including launch/configuration |
|---|---:|---:|
| 8 | 26.20 s | 26.84 s |
| 32 | 46.03 s | 46.67 s |
| 128 | 198.81 s | 199.52 s |

These runs use separate memory endpoint components with four read slots each.
See the timing-scope distinction below before comparing wall times with RTL.

`tests/validation.json` records parameters, RTL references, GVSoC cycles,
deviation, workload checksums, host times, and source hashes. The runner
fails on more than 5% absolute runtime-cycle deviation, wrong traffic
counts, or a different sparse workload checksum. Every response is also
checked for request identity, destination, duplicate delivery, and data.

Coverage includes both native fabrics' all-to-all and Philox sparse/grouped
patterns, all three fat-tree routing modes, seed boundaries, and the SoC
all-to-all pattern with different dimensions, data widths, burst lengths,
read slots, and context counts. It uses the RTL repository's saved results
and checks their production RTL hashes. A fresh Questa 3×2 mesh, seven-beat
SoC run is included when `target/checks/gvsoc_3d_network/benchmark_results.json`
exists. Historical all-to-all wrappers were refactored; their current
workload-source hashes are recorded separately rather than misrepresented
as matching the old wrapper hashes.

To regenerate that additional SoC reference, run from `3D-Fattree-Impl`:

```bash
python3 -B scripts/check_soc.py --only benchmark --fabric mesh \
  --generic NumX=3 --generic NumY=2 --generic BurstBeats=7 \
  --generic SourceContexts=3 --generic MemoryContexts=2 \
  --generic MemoryReadSlots=1 --generic StartOffset=64 \
  --output-dir target/checks/gvsoc_3d_network
```

Without this optional fresh SoC result, the saved native references plus
the RTL repository's committed results provide 32 accuracy comparisons.

The accuracy threshold applies to the recorded configurations. Additional
parameter combinations and application traffic should be compared with
RTL before extending that claim. In particular, the whole-transaction
source interface abstracts independent AXI pin timing. Changing the endpoint
changes memory timing; the recorded comparisons use the supplied `MemoryEndpoint`.

The fabric uses one GVSoC event per active network cycle. Each active memory
endpoint uses the engine's recurring-event API, avoiding repeated insertion
into the sorted event queue, and disables its event when idle. C++ FIFO/route
metadata replaces RTL signal evaluation; packets hold transaction references
instead of copied signal vectors.
Reported GVSoC `wall_seconds` measures the simulation loop; the report also
includes process wall time with Python startup and configuration. Historical
RTL wall times include elaboration and sometimes compilation, so their
ratios are indicative turnaround improvements, not controlled simulator-only
speedup measurements.

Kernel memory-safety and reset checks can be run separately:

```bash
g++ -O1 -g -std=c++17 -fsanitize=address,undefined \
  -fno-omit-frame-pointer pulp/3d_network/tests/kernel_checks.cpp \
  -o ../build/network3d/kernel_checks
ASAN_OPTIONS=detect_leaks=0 ../build/network3d/kernel_checks
```

These exercise random stalls, reset flushing, three spill depths, all
fat-tree routing modes, level-4 topology, mixed reads/writes, a 64-bit
interleaved map, endpoint slot reuse, held R selection, AW/W/B timing, and
waiting for externally supplied memory responses. The current engine loader
uses `RTLD_DEEPBIND`, which prevents launching the full GVSoC process with
AddressSanitizer; sanitizer coverage is
therefore limited to these shared kernels. The actual IO_v2 wrapper is covered
by the GVSoC functional and backpressure tests.
