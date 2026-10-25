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

**Progress:** phases 0 and 1a–1e are implemented; see section 7.

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
