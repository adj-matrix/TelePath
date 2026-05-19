# Experiment Guide

## Purpose

This guide describes the experiment path that is mature enough to support paper writing and defense preparation.

The current goal is not to claim absolute storage performance. The goal is to produce repeatable evidence for:

- cache-behavior differences across workloads and replacement policies,
- thread-scaling trends,
- writeback and cleaner behavior under dirty-page pressure,
- operation-level tail latency,
- backend selection and fallback behavior.

## Metrics

The benchmark output reports the following paper-facing metrics:

| Metric | Meaning | Paper Use |
| --- | --- | --- |
| `throughput_ops_per_sec` | Completed logical buffer operations per second | Thread-scaling and configuration comparison |
| `hit_rate` | `buffer_hits / (buffer_hits + buffer_misses)` | Cache policy and workload locality analysis |
| `operation_latency_p50_ns` | Median operation latency | Typical request behavior |
| `operation_latency_p95_ns` | 95th percentile operation latency | Tail-latency and stability discussion |
| `operation_latency_p99_ns` | 99th percentile operation latency | Severe tail behavior |
| `disk_reads` / `disk_writes` | Backend I/O activity observed by telemetry | Cache misses and writeback pressure |
| `dirty_flushes` | Dirty pages successfully flushed | Writeback effectiveness |
| `flush_tasks_scheduled` / `flush_tasks_completed` | Async scheduler activity | Writeback scheduler explanation |
| `cleaner_flushes_*` | Background cleaner activity | Dirty-page control explanation |

Operation latency measures a benchmark operation from `ReadBuffer()` through optional dirty marking and foreground flush completion.

JSON benchmark output also includes a small `sampled_snapshots` array for Web and paper-demo use. These samples are captured while worker threads still hold pages or while runtime I/O state is observable, so they can show pinned, dirty, queued-flush, or in-flight-flush states that may disappear from the final end-of-run snapshot. The Web frame map exposes both the final snapshot and each captured runtime sample through an explicit selector, so a run can be inspected from more than one observation point.

## Observation Export

Benchmark runs can export the same point-in-time buffer-pool snapshot through two paths:

- `--telemetry-export-path <path>` appends one compact JSON document per run as JSONL.
- `--telemetry-shm-name /telepath_name` publishes the latest compact JSON document into a POSIX shared-memory object.

The shared-memory path is meant for observation-plane prototype validation. It proves that the telemetry snapshot format can cross a process boundary without changing the buffer manager hot path. It is not yet a live event stream or a final real-time monitoring protocol.

The `snapshot` field in benchmark JSON is the final point-in-time state after the run. The `sampled_snapshots` field is a bounded sampling aid for presentation and diagnosis, not a full trace. The Web console keeps these views separate rather than replacing the final snapshot with a single automatically selected sample.

Example:

```bash
./scripts/bench/run.sh \
  --output-format json \
  --workload hotspot \
  --replacer lru_k \
  --disk-backend posix \
  --telemetry-shm-name /telepath_benchmark_snapshot \
  --telemetry-shm-capacity 65536
```

## Local Collection

Single focused run:

```bash
./scripts/bench/run.sh \
  --output-format json \
  --workload hotspot \
  --replacer lru_k \
  --disk-backend posix \
  --threads 4 \
  --pool-size 256 \
  --block-count 1024 \
  --ops-per-thread 10000 \
  --hotset-size 64 \
  --hot-access-percent 80
```

Thread sweep:

```bash
THREADS="1 2 4 8" \
WORKLOAD=hotspot \
REPLACER=lru_k \
DISK_BACKEND=posix \
OPS_PER_THREAD=10000 \
./scripts/bench/sweep.sh > /tmp/telepath_hotspot.csv
```

Multi-dimensional matrix:

```bash
THREADS="1 2 4" \
WORKLOADS="hotspot uniform sequential_shared sequential_disjoint" \
REPLACERS="lru_k clock lru two_queue" \
DISK_BACKENDS="posix" \
WRITE_PERCENTS="0 25" \
./scripts/bench/matrix.sh > /tmp/telepath_matrix.csv
```

Markdown summary:

```bash
python3 scripts/bench/summarize.py /tmp/telepath_matrix.csv
```

## Suggested Paper Experiments

### 1. Thread Scaling

Vary `THREADS`, keep all other parameters fixed, and report throughput, hit rate, p95 latency, and p99 latency.

Recommended workloads:

- `hotspot`,
- `uniform`,
- `sequential_shared`,
- `sequential_disjoint`.

This answers whether the concurrent buffer manager scales under different locality patterns.

### 2. Replacement Policy Comparison

Vary `REPLACERS` across:

- `lru_k`,
- `clock`,
- `lru`,
- `two_queue`.

Use the same workload and thread settings for every policy. Report hit rate, throughput, and tail latency together. A policy with a higher hit rate can still have different latency behavior because of bookkeeping and contention.

### 3. Dirty-Page Writeback Pressure

Use write-heavy settings:

```bash
WRITE_PERCENTS="25 50 100" \
FLUSH_EVERY_OPS=0 \
BACKGROUND_CLEANER=true \
DIRTY_PAGE_HIGH_WATERMARK=128 \
DIRTY_PAGE_LOW_WATERMARK=64 \
FLUSH_WORKERS=2 \
./scripts/bench/matrix.sh > /tmp/telepath_dirty.csv
```

Report:

- throughput,
- p95/p99 latency,
- `disk_writes`,
- `dirty_flushes`,
- `flush_tasks_scheduled`,
- `cleaner_flushes_scheduled`,
- final dirty-page snapshot fields.

This explains whether the async writeback path and cleaner reduce dirty-page pressure without blocking every foreground operation.

### 4. Backend Path Behavior

Compare `DISK_BACKENDS="posix io_uring"` only on environments where native `io_uring` is supported.

For WSL2 or restricted CI environments, treat `io_uring_fallback` as a correctness/fallback result rather than a performance claim.

## Validity Boundaries

GitHub-hosted benchmark results are useful for trend data and regression checks, but not final storage-performance claims.

For final paper tables:

- prefer controlled local or lab hardware,
- report CPU, kernel, storage device, filesystem, compiler, and build mode,
- keep benchmark parameters fixed across compared runs,
- avoid comparing native `io_uring` performance from WSL2,
- describe OS page cache effects as a limitation unless bypass/direct-I/O experiments are added later.

## Artifact Checklist

For each paper figure or table, keep:

- the exact command,
- raw CSV or JSON output,
- any JSONL or shared-memory snapshot validation used to explain runtime state,
- summarized Markdown table,
- commit SHA,
- machine and OS description,
- short interpretation notes.

This keeps the experiment section reproducible and makes the final paper easier to audit.
