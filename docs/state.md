# TelePath State 3

## Overview

State 3 is the phase where TelePath stops being only "async-ready in shape" and starts behaving like a usable writeback-oriented concurrent buffer engine.

The goal of this stage is not to finish every long-term subsystem. The goal is to make the current architecture internally coherent enough that future `io_uring`, shared-memory telemetry, and higher-contention experiments can land on top of a working writeback pipeline rather than on top of placeholders.

## What Was Added Beyond State 2

Compared with State 2, the current implementation now includes:

- a dedicated completion dispatcher for disk request completions,
- same-page miss coordination through explicit miss-state ownership,
- a real asynchronous writeback scheduler,
- foreground and background flush queue separation,
- batch write submission with configurable limits,
- background cleaner coordination with dirty-page watermarks,
- stronger flush consistency handling for re-dirty races,
- backend capability negotiation for fallback and native paths,
- broader correctness and regression coverage around writeback behavior.

## Implemented Components

The current State 3 codebase includes:

- `BufferManager`
- `BufferHandle`
- `BufferDescriptor`
- contiguous `FrameMemoryPool`
- `DiskBackend`
- `PosixDiskBackend`
- `IoUringDiskBackend`
- `DiskBackendFactory`
- `CompletionDispatcher`
- `Replacer`
- `ClockReplacer`
- `LruReplacer`
- `LruKReplacer`
- `TelemetrySink`
- `CounterTelemetrySink`
- `NoOpTelemetrySink`

## Architectural Changes

### 1. Completion Ownership Is Centralized

Disk completions are no longer consumed opportunistically by whichever caller happens to be waiting.

State 3 introduces a dedicated completion-dispatch path that registers request ids, waits for backend completions, and routes the result back to the owning waiter. This closes the earlier class of completion-stealing bugs and gives the storage layer a model that can scale from the POSIX fallback backend to a native `io_uring` backend.

### 2. Same-Page Misses Now Coordinate Explicitly

When several threads fault the same page concurrently, only one thread performs the miss work and the others join an explicit miss state.

This reduces duplicate load attempts and keeps page install semantics much closer to what a real concurrent buffer manager needs.

### 3. Writeback Is Now a First-Class Subsystem

State 3 introduces an actual flush scheduler instead of ad hoc direct writeback.

The scheduler now supports:

- worker-driven flush execution,
- queue-based decoupling between requesters and disk submission,
- configurable submission batching,
- waiting by task completion rather than by inline disk call ownership.

### 4. Foreground and Background Writeback Are Separated

Explicit flushes triggered by foreground callers are no longer treated as the same queue as cleaner-owned writeback.

The implementation now maintains separate foreground and background queues plus a burst limit so that foreground traffic can stay responsive without starving cleaner progress indefinitely.

### 5. Cleaner Policy Is Usable, Not Decorative

State 3 adds a background cleaner that reacts to dirty-page watermarks and only targets evictable dirty pages.

The cleaner is still intentionally conservative, but it is now real enough to:

- pre-clean dirty frames before eviction pressure becomes critical,
- cooperate with the main flush scheduler,
- avoid duplicate cleaner ownership on the same frame,
- respect in-flight flush state.

### 6. Flush Correctness Handles Re-Dirty Races

The writeback path no longer assumes that a successful flush means the frame is now globally clean.

Dirty state is only cleared when the flushed generation still matches the resident page generation at completion time. If the page is modified again while a flush is in flight, the later dirty state survives and can be re-queued safely.

### 7. Backend Capability Negotiation Is Now Real

Backends now expose capability information that affects runtime policy decisions, such as batching defaults and fallback behavior.

This keeps the public model stable while allowing the implementation to choose different defaults for POSIX fallback and native `io_uring` execution environments.

## Test Coverage

State 3 now validates the following categories:

- smoke-path lifecycle behavior,
- handle and pin/unpin semantics,
- same-page miss ownership and recovery,
- different-page parallel miss behavior,
- eviction and dirty-page persistence,
- flush consistency during re-dirty scenarios,
- read-lock and write-stable flush behavior,
- completion ordering and dispatcher idle behavior,
- async flush scheduling and fairness,
- `FlushAll()` persistence and failure propagation,
- cleaner wakeup and background writeback behavior,
- submit-time and completion-time writeback failures,
- options resolution,
- replacer correctness,
- benchmark workload semantics.

At the time of writing:

- the baseline debug/ASAN suite contains 27 tests,
- the native Linux `io_uring` workflow adds 6 kernel-sensitive tests,
- the writeback scheduler is covered by dedicated adversarial cases, including batch failure, foreground/background interaction, cleaner ownership, and in-flight `FlushAll()` coordination.

## Current Limits

State 3 still does **not** mean TelePath is finished.

The following remain future work:

- stronger long-running stress and soak testing,
- more mature benchmark interpretation and reporting,
- shared-memory telemetry transport,
- a richer observability event model,
- deeper `io_uring` optimization beyond correctness-first support,
- NUMA-aware or contention-aware memory/layout tuning,
- WAL / recovery,
- transaction or query-layer features.

## Practical Conclusion

State 3 is the point where TelePath becomes a credible concurrent buffer-engine core rather than only a promising architecture direction.

It now has:

- explicit miss coordination,
- centralized completion ownership,
- a usable asynchronous writeback path,
- cleaner-backed dirty-page management,
- fallback and native backend separation,
- and a regression suite broad enough to support the next stage of implementation work.

That is enough to treat State 3 as a real architectural milestone.
