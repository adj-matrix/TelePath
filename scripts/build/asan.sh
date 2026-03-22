#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/asan"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DTELEPATH_ENABLE_ASAN=ON
cmake --build "${BUILD_DIR}" -j"$(nproc)"

echo "ASAN build completed: ${BUILD_DIR}"
