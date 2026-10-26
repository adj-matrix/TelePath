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
| Randomized chaos | `chaos_test` | Runs most of the kernel API (shared and exclusive reads, `kZeroAndLock`, bulk-read strategies, `StartReadBuffers()`, `PrefetchBuffer()`, every flush and sync entry point, `CheckpointBuffers()`, and `DropFileBuffers()` on a scratch file read concurrently) from several threads with injected read, write, and sync faults, for every replacer, both foreground writeback modes, pools of one and two frames, the io_uring backend (skipped where unavailable unless `TELEPATH_REQUIRE_IO_URING_SUCCESS` is set), and two processes sharing one segment and one model. Every read must match a model of each page's write counter; afterwards the dirty-page counter must settle to the number of dirty frames, the pool must be clean and unpinned, and a reopened manager must read the same values. `TELEPATH_CHAOS_SEED`, `TELEPATH_CHAOS_CONFIG`, `TELEPATH_CHAOS_SKIP`, and `TELEPATH_CHAOS_NO_FAULTS` vary and narrow a run. |
| Lifecycle limits | `lifecycle_stress_test`, `init_failure_test` | Covers `Close()` racing with threads that keep calling in (it may refuse, but never hangs, and afterwards every call returns `kUnavailable`), the 255-holder shared-latch limit of a page, and pools beyond the 32-bit frame id range being rejected before allocation. |
| Concurrency primitives | `primitives_stress_test` | Covers the lock-free free list handing each frame to one owner (one to three frames shared by eight threads, where an ABA defect shows), the process-shared futex mutex losing no increment between threads or between processes, page-table lookups finding every mapping that stayed stable during the lookup and never a replaced one while writers move mappings across colliding stripes, and the I/O engine delivering every completion exactly once with waiter errors matching their requests through a backend that completes out of order, fails requests, and rejects or partly accepts submissions. |
| Basic buffer lifecycle | `smoke_test`, `init_failure_test`, `resource_exhaustion_test`, `fatal_state_test` | Covers `Create()` failures (invalid page size, backend block-size mismatch, missing dependencies), basic read, release, no-victim handling, and the Failed state. |
| Handle semantics | `handle_test`, `memory_layout_test`, `frame_snapshot_test` | Covers handle release (`ReleaseBuffer`, `Reset`) and moves, contiguous frame layout, and exported metadata. |
| Frame control word | `frame_control_test`, `block_geometry_test` | Covers pin/reserve CAS rules, latch exclusion, dirty clearing with `redirtied`, futex wake-ups, and block-size validation. |
| Concurrency stress | `stress_integrity_test`, `eviction_race_test`, `page_table_test` | Covers torn-page and lost-update detection under sustained mixed load, eviction/writeback interleavings, the replacer contract, and newest-mapping-first page table lookups. |
| Read miss coordination | `same_page_miss_test`, `same_page_miss_failure_recovery_test`, `different_page_parallel_miss_test`, `page_table_test` | Covers readers waiting on a loading frame, retry after a failed load, independent parallel misses, and the conditional install that keeps one load per page while ignoring stale mappings. |
| Foreground writeback | `foreground_writeback_test`, `async_flush_scheduler_test` | Covers direct mode writing `FlushBuffer()` and dirty eviction victims on the calling thread and keeping a page dirty after a failed write, and queued mode routing them through flush workers with the foreground/background ordering and burst limit. |
| MLPrefetchBuffer facade | `ml_prefetch_buffer_test`, `plan_replacer_test` | Covers a miss count equal to Belady's optimum, `Next()` handing out every position exactly once across threads, deterministic `Fetch()` partitions, prefetching ahead, new plans, out-of-range positions, `Close()`, empty plans, blocks past the end of the dataset, prefetching deeper than the pool, many threads fetching random positions from a three-frame pool, and the replacer's furthest-next-use ordering. |
| PageCache facade | `page_cache_test`, `page_cache_model_test` | Covers unaligned reads and writes with logical sizes, zero-filled gaps, whole-block writes that skip disk reads, `Sync()` persisting data and the exact size across a reopen, truncation discarding data and reading zeros after extension, readahead on sequential reads only, concurrent writers and readers, `Close()` (including a failed sync keeping the cache open for a retry), and a failed multi-block write whose size covers the written prefix so no hidden bytes surface in a later gap. `page_cache_model_test` checks random writes (across block boundaries, far past the end, zero-length), reads (short at the end, none past it), truncations, and syncs by several threads against a byte model per file, with a pool small enough to evict constantly, and again after reopening. |
| Durability | `durability_test` | Covers `kZeroAndLock` never exposing its zero-filled page and dropping a page right after its prefetch completes (both deterministic through telemetry hooks in the critical window), `FlushAll()` syncing each written file once, retrying a failed sync, `Close()` flushing and then rejecting calls (and refusing while pages are pinned), and the pre-write hook seeing the snapshot and failing a writeback, a failed `Close()` keeping the manager open for a retry, plus `kZeroAndLock`, per-file `FlushFileBuffers()`/`SyncFile()`, and `DropFileBuffers()` discarding dirty pages and refusing pinned ones. |
| WAL support | `rec_lsn_test`, `rec_lsn_stress_test` | Covers the earliest LSN since the last writeback becoming a page's recLSN, untracked changes without an LSN, `FlushBuffer()` resetting it, `CheckpointBuffers()` writing only pages below the target and syncing them, a change made during a writeback (held at the snapshot's recLSN until completion, then moved to the new change's LSN), a failed or refused writeback keeping the recLSN, and dropped pages forgetting it. `shared_memory_test` and `observation_test` check the minimum across processes and in the observation header. `rec_lsn_stress_test` pauses concurrent writers, under writeback, eviction, and write faults, to check that `MinRecLsn()` never exceeds the LSN of a change not yet on disk and that after `CheckpointBuffers(lsn)` every change below `lsn` is on disk. |
| Cross-process mode | `shared_memory_test`, `shared_memory_stress_test` | Runs a second process by re-executing the test binary through `/proc/self/exe`: the child sees a page the parent dirtied without a disk read and vice versa, both processes rewrite disjoint halves of a working set four times the pool size concurrently and every value survives eviction and reload, `Close()` writes back the other process's dirty page, `kCreate`/`kAttach` errors, shape and replacer-kind mismatches, removal with the last manager, and a persistent segment removed with `RemoveSharedMemory()`. `shared_memory_stress_test` covers exhausting the 64 process slots, foreign objects under the segment name, eight processes racing to create one segment, attaching while the last manager removes the name, and files written by a process that exits before syncing, beyond the 1024-entry shared table, still being synced. |
| Observation plane | `observation_test`, `observation_reader_test`, `observation_stress_test` | `observation_test` reads the published area through its own read-only mapping: summary, per-frame records, and per-process counters after `PublishObservation()`, background publishing on the interval, removal with the last user, a second user keeping the name alive, pool-size and name validation, and a re-executed child process of a cross-process cache that adds its own counter record while the parent stays the frame publisher. It also runs the Python reader on the live area. `observation_reader_test` decodes a hand-built area and rejects torn snapshots, unpublished, uninitialized, and missing areas, and malformed names, and checks the Web route's 200, 400, and 404 answers. `observation_stress_test` has a C++ and the Python reader in other processes check every snapshot for consistency while frames churn, kills the publishing process and checks that another takes over, exhausts the 64 process records and reclaims one from a dead process, and rejects a foreign object. |
| Eviction/writeback | `eviction_test`, `dirty_eviction_failure_recovery_test`, `failure_path_test`, `wait_evict_interleave_test` | Covers dirty victim persistence, failed victim writeback rollback, retry behavior, read/write failure propagation, and waiter/eviction interleaving. |
| Flush correctness | `flush_consistency_test`, `read_lock_flush_test`, `flush_all_persistence_test` | Covers writer-held flush waiting, read-latch flush progress, and `FlushAll()` persistence. |
| Async flush scheduler | `async_flush_scheduler_test` | Covers worker-owned flushes, batching, foreground/background interaction, cleaner ownership, retries, and in-flight `FlushAll()`. |
| Completion delivery | `io_engine_test`, `completion_order_test` | Covers token routing, out-of-order and counted completions, waiters reaping their own inline completions, rejected submissions, fatal reaper failure, an idle reaper that does not spin, and reordered read completions. |
| Asynchronous loads | `async_read_test` | Covers `PrefetchBuffer()` (unpinned loads, skipping resident blocks, stopping when the pool is pinned), `StartReadBuffers()`/`WaitReadBuffers()` (handles in block order, combined multi-block requests, failure releasing every pin, abandoned operations, misuse), prefetch racing ordinary readers, empty ranges, and prefetches and read operations wider than the pool finishing instead of spinning. |
| Disk backends | `disk_backend_test`, `disk_backend_factory_test`, `read_zero_fill_test`, `io_uring_*` | Covers malformed requests, inline `kWait` completion, multi-block reads and writes with zero fill past end-of-file, batched submission, shutdown draining, the descriptor cache, factory policy, and native/stub `io_uring` paths. |
| Replacement policy | `replacer_model_test`, `replacer_test`, `lru_k_behavior_test`, `two_queue_behavior_test`, `clock_family_behavior_test`, `replacer_concurrency_test`, `replacer_shared_state_test`, `indexed_ordered_set_test` | Covers probe-filtered nomination for all six policies (Clock, clock-sweep, SIEVE, LRU, LRU-K, 2Q), policy-specific ordering, concurrent access/nomination/reload, every policy's state bound into shared memory by two instances at once, rejection of undersized or misaligned state memory, and the index-based treap against `std::set` under random operations. `replacer_model_test` drives every policy with random loads, accesses, removals, evictability changes, and ids outside the capacity, checks the contract (a victim exactly when a tracked frame is evictable, always tracked and evictable, still tracked afterwards) and exact victims against reference LRU, LRU-K (K = 1, 2, 3), and Belady models, and checks that two replacers sharing one state block choose the same victims as one heap replacer. |
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
