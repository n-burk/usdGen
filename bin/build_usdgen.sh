#!/bin/bash
# Configure + build usdGen. Usage:
#   bin/build_usdgen.sh [--test]      # also run ctest (T0+T1)
#
# Requires ../OpenUSD_26_08 (override with -DUSD_INSTALL_DIR=...).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"
GEN=make

if command -v ninja >/dev/null 2>&1; then
    GEN=ninja
fi

[ -d "$BUILD" ] || cmake -S "$ROOT" -B "$BUILD" -G "$GEN" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" -j"$(nproc)"

if [ "${1:-}" = "--test" ]; then
    ctest --test-dir "$BUILD" -L 'T0|T1' --output-on-failure -j"$(nproc)"
fi
