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
- a stable POSIX storage backend for early development,
- a future path toward `io_uring`,
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

## State 1 Status

The current implementation already includes a working State 1 skeleton:

- `BufferManager`, `BufferHandle`, and `BufferDescriptor`
- `DiskBackend` and `PosixDiskBackend`
- `Replacer` abstraction with `ClockReplacer` and `LruReplacer`
- `TelemetrySink` abstraction with counter-based and no-op sinks
- CMake-based debug and sanitizer build scripts
- smoke, replacer, handle, and telemetry tests

This means the project already has a minimal but functioning end-to-end loop:

1. load a block through the buffer manager,
2. access page memory through a controlled handle,
3. mark and flush dirty data through the backend,
4. observe basic telemetry counters,
5. validate behavior through tests.

## Architecture

<p align="center">
  <img src="https://raw.githubusercontent.com/adj-matrix/adjmatrix-assets/main/TelePath/Architecture.png" width="60%">
</p>

## Build

TelePath keeps project-related environment scripts under `support/`.

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
./support/build_debug.sh
./support/test.sh
```

### ASAN Build

```bash
./support/build_asan.sh
./support/test_asan.sh
```

### LSAN Notes

`LeakSanitizer` support is prepared through:

```bash
./support/build_lsan.sh
```

However, LSAN execution may fail in ptrace-constrained or restricted WSL-style environments. That limitation is environmental rather than a known TelePath correctness issue.

## Repository Layout

The repository structure and ownership rules are documented here:

- [Repository Tree Guide](./docs/tree.md)

The main implementation currently lives under:

- `src/cpp/include/telepath`
- `src/cpp/lib`
- `test/cpp`
- `support`

## Roadmap

- Phase 1: establish a stable, testable buffer pool skeleton
- Phase 2: strengthen buffer lifecycle semantics and expand test coverage
- Phase 3: add richer replacement policies and benchmark scaffolding
- Phase 4: introduce async backends and external observability integrations

The current implementation is still early-stage. The main focus is architectural correctness, stable interfaces, and controlled extensibility rather than premature performance claims.

## Documentation

Public project documentation lives under `docs/`.

Recommended entry points:

- [State 1 Summary](./docs/state1.md)
- [Repository Tree Guide](./docs/tree.md)
- [State 1 Architecture Mermaid](./docs/mermaid/architecture-state1.mmd)

> 📜 **License**
>
> TelePath is Open Source software released under the MIT License.
>
> 🎨 **Credits**
>
> The header illustration is from "テレパシ (Telepathy)", which inspired my project name. All rights reserved by the original artists.
