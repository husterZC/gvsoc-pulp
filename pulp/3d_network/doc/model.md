# Model reference and validation

For setup, build and run instructions, start with the [module README](../README.md).

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
`io_spill` explicitly. See [benchmarks/network.py](../benchmarks/network.py) for a
complete target with masters and memories at every terminal.

`MemoryEndpoint` is a separate component. It owns storage and memory service;
`SocInterconnect` owns the NoC and its source/sink network interfaces. To use
external backing storage, bind `memory.o_OUTPUT(storage.i_INPUT())`; otherwise
the endpoint uses its internal RAM. You can also connect the NoC directly to
another IO_v2 beat endpoint, which then supplies its own timing and capacity.
Clock the NoC and this endpoint together; insert the engine's clock bridge
when connecting components in different clock domains.

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

`read_slots` belongs to `MemoryEndpoint`. `memory_contexts` belongs to the NoC:
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

## Validation and performance

The optional [regression tools](../tests/README.md) run the installed GVSoC
benchmark targets. `validate.py` compares the native and SoC/RAM workloads
against RTL references, checks source hashes and fails on more than 5% absolute
runtime-cycle deviation, wrong traffic counts or mismatched workload checksums.
The traffic drivers check response identity, destination, duplicate delivery
and data. Results and logs are generated under `gvsoc/build/`.

Coverage includes all-to-all and Philox sparse/grouped native traffic, the
fat-tree routing modes, mesh dimensions, and SoC reads with different widths,
burst lengths, read slots and context counts. Protocol checks exercise stalls,
mixed reads/writes and invalid accesses. The kernel checks additionally cover
reset flushing, spill depths, held response selection and endpoint slot reuse.

Accuracy depends on the measured topology, workload and endpoint. The
transaction source interface abstracts independent AXI pin timing; a different
memory endpoint changes memory timing. Comparing HBM4 against RTL requires a
matching DRAM configuration and traffic pattern.

The fabric uses one GVSoC event per active network cycle. Each active memory
endpoint uses recurring events and disables its event when idle. C++ route and
FIFO metadata replace RTL signal evaluation, and packets hold transaction
references instead of copied signal vectors.

The driver's `wall_seconds` measures the simulation loop. The regression tools
also record process wall time, including Python startup and target generation.
Compare equivalent timing scopes when measuring simulator speed.
