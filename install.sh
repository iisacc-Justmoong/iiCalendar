#!/usr/bin/env bash
set -euo pipefail
module_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
prefix="${IICALENDAR_INSTALL_PREFIX:-$HOME/.local/SDK/iiCalendar}"
cmake -S "$module_dir" -B "$module_dir/build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" "$@"
cmake --build "$module_dir/build" --parallel "${BUILD_JOBS:-4}"
if [[ "${IICALENDAR_VALIDATE_INTEGRATION:-0}" == 1 ]]; then
    ctest --test-dir "$module_dir/build" --output-on-failure
else
    ctest --test-dir "$module_dir/build" --output-on-failure \
        -E '^iiCalendar\.(installed_consumer|external_apple)$'
fi
cmake --install "$module_dir/build"
