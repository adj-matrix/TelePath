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

**Progress:** phases 0, 1a–1e, and 2 are implemented; see section 7.

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
| 4 | Writeback and durability contract (`Close()`, pre-write hook, ordering); observation plane on shared metadata | 2, 3 |
| 5 | Facades: PageCache, MLPrefetchBuffer; cross-process mode | 4 |

Topics deferred to their phase: policy interface details (2), I/O interface and threading (3), durability contract and event schema (4), facade APIs (5).

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
