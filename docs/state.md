# TelePath State 2

## Overview

State 2 is the phase where TelePath moves beyond a functional buffer pool skeleton and begins to behave like an extensible concurrent systems core.

The purpose of this stage is not to finalize every subsystem. The purpose is to remove the most obvious architectural limits left in State 1 and replace them with structures that can support future async I/O, richer replacement policies, and more realistic performance experiments.

## What Was Added Beyond State 1

Compared with State 1, the current implementation now includes:

- contiguous frame memory allocation,
- cache-line-aware descriptor alignment preparation,
- a less serialized hit path,
- an async-ready disk abstraction with submit/poll semantics,
- page-content latching for stable flush snapshots,
- descriptor-level in-flight I/O coordination,
- a baseline `LRU-K` implementation,
- a configurable `BufferManagerOptions` entrypoint,
- an initial benchmark skeleton,
- broader tests for concurrency, eviction, and failure paths.

## Implemented Components

The current State 2 codebase includes:

- `BufferManager`
- `BufferHandle`
- `BufferDescriptor`
- contiguous `FrameMemoryPool`
- `DiskBackend`
- `PosixDiskBackend`
- `Replacer`
- `ClockReplacer`
- `LruReplacer`
- `LruKReplacer`
- `TelemetrySink`
- `CounterTelemetrySink`
- `NoOpTelemetrySink`

## Architectural Changes

### 1. Contiguous Frame Memory

Frame storage is no longer implemented as nested vectors.

Instead, TelePath now allocates a contiguous frame memory pool and maps `frame_id` to page memory by offset. This is a much better fit for systems work because it improves locality and prepares the project for future aligned I/O and async backend evolution.

### 2. Descriptor State Tightening

Descriptors now carry more explicit state around:

- resident vs loading,
- in-flight I/O,
- last I/O status.

This provides the minimum machinery required to coordinate concurrent readers around a page that is still being loaded.

### 3. Reduced Global Serialization

State 2 begins to reduce the reliance on a single global coordination lock.

The current implementation separates:

- striped page-table synchronization,
- free-list synchronization,
- miss-path serialization,
- descriptor-local locking.

This is not the final concurrency model, but it is materially better than forcing every lookup through one global latch.

### 4. Async-Ready Disk Interface

The disk abstraction is no longer purely synchronous in shape.

It now exposes:

- `SubmitRead`
- `SubmitWrite`
- `PollCompletion`

The current POSIX backend is still a transitional implementation, but it now runs requests through a background worker and completion queue. This keeps the public boundary aligned with a future async backend instead of hard-coding synchronous call/return behavior into the API.

### 5. Descriptor-Level Waiting

When multiple threads request the same block and one thread is already loading it, later threads can now wait on descriptor state rather than all attempting independent load logic.

This is an important semantic step toward a real concurrent buffer manager.

### 6. `LRU-K` Support

State 2 adds a baseline `LRU-K` replacer implementation.

The current implementation is intentionally straightforward and correctness-oriented. It is suitable for behavior testing and benchmark exploration, even if it is not yet the final high-performance form.

### 7. Benchmark Skeleton

State 2 introduces a small but usable benchmark executable and scripts under `scripts/`.

The current benchmark supports:

- configurable thread count,
- configurable pool size,
- configurable block count,
- configurable operations per thread,
- multiple workload modes,
- CSV-friendly output,
- simple sweep execution.

This is enough to begin throughput and hit-rate exploration without claiming a finished benchmark platform.

## Test Coverage

State 2 now validates the following categories:

- smoke-path lifecycle
- replacer baseline behavior
- `LRU-K` behavior
- buffer handle semantics
- telemetry sink behavior
- memory layout assumptions
- concurrent read behavior
- same-page miss coordination
- disk backend submit/poll behavior
- dirty eviction behavior
- failure paths for read and write I/O
- wait/evict interleaving in small-capacity scenarios

At the time of writing, the project test suite contains 17 tests and passes in both debug and ASAN configurations under the current development environment.

## Current Limits

State 2 still does **not** mean TelePath is finished.

The following remain future work:

- a real `io_uring` backend,
- a dedicated completion thread or more advanced completion handling,
- shared-memory telemetry transport,
- stronger contention-aware observability,
- larger concurrency stress coverage,
- more mature benchmark automation,
- WAL / recovery,
- transaction or query-layer features.

## Practical Conclusion

State 2 is the point where TelePath becomes meaningfully closer to its intended long-term architecture.

It now has:

- a better memory model,
- a more credible concurrency direction,
- an async-ready I/O boundary,
- richer replacement policy support,
- and a benchmark entrypoint for early experimental work.

That is enough to treat State 2 as a real architectural milestone rather than just a collection of incremental patches.
