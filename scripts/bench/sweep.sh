#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BENCH="${ROOT_DIR}/scripts/bench/run.sh"

THREADS="${THREADS:-1 2 4 8}"
WORKLOAD="${WORKLOAD:-hotspot}"
POOL_SIZE="${POOL_SIZE:-256}"
BLOCK_COUNT="${BLOCK_COUNT:-1024}"
OPS_PER_THREAD="${OPS_PER_THREAD:-10000}"
HOTSET_SIZE="${HOTSET_SIZE:-64}"
HOT_ACCESS_PERCENT="${HOT_ACCESS_PERCENT:-80}"
REPLACER="${REPLACER:-lru_k}"
DISK_BACKEND="${DISK_BACKEND:-posix}"
WRITE_PERCENT="${WRITE_PERCENT:-0}"
FLUSH_EVERY_OPS="${FLUSH_EVERY_OPS:-0}"
BACKGROUND_CLEANER="${BACKGROUND_CLEANER:-false}"
DIRTY_PAGE_HIGH_WATERMARK="${DIRTY_PAGE_HIGH_WATERMARK:-0}"
DIRTY_PAGE_LOW_WATERMARK="${DIRTY_PAGE_LOW_WATERMARK:-0}"
FLUSH_WORKERS="${FLUSH_WORKERS:-0}"
FLUSH_SUBMIT_BATCH_SIZE="${FLUSH_SUBMIT_BATCH_SIZE:-0}"
FLUSH_FOREGROUND_BURST_LIMIT="${FLUSH_FOREGROUND_BURST_LIMIT:-0}"
QUEUE_DEPTH="${QUEUE_DEPTH:-0}"
MAX_OPEN_FILES="${MAX_OPEN_FILES:-0}"

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
    --hot-access-percent "${HOT_ACCESS_PERCENT}" \
    --replacer "${REPLACER}" \
    --disk-backend "${DISK_BACKEND}" \
    --write-percent "${WRITE_PERCENT}" \
    --flush-every-ops "${FLUSH_EVERY_OPS}" \
    --background-cleaner "${BACKGROUND_CLEANER}" \
    --dirty-page-high-watermark "${DIRTY_PAGE_HIGH_WATERMARK}" \
    --dirty-page-low-watermark "${DIRTY_PAGE_LOW_WATERMARK}" \
    --flush-workers "${FLUSH_WORKERS}" \
    --flush-submit-batch-size "${FLUSH_SUBMIT_BATCH_SIZE}" \
    --flush-foreground-burst-limit "${FLUSH_FOREGROUND_BURST_LIMIT}" \
    --queue-depth "${QUEUE_DEPTH}" \
    --max-open-files "${MAX_OPEN_FILES}")"

  if [[ ${first} -eq 1 ]]; then
    printf '%s\n' "${output}"
    first=0
  else
    printf '%s\n' "${output}" | tail -n 1
  fi
done
