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
- async flush scheduling and fairness,
- cleaner-triggered background writeback,
- flush consistency under re-dirty races,
- writeback failure handling at both submit time and completion time,
- failure and resource exhaustion paths,
- disk backend correctness,
- completion dispatcher behavior,
- replacer correctness,
- telemetry correctness,
- options resolution,
- benchmark workload semantics.

## Coverage Matrix

The current behavior-oriented coverage map is:

| Area | Primary Tests | Notes |
| --- | --- | --- |
| Basic buffer lifecycle | `smoke_test`, `init_failure_test`, `resource_exhaustion_test` | Covers construction failure surfacing, basic read, release, and no-victim handling. |
| Handle semantics | `handle_test`, `memory_layout_test`, `frame_snapshot_test` | Covers RAII release, invalid handles, contiguous frame layout, and exported metadata. |
| Read miss coordination | `same_page_miss_test`, `same_page_miss_failure_recovery_test`, `different_page_parallel_miss_test` | Covers joined same-page misses, failure recovery, and independent parallel misses. |
| Eviction/writeback | `eviction_test`, `failure_path_test`, `wait_evict_interleave_test` | Covers dirty victim persistence, read/write failure propagation, and waiter/eviction interleaving. |
| Flush correctness | `flush_consistency_test`, `read_lock_flush_test`, `flush_all_persistence_test` | Covers writer-held flush waiting, read-latch flush progress, and `FlushAll()` persistence. |
| Async flush scheduler | `async_flush_scheduler_test` | Covers worker-owned flushes, batching, foreground/background interaction, cleaner ownership, retries, and in-flight `FlushAll()`. |
| Completion dispatch | `completion_dispatcher_test`, `completion_dispatcher_idle_test`, `completion_order_test` | Covers request-id routing, out-of-order completion, early completion before registration, backend failure, idle shutdown, and reordered read completions. |
| Disk backends | `disk_backend_test`, `disk_backend_factory_test`, `read_zero_fill_test`, `io_uring_*` | Covers POSIX fallback behavior, factory policy, zero-fill reads, and native/stub `io_uring` paths. |
| Replacement policy | `replacer_test`, `lru_k_behavior_test`, `two_queue_behavior_test` | Covers shared interface expectations and policy-specific behavior. |
| Telemetry/options/benchmark | `telemetry_test`, `options_test`, `benchmark_*` | Covers counter snapshots, option resolution, workload selection, and JSON output shape. |

The remaining intentional gaps are larger-scale rather than unit-test-sized:

- long-running stress/soak tests under sustained dirty-page pressure,
- native `io_uring` performance validation on real hardware,
- crash/recovery semantics, which are outside the current project scope,
- latency distribution assertions, which should come from benchmark/telemetry work rather than brittle unit tests.

In addition to the baseline suite, native Linux CI runs a separate `io_uring`-only test group so that kernel-sensitive validation does not get mixed into the normal fallback path.

## How To Run

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

- a batched flush where one page fails during `SubmitWrite()` and the other still persists correctly,
- a `FlushAll()` caller waiting on cleaner-owned in-flight writeback without duplicate submission,
- foreground/background flush interaction without brittle timing assumptions.
