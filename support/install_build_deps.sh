#!/usr/bin/env bash

set -euo pipefail

TELEPATH_GCC_VERSION="${TELEPATH_GCC_VERSION:-14}"
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

has_candidate() {
  apt-cache policy "$1" 2>/dev/null | grep -q 'Candidate: [^(]'
}

ensure_gcc_repository() {
  has_candidate "g++-${TELEPATH_GCC_VERSION}" && return
  add-apt-repository -y ppa:ubuntu-toolchain-r/test
  apt-get update
}

ensure_llvm_repository() {
  has_candidate "clang-${TELEPATH_LLVM_VERSION}" && return
  local keyring=/usr/share/keyrings/apt.llvm.org.gpg
  wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor --yes -o "${keyring}"
  echo "deb [signed-by=${keyring}] http://apt.llvm.org/${VERSION_CODENAME}/ llvm-toolchain-${VERSION_CODENAME}-${TELEPATH_LLVM_VERSION} main" \
    > "/etc/apt/sources.list.d/llvm-toolchain-${TELEPATH_LLVM_VERSION}.list"
  apt-get update
}

apt-get update
apt-get install -y \
  build-essential \
  cmake \
  gnupg \
  liburing-dev \
  pkg-config \
  software-properties-common \
  wget

ensure_gcc_repository
ensure_llvm_repository

apt-get install -y \
  "g++-${TELEPATH_GCC_VERSION}" \
  "clang-${TELEPATH_LLVM_VERSION}"

echo "Installed TelePath build dependencies (g++-${TELEPATH_GCC_VERSION}, clang-${TELEPATH_LLVM_VERSION})."
