# Kernel Refactor Design

## Status

Agreed design for the `src/cpp` refactor. Items marked **(proposal)** still need confirmation during review. Concurrency rules that survive the refactor stay in [`docs/concurrency.md`](../concurrency.md) and are updated as each phase lands.

## 1. Positioning and Scope

TelePath is a long-term open-source SDK. Its kernel is a **keyed, fixed-size block cache over a backing store**. Three facades sit on one kernel:

| Facade | Access semantics | Kernel features it relies on |
| --- | --- | --- |
| BufferPool | in-place latched read/write | writeback, pre-write hook (WAL rule) |
| PageCache | byte-range copy read/write | readahead, write-behind |
| MLPrefetchBuffer | read-only, block-level dataset cache | plan-driven prefetch, admission policy, large blocks |

Out of scope: network-stack buffer pools, compiler symbol interning, sample-level ML caches, mmap semantics. Shuffling, decoding, and batching stay outside TelePath.

"Three facades on one kernel" means three kinds of products reuse one kernel implementation. An application normally uses one facade, and each facade creates and owns its kernel instance; any number of threads may use one facade instance concurrently. Sharing one cache between facades or between processes is a future plan (section 8), not part of the current design.

## 2. Ground Rules

- **Language**: C++23. No backward-compatibility target. Avoid modules and library features without mature GCC/Clang support. Minimum compilers are **GCC 14** and **Clang 19**, both building against libstdc++ 14; `support/install_build_deps.sh` installs them. Clang 18 is excluded because it reports `__cpp_concepts` as `201907L`, which makes libstdc++ 14 hide `std::expected`.
- **Shared-memory-capable metadata**: kernel metadata uses offsets instead of pointers, fixed-size tables, atomics plus futex for synchronization, and no heap-owning objects. v1 runs in-process; cross-process mode (pin-holder ledger, crash recovery, shared futexes) is a later runtime mode with its fields reserved now.
- **Block size**: per-instance runtime parameter fixed at creation, a power of two in `[4 KiB, 4 MiB]`. It lives in one immutable `BlockGeometry` (`block_shift`, `block_mask`); hot paths only shift and mask. In cross-process mode it is stored in the shared header and validated on attach.
- **Pay for what you use**: writeback, content latches, and dirty tracking are per-namespace features with zero cost when disabled.
- **Multi-tenancy**: a reserved policy submodule with a no-op single-tenant default. Frame metadata reserves a tenant id; the kernel calls the module on reservation, victim selection (quota picks the tenant, replacer picks within it), and accounting.
- **Comments**: public headers carry accurate API comments; implementation files stay lean; design rationale lives under `docs/`.

## 3. Access Model

### 3.1 Buffer Handles

| Handle | Latch | Notes |
| --- | --- | --- |
| `SharedBufferHandle` | shared | `std::span<const std::byte>` view |
| `ExclusiveBufferHandle` | exclusive | handing out the mutable span marks the page dirty |
| pin-only access | none | internal (`FlushBuffer(file, block)`, prefetch, writeback); no public handle type |

- No in-place upgrade. Release the shared handle, acquire an exclusive handle, and re-check.
- Read-only namespaces take no latch for `SharedBufferHandle`; requesting an `ExclusiveBufferHandle` returns an error.
- PageCache copy semantics are implemented in the facade on top of the two handle types.
- Handles are movable across threads but never shared between threads.
- Holding two handles on the same page in one thread is a contract violation. **(open)** Debug detection is deferred: handles may move between threads, so a thread-local registry would report false positives; it needs move-aware tracking.
- Optimistic reads have no API in v1; the control word reserves a version for them.
- Cross-process pins are invisible in the API; recovery is internal.

### 3.2 Frame States

Main states: `Free`, `Loading`, `Resident`. Resident frames carry flags: `dirty`, `writeback_queued`, `writeback_active`, `prefetched` (loaded but not yet referenced).

| From | Event | To |
| --- | --- | --- |
| `Free` | reserved for a tag | `Loading` |
| `Loading` | read completes | `Resident` (`prefetched` if loaded by prefetch) |
| `Loading` | read fails | `Free` |
| `Resident` | first handle on a prefetched frame | clears `prefetched` |
| `Resident` | `ExclusiveBufferHandle` exposes data | sets `dirty` |
| `Resident` + `dirty` | flush scheduled | sets `writeback_queued` |
| `writeback_queued` | worker submits | `writeback_active` |
| `writeback_active` | completion succeeded, `redirtied` clear | clears `dirty` and writeback flags |
| `writeback_active` | completion otherwise, or failure | clears writeback flags and `redirtied`, keeps `dirty` |
| `Resident`, clean, unpinned, no writeback | chosen as victim | `Loading` for the new tag |

A dirty victim is written back while still mapped under its old tag (see `docs/concurrency.md`, section 5.1).

### 3.3 Frame Control Word

Each frame has one hot 32-bit atomic control word plus colder metadata:

```
control word (32-bit atomic, futex word)
  [0:1]   state            Free / Loading / Resident
  [2]     dirty
  [3]     writeback_queued
  [4]     writeback_active
  [5]     prefetched       loaded but not yet referenced (reserved for phase 3)
  [6]     redirtied        page changed (exclusive release or dirty marking) while a writeback was pending
  [7]     exclusive        content latch held exclusively
  [8:15]  shared count     content latch readers (max 255; more readers wait)
  [16]    waiters          a thread sleeps on this word
  [17:31] pin count        max 32767

version (64-bit atomic)    incremented on every exclusive release and frame reuse;
                           reserved for optimistic reads
cold metadata              tag (atomic file id / block id), packed last writeback error,
                           tenant id and cross-process holder slot (reserved)
```

- Linux futexes operate on exactly 32 bits, so everything a thread may wait for (state, writeback flags, latch bits) lives in this one word.
- Pin and eviction are decided by the same CAS: eviction requires `Resident`, pin 0, no latch holders, clean, and no writeback flags in one atomic step, so there is no window between "pin count is zero" and "frame is reserved".
- **Dirty clearing.** A flush copies its snapshot from a stable source (under the shared latch, or from a caller whose handle holds a latch). While `writeback_queued` or `writeback_active` is set, releasing the exclusive latch or marking the page dirty sets `redirtied`. Completion clears `dirty` only if the write succeeded, `redirtied` is clear, and no exclusive holder exists, deciding this in the same CAS that clears the writeback flags. Completion never takes a latch, so a writer waiting for its own flush cannot deadlock with it. Because a writer may keep writing through a view obtained before the flush, a page with an exclusive holder is never cleaned; `FlushBuffer(ExclusiveBufferHandle&)` therefore runs a single round and leaves the page dirty, while the other flush entry points loop until clean.
- This replaces `dirty_generation`. The earlier proposal to compare a separate version counter was rejected: checking a version in one atomic and clearing `dirty` in another leaves a window in which a writer can acquire, write, and release, losing the update.
- `FrameSnapshot` keeps its field names for the observation contract: `is_valid` is `state == Resident`, `io_in_flight` is `state == Loading`, and `dirty_generation` reports the version.

## 4. Error Model

- Public and kernel functions return `std::expected<T, Error>`; no exceptions cross the public API; hot-path functions are `noexcept`.
- `Error` holds an `ErrorCode` and a static `const char*` message. An optional allocated detail (errno, path) is attached only on cold paths.
- Error classes:

| Class | Codes (initial) | Caller action |
| --- | --- | --- |
| page-level | `kIoError` | affects this call only |
| resource | `kResourceExhausted` | retryable |
| usage | `kInvalidArgument`, `kNotFound` | fix the call |
| fatal | `kFailed`, `kInternal` | instance is unusable |

- After a fatal error the instance enters a `Failed` state; every later call fails fast and the cause can be queried.
- Instances are created through `Create(options) -> std::expected<std::unique_ptr<...>, Error>`; half-initialized objects do not exist.

## 5. Migration

- Refactor in place, phase by phase; git is the rollback mechanism. Reuse logic that does not need redesign.
- Tests keep their logic and change only their API calls. White-box tests of removed internals are rewritten or dropped together with those internals.
- No versioning or compatibility work: the SDK has never been released.
- Every phase ends with the debug, ASAN, and Clang TSAN suites green and a benchmark comparison against the phase 0 baseline.

## 6. Phases

**Progress:** phases 0, 1a–1e, 2, 3, 4, 5a, 5b, and 6a–6d are implemented; see section 7.

| Phase | Content | Depends on |
| --- | --- | --- |
| 0 | C++23 toolchain and CI; Clang TSAN in CI; black-box and stress tests; benchmark baseline | nothing |
| 1a | Error model: `Status`/`Result` to `std::expected<T, Error>`; `Create()` factories | 0 |
| 1b | `BlockGeometry`; single source of block size; span-based page access | 1a |
| 1c | Frame control word; futex latches replace `std::mutex`/`condition_variable`/`shared_mutex` in descriptors | 1b |
| 1d | `SharedBufferHandle`/`ExclusiveBufferHandle` public API with write-implies-dirty | 1c |
| 1e | Fixed-size open-addressing page table | 1c |
| 2 | Policy layer: admission, eviction, hints, tenant hook; evictability derived from frame state | 1 |
| 3 | I/O interface: completion tokens, batch and multi-block submission; async load completion; threading model | 1 |
| 4 | Writeback and durability contract (`Close()`, pre-write hook, ordering) | 2, 3 |
| 5 | Facades: PageCache (5a), MLPrefetchBuffer with a plan-driven replacer (5b); each facade owns its kernel instance | 4 |
| 6 | Cross-process mode: metadata in shared memory, pin-holder ledger, crash recovery; the shared-memory layout reserves the namespace fields of section 8; observation plane v1 (read-only frame states and counters for other processes) | 5 |
| 7 | Shared namespaces (section 8): sharing one cache between facades or processes with per-namespace policy and quotas; eviction event stream; Web namespace views | 6 |

Topics deferred to their phase: facade APIs (5), shared-memory layout and observation schema (6), namespace policy, quotas, and event schema (7).

## 7. Implementation Notes

### Phase 0

- `support/install_build_deps.sh` installs GCC 14 and Clang 19 (adding the toolchain PPA and apt.llvm.org on Ubuntu 22.04). Build scripts default to `g++-14` and reconfigure build trees whose cached compiler differs.
- New presets: `tsan` (Clang) and `release` / `io_uring_release` (RelWithDebInfo). Tests keep `assert()` active in every build type.
- Benchmarks and the Web console use the release build; the baseline is in `docs/design/baseline/`.

### Phase 1

- **1a** `telepath/common/status.h`: `Error`, `Result<T>`, `Status`; `BufferManager::Create` and `IoUringDiskBackend::Create`; the Failed state is entered when the completion channel breaks outside an orderly shutdown.
- **1b** `telepath/common/block_geometry.h`; backends take a `BlockGeometry` and report it in `DiskBackendCapabilities::block_size`, which `Create` checks.
- **1c** `src/cpp/lib/buffer/frame_control.h`: the control word and all transitions; `BufferDescriptor` is one cache line.
- **1d** `telepath/buffer/buffer_handle.h`: `SharedBufferHandle` / `ExclusiveBufferHandle`; `FlushBuffer(file, block)` flushes without a handle by pinning the page.
- **1e** `src/cpp/lib/buffer/page_table.h`: intrusive chains with seqlock lookups.

Hit-path throughput after phase 1 (16384-frame pool, every access a hit, release build):

| Threads | LRU-K Mops/s (baseline → 1e) | Clock Mops/s (baseline → 1e) |
| --- | --- | --- |
| 1 | 0.54 → 0.65 | 1.45 → 1.96 |
| 4 | 0.14 → 0.16 | 1.14 → 1.24 |
| 16 | 0.11 → 0.11 | 0.73 → 0.77 |

Multi-thread scaling is still bounded by the replacer's global lock (two calls per hit) and the shared telemetry counter line, which phases 2 and 4 address.

### Phase 2

- **Replacer contract** (`telepath/replacer/replacer.h`): `SetEvictable()` and `Size()` are removed; evictability comes from the frame control word through an `EvictionProbe` passed to `Victim()`. `Victim()` nominates without untracking; `RecordLoad()` resets a reused frame's policy state. See `docs/concurrency.md`, section 6.
- **Policies** (all pluggable, factories in `replacer.h`):

| Policy | Hit path | Victim path |
| --- | --- | --- |
| Clock | lock-free reference bit | lock-free hand |
| Clock-sweep (PostgreSQL) | lock-free saturating usage count (max 5) | lock-free hand, decrement per pass |
| SIEVE | lock-free visited bit | queue mutex, hand from oldest toward newest |
| LRU, LRU-K, 2Q | per-thread batch of 64 accesses, applied under the policy latch | policy latch, all batches applied first |

  Clock and clock-sweep are separate algorithms (one reference bit versus a usage count), so both are kept. Hit paths read before writing, so already-referenced or saturated frames cause no cache-line traffic.
- **Access strategies** (`telepath/buffer/buffer_access_strategy.h`): `BufferAccessStrategy` with `kNormal` and `kBulkRead`, modeled after PostgreSQL. Bulk-read hits skip `RecordAccess()`; misses recycle a ring of frames (default 32). Optional parameter on `ReadBuffer()`/`ReadBufferExclusive()`; benchmark flag `--bulk-read-scans`.
- **Telemetry**: `TelemetryCounters` is split into 64 cache-line-aligned `TelemetryCounterShard`s; each thread increments its own shard and snapshots sum them.
- **Reserved, not implemented**: admission control and per-tenant victim selection. The tenant id stays reserved in `BufferDescriptor`; the extension point is the probe/nomination split in `Victim()`.

Hit-path throughput after phase 2 (same setup as the phase 1 table, Mops/s):

| Policy | 1 thread | 4 threads | 16 threads | 16T / 1T | p99 at 16T |
| --- | --- | --- | --- | --- | --- |
| Clock-sweep | 3.51 | 9.61 | 33.63 | 9.6x | 0.7 µs |
| SIEVE | 2.81 | 9.69 | 18.67 | 6.6x | 0.7 µs |
| Clock | 3.68 | 8.76 | 16.78 | 4.6x | 0.7 µs |
| 2Q | 3.51 | 8.46 | 13.38 | 3.8x | 16 µs |
| LRU | 3.48 | 7.53 | 9.94 | 2.9x | 19 µs |
| LRU-K | 0.91 | 0.82 | 0.56 | 0.6x | 1.6 ms |

The benchmark itself had a global mutex on every operation in JSON mode (`BenchmarkSnapshotSampler::TryCapture` locked before checking whether sampling was finished), which capped every earlier multi-thread number, including the phase 0 baseline and the phase 1 table. It was fixed in phase 2. Rerunning the phase 0 code with the fixed sampler gives Clock 2.26 / 1.45 / 1.10 Mops/s and LRU-K 0.75 / 0.13 / 0.10 Mops/s at 1 / 4 / 16 threads, so against that fair baseline phase 2 is 15x (Clock) to 30x (clock-sweep) faster at 16 threads.

LRU-K still scales negatively: batching cuts lock acquisitions, but every batched access performs ordered-set updates and history-deque work under the single policy latch, so the policy saturates at roughly 0.9 M accesses/s regardless of thread count.

Scan pollution (`scan_pollution`, 4 threads, pool 2048, dataset 16384, hot set 1024): `--bulk-read-scans true` raises the hit rate from 17.1% to 22.8% for LRU and clock-sweep (disk reads -7%); SIEVE and LRU-K, which already resist scans, go from 18.3% to 22.8%.

### Phase 3

- **Backend contract** (`telepath/io/disk_backend.h`): `Submit(std::span<const DiskRequest>, DiskSubmitMode)`, `Reap(std::span<DiskCompletion>, bool wait)`, `Shutdown()`, `GetCapabilities()`; all thread-safe. A `DiskRequest` covers up to `kMaxDiskRequestBlocks` (32) consecutive blocks with one buffer per block and carries an opaque `token` that comes back in its `DiskCompletion`. `kWait` means the submitter reaps right away, so a backend may complete the request on the calling thread; `kAsync` means nobody waits yet, so the backend must not block the caller and must wake a blocked reaper. `Submit()` accepts a prefix and fails only when it accepts nothing, the error describing the first request. `Reap(wait)` may return 0 after a spurious wakeup. `DiskBackendCapabilities` reports `queue_depth` and `max_request_blocks`; `recommended_queue_depth`, `supports_submit_batching`, and `supports_completion_batching` are gone.
- **IoEngine** (`src/cpp/lib/buffer/io_engine.h`) replaces `BufferManagerCompletionDispatcher`. A token points to an `IoCompletion`: an `IoWaiter` (a countdown a thread can wait on) or an async load. A waiting thread first reaps ready completions itself and sleeps on its waiter's futex only when nothing is ready. The reaper thread waits on the backend only while asynchronous requests are outstanding or a waiter is asleep, so it does not race waiters for their own completions. The request-id map, the global mutex, and `notify_all` are gone. Tokens are process-local pointers by design: in cross-process mode each process owns its backend, and completion handlers touch shared metadata only through frame ids.
- **POSIX backend**: `kWait` requests run `preadv`/`pwritev` on the calling thread; `kAsync` requests go to a worker pool (`DiskBackendOptions::io_worker_count`, default 4), which also sets `queue_depth`. The descriptor cache (`src/cpp/lib/io/file_descriptor_cache.h`) is shared with io_uring and pins descriptors while I/O uses them. Durable writes call `fdatasync` instead of `fsync`.
- **io_uring backend**: one ring per group of submitting threads (default one per hardware thread, at most 16). A single shared ring collapses under concurrent submitters: on a 20-thread i9-12900H, raw 4 KiB page-cache reads reach 1.2 M/s on one thread, but 0.44 M/s at 4 threads and 0.29 M/s at 16 threads with one shared ring, against 4.2 M/s and 6.5 M/s with one ring per thread. All rings signal one eventfd for blocking reaps. Multi-block requests use `readv`/`writev`; a durable write carries a linked `IORING_OP_FSYNC` with `IORING_FSYNC_DATASYNC` instead of a blocking `fdatasync` in the reaping thread; short transfers are resubmitted for the remainder; a hard `io_uring_enter` failure takes back the requests the kernel never consumed and retires that ring.
- **Asynchronous loads** (`telepath/buffer/read_buffers_operation.h`): `PrefetchBuffer(file, block, count)` starts reads without pinning; `StartReadBuffers()` / `WaitReadBuffers()` with a `ReadBuffersOperation`, modeled after PostgreSQL 17, pin a range and overlap its reads with the caller's work. Consecutive misses are combined into one request of up to `max_request_blocks`. The thread that reaps a completion publishes the frames and completes the miss for followers. A miss owner that gives up without loading (no frame for a prefetch) completes the miss with `Ok` and `kInvalidFrameId`, which tells followers to retry. Benchmark flag `--prefetch-distance` (`PREFETCH_DISTANCE`, Web console field).
- **Fix to phase 2**: `ReserveFrameForTag()` retried only when an unpinned frame was latched or under writeback, so a frame that concurrent clock scans happened to miss produced a spurious `kResourceExhausted` (`completion_order_test`, 8 threads on an 8-frame pool, failed about 3% of runs on 2 CPUs). It now retries while any frame is unpinned, which matches the documented contract.
- **Semantic changes**: `Submit()` rejection now fails only the rejected request; the POSIX backend honors `flush_submit_batch_size` and its default `queue_depth` of 4 gives it up to 4 flush workers (previously 1).

Miss-path throughput (`uniform`, pool 1024, dataset 32768 blocks held in the OS page cache, clock-sweep, `--sync-on-write false`, release build, ops/s):

| Backend | Threads | Phase 2 | Phase 3 | p50 phase 2 → 3 |
| --- | --- | --- | --- | --- |
| POSIX | 1 | 18 K | 442 K | 47 µs → 2.1 µs |
| POSIX | 4 | 24 K | 766 K | 147 µs → 3.6 µs |
| POSIX | 16 | 22 K | 759 K | 677 µs → 8.4 µs |
| io_uring | 1 | 22 K | 383 K | 37 µs → 2.4 µs |
| io_uring | 4 | 23 K | 835 K | 159 µs → 3.6 µs |
| io_uring | 16 | 23 K | 990 K | 658 µs → 7.8 µs |

Phase 2 spent most of each miss handing work between threads: submitter, the single POSIX worker, the dispatcher, and back, with `notify_all` waking every waiter on each completion. The 16-thread plateau around 0.8–1.0 M ops/s now comes from the miss coordinator stripes, the free-list mutex, and victim search, not from I/O.

Prefetching a sequential scan (`sequential_disjoint`, 1 thread, same dataset; data in the page cache, so every read is cheap):

| Backend | No prefetch | `--prefetch-distance 128` | `256` |
| --- | --- | --- | --- |
| POSIX | 460 K ops/s, p99 5.2 µs | 513 K ops/s, p99 34 µs | 511 K ops/s, p99 3.3 µs |
| io_uring | 355 K ops/s, p99 7.0 µs | 449 K ops/s, p99 2.4 µs | 554 K ops/s, p99 2.2 µs |

Distances of 16–64 blocks are slower than no prefetch here: the reader catches up with reads still in flight and waits for them. The prefetch window must cover the asynchronous hand-off latency; with real device latency the gain is larger.

### Phase 4

- **Miss path without a miss table**: `BufferManagerMissCoordinator` (a mutex, a `shared_ptr` state, and a condition variable per miss) is gone. A thread that misses reserves a frame and installs the tag with `BufferManagerPageTable::ReplaceUnlessLive`, which, under the stripe lock, refuses when another frame is live for the tag (not `Free`, same tag in its descriptor); the loser returns its frame and retries. Readers that find a frame `Loading` with their tag sleep on its control word (`WaitForLoad`) and retry the lookup when it changes, as PostgreSQL's `WaitIO` does. After a failed load the waiters retry the read themselves instead of receiving the loader's error. See `docs/concurrency.md`, section 5.5.
- **Transient exhaustion**: concurrent misses on one page each reserve a frame before learning at install time that they lost, so a small pool can be briefly all pinned (`same_page_miss_failure_recovery_test`, 6 threads on a 4-frame pool, failed 5 of 300 TSAN runs with `kResourceExhausted`). `ReserveFrameForTag()` now also retries while any frame is `Loading` and fails only when every frame is pinned by a resident page.
- **Free list**: a lock-free stack of frame indices with a versioned head (`src/cpp/lib/buffer/frame_free_list.h`) replaces the mutex-protected vector.
- **Fix**: `ReleaseToFree` used to clear the whole control word, including a shared latch that `FlushAll()` may hold on any frame, so the later release underflowed the word to `0xffffff00`. It now keeps the latch bits. The bug predates phase 4 (load failures), but the new install race made it reachable under `stress_integrity_test`.
- **Durability contract**: `DiskBackendOptions::sync_on_write` now defaults to `false`, so a writeback completes once the OS has the bytes. `DiskOperation::kSync` makes a file's completed writes durable (POSIX `fdatasync`, io_uring `IORING_OP_FSYNC` with `IORING_FSYNC_DATASYNC`). The manager records files written since the last sync; `FlushAll()` writes every dirty page and then syncs those files, re-recording them if a sync fails. `Close()` refuses while any page is pinned, otherwise runs `FlushAll()`, stops background work, and moves the manager to a closed state where calls return `kUnavailable`. Destroying a manager does not write dirty pages. Writes to different pages have no relative order. Benchmark, Web console, and scripts default to `sync_on_write=false` as well.
- **Pre-write hook**: `BufferManagerOptions::pre_write_hook(tag, page)` runs on the thread that schedules a writeback, with the stable snapshot, before submission; an error fails that writeback and keeps the page dirty. It is the place to enforce the WAL rule without the kernel knowing the page format.
- **Observation plane (reserved)**: `BufferManagerOptions::observation` (`ObservationOptions{enabled, shared_memory_name}`) fixed the configuration surface; `Create()` rejected `enabled` with `kUnavailable` until phase 6c implemented it.
- **Rejected: asynchronous writeback.** Flush workers that submit writes as `kAsync` and let completions finish the tasks were implemented and measured, and were slower: every write gained a hand-off to the backend's worker pool and to the single reaper thread, which became the serialization point. Flush workers keep submitting `kWait` batches and reaping them themselves.

Write-heavy miss workload (`uniform`, 50% writes, pool 1024, dataset 32768 blocks, 4 flush workers, ops/s):

| Backend | Threads | Workers wait (kept) | Asynchronous writeback (rejected) |
| --- | --- | --- | --- |
| POSIX | 1 | 28 K | 23 K |
| POSIX | 4 | 76 K | 30 K |
| POSIX | 16 | 293 K | 36 K |
| io_uring | 1 | 23 K | 16 K |
| io_uring | 4 | 48 K | 23 K |
| io_uring | 16 | 146 K | 42 K |

- **Foreground writeback modes** (`BufferManagerOptions::foreground_writeback`): single-page flushes, meaning dirty eviction victims and every `FlushBuffer()` overload, either go through the flush workers (`ForegroundWriteback::kQueued`, the previous behavior, sharing batching and the burst limit with background writeback) or are written by the requesting thread itself (`kDirect`, the default, as PostgreSQL does). Both paths share snapshotting, the pre-write hook, and the `BeginWriteback`/`CompleteWriteback` transitions; only who submits and reaps the write differs. `FlushAll()` and the background cleaner always use the workers. `async_flush_scheduler_test` runs in queued mode, which is what it specifies. Benchmark flag `--foreground-writeback`.

Same write-heavy workload with the default worker count, ops/s:

| Backend | Threads | `kQueued` | `kDirect` |
| --- | --- | --- | --- |
| POSIX | 1 | 39 K | 294 K |
| POSIX | 4 | 75 K | 451 K |
| POSIX | 16 | 224 K | 302 K |
| io_uring | 1 | 24 K | 50 K |
| io_uring | 4 | 51 K | 87 K |
| io_uring | 16 | 129 K | 128 K |

io_uring gains less because the kernel usually punts buffered writes to its io-wq workers, so a direct write still waits for a thread hand-off inside the kernel.

Read-only miss path (same setup as the phase 3 table, ops/s, phase 3 → phase 4): POSIX 442 K → 555 K (1 thread), 766 K → 861 K (4), 759 K → 748 K (16); io_uring 383 K → 456 K, 835 K → 1006 K, 990 K → 1067 K. The 16-thread plateau remains and has not been located yet.

### Phase 5a: PageCache

- **Facade** (`telepath/facade/page_cache.h`): `PageCache::Create(PageCacheOptions, backend, replacer, telemetry)` creates and owns a `BufferManager` (clock-sweep by default). `Read(file, offset, out)` copies up to the file's logical size and returns the byte count; `Write(file, offset, data)` extends the size, and a gap reads as zeros; `FileSize`, `Truncate`, `Sync(file)`, and `Close()`. All methods are thread-safe; overlapping concurrent writes have no atomicity across blocks, and `Truncate()` must not race with I/O on the same file.
- **File sizes**: the cache keeps each file's logical size in memory, loaded on first use with `DiskBackend::GetFileSize()`. Writebacks round the backing file up to whole blocks, so `Sync()` writes back the file's dirty pages, sets the backing size with `DiskBackend::SetFileSize()`, and then syncs; until then a crash can leave a block-rounded size. Shrinking discards cached pages past the new size, zeroes the tail of the last page, and shrinks the backing file at once, so a later extension reads zeros rather than old bytes.
- **Readahead**: on the second consecutive sequential read of a file the window starts at 4 blocks and doubles up to `max_readahead_blocks` (default 32); the cache prefetches only blocks past what it already requested. Random reads reset the window.
- **Write-behind**: `write_behind` (default on) enables the background cleaner.
- **Kernel additions**: `ReadBufferMode::kZeroAndLock` for `ReadBufferExclusive()` zero-fills a missing page instead of reading it (PostgreSQL's `RBM_ZERO_AND_LOCK`); the cache uses it only for whole-block writes: a partial write past the old end must still read, because a concurrent writer may already have written, evicted, and persisted part of that block. `FlushFileBuffers(file)` writes back one file's dirty pages without syncing (`FlushRelationBuffers`), `SyncFile(file)` syncs one file, and `DropFileBuffers(file, first_block)` discards a file's pages from a block on, dirty ones included (`DropRelationBuffers`); see `docs/concurrency.md`, section 5.7.
- **Backend additions**: `DiskBackend::GetFileSize()` and `SetFileSize()` (synchronous `fstat` and `ftruncate` through the shared descriptor cache). They have default implementations that return `kUnavailable`, so existing custom backends still compile; `PageCache` needs a backend that implements them.

### Phase 5b: MLPrefetchBuffer

- **Facade** (`telepath/facade/ml_prefetch_buffer.h`): `MLPrefetchBuffer::Create(MLPrefetchBufferOptions, backend, telemetry)` creates and owns a read-only `BufferManager` (1 MiB blocks unless set, background writeback off) with a `PlanReplacer`. `SetPlan(blocks)` starts an epoch whose consumption order is known; a block may appear more than once. `Fetch(position)` returns the block at a plan position, pinned for reading, so a data loader can distribute positions to its workers deterministically, as PyTorch's DataLoader does with a map-style dataset; `Next()` hands out positions in order to whichever thread asks and returns `std::nullopt` at the end of the plan. One style per plan.
- **Prefetching**: each fetch prefetches up to `prefetch_depth` (default 8) positions past the furthest position requested so far.
- **Belady eviction** (`telepath/replacer/plan_replacer.h`): the facade precomputes, for every position, the next position of the same block, and after each fetch tells `PlanReplacer::SetNextUse()` when that frame is needed again. `Victim()` evicts the evictable frame needed furthest in the future, pages not needed again first; a freshly loaded (prefetched) frame counts as needed immediately until it is consumed. With prefetching off and one consumer, the miss count equals Belady's optimum (`ml_prefetch_buffer_test`). Deterministic partitions make workers progress unevenly, which only makes the estimate less exact. The replacer interface is unchanged; only this facade uses it.
- **Kernel change**: `Close()` now waits for loads in flight (such as prefetches) before checking for pinned pages.
- **Overhead for other users**: none by construction (separate facade, replacer chosen only by it, no kernel hot-path change). A release A/B against phase 4 showed equal hit-path throughput at 1 and 4 threads; at 16 threads run-to-run variance on the development machine (22–35 M ops/s for the same binary) exceeds any difference, so the phase 7 parity gate needs pinned cores or a quiet benchmark host.

### Phase 6 Plan: Cross-Process Mode

**Status: agreed design, in progress.** Decisions:

| Topic | Decision |
| --- | --- |
| Process model | Symmetric create-or-attach: any process may create the named segment or attach to it; the segment can outlive its last process or be removed with it. A dedicated daemon can be built on top later. |
| I/O | Each process owns its backend, I/O engine, descriptors, and rings and opens the same files under the same root. Completions change shared state only through frame ids (phase 3 tokens are process-local). |
| Replacers | All policies keep one implementation that places its state in caller-provided memory and locks it with a futex-based lock, so the same code serves the heap (single process) and the segment. Only LRU-K and `PlanReplacer` change data structure (ordered sets become index-based structures); the algorithms stay. |
| Background work | One process at a time runs the background cleaner, elected through a lock word in the segment; foreground writeback stays with each process. |
| Observation | A separate observation area with a published, versioned binary format (counters, per-frame state summary, reserved namespace id). Python maps it and parses it directly; there is no exporter program. The kernel's internal layout stays free to change. Revised in 6c: the area is its own small shared-memory object rather than part of the segment, so a single-process manager can publish it too and a reader never maps the kernel segment. |
| WAL support | The kernel provides what a WAL needs from a buffer pool, not a WAL: the pre-write hook (phase 4), a per-frame recLSN (log position when the page first became dirty) with a query for the minimum, and checkpoint support (write back pages whose recLSN precedes a position). An optional generic log module (append, group commit, durable flush, read back; log contents owned by the user) is a later phase after 7. |
| Process crash | **Open.** Exclusive latches have one holder and can be recovered exactly through `holder_slot`, but shared pins and latches are summed counts: a per-process ledger cannot tell whether a process that died between updating the control word and its ledger had applied the pin, and guessing wrong corrupts other processes' pins or leaks a frame. PostgreSQL resets shared memory after a backend crash, and CacheLib drops a shared cache after an unclean exit. A recovery protocol is designed separately (6e) before it is implemented. |

Steps:

- **6a** Metadata in caller-provided memory (descriptors, page table, free list, replacer state) and process-shared futexes and locks; single-process behavior unchanged. **Done**: all futex operations use process-shared futexes (`src/cpp/lib/sync/futex.h`, only the sleep and wake slow paths differ), and `sync::FutexMutex` replaces `std::mutex` in policy state. Kernel metadata and frames live in one `KernelRegion` laid out by `KernelLayout`; the page table, free list, frame pool, and descriptors are placed in it instead of allocating themselves. Every replacer places its state in one block through `Replacer::SharedStateBytes()` and `BindSharedState(memory, initialize)` (default: unsupported, so custom replacers still compile); single-process mode keeps the block on the heap. LRU-K's ordered sets and history deques and `PlanReplacer`'s ordered set became an index-based treap (`src/cpp/lib/replacer/indexed_ordered_set.h`) and per-frame history rings; ordering rules are unchanged. Per-thread access batches stay process-local, since losing them only loses recency hints. Fix found while verifying 6a: with a one-frame pool, a reader could fail with `kResourceExhausted` while another reader held the very page it wanted (`wait_evict_interleave_test`, once in a clang debug run); `PinPage()` now retries the lookup in that case. The race came from the phase 4 removal of the miss table.
- **6b** Segment layout (header with magic, layout version, geometry, replacer kind, reserved namespace registry; process slots), create-or-attach, and validation on attach. **Done**: `BufferManagerOptions::shared_memory` (`SharedMemoryOptions`: name, `SharedMemoryMode` `kCreateOrAttach`/`kCreate`/`kAttach`, `remove_when_unused`) selects cross-process mode; an empty name keeps the private heap region. `SharedSegment` (`src/cpp/lib/buffer/shared_segment.{h,cc}`) maps one POSIX shared-memory object: a header (magic, layout version, initialization state, shape, replacer kind as the type name, offsets, attached count, 64 process slots holding pid and start time, 64 reserved 64-byte namespace entries, a reserved cleaner-leader word, and a shared table of files written since their last sync), then the replacer state, then the `KernelLayout` region at a page-aligned offset. The creator uses `O_CREAT | O_EXCL`, initializes the kernel metadata and replacer state, seeds the free list, and marks the segment ready; an attacher waits up to 10 s for the ready state, then checks the layout and shape (block size, pool size, page-table stripe count, replacer kind and state size, region size) and places the same components without initializing them. Errors: `kCreate` on an existing segment returns `kInvalidArgument`, `kAttach` on a missing one `kNotFound`, a shape mismatch `kInvalidArgument`, and a replacer without `BindSharedState()` support `kInvalidArgument`. The last manager to detach with `remove_when_unused` removes the name; it first swaps the attached count from 0 to a removing marker, so a process attaching at the same moment retries and creates a fresh segment instead of joining a dying one. A creator that fails before marking the segment ready removes it. `BufferManager::RemoveSharedMemory(name)` removes a persistent segment's name. Written files go to the shared table so that `FlushAll()` in any process syncs files written by every process (a per-process list catches overflow beyond 1024 files). `Close()` in cross-process mode does not refuse while pages are pinned, because the pins may belong to other processes, and its `FlushAll()` writes back every process's dirty pages through the closing process's backend. Not yet in 6b: the cleaner runs in every process (redundant but safe, since queueing writeback is a control-word CAS), and process slots are recorded but not yet used for crash detection (6e). Tests: `shared_memory_test` re-executes itself through `/proc/self/exe` as the second process.
- **6c** Observation area and its published format; the Web backend reads it with Python. **Done**: the format is the public header `telepath/observation/observation_format.h` (version 1, little-endian): a 256-byte header (magic, version, record sizes, pool and page size, offsets, cross-process flag, and a summary of free, loading, resident, dirty, pinned, writeback-queued, and writeback-active frames plus the dirty-page counter), 64 process records of 128 bytes (pid, update time, the process's 13 `TelemetrySink` counters), and one 32-byte record per frame (frame id, state, dirty and writeback flags, pins, file id, block id, version, and a namespace id that is always 0 for now). The summary and frames are one seqlock-guarded snapshot; each process record has its own sequence. `ObservationOptions` gained `publish_interval` (default 1 s); `ObservationPublisher` (`src/cpp/lib/buffer/observation_publisher.{h,cc}`) runs one thread per manager that wakes every interval, scans the descriptors, and copies the sink's counters, so page access paths do no extra work. `BufferManager::PublishObservation()` publishes immediately, and `Close()` publishes a final snapshot and stops the thread. Managers of one cross-process cache share the object: each writes its own process record, and one of them, elected through an open-file-description lock that the kernel releases when its process dies, writes the frames, so another process takes over within one interval. The object is created on first use and removed by its last user; a second byte lock serializes opening against removal, so a process opening the name while the last user leaves either joins it before removal or creates a fresh one. Opening an object built for a different pool size, page size, or cross-process setting returns `kInvalidArgument`. Records of processes that died are reused once the 64 records are taken. Python: `web/backend/observation.py` (`read_observation(name, include_frames=True)`) mirrors the header with `struct`, retries torn snapshots, and sums the per-process counters; the Web server serves it as `GET /api/observation?name=/name[&frames=0]` (400 for a malformed name, 404 when the area is missing or unreadable). No frontend view yet; the Web namespace views are phase 7.
- **6d** recLSN per frame, minimum-recLSN query, and checkpoint writeback. **Done**: `Lsn` (`std::uint64_t`, `kInvalidLsn = 0`) in `types.h`. Callers report each logged change with `ExclusiveBufferHandle::MarkDirty(lsn)` (marks dirty like `mutable_data()` and records the LSN; PostgreSQL's `MarkBufferDirty()` plus `PageSetLSN()`). `BufferManager::MinRecLsn()` returns the minimum recLSN over all pages, and `BufferManager::CheckpointBuffers(lsn)` (after PostgreSQL's `CheckPointBuffers()`) writes back every page whose recLSN is below `lsn`, waits, and syncs the written files. `FrameSnapshot::rec_lsn` and the observation header's `min_rec_lsn` (taken from reserved space, so the format stays version 1) expose them. Descriptors gained `rec_lsn` and `writeback_rec_lsn`; to keep one cache line, `last_writeback_error` shrank to 32 bits (error code in the low byte, errno in the upper 24 bits, saturated) and the fields were reordered. A change made during a writeback becomes the page's recLSN once that writeback completes, rather than holding the old recLSN back; the rules are in concurrency.md section 4.4. Pages changed without an LSN are not tracked and do not hold the minimum back. The descriptors are shared, so in cross-process mode the minimum covers every process. Also fixed: `SyncFile()` now also removes the file from the cross-process written-files table; and `Close()` could refuse with `kInvalidArgument` right after a `PrefetchBuffer()` read completed, because the completion publishes the frame as resident before it drops the prefetch's pin (`ml_prefetch_buffer_test` failed once under ASAN), so `Close()` now first waits for a per-process count of prefetches in flight to reach zero.
- **6e** Process-crash detection and recovery, and cleaner election.

## 8. Future Plan: Shared Namespaces and Observation

**Status: planned for phases 6 and 7; nothing here is implemented.** Until then each facade owns its kernel instance (section 1). This section records the plan and the constraints that keep it possible.

### 8.1 Motivation

Running two kinds of cache users against one cache is uncommon but real: a database that also trains or serves models in-process, an engine that both answers SQL and feeds training data, and, most realistically, a node-level cache service shared by several processes (a database and training jobs on one machine). Separate instances already let such users coexist, but they partition memory statically, duplicate background threads, and cache a shared file twice without coherence. Sharing one cache removes those costs.

### 8.2 What sharing needs

| Piece | Cost | Notes |
| --- | --- | --- |
| Namespace id in `BufferTag` and in each frame | one more key compare on lookups; spare bytes exist in `BufferDescriptor` | single-namespace users always use namespace 0 |
| Per-namespace replacer and writeback settings | one table lookup in `RecordAccess`; victim selection picks a namespace first | this is the "quota picks the tenant, replacer picks within it" split of section 2 |
| Quotas and isolation | only when enabled | lower and upper bounds per namespace |
| Different block sizes | see below | the only real conflict |

Different block sizes (for example a 1 MiB dataset cache next to 8 KiB database pages):

- **(a) Share only between users with the same block size.** No cost.
- **(b) Sub-pools per block size inside one instance**, as Oracle does with its per-block-size caches: page table, I/O engine, threads, and namespace registry are shared; memory is not lent between sub-pools. Every sub-pool keeps fixed-size frames, so hot paths still only shift and mask.
- **(c) Variable-size frames in one elastic pool.** Rejected: it breaks fixed-size frames, which the shared-memory frame table, shift-and-mask addressing, and fragmentation-free allocation all depend on.

### 8.3 Observation

Once metadata lives in shared memory and frames carry a namespace id, an external process can show, without involving the engine: frame ownership per namespace (the block map colored by namespace), cross-namespace eviction ("whose load evicted whose page", from an eviction event ring written only on the miss path), quota versus occupancy over time, and per-namespace hit rate, miss latency, dirty pages, and writeback volume; after phase 6 also which process pins which pages. The Web console renders these views. Per-namespace counters extend the existing per-thread counter shards; events never touch the hit path.

### 8.4 Related work

Meta's CacheLib supports several memory pools with independent eviction policies and rebalancing inside one cache, but it is a key-value object cache without buffer-pool semantics. The combination targeted here is buffer-pool semantics (page latches, writeback, the pre-write hook) with cross-process sharing, per-namespace policy and quotas, and an external observation plane.

### 8.5 Constraints in force now

These keep phases 6 and 7 from forcing a rewrite and cost nothing today:

- Facades create their kernel internally and do not expose ownership in a way that would block a later constructor that attaches to an existing shared kernel.
- The phase 6 shared-memory layout reserves a namespace id in `BufferTag` and `BufferDescriptor` and a fixed-size namespace registry in the shared header, next to the existing tenant id.
- Replacement and writeback settings stay behind one per-instance policy point (today a single replacer and options object), so per-namespace dispatch replaces a field rather than call sites.
- Telemetry counters keep their sharded layout so a namespace dimension can be added.
- Frames stay fixed-size (option (c) is out).
- Acceptance gate for phase 7: with a single namespace, hit-path throughput must match the phase 6 baseline.

