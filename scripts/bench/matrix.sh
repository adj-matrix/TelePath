#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../lib/cmake.sh"

TELEPATH_ROOT_DIR="$(telepath_root_dir)"
BENCHMARK_PRESET="${BENCHMARK_PRESET:-release}"
BUILD_DIR="$(telepath_build_dir_for_preset "${BENCHMARK_PRESET}")"
BENCH="${BUILD_DIR}/test/telepath_benchmark"

THREADS="${THREADS:-1 2 4 8}"
WORKLOADS="${WORKLOADS:-hotspot uniform sequential_shared sequential_disjoint scan_pollution}"
REPLACERS="${REPLACERS:-lru_k clock clock_sweep sieve lru two_queue}"
DISK_BACKENDS="${DISK_BACKENDS:-posix}"
WRITE_PERCENTS="${WRITE_PERCENTS:-0 25}"
POOL_SIZE="${POOL_SIZE:-256}"
BLOCK_COUNT="${BLOCK_COUNT:-1024}"
OPS_PER_THREAD="${OPS_PER_THREAD:-10000}"
HOTSET_SIZE="${HOTSET_SIZE:-64}"
HOT_ACCESS_PERCENT="${HOT_ACCESS_PERCENT:-80}"
FLUSH_EVERY_OPS="${FLUSH_EVERY_OPS:-0}"
BACKGROUND_CLEANER="${BACKGROUND_CLEANER:-false}"
DIRTY_PAGE_HIGH_WATERMARK="${DIRTY_PAGE_HIGH_WATERMARK:-0}"
DIRTY_PAGE_LOW_WATERMARK="${DIRTY_PAGE_LOW_WATERMARK:-0}"
FLUSH_WORKERS="${FLUSH_WORKERS:-0}"
FLUSH_SUBMIT_BATCH_SIZE="${FLUSH_SUBMIT_BATCH_SIZE:-0}"
FLUSH_FOREGROUND_BURST_LIMIT="${FLUSH_FOREGROUND_BURST_LIMIT:-0}"
FLUSH_ALL_AFTER_RUN="${FLUSH_ALL_AFTER_RUN:-false}"
QUEUE_DEPTH="${QUEUE_DEPTH:-0}"
MAX_OPEN_FILES="${MAX_OPEN_FILES:-0}"
SYNC_ON_WRITE="${SYNC_ON_WRITE:-true}"
BULK_READ_SCANS="${BULK_READ_SCANS:-false}"
PREFETCH_DISTANCE="${PREFETCH_DISTANCE:-0}"

telepath_configure_and_build "${BENCHMARK_PRESET}" >&2

first=1
for workload in ${WORKLOADS}; do
  for replacer in ${REPLACERS}; do
    for disk_backend in ${DISK_BACKENDS}; do
      for write_percent in ${WRITE_PERCENTS}; do
        for threads in ${THREADS}; do
          output="$("${BENCH}" \
            --output-format csv \
            --workload "${workload}" \
            --replacer "${replacer}" \
            --disk-backend "${disk_backend}" \
            --threads "${threads}" \
            --pool-size "${POOL_SIZE}" \
            --block-count "${BLOCK_COUNT}" \
            --ops-per-thread "${OPS_PER_THREAD}" \
            --hotset-size "${HOTSET_SIZE}" \
            --hot-access-percent "${HOT_ACCESS_PERCENT}" \
            --write-percent "${write_percent}" \
            --flush-every-ops "${FLUSH_EVERY_OPS}" \
            --background-cleaner "${BACKGROUND_CLEANER}" \
            --dirty-page-high-watermark "${DIRTY_PAGE_HIGH_WATERMARK}" \
            --dirty-page-low-watermark "${DIRTY_PAGE_LOW_WATERMARK}" \
            --flush-workers "${FLUSH_WORKERS}" \
            --flush-submit-batch-size "${FLUSH_SUBMIT_BATCH_SIZE}" \
            --flush-foreground-burst-limit "${FLUSH_FOREGROUND_BURST_LIMIT}" \
            --flush-all-after-run "${FLUSH_ALL_AFTER_RUN}" \
            --queue-depth "${QUEUE_DEPTH}" \
            --sync-on-write "${SYNC_ON_WRITE}" \
            --bulk-read-scans "${BULK_READ_SCANS}" \
            --prefetch-distance "${PREFETCH_DISTANCE}" \
            --max-open-files "${MAX_OPEN_FILES}")"

          if [[ ${first} -eq 1 ]]; then
            printf '%s\n' "${output}"
            first=0
          else
            printf '%s\n' "${output}" | tail -n 1
          fi
        done
      done
    done
  done
done
