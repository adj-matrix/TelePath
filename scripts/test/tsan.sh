#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../lib/cmake.sh"

TELEPATH_ROOT_DIR="$(telepath_root_dir)"

telepath_configure_and_build tsan

# ThreadSanitizer cannot map its shadow memory on kernels with high mmap
# randomization entropy (recent WSL2 and GitHub runner kernels); running the
# suite with ASLR disabled avoids "unexpected memory mapping" aborts.
if command -v setarch >/dev/null 2>&1; then
  exec 8>"${TELEPATH_ROOT_DIR}/build/.telepath-tests.lock"
  flock 8
  cd "${TELEPATH_ROOT_DIR}"
  setarch "$(uname -m)" -R ctest --preset tsan
else
  telepath_run_tests tsan
fi
