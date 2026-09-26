#!/usr/bin/env bash
set -euo pipefail
test_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
gvsoc_root=$(cd "$test_dir/../../../.." && pwd)
export NETWORK3D_BUILD_DIR=${NETWORK3D_BUILD_DIR:-"$gvsoc_root/build/network3d_hbm4"}
export NETWORK3D_TARGET=${NETWORK3D_TARGET:-hbm4_benchmark}
export SYSTEMC_HOME=${SYSTEMC_HOME:-"$gvsoc_root/third_party/systemc_install"}
# The bundled library needs a prepared copy of the canonical configuration.
# Use the same override as memory.dramsys, never a configuration under doc/.
prepare_default_config=0
if [[ -z ${DRAMSYS_PATH:-} ]]; then
    prepare_default_config=1
fi
export DRAMSYS_PATH=${DRAMSYS_PATH:-"$NETWORK3D_BUILD_DIR"}
if [[ -z ${PYTHON:-} && -x "$gvsoc_root/build/network3d/venv/bin/python" ]]; then
    export PYTHON="$gvsoc_root/build/network3d/venv/bin/python"
else
    export PYTHON=${PYTHON:-python3}
fi
export LD_LIBRARY_PATH="$SYSTEMC_HOME/lib64:$SYSTEMC_HOME/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}:$gvsoc_root/third_party/DRAMSys"
if [[ ! -d "$SYSTEMC_HOME/include/sysc" ]]; then
    echo 'Set SYSTEMC_HOME to a SystemC installation compatible with the DRAMSys library (bundled library: 2.3.3, C++17).' >&2
    exit 1
fi
action=${1:-run}
shift || true
if [[ "$action" == build ]]; then
    if [[ "$prepare_default_config" == 1 ]]; then
        "$PYTHON" "$test_dir/prepare_hbm4.py" --output "$DRAMSYS_PATH/dramsys_configs"
    fi
    exec bash "$test_dir/build.sh" "$@"
elif [[ "$action" == run ]]; then
    exec bash "$test_dir/run.sh" "$@"
else
    echo 'Usage: hbm4.sh {build|run} [gvrun arguments]' >&2
    exit 2
fi
