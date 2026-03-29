#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/io_uring_debug"

rm -f "${BUILD_DIR}/CMakeCache.txt"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DTELEPATH_ENABLE_IO_URING=ON
cmake --build "${BUILD_DIR}" -j"$(nproc)"

echo "io_uring debug build completed: ${BUILD_DIR}"
