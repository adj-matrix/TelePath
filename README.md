# TelePath: An Observable Buffer Pool SDK for Modern Storage

<!-- # TelePath: A High-Performance, Observable Kernel (e.g., Cache) SDK for Heterogeneous Storage -->

<p align="center">
  <img src="https://raw.githubusercontent.com/adj-matrix/adjmatrix-assets/main/TelePath/Telepathy.webp" width="60%">
</p>

<p align="center">
  <strong>!!!🤖WIP Working in Progress🤖!!! In order to meet academic integrity requirements, I am sorry to not accept any Issue and Pull Request (PR) until July.</strong>
</p>

<p align="center">
  English | <a href="./localization/README.zh-CN.md">简体中文</a>
</p>

**TelePath** (Telepathy, Telemetry + Path) is a research-oriented C++17 SDK for building an observable buffer pool and page cache engine for modern storage environments.

It is not intended to be a full DBMS. The project is scoped as a reusable systems library with:

- a concurrent buffer pool core,
- pluggable replacement policies,
- a stable POSIX fallback backend,
- a native `io_uring` backend path for supported Linux environments,
- a low-overhead telemetry interface designed to stay off the hot path.

TelePath is being developed with a deliberate split between the data plane and the observation plane so that instrumentation remains part of the architecture rather than an afterthought.

## Motivation

Modern storage devices can expose far more concurrency than traditional teaching-oriented buffer managers were designed to exploit. At the same time, many systems prototypes remain difficult to observe internally without resorting to invasive logging or heavyweight tracing.

TelePath exists to provide a smaller and more auditable experimental base for studying questions such as:

- how a buffer pool scales under increasing thread counts,
- how replacement policy behavior changes under skewed workloads,
- how much observability can be added before it disturbs the data path,
- how an async-ready design should be staged under realistic development constraints such as WSL2.

## Current Scope

TelePath currently targets the following system boundary:

- It is a C++ library.
- It manages page-sized blocks in memory and coordinates persistence through a storage backend.
- It exposes observability through telemetry sinks.
- It is designed as a foundation for benchmarking and future experimentation.

TelePath does **not** currently aim to include:

- SQL parsing,
- query execution,
- B+Tree indexes,
- MVCC or transaction management,
- WAL or crash recovery,
- production-grade monitoring integrations in the first milestone.

## State 3 Status

The current implementation has moved beyond the original State 1/2 skeleton and now includes a usable State 3 writeback core:

- `BufferManager`, `BufferHandle`, and `BufferDescriptor`
- contiguous frame memory allocation
- `DiskBackend`, `PosixDiskBackend`, `IoUringDiskBackend`, and `DiskBackendFactory`
- miss coordination for concurrent same-page faults
- centralized completion dispatch for submitted disk requests
- asynchronous flush scheduling with worker threads
- foreground/background flush queue separation with configurable batching
- cleaner-backed dirty-page writeback with watermark-driven activation
- `ClockReplacer`, `LruReplacer`, and `LruKReplacer`
- counter-based and no-op telemetry sinks
- benchmark and CI paths for fallback and native validation

This means TelePath now supports not only the basic buffer lifecycle, but also:

1. synchronous external page access on top of internal async-style disk orchestration,
2. explicit same-page miss ownership instead of duplicate load work,
3. a real writeback path with completion routing and task waiting,
4. cleaner-assisted dirty-page management,
5. stronger regression coverage around flush correctness, failure handling, and scheduler behavior.

## Architecture

<p align="center">
  <img src="https://raw.githubusercontent.com/adj-matrix/adjmatrix-assets/main/TelePath/Architecture.png" width="80%">
</p>

## Build

TelePath keeps environment bootstrap scripts under `support/` and day-to-day project entry scripts under `scripts/`.

### Minimal Build Dependencies

```bash
sudo ./support/install_build_deps.sh
```

### Recommended Development Tools

```bash
sudo ./support/install_dev_tools.sh
```

This installs tools such as `clang`, `clang-format`, `clang-tidy`, `lldb`, and `valgrind`. These are recommended for development, but they are not required for the minimal build path.

### Debug Build

```bash
./scripts/build/debug.sh
./scripts/test/debug.sh
```

### ASAN Build

```bash
./scripts/build/asan.sh
./scripts/test/asan.sh
```

### Native `io_uring` Validation

On supported native Linux kernels, the dedicated `io_uring` path can be built and tested separately:

```bash
./scripts/build/io_uring_debug.sh
./scripts/test/io_uring_native.sh
```

### LSAN Notes

`LeakSanitizer` support is prepared through:

```bash
./scripts/build/lsan.sh
```

However, LSAN execution may fail in ptrace-constrained or restricted WSL-style environments. That limitation is environmental rather than a known TelePath correctness issue.

### Web Console

TelePath also includes a browser-based console for running workloads and inspecting recent results from the engine.

Run it with:

```bash
./scripts/web/serve.sh
```

This starts a single local HTTP service that:

- serves the browser UI,
- supports runtime language switching between English and Simplified Chinese,
- runs single benchmark passes,
- runs thread-sweep experiments,
- retains recent run and sweep history in the server session,
- and exposes all of that through a thin API backed by the existing TelePath debug build.

Frontend localization resources live under `localization/web-console/`.

## Repository Layout

The repository structure and ownership rules are documented here:

- [Repository Tree Guide](./docs/tree.md)

The main implementation currently lives under:

- `src/cpp/include/telepath`
- `src/cpp/lib`
- `test/cpp`
- `web/frontend`
- `web/backend`
- `gui`
- `support`
- `scripts`

## Roadmap

- Phase 1: establish a stable, testable buffer pool skeleton
- Phase 2: strengthen concurrent lifecycle semantics, async-ready I/O boundaries, and benchmark scaffolding
- Phase 3: deliver centralized completion ownership, async writeback scheduling, and cleaner-backed dirty-page management
- Phase 4: continue toward shared-memory telemetry transport, deeper native backend optimization, and larger-scale experimentation

The current implementation is still early-stage. The main focus is architectural correctness, stable interfaces, and controlled extensibility rather than premature performance claims.

## Documentation

Public project documentation lives under `docs/`.

Recommended entry points:

- [State Summary](./docs/state.md)
- [CI Guide](./docs/ci.md)
- [Test Guide](./docs/test.md)
- [Repository Tree Guide](./docs/tree.md)
- [Architecture Mermaid](./docs/mermaid/architecture.mmd)

> 📜 **License**
>
> TelePath is Open Source software released under the MIT License.
>
> 🎨 **Credits**
>
> The header illustration is from "テレパシ (Telepathy)", which inspired my project name. All rights reserved by the original artists.
