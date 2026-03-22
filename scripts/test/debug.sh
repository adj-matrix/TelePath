#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/debug"

if [[ ! -d "${BUILD_DIR}" ]]; then
  echo "Build directory not found: ${BUILD_DIR}"
  echo "Run scripts/build/debug.sh first."
  exit 1
fi

ctest --test-dir "${BUILD_DIR}" --output-on-failure
