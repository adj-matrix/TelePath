#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../lib/cmake.sh"

TELEPATH_ROOT_DIR="$(telepath_root_dir)"
BUILD_DIR="${TELEPATH_ROOT_DIR}/build/io_uring_debug"

telepath_configure_and_build io_uring_debug

echo "io_uring debug build completed: ${BUILD_DIR}"
