#!/usr/bin/env bash
# Build the warning-clean Qt tree and run every registered test.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

verbose=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    -v | --verbose)
      verbose=1
      shift
      ;;
    -h | --help)
      echo "Usage: $0 [-v|--verbose] [-h|--help]"
      echo "  Runs the qt-dev configure, build, and complete registered CTest suite"
      exit 0
      ;;
    --)
      shift
      break
      ;;
    *)
      echo "Unknown option: $1" >&2
      echo "Usage: $0 [-v|--verbose] [-h|--help]" >&2
      exit 1
      ;;
  esac
done

cmake --preset qt-dev \
  -DBTE_BUILD_TESTS=ON \
  -DCMAKE_COMPILE_WARNING_AS_ERROR=ON
cmake --build --preset qt-dev --parallel

if [[ "${verbose}" -eq 1 ]]; then
  ctest --preset qt-dev --no-tests=error --verbose
else
  ctest --preset qt-dev --no-tests=error
fi
