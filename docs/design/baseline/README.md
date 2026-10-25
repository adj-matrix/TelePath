# Phase 0 Benchmark Baseline

Reference numbers for the kernel refactor, measured on the code as it stood after the P0 race fixes and before any refactor phase. Every refactor phase is compared against these tables.

The raw CSV files named below are kept locally and are not tracked (`.gitignore`); rerun the listed commands to regenerate them.

## Environment

| Item | Value |
| --- | --- |
| CPU | 12th Gen Intel Core i9-12900H, 20 hardware threads |
| Memory | 15 GiB |
| Kernel | 6.18.40.1-microsoft-standard-WSL2 |
| Compiler | GCC 14.3, `-std=c++23`, RelWithDebInfo (`release` preset) |
| Backend | POSIX, `--sync-on-write false` |

WSL2 numbers are suitable for relative comparison between phases on the same machine, not for absolute performance claims.

## 1. Hit Path (`phase0-hit-path.csv`)

Pool size equals the dataset (16384 blocks of 4 KiB), so after preload every access hits. This isolates the in-memory path: page table, descriptor latches, replacer, telemetry.

```
./build/release/test/telepath_benchmark --workload hotspot --pool-size 16384 --block-count 16384 \
  --ops-per-thread 200000 --hotset-size 1024 --hot-access-percent 80 \
  --replacer <lru_k|clock> --write-percent <0|25> --threads <T> --sync-on-write false
```

| Threads | LRU-K Mops/s | LRU-K p99 µs | Clock Mops/s | Clock p99 µs |
| --- | --- | --- | --- | --- |
| 1 | 0.54 | 3.7 | 1.45 | 0.9 |
| 2 | 0.16 | 101.4 | 1.48 | 1.8 |
| 4 | 0.14 | 484.3 | 1.14 | 21.1 |
| 8 | 0.13 | 657.4 | 0.94 | 66.1 |
| 16 | 0.11 | 946.5 | 0.73 | 540.2 |

(Read-only rows; the 25% write rows are within a few percent and are in the CSV.)

**Observation:** throughput falls as threads are added. Every hit takes the replacer's global lock twice (`RecordAccess` and `SetEvictable`); LRU-K additionally performs ordered-set operations under that lock. Telemetry counters share one cache line.

## 2. Mixed Matrix (`phase0-matrix.csv`)

Pool 4096 blocks, dataset 16384 blocks, 50000 operations per thread, hot set 1024 blocks at 80% access probability.

```
THREADS="1 2 4 8 16" WORKLOADS="hotspot uniform scan_pollution" REPLACERS="lru_k clock" \
DISK_BACKENDS=posix WRITE_PERCENTS="0 25" POOL_SIZE=4096 BLOCK_COUNT=16384 \
OPS_PER_THREAD=50000 HOTSET_SIZE=1024 HOT_ACCESS_PERCENT=80 SYNC_ON_WRITE=false \
./scripts/bench/matrix.sh
```

Hotspot, read-only:

| Threads | LRU-K Mops/s | LRU-K p99 µs | Clock Mops/s | Clock p99 µs | Hit rate |
| --- | --- | --- | --- | --- | --- |
| 1 | 0.09 | 118 | 0.10 | 94 | 0.83 |
| 4 | 0.11 | 341 | 0.14 | 325 | 0.84 |
| 16 | 0.12 | 1211 | 0.14 | 1339 | 0.85 |

**Observation:** throughput is flat across thread counts while p99 grows roughly linearly. Misses serialize on the POSIX backend's single worker thread and the single completion dispatcher.
