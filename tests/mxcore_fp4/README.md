# MXCoreFP4 regression and RTL calibration

The [component guide](../../pulp/mxcore_fp4/README.md) specifies the formats,
configuration, layouts, timing scope, and register interface.

## Run the GVSoC regression

Use Python 3.11 or newer and the repository's usual build dependencies. From the
repository root, with its installed tools on PATH and PYTHONPATH:

```sh
make -C pulp/tests/mxcore_fp4 build run
make -C pulp/tests/mxcore_fp4 CASE=delayed build run
make -C pulp/tests/mxcore_fp4 CASE=async build run
make -C pulp/tests/mxcore_fp4 CASE=denied build run
make -C pulp/tests/mxcore_fp4 CASE=error build run
```

These tests are also registered in `pulp/tests/testset.cfg`. Each mode covers all
54 shapes. The normal regression does **not** require Verilator or an RTL
checkout: `fixtures.py` regenerates deterministic vectors and checks their SHA256
hashes against the checked-in RTL measurements before using them. Build products
and large raw traces stay under the ignored `build/` directory.

[validation.json](validation.json) records the 270 GVSoC cases and all 54
nominal RTL/GVSoC cycle comparisons. To regenerate it, save each mode's run
output as `MODE.log` in one directory, then run `check_results.py DIRECTORY`.

The sync case checks every packed result byte, scale byte, TCDM address,
direction, transfer issue cycle, and completion cycle. Other cases check the
model's conservative delay policy, asynchronous completion, retry in the
slave's acceptance window, error propagation, register validation, job acquire,
busy rejection, and completion acknowledgement. They do not establish RTL
timing accuracy under contention.

## Reproduce the RTL measurements

Requirements: a C++17 compiler, Git, Bender 0.28.1, Verilator 5.020 or newer, and
Python 3.11+. The RTL driver and golden generator do not require NumPy or a
commercial simulator license.

```sh
python3 pulp/tests/mxcore_fp4/calibrate.py --fetch --export \
    --bender bender-0.28.1 --verilator verilator
```

On an ETH SEPP installation, select the tool explicitly:

```sh
python3 pulp/tests/mxcore_fp4/calibrate.py --fetch --export \
    --bender bender-0.28.1 --verilator 'verilator-5.020 verilator'
```

The runner clones MXCore at `edf45a65a9c19dcb901eed2148ab9621573a6621`, fetches
the dependencies pinned in its Bender.lock, and applies `rtl/prepare.py`.
`rtl/mxcore.patch` and `rtl/fpnew.patch` show the resulting changes to the
upstream sources. `rtl/mxcore_fp4_quantizer.sv` is the added output quantizer.
FPnew's pinned revision is `eb505fefc554c1d7b842eed66966a4ccf1dfd888`.

`rtl/driver.cpp` instantiates the actual `mxcore_hwpe_wrap`, drives its MMIO
interface, supplies a one-cycle memory, captures output memory, and records
TCDM transfers from the accepted trigger through the completion event.
`reference.py` independently computes tiled GEMM and output quantization.
Two random seeds run per shape, followed by six directed numerical cases; the
directed cases also check the C++ implementation independently of GVSoC.

Verilator's `BLKANDNBLK` diagnostic is disabled for upstream packed structures
whose distinct fields/slices use continuous and sequential assignments. All
warnings remain in the build log. `--no-timing` is appropriate for this external
clock driver; the DUT's logic is clocked by `driver.cpp`, not a timed SV bench.

Only after all RTL results match does `--export` replace `profiles.inc`,
`calibration.json`, and the source patches. Rebuild GVSoC after exporting.
Raw per-shape CSV traces, inputs, results, and logs are retained under
`build/calibration`. Omitting `--export` validates a fresh run without changing
the checked-in evidence. `--rtl`, `--work`, and `--jobs` select alternate paths
and build parallelism. `--skip-build --executable PATH` reuses an already-built
simulator from the same prepared RTL revision.
