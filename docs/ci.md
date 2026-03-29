# CI Guide

TelePath uses GitHub Actions to keep the main branch continuously buildable and testable on a clean Linux runner.

## Workflow Location

The primary CI workflow lives at:

- `.github/workflows/ci.yml`
- `.github/workflows/benchmark.yml`
- `.github/workflows/io_uring-native.yml`

## Triggers

The current workflow runs on:

- `push` to `main` and `master`
- `pull_request`
- `workflow_dispatch`

## Current Jobs

The first CI iteration intentionally stays small and reliable. It currently runs:

- `GCC Debug`
- `GCC ASAN`
- `Clang Debug`
- `io_uring Debug` in a separate native-storage workflow

These jobs reuse the same repository scripts developers run locally:

- `./scripts/build/debug.sh`
- `./scripts/test/debug.sh`
- `./scripts/build/asan.sh`
- `./scripts/test/asan.sh`

This keeps local and CI behavior aligned and reduces workflow drift.

## Native io_uring Workflow

`io_uring` validation now lives in a separate workflow so that native-kernel-sensitive checks do not get mixed into the baseline portability jobs.

That workflow:

- builds with `TELEPATH_ENABLE_IO_URING=ON`,
- uses `./scripts/build/io_uring_debug.sh`,
- runs only tests labeled `native_io_uring`,
- sets `TELEPATH_REQUIRE_IO_URING_SUCCESS=1` so the job fails if the runner cannot execute the real `io_uring` path.

This separation matters because the normal Linux/WSL development path must continue to validate fallback behavior even on systems where `io_uring` is blocked or unavailable.

## Benchmark Workflow

Benchmark collection runs in a separate workflow so that performance trend gathering does not block normal development checks.

The benchmark workflow:

- runs on `workflow_dispatch`,
- runs on a nightly `schedule`,
- builds the debug benchmark target,
- executes workload sweeps for `hotspot`, `uniform`, `sequential_shared`, and `sequential_disjoint`,
- uploads CSV results as GitHub Actions artifacts,
- publishes a compact workflow summary directly in the Actions UI.

## What CI Is For

Current CI is intended to catch:

- compile failures,
- unit and integration test regressions,
- sanitizer-detectable memory problems,
- basic compiler-environment differences between GCC and Clang.

## What CI Is Not For

GitHub-hosted runners are useful for correctness validation and coarse regression tracking, but they are not a stable source of final systems-performance data.

Current CI should not be treated as the final authority for:

- throughput claims,
- IOPS claims,
- latency distributions,
- storage-sensitive benchmark conclusions.

Those measurements should still be collected on controlled machines.

## Why This Matters For TelePath

TelePath is already in a stage where the core codebase includes:

- concurrent buffer lifecycle logic,
- async-style disk request handling,
- dirty-page flush coordination,
- sanitizer-backed validation paths.

At this point, automatic Linux-side validation is part of basic engineering hygiene rather than an optional convenience.

## Future Extensions

Reasonable next steps for CI include:

- artifact upload for benchmark CSV output,
- broader compiler and Ubuntu matrices,
- stricter branch protection based on required CI checks.
