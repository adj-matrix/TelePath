# Test Guide

## Purpose

TelePath keeps its test suite focused on two goals:

- verifying correctness under normal and adversarial conditions,
- keeping benchmark workload semantics explainable and regression-friendly.

The suite is meant to stay broad enough to catch real regressions without turning into a collection of near-duplicate cases.

## Current Layout

The C++ tests live under `test/cpp/` and are grouped by subsystem:

- `benchmark/`
- `buffer/`
- `config/`
- `io/`
- `replacer/`
- `telemetry/`

This keeps the tests close to the structure of the library itself and makes future additions easier to place consistently.

## Current Coverage

The current suite covers:

- smoke-path lifecycle behavior,
- handle and pin/unpin semantics,
- concurrent read, same-page miss ownership, and miss recovery,
- eviction, `FlushBuffer()`, and `FlushAll()` behavior,
- dirty-victim writeback rollback and retry behavior,
- async flush scheduling and fairness,
- cleaner-triggered background writeback and retry behavior,
- flush consistency under re-dirty races,
- writeback failure handling at both submit time and completion time,
- failure and resource exhaustion paths,
- disk backend correctness,
- io engine completion delivery,
- asynchronous loads (prefetch, multi-block reads),
- replacer correctness,
- telemetry correctness,
- telemetry JSONL and shared-memory snapshot export shape,
- options resolution,
- benchmark workload, experiment-parameter, sampled-snapshot, Web snapshot-selector, and operation-latency summary semantics.

## Coverage Matrix

The current behavior-oriented coverage map is:

| Area | Primary Tests | Notes |
| --- | --- | --- |
| Basic buffer lifecycle | `smoke_test`, `init_failure_test`, `resource_exhaustion_test`, `fatal_state_test` | Covers `Create()` failures (invalid page size, backend block-size mismatch, missing dependencies), basic read, release, no-victim handling, and the Failed state. |
| Handle semantics | `handle_test`, `memory_layout_test`, `frame_snapshot_test` | Covers handle release (`ReleaseBuffer`, `Reset`) and moves, contiguous frame layout, and exported metadata. |
| Frame control word | `frame_control_test`, `block_geometry_test` | Covers pin/reserve CAS rules, latch exclusion, dirty clearing with `redirtied`, futex wake-ups, and block-size validation. |
| Concurrency stress | `stress_integrity_test`, `eviction_race_test`, `page_table_test` | Covers torn-page and lost-update detection under sustained mixed load, eviction/writeback interleavings, the replacer contract, and newest-mapping-first page table lookups. |
| Read miss coordination | `same_page_miss_test`, `same_page_miss_failure_recovery_test`, `different_page_parallel_miss_test` | Covers joined same-page misses, failure recovery, and independent parallel misses. |
| Eviction/writeback | `eviction_test`, `dirty_eviction_failure_recovery_test`, `failure_path_test`, `wait_evict_interleave_test` | Covers dirty victim persistence, failed victim writeback rollback, retry behavior, read/write failure propagation, and waiter/eviction interleaving. |
| Flush correctness | `flush_consistency_test`, `read_lock_flush_test`, `flush_all_persistence_test` | Covers writer-held flush waiting, read-latch flush progress, and `FlushAll()` persistence. |
| Async flush scheduler | `async_flush_scheduler_test` | Covers worker-owned flushes, batching, foreground/background interaction, cleaner ownership, retries, and in-flight `FlushAll()`. |
| Completion delivery | `io_engine_test`, `completion_order_test` | Covers token routing, out-of-order and counted completions, waiters reaping their own inline completions, rejected submissions, fatal reaper failure, an idle reaper that does not spin, and reordered read completions. |
| Asynchronous loads | `async_read_test` | Covers `PrefetchBuffer()` (unpinned loads, skipping resident blocks, stopping when the pool is pinned), `StartReadBuffers()`/`WaitReadBuffers()` (handles in block order, combined multi-block requests, failure releasing every pin, abandoned operations, misuse), and prefetch racing ordinary readers. |
| Disk backends | `disk_backend_test`, `disk_backend_factory_test`, `read_zero_fill_test`, `io_uring_*` | Covers malformed requests, inline `kWait` completion, multi-block reads and writes with zero fill past end-of-file, batched submission, shutdown draining, the descriptor cache, factory policy, and native/stub `io_uring` paths. |
| Replacement policy | `replacer_test`, `lru_k_behavior_test`, `two_queue_behavior_test`, `clock_family_behavior_test`, `replacer_concurrency_test` | Covers probe-filtered nomination for all six policies (Clock, clock-sweep, SIEVE, LRU, LRU-K, 2Q), policy-specific ordering, and concurrent access/nomination/reload. |
| Access strategies | `buffer_access_strategy_test` | Covers bulk-read scans keeping the hot set resident within the ring bound, and that plain LRU is polluted without a strategy. |
| Telemetry/options/benchmark/Web | `telemetry_test`, `options_test`, `benchmark_*`, `web_*` | Covers counter snapshots, JSONL telemetry export, POSIX shared-memory telemetry snapshot export, malformed shared-memory headers/bounds, flush/cleaner/eviction telemetry, option resolution, workload selection, experiment knob parsing, operation-latency summaries, final and sampled runtime snapshots, selectable Web snapshot views, snapshot aggregates, and JSON output shape. |

The remaining intentional gaps are larger-scale rather than unit-test-sized:

- long-running stress/soak tests under sustained dirty-page pressure,
- native `io_uring` performance validation on real hardware,
- live shared-memory event streaming and external consumer behavior,
- crash/recovery semantics, which are outside the current project scope,
- cross-run latency stability assertions, which should come from benchmark artifact analysis rather than brittle unit tests.

In addition to the baseline suite, native Linux CI runs a separate `io_uring`-only test group so that kernel-sensitive validation does not get mixed into the normal fallback path.

## How To Run

TSAN validation (Clang 19; the script disables ASLR, which TSAN needs on recent kernels):

```bash
./scripts/build/tsan.sh
./scripts/test/tsan.sh
```

Release validation (assertions stay active in tests):

```bash
./scripts/build/release.sh
ctest --preset release
```

Debug validation:

```bash
./scripts/build/debug.sh
./scripts/test/debug.sh
```

ASAN validation:

```bash
./scripts/build/asan.sh
./scripts/test/asan.sh
```

Native `io_uring` validation on supported Linux kernels:

```bash
./scripts/build/io_uring_debug.sh
./scripts/test/io_uring_native.sh
```

## Benchmark Note

Benchmark correctness is tested separately from benchmark performance output.

In practice this means:

- normal tests validate workload-selection logic,
- benchmark tests validate experiment parameter parsing and output shape,
- benchmark output reports operation-level min/average/p50/p95/p99/max latency,
- JSON benchmark output includes bounded runtime snapshot samples, and the Web frame map can switch between the final snapshot and sampled runtime snapshots without turning the benchmark into a full tracing system,
- `scripts/bench/matrix.sh` emits clean CSV for reproducible matrix collection,
- `scripts/bench/summarize.py` converts CSV artifacts into Markdown summaries for paper-facing tables,
- GitHub benchmark workflows collect trend data,
- benchmark numbers are useful for regression observation, not final performance claims.

## Maintenance Rule

New tests should prefer one of the following:

- a new boundary,
- a new concurrency shape,
- a new failure mode,
- a new workload semantic guarantee.

Avoid adding tests that only restate behavior already proven elsewhere.

Recent high-value examples include:

- a dirty victim whose first writeback fails, rolls the frame back without losing data, and then evicts cleanly on retry,
- a batched flush where the backend rejects one page at submission and the other still persists correctly,
- a cleaner-owned flush that fails, gets requeued, and succeeds on retry,
- a `FlushAll()` caller waiting on cleaner-owned in-flight writeback without duplicate submission,
- foreground/background flush interaction without brittle timing assumptions.
