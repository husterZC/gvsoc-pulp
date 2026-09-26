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

The benchmark compositions and drivers are in `../benchmarks/`. They are
independent of these regression scripts. `dramsys_config.py` reads commented
DRAMSys JSON for the HBM4 checks and sweep; `soc_memory.hpp` supplies the test
memory used by the kernel checks.

## RTL comparison and protocol checks

```bash
python pulp/pulp/3d_network/tests/validate.py \
    --rtl ../3D-Fattree-Impl --output build/network3d/validation.json
```

Use `--quick` to skip the full 1024-terminal SoC B16 sweeps. The RTL repository
must contain benchmark reference results with matching source hashes. Logs and
generated target configurations go to `build/network3d/validation/`; change
that location with `--work-dir`.

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
reads `soc_context_b16_hbm4_results.json` there and writes Markdown, CSV, a copy
of the results JSON and compressed logs to its `report/` subdirectory. Use
`--input` and `--output-dir` to select other report paths. All three tools honor
`NETWORK3D_BUILD_DIR` for their default paths.

Select DRAMSys inputs with the standard `DRAMSYS_PATH` and `LD_LIBRARY_PATH`.
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

These cover the shared C++ kernels. The GVSoC functional and backpressure tests
cover the IO_v2 component wrappers.
