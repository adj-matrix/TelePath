#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../lib/cmake.sh"

TELEPATH_ROOT_DIR="$(telepath_root_dir)"
BUILD_DIR="${TELEPATH_ROOT_DIR}/build/tsan"

telepath_configure_and_build tsan

echo "TSAN build completed: ${BUILD_DIR}"
