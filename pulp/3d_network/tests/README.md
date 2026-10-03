# Developer regression tools

First follow the four-step workflow in the [module README](../README.md) to
set up the environment, build DRAMSys if needed, and build the public targets.
Run the commands below from the `gvsoc/` root. Simulation checks use the
installed `gvrun` command and the `network3d*` targets.

| Tool | Purpose |
|---|---|
| `validate.py` | Compare traffic with RTL references and check the IO_v2 protocol |
| `rtl_holdout.py` | Generate additional RTL references; requires the RTL repository and Questa |
| `hbm4_checks.py` | Functional, width, backpressure and timing-sensitivity checks with DRAMSys |
| `hbm4_sweep.py` | B16 context sweep with source/configuration hashes and checked results |
| `report_hbm4.py` | Format a completed HBM4 sweep as Markdown and CSV, with compressed logs |
| `kernel_checks.cpp` | Standalone kernel memory-safety and reset checks |
| `memory_storage_checks.cpp` | Sparse RAM, byte strobes, 64-bit addressing and direct file preload checks |

The benchmark compositions and drivers are in `../benchmarks/`. They are
independent of these regression scripts. `dramsys_config.py` reads commented
DRAMSys JSON for the HBM4 checks and sweep; `i3d_memory.hpp` supplies the test
memory used by the kernel checks.

## RTL comparison and protocol checks

```bash
python pulp/pulp/3d_network/tests/validate.py \
    --rtl ../3D-Fattree-Impl --output build/network3d/validation.json
```

Use `--quick` to skip the full 1024-terminal I3D B16 sweeps. The RTL repository
must contain benchmark reference results with matching source hashes. Logs and
generated target configurations go to `build/network3d/validation/`; change
that location with `--work-dir`.

RTL reference filenames and imports keep their names from the separate RTL
repository. The GVSoC model, target parameters and generated HBM4 reports use
the `i3d` naming.

The protocol checks also exercise `fabric=2` with 1, 6, 63 and 1024 terminals,
multiple spill depths, mixed read/write traffic and 256-beat bursts. Crossbar
coverage is functional and timing-contract validation; the RTL SoC wrapper
does not expose a crossbar fabric for a full I3D cycle comparison.

To generate additional native reference cases with Questa and include them:

```bash
python pulp/pulp/3d_network/tests/rtl_holdout.py \
    --rtl ../3D-Fattree-Impl --output-dir build/network3d/rtl
python pulp/pulp/3d_network/tests/validate.py \
    --rtl ../3D-Fattree-Impl --native-reference build/network3d/rtl/results.json \
    --output build/network3d/validation.json
```

## HBM4 checks and measurements

```bash
python pulp/pulp/3d_network/tests/hbm4_checks.py
python pulp/pulp/3d_network/tests/hbm4_sweep.py
python pulp/pulp/3d_network/tests/report_hbm4.py
```

The checks and sweep write results under `build/network3d_hbm4/`. The formatter
reads `i3d_context_b16_hbm4_results.json` there and writes Markdown, CSV, a copy
of the results JSON and compressed logs to its `report/` subdirectory. Use
`--input` and `--output-dir` to select other report paths. All three tools honor
`NETWORK3D_BUILD_DIR` for their default paths.

Select DRAMSys inputs with the standard `DRAMSYS_PATH` and `LD_LIBRARY_PATH`.
The checks and sweep use `hbm4-emu-fast.json`, matching the benchmark targets'
default with database recording, windowing and the progress bar disabled.
Timing and queue-capacity checks modify copies under `build/`. The module's
`doc/` directory is reserved for Markdown model documentation.

## Kernel checks

```bash
mkdir -p build/network3d
g++ -O1 -g -std=c++17 -fsanitize=address,undefined \
    -fno-omit-frame-pointer pulp/pulp/3d_network/tests/kernel_checks.cpp \
    -o build/network3d/kernel_checks
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 build/network3d/kernel_checks
```

These cover the shared C++ kernels, including crossbar latency, parallel
throughput, contention, arbitration locks and reset. The GVSoC functional and
backpressure tests cover the IO_v2 component wrappers.

The RAM storage checks require no GVSoC installation or DRAMSys library:

```bash
g++ -O1 -g -std=c++17 -fsanitize=address,undefined \
    -fno-omit-frame-pointer pulp/pulp/3d_network/tests/memory_storage_checks.cpp \
    -o build/network3d/memory_storage_checks
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 build/network3d/memory_storage_checks
```

These compare randomized unaligned/strobed writes against a dense reference,
check file fragments and zero-fill over the benchmark pattern, reject invalid
ranges, and exercise 1,024 logical 128-MiB channels without dense allocation.
