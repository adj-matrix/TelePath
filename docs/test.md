# Test Guide

## Purpose

TelePath keeps its test suite focused on two goals:

- verifying correctness under normal and adversarial conditions,
- keeping benchmark workload semantics explainable and regression-friendly.

The suite is meant to stay broad enough to catch real regressions without turning into a collection of near-duplicate cases.

## Current Layout

The C++ tests live under `test/cpp/` and are grouped by subsystem:

- `benchmark/`
- `buffer/`
- `config/`
- `io/`
- `replacer/`
- `telemetry/`

This keeps the tests close to the structure of the library itself and makes future additions easier to place consistently.

## Current Coverage

The current suite covers:

- smoke-path lifecycle behavior,
- handle and pin/unpin semantics,
- concurrent read and same-page miss coordination,
- eviction and flush behavior,
- failure and resource exhaustion paths,
- disk backend correctness,
- replacer correctness,
- telemetry correctness,
- options resolution,
- benchmark workload semantics.

## How To Run

Debug validation:

```bash
./scripts/build/debug.sh
./scripts/test/debug.sh
```

ASAN validation:

```bash
./scripts/build/asan.sh
./scripts/test/asan.sh
```

## Benchmark Note

Benchmark correctness is tested separately from benchmark performance output.

In practice this means:

- normal tests validate workload-selection logic,
- GitHub benchmark workflows collect trend data,
- benchmark numbers are useful for regression observation, not final performance claims.

## Maintenance Rule

New tests should prefer one of the following:

- a new boundary,
- a new concurrency shape,
- a new failure mode,
- a new workload semantic guarantee.

Avoid adding tests that only restate behavior already proven elsewhere.
