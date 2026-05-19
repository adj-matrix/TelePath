# TelePath State 4

## Overview

State 4 is the phase where TelePath turns the State 3 writeback-oriented buffer engine into a prototype with stronger observability, benchmark evidence, and engineering proof material.

The goal of this stage is not to finish every long-term subsystem. The goal is to make the current architecture internally coherent enough that future `io_uring`, richer telemetry transports, and higher-contention experiments can land on top of a working writeback pipeline rather than on top of placeholders.

The post-midterm items from `docs/archive/archive2.md` are now represented in the implementation as:

- expanded writeback and cleaner telemetry,
- operation-level latency summaries and benchmark matrix tooling,
- JSONL plus shared-memory snapshot export paths,
- Web and script output that expose benchmark parameters, throughput, hit rate, writeback pressure, sampled runtime snapshots, and tail latency together,
- documentation for experiment collection, CI evidence, and test coverage.

## What Was Added Beyond State 2

Compared with State 2, the current implementation now includes:

- a dedicated completion dispatcher for disk request completions,
- same-page miss coordination through explicit miss-state ownership,
- a real asynchronous writeback scheduler,
- foreground and background flush queue separation,
- batch write submission with configurable limits,
- background cleaner coordination with dirty-page watermarks,
- stronger flush consistency handling for re-dirty races,
- flush, cleaner, eviction-failure, and snapshot aggregate telemetry,
- backend capability negotiation for fallback and native paths,
- benchmark parameterization for replacer/backend/write-pressure/writeback experiments,
- benchmark operation-latency summaries for p50/p95/p99 tail-latency analysis,
- JSONL telemetry export for point-in-time benchmark snapshots,
- POSIX shared-memory telemetry snapshot export for prototype IPC validation,
- broader correctness and regression coverage around writeback behavior.

## Implemented Components

The current State 4 codebase includes:

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
- `TwoQueueReplacer`
- `TelemetrySink`
- `CounterTelemetrySink`
- `NoOpTelemetrySink`
- telemetry JSONL and shared-memory snapshot export helpers

## Architectural Changes

### 1. Completion Ownership Is Centralized

Disk completions are no longer consumed opportunistically by whichever caller happens to be waiting.

The implementation uses a dedicated completion-dispatch path that registers request ids, waits for backend completions, and routes the result back to the owning waiter. This closes the earlier class of completion-stealing bugs and gives the storage layer a model that can scale from the POSIX fallback backend to a native `io_uring` backend.

### 2. Same-Page Misses Now Coordinate Explicitly

When several threads fault the same page concurrently, only one thread performs the miss work and the others join an explicit miss state.

This reduces duplicate load attempts and keeps page install semantics much closer to what a real concurrent buffer manager needs.

### 3. Writeback Is Now a First-Class Subsystem

The implementation uses an actual flush scheduler instead of ad hoc direct writeback.

The scheduler now supports:

- worker-driven flush execution,
- queue-based decoupling between requesters and disk submission,
- configurable submission batching,
- waiting by task completion rather than by inline disk call ownership.

### 4. Foreground and Background Writeback Are Separated

Explicit flushes triggered by foreground callers are no longer treated as the same queue as cleaner-owned writeback.

The implementation now maintains separate foreground and background queues plus a burst limit so that foreground traffic can stay responsive without starving cleaner progress indefinitely.

### 5. Cleaner Policy Is Usable, Not Decorative

The implementation includes a background cleaner that reacts to dirty-page watermarks and only targets evictable dirty pages.

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

### 8. Writeback State Is Observable

The telemetry surface now reports flush task scheduling, completion, failures, cleaner-owned scheduling and completion, cleaner skips, and eviction rollback failures.

The exported buffer-pool snapshot also includes aggregate dirty, queued-flush, and in-flight-flush counts, so benchmark output and the Web console can explain writeback pressure without re-deriving it from every frame.

Benchmark runs can now append compact JSONL telemetry snapshots containing counters, aggregate writeback state, and per-frame state. They can also publish the latest compact snapshot into a POSIX shared-memory object with `--telemetry-shm-name`, using a fixed header with magic, version, ready flag, sequence, payload size, and payload capacity followed by the serialized JSON payload.

This closes the minimum prototype requirement for shared-memory telemetry export without pretending that the final observation plane is done. The current shared-memory path is a snapshot transport, not a live multi-consumer event ring.

### 9. Benchmark Experiments Are Parameterized

The benchmark entry point can now vary workload, replacer, requested disk backend, write percentage, foreground flush cadence, background cleaner settings, dirty-page watermarks, flush worker limits, queue depth, and backend file-cache size.

`scripts/bench/sweep.sh` runs thread sweeps with the same replacer, backend, write pressure, and writeback knobs accepted by the benchmark binary. `scripts/bench/matrix.sh` builds once and emits a clean CSV matrix across workload, thread count, replacement policy, backend, and write pressure dimensions. `scripts/bench/summarize.py` turns CSV artifacts into Markdown tables that call out best throughput, best hit rate, and lowest p95 latency. The Web console also forwards the same experiment knobs to the benchmark binary so ad hoc browser runs and scripted runs use the same parameter surface.

Benchmark output now includes operation-level latency summaries: min, average, p50, p95, p99, and max. These values measure each logical benchmark operation from `ReadBuffer()` through optional dirty marking and foreground flush completion. This gives the experiment plane enough signal to compare throughput, hit rate, writeback pressure, and tail latency together.

JSON benchmark output also carries a bounded `sampled_snapshots` array. These samples are captured during the run around page reads, dirty marks, and observable runtime I/O states. The Web frame map exposes the final snapshot and the sampled runtime snapshots as selectable views, so pinned, dirty, queued, or in-flight states can be inspected without losing the end-of-run state.

`docs/experiment.md` records the paper-facing experiment path, including recommended matrices, metric definitions, and validity boundaries for GitHub-hosted versus controlled-hardware results.

## Test Coverage

State 4 now validates the following categories:

- smoke-path lifecycle behavior,
- handle and pin/unpin semantics,
- same-page miss ownership and recovery,
- different-page parallel miss behavior,
- eviction and dirty-page persistence,
- dirty-victim writeback rollback and retry behavior,
- flush consistency during re-dirty scenarios,
- read-lock and write-stable flush behavior,
- completion ordering and dispatcher idle behavior,
- async flush scheduling and fairness,
- `FlushAll()` persistence and failure propagation,
- cleaner wakeup and background writeback behavior,
- cleaner failure requeue and retry behavior,
- submit-time and completion-time writeback failures,
- flush/cleaner/eviction telemetry and snapshot aggregate output,
- JSONL and shared-memory telemetry snapshot export,
- options resolution,
- replacer correctness,
- benchmark workload semantics,
- benchmark experiment parameter parsing, latency summaries, sampled runtime snapshots, selectable Web snapshot views, and JSON output shape.

At the time of writing:

- the baseline debug/ASAN suite contains 38 tests,
- the native Linux `io_uring` workflow adds 6 kernel-sensitive tests,
- the writeback scheduler is covered by dedicated adversarial cases, including batch failure, foreground/background interaction, cleaner ownership, and in-flight `FlushAll()` coordination.

## Current Limits

State 4 still does **not** mean TelePath is finished.

The following remain future work:

- stronger long-running stress and soak testing,
- richer benchmark interpretation, charting, and cross-run statistical reporting,
- live shared-memory event-ring telemetry and standalone consumers,
- Socket or other IPC transport integrations,
- richer non-counter observability events,
- deeper `io_uring` optimization beyond correctness-first support,
- NUMA-aware or contention-aware memory/layout tuning,
- WAL / recovery,
- transaction or query-layer features.

## Practical Conclusion

State 4 is the point where TelePath becomes a credible prototype rather than only a promising architecture direction.

It now has:

- explicit miss coordination,
- centralized completion ownership,
- a usable asynchronous writeback path,
- cleaner-backed dirty-page management,
- fallback and native backend separation,
- and a regression suite broad enough to support the next stage of implementation work.

That is enough to treat State 4 as a real architectural milestone.
