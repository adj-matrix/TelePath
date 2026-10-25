#!/usr/bin/env bash

set -euo pipefail

TELEPATH_LLVM_VERSION="${TELEPATH_LLVM_VERSION:-19}"

if [[ "${EUID}" -ne 0 ]]; then
  echo "Please run this script with sudo."
  exit 1
fi

if [[ ! -f /etc/os-release ]]; then
  echo "Unsupported environment: /etc/os-release not found."
  exit 1
fi

. /etc/os-release

if [[ "${ID:-}" != "ubuntu" ]]; then
  echo "Unsupported distribution: ${ID:-unknown}."
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"${SCRIPT_DIR}/install_build_deps.sh"

apt-get install -y \
  "clangd-${TELEPATH_LLVM_VERSION}" \
  "clang-format-${TELEPATH_LLVM_VERSION}" \
  "clang-tidy-${TELEPATH_LLVM_VERSION}" \
  "lldb-${TELEPATH_LLVM_VERSION}" \
  doxygen \
  gdb \
  valgrind

cat <<EOF
Installed TelePath development tools.

Notes:
- LLVM tools are versioned (clangd-${TELEPATH_LLVM_VERSION}, clang-tidy-${TELEPATH_LLVM_VERSION}, ...).
- ASAN, LSAN, and TSAN are compiler/runtime sanitizers, not standalone apt packages.
- Enable them through the CMake presets, for example:
    ./scripts/build/asan.sh
    ./scripts/build/tsan.sh
EOF
