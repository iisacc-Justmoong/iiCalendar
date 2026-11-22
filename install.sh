#!/usr/bin/env bash
set -euo pipefail
module_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
prefix="${IICALENDARINTEGRATION_INSTALL_PREFIX:-$HOME/.local/SDK/iiCalendarIntegration}"
cmake -S "$module_dir" -B "$module_dir/build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" "$@"
cmake --build "$module_dir/build" --parallel "${BUILD_JOBS:-4}"
if [[ "${IICALENDARINTEGRATION_VALIDATE_INTEGRATION:-0}" == 1 ]]; then
    ctest --test-dir "$module_dir/build" --output-on-failure
else
    ctest --test-dir "$module_dir/build" --output-on-failure \
        -E '^iiCalendarIntegration\.(installed_consumer|external_apple)$'
fi
cmake --install "$module_dir/build"
