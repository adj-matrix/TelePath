#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BENCH="${ROOT_DIR}/support/bench.sh"

THREADS="${THREADS:-1 2 4 8}"
WORKLOAD="${WORKLOAD:-hotspot}"
POOL_SIZE="${POOL_SIZE:-256}"
BLOCK_COUNT="${BLOCK_COUNT:-1024}"
OPS_PER_THREAD="${OPS_PER_THREAD:-10000}"
HOTSET_SIZE="${HOTSET_SIZE:-64}"
HOT_ACCESS_PERCENT="${HOT_ACCESS_PERCENT:-80}"

first=1
for t in ${THREADS}; do
  output="$("${BENCH}" \
    --workload "${WORKLOAD}" \
    --output-format csv \
    --threads "${t}" \
    --pool-size "${POOL_SIZE}" \
    --block-count "${BLOCK_COUNT}" \
    --ops-per-thread "${OPS_PER_THREAD}" \
    --hotset-size "${HOTSET_SIZE}" \
    --hot-access-percent "${HOT_ACCESS_PERCENT}")"

  if [[ ${first} -eq 1 ]]; then
    printf '%s\n' "${output}"
    first=0
  else
    printf '%s\n' "${output}" | tail -n 1
  fi
done
