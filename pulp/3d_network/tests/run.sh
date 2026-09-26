#!/usr/bin/env bash
set -euo pipefail
test_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
gvsoc_root=$(cd "$test_dir/../../../.." && pwd)
build_dir=${NETWORK3D_BUILD_DIR:-"$gvsoc_root/build/network3d"}
python_bin=${PYTHON:-python3}
if [[ -z ${PYTHON:-} && -x "$build_dir/venv/bin/python" ]]; then
    python_bin="$build_dir/venv/bin/python"
fi
run_dir=${NETWORK3D_RUN_DIR:-"$build_dir/run"}
export USE_GVRUN=1 USE_GVRUN2=1
export PYTHONPATH="$build_dir/install/python:$gvsoc_root/core/models:$gvsoc_root/pulp:$gvsoc_root/gvrun/python:$gvsoc_root/config_tree${PYTHONPATH:+:$PYTHONPATH}"
export LD_LIBRARY_PATH="$build_dir/install/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PATH="$build_dir/install/bin:$PATH"
exec "$python_bin" -m gvrun --target="${NETWORK3D_TARGET:-benchmark}" \
    --target-dir="$test_dir" --target-dir="$gvsoc_root/core/models" \
    --target-dir="$gvsoc_root/pulp" --target-dir="$build_dir/install/python" \
    --model-dir="$build_dir/install/models" --builddir="$run_dir" --work-dir="$run_dir" \
    --installdir="$build_dir/install" "$@" run
