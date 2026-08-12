#!/usr/bin/env bash
# Rebuild the warning-clean Qt desktop application and launch it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

cmake --preset qt-dev \
  -DBTE_BUILD_TESTS=ON \
  -DCMAKE_COMPILE_WARNING_AS_ERROR=ON
cmake --build --preset qt-dev --parallel

exec "$ROOT/Output/qt-dev/Src/App/stockBacktester"
