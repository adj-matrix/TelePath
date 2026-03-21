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
  build-essential \
  cmake \
  pkg-config

echo "Installed TelePath build dependencies."
