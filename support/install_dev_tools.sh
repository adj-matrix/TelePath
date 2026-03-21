#!/usr/bin/env bash

set -euo pipefail

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

apt-get update
apt-get install -y \
  clang \
  clang-format \
  clang-tidy \
  doxygen \
  gdb \
  lldb \
  valgrind

cat <<'EOF'
Installed TelePath development tools.

Notes:
- ASAN and LSAN are compiler/runtime sanitizers, not standalone apt packages.
- Enable them with CMake options, for example:
    cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug \
      -DTELEPATH_ENABLE_ASAN=ON -DTELEPATH_ENABLE_LSAN=ON
EOF
