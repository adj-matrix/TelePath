#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/debug"
BENCH_BIN="${BUILD_DIR}/test/telepath_benchmark"

if [[ ! -x "${BENCH_BIN}" ]]; then
  "${ROOT_DIR}/scripts/build/debug.sh"
fi

"${BENCH_BIN}" "$@"
