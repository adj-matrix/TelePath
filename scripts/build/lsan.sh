#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/lsan"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DTELEPATH_ENABLE_ASAN=ON \
  -DTELEPATH_ENABLE_LSAN=ON
cmake --build "${BUILD_DIR}" -j"$(nproc)"

cat <<EOF
ASAN+LSAN build completed: ${BUILD_DIR}

Note:
- LeakSanitizer may fail to execute under some restricted environments, including
  ptrace-constrained shells, test harnesses, or parts of WSL tooling.
- If that happens, try running the test binary directly outside the harness first.
EOF
