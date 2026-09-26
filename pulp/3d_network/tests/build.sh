#!/usr/bin/env bash
set -euo pipefail
test_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
gvsoc_root=$(cd "$test_dir/../../../.." && pwd)
build_dir=${NETWORK3D_BUILD_DIR:-"$gvsoc_root/build/network3d"}
python_bin=${PYTHON:-python3}
if [[ -z ${PYTHON:-} && -x "$build_dir/venv/bin/python" ]]; then
    python_bin="$build_dir/venv/bin/python"
fi
mkdir -p "$build_dir/bin"
export USE_GVRUN=1 USE_GVRUN2=1
export PYTHONPATH="$gvsoc_root/engine/python:$gvsoc_root/core/models:$gvsoc_root/pulp:$gvsoc_root/gvrun/python:$gvsoc_root/config_tree${PYTHONPATH:+:$PYTHONPATH}"
export PATH="$build_dir/bin:$PATH"
cat > "$build_dir/bin/gvrun" <<EOF
#!/usr/bin/env bash
exec "$python_bin" -m gvrun "\$@"
EOF
chmod +x "$build_dir/bin/gvrun"
cmake -S "$gvsoc_root/engine" -B "$build_dir/cmake" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$build_dir/install" \
    -DBUILD_DEBUG=OFF -DBUILD_PROFILE=OFF -DBUILD_ASSERT=OFF \
    -DGVSOC_MODULES="$gvsoc_root/engine/python;$gvsoc_root/core/models;$gvsoc_root/pulp" \
    -DGVSOC_EXTRA_MODULES="$test_dir" -DGVSOC_TARGETS="${NETWORK3D_TARGET:-benchmark}"
cmake --build "$build_dir/cmake" --parallel "${JOBS:-8}"
cmake --install "$build_dir/cmake"
printf '%s\n' "Built network benchmark in $build_dir"
