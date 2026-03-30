#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../lib/cmake.sh"

TELEPATH_ROOT_DIR="$(telepath_root_dir)"
BUILD_DIR="${TELEPATH_ROOT_DIR}/build/lsan"

telepath_configure_and_build lsan

cat <<EOF
ASAN+LSAN build completed: ${BUILD_DIR}

Note:
- LeakSanitizer may fail to execute under some restricted environments, including
  ptrace-constrained shells, test harnesses, or parts of WSL tooling.
- If that happens, try running the test binary directly outside the harness first.
EOF
