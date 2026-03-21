# TelePath State 1

## Overview

State 1 is the first stable engineering checkpoint for TelePath.

Its purpose is not to maximize performance. Its purpose is to establish a correct and extensible foundation for the SDK:

- a working buffer manager,
- controlled page access through buffer handles,
- a stable synchronous storage backend,
- pluggable replacement policy interfaces,
- basic observability hooks,
- reproducible build and test entrypoints.

## What Is Implemented

The current State 1 implementation includes:

- `BufferManager`
- `BufferHandle`
- `BufferDescriptor`
- `DiskBackend`
- `PosixDiskBackend`
- `Replacer`
- `ClockReplacer`
- `LruReplacer`
- `TelemetrySink`
- `CounterTelemetrySink`
- `NoOpTelemetrySink`

The build system also includes:

- debug builds,
- ASAN builds,
- optional LSAN-oriented build configuration,
- project-local setup scripts under `support/`.

## Current Behavior

At this stage, TelePath supports the following minimal lifecycle:

1. read a block by `(file_id, block_id)`,
2. pin the corresponding in-memory frame,
3. access page memory through a controlled handle,
4. mark the page dirty,
5. flush dirty contents through the storage backend,
6. release the handle and make the frame evictable again,
7. emit basic telemetry counters for key operations.

## Design Characteristics

State 1 intentionally favors architectural clarity over aggressive optimization.

Key choices:

- POSIX backend first, `io_uring` later
- explicit buffer metadata through descriptors
- move-only buffer handles
- telemetry via sink abstraction rather than external metrics coupling
- replacement policy behind a stable interface
- page lifecycle checks tightened before concurrency is expanded further

## Test Coverage

State 1 currently validates:

- smoke-path buffer lifecycle
- replacement policy baseline behavior
- buffer handle release and invalidation semantics
- telemetry sink semantics

This is enough to validate the first end-to-end skeleton, but not enough to claim concurrency completeness or production readiness.

## Known Limits

State 1 does not yet provide:

- `io_uring` backend
- `LRU-K`
- shared-memory telemetry transport
- benchmark harness
- concurrency stress coverage
- recovery or WAL
- transaction or query-layer features

## Recommended Next Direction

After State 1, the most sensible next steps are:

- strengthen concurrent lifecycle correctness,
- add richer unit and race-oriented tests,
- introduce `LRU-K`,
- begin benchmark scaffolding,
- stage the future async backend behind the existing disk abstraction.
