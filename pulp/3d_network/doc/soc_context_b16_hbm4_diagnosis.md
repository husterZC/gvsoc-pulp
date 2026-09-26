# B16 HBM4 bandwidth diagnosis

Investigation dated 2026-09-26. These are simulated cycle/bandwidth results;
host execution time is a separate metric. The initial investigation used a
diagnostic copy of the endpoint. **Ready-beat round-robin arbitration has since
been applied to the production `DramsysEndpoint`.** The NoC model and the user's
DRAM timings are unchanged. The measurements below retain the original FIFO
baseline and identify the diagnostic runs separately.

**Response arbitration explains essentially the entire gap to the original
simple-memory policy in this benchmark.** A test-only ready-beat round-robin
variant takes 56,991.5 cycles with real DRAM timings, versus 71,050.5 for FIFO
HBM4 and 57,035.5 for simple memory with four read slots.

The current DRAMSys endpoint can deliver a 512-bit read beat every 1 ns when
supplied with enough reads and an accepting consumer. The full SoC benchmark
does not keep the DRAM controllers continuously busy. Replacing simple RAM also
changes AXI response arbitration: the RAM endpoint interleaves beats across
reads, whereas the former DRAMSys endpoint drained each burst in FIFO order.

[Measured results, commands, hashes, and log index](hbm4_diagnosis/results.json)
include 18 completed diagnostic measurements. Compressed logs are stored beside
that index. [The diagnostic source patch](hbm4_diagnosis/response_probe.patch)
records the complete endpoint changes used for the A/B comparison.

## Production validation

The production endpoint was rebuilt in `build/network3d_hbm4` and tested with
the original bundled DRAMSys library. The full B16/X=8 run passes in
**56,991.5 cycles**, exactly matching the diagnostic round-robin result:
1,048,576 reads, 1 GiB of checked data, and zero pending jobs on all 1,024
channels. Native request counts and DRAM timings are unchanged.

[Production results, hashes, and compressed logs](hbm4_rr_validation/results.json)
record the full run and 13 additional checks:

- Five mixed read/write and invalid-access tests cover 8-, 64-, 128-, 512-, and
  1,024-bit interfaces with source response backpressure.
- Four integration checks cover B16, an 8x8 mesh under response stalls, a slower
  DRAM clock, and reduced native request queues.
- Four direct-endpoint checks cover sustained bandwidth and forced response
  stalls, including reduced DRAM queues. The two stall tests verify 1,912 and
  1,919 switches away from unfinished reads, respectively, and 187 held-beat
  retries each. They check that a denied beat is unchanged, no different beat
  is sent before retry, and acceptance never exceeds one beat per cycle.

The production isolated-channel measurements remain 63.988 GB/s for 4,096 B16
reads and 60.936 GB/s for 65,536 reads with refresh enabled. No extra endpoint
configuration is needed to enable round robin, and no read-slot cap was added.

After building `hbm4_benchmark` and `endpoint_bandwidth` with the environment
shown below, reproduce the production checks with:

```bash
python3.12 pulp/pulp/3d_network/tests/hbm4_checks.py --output "$NETWORK3D_BUILD_DIR/production_checks.json"
NETWORK3D_TARGET=endpoint_bandwidth \
NETWORK3D_RUN_DIR="$NETWORK3D_BUILD_DIR/production_stress" \
    bash pulp/pulp/3d_network/tests/run.sh \
    --parameter=count=128 --parameter=window=8 \
    --parameter=stress=1 --parameter=check_interleaving=1
NETWORK3D_RUN_DIR="$NETWORK3D_BUILD_DIR/production_full" \
    bash pulp/pulp/3d_network/tests/hbm4.sh run
```

## Comparison at identical bus width and clock

The full network measurements below use B16, 512-bit AXI data, 64-bit addresses, 1 GHz,
1,024 terminals, NCA_HASH, source/memory contexts X=8, 4 KiB interleaving and
4 KiB mapped memory per endpoint. Each source reads every destination once,
always at local address 0. Each full run checks 1,048,576 reads and 1 GiB of data.

| Endpoint | Runtime (cycles) | Mean useful GB/s/channel |
|---|---:|---:|
| Simple memory, original 4 read slots, beat round robin | 57,035.5 | 18.385 |
| Simple memory, 8 read slots, beat round robin | 55,714.5 | 18.821 |
| Original HBM4, burst FIFO, no extra read-slot cap | 71,050.5 | 14.758 |
| HBM4 diagnostic, ready-beat round robin, no extra read-slot cap | 56,991.5 | 18.399 |
| HBM4 production, ready-beat round robin, original DRAMSys library | 56,991.5 | 18.399 |

Against the original four-slot RAM policy at the **same** new clock and width,
HBM4 needs 24.57% more cycles and delivers 19.73% less useful bandwidth. The
original RTL document used 64-bit data at 100 MHz; comparing its raw GB/s directly
against HBM4 would mix endpoint effects with changes in clock and bus width.

Changing only HBM4 response arbitration reduces its runtime by **19.79%**.
It comes within **0.08%** of the original four-slot RAM result. Against the
eight-slot RAM control, which can accommodate the same number of unfinished
reads as X=8 HBM4, the HBM4 round-robin variant takes **2.29%** more cycles.
These finite-workload comparisons include traffic phasing, latency, and drain;
they are not a claim that physical HBM4 has RAM-like access latency.

## Isolated endpoint measurements

`tests/endpoint_bandwidth.py` connects an IO_v2 beat traffic source directly to
one memory endpoint, bypassing the NoC. The source maintains a configurable
outstanding-read window, accepts every R beat, and checks data, transaction
identity, beat order, FIRST/LAST, and the one-beat-per-cycle limit. Requests read
the same 1 KiB local range as the full benchmark. These tests use the original
bundled DRAMSys library and, except for the explicitly marked refresh control,
the original prepared HBM4 configuration.

| Memory | Outstanding read window | Reads | Overall GB/s | Middle 75% GB/s |
|---|---:|---:|---:|---:|
| Simple memory | 1 | 4,096 | 64.000 | 64.000 |
| HBM4 | 1 | 4,096 | 40.932 | 40.960 |
| HBM4 | 2 | 4,096 | 63.988 | 64.000 |
| HBM4 | 8 | 4,096 | 63.988 | 64.000 |
| HBM4 | 2 | 65,536 | 58.450 | 57.766 |
| HBM4 | 8 | 65,536 | 60.936 | 60.540 |
| HBM4, refresh disabled for diagnosis only | 8 | 65,536 | 63.999 | 64.000 |

The first HBM4 R beat arrives after 13 cycles, versus one cycle for simple
memory in this probe. Overlapping reads hides that latency: windows 2, 4, 8,
and 16 all deliver every subsequent beat without a gap in the short test.
The 65,536-read test transfers 64 MiB over approximately 1.1 ms and exposes
refresh overhead. Disabling only refresh in a copied diagnostic configuration
removes those gaps. The short result is therefore not a guarantee of 64 GB/s
indefinitely. The configured refresh postponement also affects how much refresh
overhead a finite measurement includes.

This demonstrates that native 32-byte request splitting, the BEGIN_REQ/END_REQ
handshake, and the SystemC/GVSoC bridge do not impose a lower sustained-rate cap
for this access pattern. It does not establish peak bandwidth for arbitrary
row-conflicting, single-pseudochannel, mixed read/write, or low-concurrency traffic.

## Source saturation versus memory saturation

The sources continuously offer reads subject to NI credits, but that does not
mean each DRAM channel continuously has work. In the original X=8 log:

- DRAMSys reports about 14.76 GB/s averaged over the entire simulation.
- Its bandwidth excluding controller-idle time is 39.53–42.17 GB/s, averaging
  40.72 GB/s across channels.
- Those two statistics imply approximately 63.8% controller-idle time on average.
  This estimate uses the rounded values printed by DRAMSys.

In this DRAMSys version, the idle-time counter starts when the controller's
outstanding native payload count becomes zero, and stops on acquisition of the
next payload. Completed read data can still be buffered in `DramsysEndpoint`
while this counter runs. An endpoint's `peak_reads=8` includes those buffered
AXI reads and does not prove that eight reads remain active inside DRAMSys.

The NI retains a memory context until the final R beat enters its output path.
Source contexts remain occupied until the complete response returns. Network
backpressure and context turnover therefore constrain arrival of later reads
at DRAMSys. Enlarging its native request queue cannot fix periods when no native
requests are supplied. The original sweep already shows that increasing both
NI context counts beyond 8 worsens total completion time for this workload.

`response_denials` is not a blocked-cycle counter: the NI initially returns
DENIED while registering each offered R beat, then accepts it through a retry
when its cycle model consumes the beat. One denial per beat is expected with
this integration. The diagnostic endpoint separately measures elapsed time
between first offering a beat and its acceptance.

## Response scheduling and interpretation

The former bridge kept `reads.front()` selected until that burst's LAST
beat is accepted. The simple endpoint advances its round-robin pointer after
every accepted beat. For this benchmark, a FIFO burst sends 16 R packets toward
one destination along the same NCA_HASH path. Interleaving ready beats from
different reads changes the distribution of destinations presented to the NoC
and allows different flows to make progress between beats.

The test-only [patch](hbm4_diagnosis/response_probe.patch) adds an optional ready-beat
round-robin policy to a **copy** of the DRAMSys endpoint. It preserves native
request submission, the C ABI's native completion order, DRAM configuration,
data availability, and the one-beat-per-cycle limit. Once a beat has been
offered and denied, that beat stays stable until accepted. The policy changes
which ready read supplies the following beat.

The instrumented FIFO control reproduces the original 71,050.5 cycles exactly.
Both full HBM4 runs check all 1 GiB of returned data, issue 33,554,432 native
32-byte reads, and finish with no pending endpoint jobs or endpoint request
denials. Mean time spent with an offered R beat waiting for NI acceptance falls
from **44.05 microseconds per channel** with FIFO to **32.85 microseconds** with
round robin. This includes terminal arbitration and NoC backpressure; it is not
a measurement of one particular router bottleneck.

The full diagnostic HBM4 runs used the existing indexed DRAMSys build, whose
queue indexing optimizations preserve scheduling semantics. As an additional
check, the round-robin probe on an 8x8 mesh with 4,096 B16 reads takes exactly
6,530.5 cycles with both the original library and the indexed library. The latter
run enabled its reference-scan assertions. Both policies also pass a 4x4 mixed
read/write, invalid-access, and response-backpressure check. Host times of the
two concurrent full runs are not an isolated simulator-speed comparison.

This is a controller/bridge scheduling choice, not evidence that physical HBM4
is intrinsically slower than the NoC needs. A production policy must match the
intended memory controller and preserve ordering for transactions with the same
AXI ID. The all-to-all benchmark uses distinct transaction identities; it does
not validate every possible ordering constraint of a general controller.

## What to change

1. Ready-beat interleaving is now the endpoint's default. Native DRAM timing and
   readiness still gate every response; each IO_v2 transaction retains its beat
   order and identity. The service policy stays outside `SocInterconnect`.
2. Keep X=8 as the reference for this workload. Change source contexts, memory
   contexts, and DRAM queue sizes independently only when counters show the
   corresponding resource is limiting throughput.
3. Compare routing policies after selecting the intended endpoint arbitration.
   The memory response pattern changes the traffic seen by the routers.
4. Keep refresh and the user's timing values in performance claims. Turning
   refresh off here is a diagnostic control, not a proposed optimization.

Even an ideal memory cannot provide 64 GB/s of **end-to-end useful read data**
per terminal in this benchmark's existing shared injection path. Each B16 read
uses a reservation, grant, AR, and 16 R packets: 19 packets for 16 data beats.
With one injected packet per terminal per cycle, the useful-data ceiling is
`64 × 16 / 19 = 53.895 GB/s/channel`, before routing contention, credit waits,
memory latency, or final drain. Approaching the simple-memory result of roughly
18–19 GB/s/channel is a different target from saturating every HBM data bus.

These diagnostics do not provide RTL-plus-DRAMSys cycle calibration or establish
the original 5% RTL agreement objective for HBM4.

## Reproduce

From the `gvsoc` repository root, use the Python environment and SystemC 2.3.3
C++17 installation described in [the original environment record](hbm4_environment.json).
For a fresh focused build:

```bash
export PYTHON="$PWD/build/network3d/venv/bin/python"
export SYSTEMC_HOME="$PWD/build/network3d_hbm4/systemc"
source sourceme.sh
export LD_LIBRARY_PATH="$SYSTEMC_HOME/lib64:$SYSTEMC_HOME/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export NETWORK3D_BUILD_DIR="$PWD/build/network3d_hbm4_diagnostics_build"
export DRAMSYS_PATH="$NETWORK3D_BUILD_DIR"
"$PYTHON" pulp/pulp/3d_network/tests/prepare_hbm4.py \
    --output "$DRAMSYS_PATH/dramsys_configs"
export NETWORK3D_PROBE_SOURCE="$NETWORK3D_BUILD_DIR/dramsys_endpoint_probe.cpp"
mkdir -p "$NETWORK3D_BUILD_DIR"
gzip -dc pulp/pulp/3d_network/doc/hbm4_diagnosis/dramsys_endpoint_fifo.cpp.gz \
    > "$NETWORK3D_BUILD_DIR/dramsys_endpoint_fifo.cpp"
patch -o "$NETWORK3D_PROBE_SOURCE" \
    "$NETWORK3D_BUILD_DIR/dramsys_endpoint_fifo.cpp" \
    pulp/pulp/3d_network/doc/hbm4_diagnosis/response_probe.patch
NETWORK3D_TARGET='hbm4_benchmark endpoint_bandwidth benchmark hbm4_probe' \
    bash pulp/pulp/3d_network/tests/build.sh

# Single endpoint: short test, then long enough to expose refresh overhead.
NETWORK3D_TARGET=endpoint_bandwidth \
NETWORK3D_RUN_DIR="$NETWORK3D_BUILD_DIR/direct_short" \
    bash pulp/pulp/3d_network/tests/run.sh --parameter=window=8
NETWORK3D_TARGET=endpoint_bandwidth \
NETWORK3D_RUN_DIR="$NETWORK3D_BUILD_DIR/direct_long" \
    bash pulp/pulp/3d_network/tests/run.sh --parameter=window=8 --parameter=count=65536

# Historical A/B: archived FIFO behavior with instrumentation, then ready-beat RR.
NETWORK3D_TARGET=hbm4_probe NETWORK3D_RUN_DIR="$NETWORK3D_BUILD_DIR/fifo" \
    bash pulp/pulp/3d_network/tests/run.sh --parameter=response_rr=0
NETWORK3D_TARGET=hbm4_probe NETWORK3D_RUN_DIR="$NETWORK3D_BUILD_DIR/rr" \
    bash pulp/pulp/3d_network/tests/run.sh --parameter=response_rr=1

# Fair simple-memory baseline. Change readslots to 8 for the second RAM control.
NETWORK3D_TARGET=benchmark NETWORK3D_RUN_DIR="$NETWORK3D_BUILD_DIR/simple" \
    bash pulp/pulp/3d_network/tests/run.sh \
    --parameter=soc=1 --parameter=endpoint=1 --parameter=backing=0 \
    --parameter=frequency=1000000000 --parameter=addrwidth=64 \
    --parameter=datawidth=512 --parameter=burst=16 \
    --parameter=sc=8 --parameter=mc=8 --parameter=readslots=4
```

The test-only `response_rr` parameter belongs to `hbm4_probe`; it is not a
production `DramsysEndpoint` constructor option. The `window` parameter belongs
to the isolated traffic generator and is not an endpoint capacity limiter.
The archived FIFO source has SHA-256
`ee3b398f56c39aa348b8f16f47df370ba59e5c2247c8ffde04c3d2515c8bc09a`.
The standalone endpoint commands above exercise the current production model;
the `hbm4_probe` commands reconstruct the historical diagnostic variant.
Install the compatible bundled library in `third_party/DRAMSys`, as described
in [DRAMSys.md](../../../../DRAMSys.md#io_v2-hbm4-network-benchmark), before running.
Configuration is generated from the canonical source tree. The
[archived configuration](hbm4_config/README.md) remains a record of the measured
inputs; it is not a runtime dependency. Compare its manifest with the generated
manifest when checking whether current source edits changed those inputs.
