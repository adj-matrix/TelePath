#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/asan"

if [[ ! -d "${BUILD_DIR}" ]]; then
  echo "ASAN build directory not found: ${BUILD_DIR}"
  echo "Run scripts/build/asan.sh first."
  exit 1
fi

ASAN_OPTIONS=detect_leaks=0 ctest --test-dir "${BUILD_DIR}" --output-on-failure
