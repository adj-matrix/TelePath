# TelePath: A Shared, Observable Cache Kernel SDK for Buffering, Page Caching, and ML Prefetching

<!-- # TelePath: A High-Performance, Observable Kernel (e.g., Cache) SDK for Heterogeneous Storage -->

<p align="center">
  <img src="https://raw.githubusercontent.com/adj-matrix/adjmatrix-assets/main/TelePath/Telepathy.webp" width="60%">
</p>

<p align="center">
  English | <a href="./localization/README.zh-CN.md">简体中文</a>
</p>

**TelePath** (Telepathy, Telemetry + Path) is a C++23 SDK built around one **cache kernel**: a keyed, fixed-size block cache over a backing store. Three products sit on that kernel:

| Facade | What it is | Access |
| --- | --- | --- |
| `BufferManager` | a database buffer pool in the style of PostgreSQL's bufmgr | pin pages in place under shared or exclusive latches; writeback, pre-write hook, recLSN, checkpoints |
| `PageCache` | a byte-addressed file cache | `Read`/`Write` byte ranges with readahead and write-behind; logical file sizes; `Sync` |
| `MLPrefetchBuffer` | a read-only dataset cache for ML training | a known read order (the plan) drives prefetching and Belady eviction |

The facades do not need separate caches. Several of them can run in **namespaces** of one kernel, each with its own replacement policy, frame quotas, and pre-write hook. They share one coherent copy of every page, one set of I/O and writeback threads, and one memory budget. The kernel can also live in **POSIX shared memory**, so several processes share one cache.

Everything the cache does can be **observed from outside**. A versioned shared-memory observation area publishes frame states and counters, and a Python reader or the Web console can read it without touching the engine's hot path.

TelePath is not a DBMS. It has no SQL, query execution, indexes, transactions, or write-ahead log of its own. It provides the buffer layer those systems need, packaged as a reusable library.

## Why

Buffer pools, OS-style page caches, and training-data caches solve the same problem: keeping hot blocks of slow storage in a fixed amount of memory. They are usually separate implementations, though. A machine that runs a database next to a training job therefore splits its memory statically, runs duplicate background threads, and caches a shared file twice without coherence.

TelePath explores what one shared kernel can do instead:

- **Share without losing isolation.** Namespaces get guaranteed (`min_frames`) and capped (`max_frames`) shares of one pool. A full scan by one namespace cannot evict another namespace's guaranteed pages.
- **Share across processes.** Metadata, frames, replacer state, and quota counts live in a shared segment. When a process crashes, every attached process is fenced and the dirty pages are salvaged or discarded, the way PostgreSQL resets shared memory after a backend crash.
- **Observe without perturbing.** The observation plane copies state on a background thread, so page hits do no extra work. External tools see frame ownership, dirty and writeback state, pins, and per-process counters.

## Features

**Kernel**

- PostgreSQL-style buffer access: `ReadBuffer` / `ReadBufferExclusive` return RAII `SharedBufferHandle` / `ExclusiveBufferHandle`, and writing through an exclusive handle marks the page dirty.
- A 32-bit frame control word (state, dirty, writeback, latch, pin count) on futexes, a fixed-size open-addressing page table, and a lock-free free list.
- Asynchronous I/O through an I/O engine with POSIX and native `io_uring` backends: multi-block reads, `PrefetchBuffer`, `StartReadBuffers` / `WaitReadBuffers`, and access strategies such as ring buffers for scans.
- Writeback: a flush scheduler with separate foreground and background queues, a background cleaner driven by dirty-page watermarks, and a defined durability contract for `FlushAll()` and `Close()`.
- Write-ahead-log support, without a log: a pre-write hook (flush the WAL up to a page's LSN before it is written), per-page recLSN through `MarkDirty(lsn)`, `MinRecLsn()`, and `CheckpointBuffers(lsn)`.
- Replacement policies: Clock, clock-sweep (PostgreSQL), SIEVE, LRU, LRU-K, 2Q, and a plan-driven Belady policy (`PlanReplacer`). Every policy can place its state in shared memory.
- `std::expected`-based errors (`Result<T>`, `Status`) with a Failed state for unrecoverable errors.

**Sharing**

- Namespaces (up to 16 per kernel). Each has its own replacer, `min_frames` / `max_frames`, and pre-write hook. A page belongs to the namespace that loaded it; hits from other namespaces never move it.
- Facades attach to an existing kernel: `PageCache::Create(options, manager, space)` and `MLPrefetchBuffer::Create(options, manager, space)`.
- Cross-process mode (`BufferManagerOptions::shared_memory`):
  - Any process may create the segment or attach to it, and attaching validates the configuration.
  - Namespaces are declared identically by every process, and their quotas and eviction order hold across processes.
  - Background writeback runs in one elected cleaner process.
  - A crashed process is detected within tens of milliseconds and the segment is fenced.

**Observation**

- An observation area (`ObservationOptions`) in shared memory, in the published binary format of `telepath/observation/observation_format.h`:
  - a summary;
  - per-process telemetry counters;
  - one record per frame (state, flags, pins, page, owning namespace).
- `web/backend/observation.py` parses it directly. The Web console serves it at `GET /api/observation`.
- Pluggable `TelemetrySink` counters, plus benchmark tooling with latency percentiles and sampled snapshots.

## Architecture

<p align="center">
  <img src="https://raw.githubusercontent.com/adj-matrix/adjmatrix-assets/main/TelePath/Architecture.png" width="80%">
</p>

## Quick Look

A buffer pool for a storage engine:

```cpp
#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/disk_backend_factory.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

telepath::BufferManagerOptions options;
options.pool_size = 4096;   // frames
options.page_size = 8192;   // bytes, a power of two in [4 KiB, 4 MiB]
options.pre_write_hook = [&](const telepath::BufferTag &tag, std::span<const std::byte> page) {
  return wal.FlushUpTo(PageLsn(page));  // the WAL rule
};
auto manager = telepath::BufferManager::Create(
    options, telepath::CreateDiskBackend("/data", options.page_size).value(),
    telepath::MakeClockSweepReplacer(options.pool_size), telepath::MakeNoOpTelemetrySink()).value();

{
  auto page = manager->ReadBufferExclusive(/*file_id=*/1, /*block_id=*/42).value();
  std::span<std::byte> bytes = page.mutable_data();  // marks the page dirty
  // ... modify bytes, append a log record at lsn ...
  page.MarkDirty(lsn);                               // records the page's recLSN
}
manager->CheckpointBuffers(redo_lsn);                // everything logged below redo_lsn is durable
manager->Close();
```

A file cache and a training dataset sharing one kernel:

```cpp
telepath::NamespaceOptions files;
files.name = "files";
files.min_frames = 1024;           // a training pass cannot evict these
auto cache = telepath::PageCache::Create({}, *manager, std::move(files)).value();

telepath::NamespaceOptions training;
training.name = "training";
training.max_frames = 2048;        // the dataset never takes more
auto dataset = telepath::MLPrefetchBuffer::Create({}, *manager, std::move(training)).value();

cache->Write(/*file_id=*/7, /*offset=*/0, bytes);
dataset->SetPlan(epoch_order);     // std::vector<telepath::BufferTag>
while (auto next = dataset->Next().value()) Consume(next->handle.data());
```

One cache shared by several processes. Every process runs the same code:

```cpp
options.shared_memory.name = "/telepath_cache";  // create or attach
std::vector<telepath::NamespaceOptions> spaces;
spaces.push_back({.name = "oltp", .replacer = telepath::MakeLruKReplacer(options.pool_size, /*k=*/2), .min_frames = 1024});
spaces.push_back({.name = "scan", .replacer = telepath::MakeSieveReplacer(options.pool_size), .max_frames = 256});
auto shared = telepath::BufferManager::Create(
  options,
  std::move(backend),
  telepath::MakeClockSweepReplacer(options.pool_size),
  telepath::MakeNoOpTelemetrySink(), std::move(spaces)
).value();
telepath::BufferNamespace scan = shared->GetNamespace("scan").value();
auto page = scan.ReadBuffer(/*file_id=*/3, /*block_id=*/0).value();
```

Reading the observation plane from Python:

```python
from web.backend.observation import read_observation  # run from the repository root
snapshot = read_observation("/telepath_observation")
print(snapshot["summary"], len(snapshot["frames"]), snapshot["counters"])
```

The public headers under `src/cpp/include/telepath` document every call precisely, including thread safety, error codes, and durability.

## Status

TelePath is developed in phases recorded in [`docs/design/kernel-refactor.md`](./docs/design/kernel-refactor.md):

| Phase | Content | State |
| --- | --- | --- |
| 0–4 | C++23 toolchain, error model, frame control word, handles, page table, policy layer, asynchronous I/O, writeback and durability | done |
| 5 | Facades: `PageCache`, `MLPrefetchBuffer` with `PlanReplacer` | done |
| 6 | Cross-process mode, observation plane, recLSN and checkpoints, cleaner election, crash detection and recovery | done |
| 7a–7c | Namespaces with quotas; facades sharing one kernel; namespaces across processes | done |
| 7d–7e | Per-namespace statistics and an eviction event stream; Web namespace views | next |
| later | Cache groups for different block sizes; an optional generic log module | planned |

Correctness work goes with each phase:

- **Randomized chaos tests** run most of the API concurrently, against a model of every page, with injected I/O faults. They run in one process and across two, and on io_uring.
- **Model-checked replacer tests** check victims against reference LRU, LRU-K, and Belady models.
- **Torn-read tests** check the observation plane.
- **Crash tests** kill attached processes.
- **Sanitizers and compilers:** the whole suite runs under ASAN and TSAN, with GCC 14 and Clang 19.

[`docs/test.md`](./docs/test.md) lists the suites.

## Build

TelePath keeps environment bootstrap scripts under `support/` and day-to-day entry scripts under `scripts/`. It is written in C++23 and requires **GCC 14 or Clang 19** on Linux.

```bash
sudo ./support/install_build_deps.sh   # GCC 14 and Clang 19 (adds the toolchain PPA and apt.llvm.org on Ubuntu 22.04)
sudo ./support/install_dev_tools.sh    # optional: clang-format, clang-tidy, lldb, valgrind
```

Build and test with the presets:

```bash
./scripts/build/debug.sh   && ./scripts/test/debug.sh
./scripts/build/asan.sh    && ./scripts/test/asan.sh
./scripts/build/tsan.sh    && ./scripts/test/tsan.sh
./scripts/build/release.sh && ctest --preset release
```

On native Linux kernels with `io_uring`, the dedicated path is built and tested separately:

```bash
./scripts/build/io_uring_debug.sh
./scripts/test/io_uring_native.sh
```

`./scripts/build/lsan.sh` prepares a LeakSanitizer build. LSAN may fail in ptrace-restricted environments such as some WSL setups; that is an environment limit, not a known TelePath defect.

Benchmarks (`scripts/bench/`) and the Web console use the release build.

## Web Console

```bash
./scripts/web/serve.sh
```

This starts one local HTTP service that:

- serves the browser UI, in English and Simplified Chinese;
- runs single benchmark passes and thread sweeps;
- shows final and sampled buffer-pool snapshots as a frame map;
- keeps recent run history for the session;
- serves a live observation area at `/api/observation?name=/name`.

Frontend localization resources live under `localization/web-console/`. Namespace views are planned for phase 7e.

## Repository Layout

- `src/cpp/include/telepath`: public headers (buffer, facade, io, replacer, observation, telemetry, options)
- `src/cpp/lib`: implementation
- `test/cpp`: unit, stress, chaos, model, and multi-process tests
- `web/backend`, `web/frontend`: Web console and the Python observation reader
- `scripts`, `support`: build, test, benchmark, and environment scripts

Ownership rules are in the [Repository Tree Guide](./docs/tree.md).

## Documentation

- [Kernel Design and Roadmap](./docs/design/kernel-refactor.md)
- [Concurrency Invariants](./docs/concurrency.md)
- [Test Guide](./docs/test.md)
- [CI Guide](./docs/ci.md)
- [Experiment Guide](./docs/experiment.md)
- [Repository Tree Guide](./docs/tree.md)
- [Architecture Mermaid](./docs/mermaid/architecture.mmd)
- [State 4 Summary (historical)](./docs/state.md)

> 📜 **License**
>
> TelePath is Open Source software released under the MIT License.
>
> 🎨 **Credits**
>
> The header illustration is from "テレパシ (Telepathy)", which inspired my project name. All rights reserved by the original artists.
