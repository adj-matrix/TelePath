#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../lib/cmake.sh"

TELEPATH_ROOT_DIR="$(telepath_root_dir)"
BENCHMARK_PRESET="${BENCHMARK_PRESET:-debug}"
BUILD_DIR="$(telepath_build_dir_for_preset "${BENCHMARK_PRESET}")"
BENCH_BIN="${BUILD_DIR}/test/telepath_benchmark"

telepath_configure_and_build "${BENCHMARK_PRESET}" >&2
"${BENCH_BIN}" "$@"
