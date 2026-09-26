# 3D network models

GVSoC fat-tree, mesh and crossbar models, with IO_v2 interfaces, SoC network
interfaces and separate RAM or DRAMSys memory endpoints.

Use the normal GVSoC build and installed runner. This branch uses **`gvrun`**
for IO_v2 targets. All commands below run from the **`gvsoc/` repository root**.
The scripts in `tests/` are optional regression tools.

## 1. Set up the environment

Start with a Linux host, a C++17 compiler, Make, Git and Python 3.12 or newer.
The shared DRAMSys source build also needs CMake; its existing setup is described
in [DRAMSys.md](../../../DRAMSys.md). The module does not replace that setup.

```bash
git clone --branch chi/3darch git@github.com:husterZC/gvsoc.git
cd gvsoc
git submodule update --init engine core pulp gvrun config_tree gvtest pulpos

python3.12 -m venv build/venv
source build/venv/bin/activate
python -m pip install -r core/requirements.txt -r gapy/requirements.txt \
    -r gvrun/requirements.txt 'cmake>=3.28,<4'
source sourceme_systemc.sh
export USE_GVRUN=1 USE_GVRUN2=1
```

In a new terminal, repeat the two `source` commands and the `export` command.
For simulations using only the RAM endpoint, use `source sourceme.sh` instead
and skip step 2. No RISC-V program or RTL simulator is needed for these traffic
benchmarks.

## 2. Build DRAMSys using the existing GVSoC setup

```bash
make dramsys_preparation
```

This is GVSoC's existing dependency target. It prepares SystemC, the DRAMSys
library in `third_party/DRAMSys`, and the configurations in
`core/models/memory/dramsys_configs`. `SYSTEMC_HOME` and the loaded DRAMSys
library must describe a compatible pair. The endpoint uses the same DRAMSys
library API as GVSoC's `memory.dramsys` model.

The HBM4 target selects `hbm4-emu-example.json` from this shared configuration
through `dram_type`. An existing `DRAMSYS_PATH` override is honored, with the
same meaning as for `memory.dramsys`: it names the **parent** of `dramsys_configs`.
The `doc/` directory contains Markdown documentation only; runtime configuration
comes from the shared DRAMSys setup above.

## 3. Build the targets

```bash
make TARGETS='network3d network3d_hbm4 network3d_endpoint' build
```

The usual GVSoC `build/` and `install/` directories are used. To build just the
native/RAM benchmark, use `make TARGETS=network3d build`.

| Target | Purpose | Defaults |
|---|---|---|
| `network3d` | Native fat-tree/mesh/crossbar traffic, or SoC traffic with RAM | Native fat tree, NCA hash, 32 × 32 terminals |
| `network3d_hbm4` | SoC traffic with one DRAMSys HBM4 channel per terminal | Fat tree, NCA hash, 1 GHz, AXI 64/512 bits, B16, X=8 |
| `network3d_endpoint` | Isolated memory-channel bandwidth and backpressure | One HBM4 channel, 4096 reads, B16, window=8 |

Target registration is in `pulp/targets/network3d*.py`. The reusable models
remain independent of these benchmark targets.

## 4. Run a simulation

First run a small SoC/RAM example:

```bash
gvrun --target=network3d --work-dir=build/runs/ram \
    --parameter=soc=1 --parameter=fabric=1 \
    --parameter=nx=2 --parameter=ny=2 --parameter=backing=0 run
```

After DRAMSys setup, the equivalent HBM4 check is:

```bash
gvrun --target=network3d_hbm4 --work-dir=build/runs/hbm4 \
    --parameter=fabric=1 --parameter=nx=2 --parameter=ny=2 run
```

For a crossbar connecting 64 terminals, each with one HBM4 channel:

```bash
gvrun --target=network3d_hbm4 --work-dir=build/runs/xbar_hbm4 \
    --parameter=fabric=2 --parameter=nx=8 --parameter=ny=8 run
```

The crossbar directly connects every input to every output. `nx * ny` sets the
number of paired ports; it does not create a two-dimensional router grid.
`spill` sets the registered stages on each side of the switch (default 2).
Different outputs can transfer concurrently; traffic to the same output is
arbitrated per packet. The SoC interfaces and memory endpoint behavior apply
to all three topologies.

The full 1024-terminal B16/X=8 fat-tree benchmark and isolated channel test are:

```bash
gvrun --target=network3d_hbm4 --work-dir=build/runs/fattree_b16_x8 run
gvrun --target=network3d_endpoint --work-dir=build/runs/channel run
```

The full network run has 1,048,576 transactions and takes substantially longer
than the small check. Successful runs print `NETWORK3D_RESULT` or
`ENDPOINT_BANDWIDTH_RESULT` with `"status":"PASS"`. `runtime_cycles` measures
network cycles; `wall_seconds` measures the simulation loop's host time.

## Change an experiment

Use the same target and change its parameters. For example, a 64-beat read
with adaptive NCA and a 32 KiB endpoint mapping is:

```bash
gvrun --target=network3d_hbm4 --work-dir=build/runs/adaptive_b64 \
    --parameter=mode=2 --parameter=burst=64 \
    --parameter=sc=8 --parameter=mc=8 \
    --parameter=interleave_bytes=32768 --parameter=memory_bytes=32768 run
```

Give independent runs different `--work-dir` paths. A burst must fit within
one interleave stripe and within the mapped endpoint capacity.
When dimensions change, GVSoC may report that it is using the JSON configuration
instead of the installed platform tree. This is the normal parameter fallback.

| Benchmark parameter | Meaning |
|---|---|
| `soc` | 0: native packets; 1: SoC read/write transactions |
| `fabric` | 0: fat tree; 1: mesh; 2: crossbar |
| `mode` | Fat-tree routing: 0 LCA, 1 NCA hash, 2 adaptive NCA; unused by mesh/crossbar |
| `nx`, `ny` | Mesh dimensions or crossbar terminal layout; the fat-tree benchmark has 32 × 32 terminals |
| `spill` | Registered stages per input and output, 1–32 for mesh/crossbar; fat tree uses 2 |
| `burst` | AXI beats per read, up to 256 |
| `sc`, `mc` | Source and memory NI contexts; X means setting both |
| `frequency` | Interconnect clock frequency in Hz |
| `addrwidth`, `datawidth` | AXI address/data widths in bits in SoC mode |
| `interleave_bytes`, `memory_bytes` | Stripe size and mapped bytes per endpoint |
| `readslots` | RAM endpoint read capacity; unused by DRAMSys |
| `dram_type` | DRAMSys simulation JSON filename |
| `sparse`, `groups`, `seed`, `repeats` | Native traffic workload selection |

For `network3d_endpoint`, `count` and `window` configure the traffic generator's
request count and outstanding requests. They do not impose a memory read-slot
limit on DRAMSys.

To experiment with DRAM timings or controller queues, copy the shared
`dramsys_configs` directory to your run area, edit the copy, and point
`DRAMSYS_PATH` at its parent. This leaves the installed dependency configuration
intact. Database recording and instrumentation are configured by DRAMSys's
`simconfig` JSON, independently of the network model.

## Connect the network in another target

Import and instantiate the component, then bind its clock and IO_v2 ports.
For example, within a target that already provides `clock` and `master`:

```python
import importlib
network3d = importlib.import_module('pulp.3d_network.interconnect')
MemoryEndpoint = importlib.import_module('pulp.3d_network.memory_endpoint').MemoryEndpoint

noc = network3d.SocInterconnect(
    self, 'noc', fabric=0, routing_mode=1,
    source_contexts=8, memory_contexts=8,
    axi_addr_width=64, axi_data_width=512,
    interleave_bytes=32768, memory_bytes=32768)
clock.o_CLOCK(noc.i_CLOCK())
master.o_OUTPUT(noc.i_INPUT(0))

for terminal in range(32 * 32):
    memory = MemoryEndpoint(self, f'memory_{terminal}',
                            data_width=512, size=32768, read_slots=4)
    clock.o_CLOCK(memory.i_CLOCK())
    noc.o_OUTPUT(terminal, memory.i_INPUT())
```

For HBM4, replace the memory constructor with
`DramsysEndpoint(self, name, dram_type='hbm4-emu-example.json', data_width=512)`
from `pulp.3d_network.dramsys_endpoint`. No `read_slots` argument is used.
The NoC derives native packet widths from its AXI configuration. Memory service
and capacity remain in the endpoint.

Build your enclosing target using `make TARGETS=<your-target> build`.
See [the model reference](doc/model.md) for all constructor parameters,
address mapping, interface contracts and validation limits.

## Directory guide

| Location | Contents |
|---|---|
| `interconnect.*`, `network.hpp`, `soc.hpp` | Reusable interconnect models |
| `memory_endpoint.*`, `dramsys_endpoint.*` | Reusable memory endpoints |
| `benchmarks/` | Benchmark system composition and traffic generators |
| `../../targets/network3d*.py` | Conventional GVSoC target registration |
| `tests/` | Optional regression checks; see [tests/README.md](tests/README.md) |
| `doc/` | Markdown model reference |

Generated logs, configurations, JSON results and CSV files belong under
`gvsoc/build/`.
