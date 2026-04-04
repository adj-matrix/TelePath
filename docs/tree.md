# TelePath Repository Tree Guide

## 1. Purpose

This document defines the intended public repository layout for TelePath.

Its goal is to reduce ambiguity for future contributors, coding agents, and Copilot-like tools. Public files should follow this structure unless there is a clear architectural reason not to.

The repository should remain organized around one principle:

**separate product code, experiment code, reference material, and evolving design notes.**

## 2. Top-Level Directory Rules

### `/docs`

Owns all public-facing project documentation.

Use this directory for:

- public architecture notes,
- user-facing project documentation,
- repository conventions,
- stable implementation summaries,
- diagrams and reference material intended for readers of the repository.

Do not use this directory as a dump for private design churn, daily development logs, or internal back-and-forth notes.

### `/src`

Owns all implementation code.

This directory should contain the actual TelePath library and any language-specific runtime components.

Recommended split:

- `/src/cpp` for the primary C++ SDK and binaries,
- `/src/rust` for future Rust support or experimental components.

### `/test`

Owns all tests.

This includes:

- unit tests,
- concurrency tests,
- integration tests,
- regression tests,
- benchmark-adjacent correctness tests.

Benchmarks that are primarily for performance reporting should not live here if they become large enough to deserve a separate area.

### `/cli`

Owns developer-facing or user-facing command-line entrypoints.

Examples:

- benchmark launcher,
- trace dumper,
- telemetry consumer demo,
- debug admin tool.

Keep CLI code thin. It should call into library code from `/src`, not reimplement logic.

### `/web`

Owns the browser-based presentation plane.

Use this directory for:

- the web frontend used for demos or dashboards,
- the thin web backend that serves browser requests,
- benchmark and sweep orchestration for the browser console,
- UI assets and browser-oriented interaction logic that stay outside the core C++ library.

Recommended split:

- `/web/frontend` for browser assets and client-side code,
- `/web/backend` for the lightweight HTTP layer that calls scripts or reads exported results.

Keep this directory presentation-oriented. It should not become the home of core storage-engine logic.

### `/gui`

Owns future desktop GUI work.

Keep this directory separate from `/web` so that browser-facing and desktop-facing presentation layers do not get mixed together.

### `/config`

Owns static configuration templates and example experiment configs.

Examples:

- benchmark workload config,
- logger config,
- telemetry export config,
- plotting presets.

Do not store generated runtime outputs here.

### `/localization`

Owns translatable language resources.

Use this directory for:

- localized README files,
- web-console translation bundles,
- future GUI language packs,
- other human-language resources that should remain independent from runtime configuration.

Recommended split:

- `/localization/web-console` for browser UI strings,
- top-level localized docs such as `README.zh-CN.md`.

Do not mix localization payloads into `/config` unless they are genuinely machine-tuned runtime options rather than user-facing language resources.

### `/support`

Owns helper assets for development workflows that are not part of the core library.

Examples:

- shell helpers,
- CMake helper modules,
- local dev scripts,
- packaging helpers,
- CI support artifacts.

If a script becomes central to benchmark execution, consider moving it into a more explicit subdirectory later.

### `/bin`

Treat as generated or utility-binary space, not as a source directory.

Do not place handwritten source files here.

If this directory is kept, document whether it stores:

- built binaries,
- wrapper scripts,
- downloaded helper tools.

### `/build`

Build output only.

Never place handwritten source or documentation here.

This directory should usually be gitignored.

### `/.reference`

Read-only external reference material.

This is where upstream inspiration or comparison code can live, such as CMU 15-445 materials. Nothing in this directory should be treated as TelePath source of truth.

Rules:

- do not edit casually,
- do not couple production code to paths inside `.reference`,
- use it for study and interface comparison only.

## 3. Recommended Source Tree

The primary implementation should converge toward the following shape:

```text
src/
  cpp/
    include/
      telepath/
        buffer/
        io/
        replacer/
        telemetry/
        common/
    lib/
      buffer/
      io/
      replacer/
      telemetry/
      common/
    apps/
    CMakeLists.txt
  rust/
```

For presentation-layer code, keep a separate tree:

```text
web/
  frontend/
  backend/

gui/

localization/
  web-console/
```

### `/src/cpp/include/telepath`

Public headers only.

This directory defines the external SDK surface. Headers here should be stable, minimal, and documented.

Recommended areas:

- `buffer/`: buffer manager API, buffer handle, descriptor types,
- `io/`: disk backend interfaces,
- `replacer/`: policy interfaces,
- `telemetry/`: sink interfaces and metric/event types,
- `common/`: status types, ids, options, utility traits.

### `/src/cpp/lib`

Private implementation files.

This is where `.cc` files and non-public internal headers should live.

Recommended internal areas:

- `buffer/`: frame table, descriptor state machine, latch helpers,
- `io/`: posix backend, future uring backend,
- `replacer/`: LRU, LRU-K, Clock implementations,
- `telemetry/`: in-process sink, future shared-memory sink,
- `common/`: low-level utilities and platform abstractions.

### `/src/cpp/apps`

Small internal applications for manual verification.

Examples:

- local benchmark runner,
- sample buffer pool demo,
- telemetry printer.

These are not the public CLI surface unless explicitly promoted.

## 4. Recommended Test Tree

Tests should mirror the module layout of the library:

```text
test/
  cpp/
    unit/
      buffer/
      io/
      replacer/
      telemetry/
    integration/
    concurrency/
    benchmark/
```

### `/test/cpp/unit`

Fast deterministic tests for isolated modules.

### `/test/cpp/integration`

Cross-module tests such as:

- buffer pool + disk backend,
- telemetry sink + buffer manager,
- replacer behavior under realistic page lifecycles.

### `/test/cpp/concurrency`

Dedicated tests for:

- pin/unpin races,
- eviction races,
- dirty page flush coordination,
- lock contention behavior.

### `/test/cpp/benchmark`

Microbenchmarks and repeatable performance experiments.

If benchmark code becomes large and starts generating datasets, it may later move into a dedicated `/bench` directory. For now, keeping it under `test` is acceptable if the scope is still small.

## 5. Recommended Documentation Tree

The `docs` directory should become more explicit:

```text
docs/
  state1.md
  tree.md
  mermaid/
  guide/
  manual/
  reference/
```

### `/docs/mermaid`

Owns diagram source files that can later be rendered into polished assets.

This is the right place for:

- architecture flowcharts,
- module relationship diagrams,
- data path sketches,
- future visual assets in source form.

### `/docs/guide`

Owns practical onboarding and usage-oriented documentation.

This is the right place for:

- getting started material,
- build instructions,
- development workflows intended for contributors,
- quickstart guides.

### `/docs/manual`

Owns more structured feature and behavior documentation.

Examples:

- buffer manager semantics,
- storage backend behavior,
- telemetry model,
- testing conventions.

### `/docs/reference`

Owns public reference-style material.

Examples:

- glossary,
- terminology notes,
- public API summaries,
- compatibility notes.

## 6. Naming and Lifecycle Rules

To keep the repository understandable, use these conventions:

- Keep `docs/` public, stable, and reader-oriented.
- Do not expose internal working drafts as first-class public documentation.
- Keep public header names domain-oriented, not assignment-oriented. Prefer `buffer_manager.h` over `bpm.h`.
- Avoid mixing generated artifacts with source-controlled files in the same directory.

## 7. Immediate Recommendation For TelePath

Given the current repository state, the next clean step is:

1. Keep `docs/` focused on public project documentation.
2. Keep historical or internal design churn out of the public doc entrypoints.
3. Build the C++ library under `src/cpp/include/telepath` and `src/cpp/lib`.
4. Mirror tests under `test/cpp`.
5. Keep browser-facing demo code under `web/frontend` and `web/backend`.
6. Reserve `gui/` for future desktop presentation work.
7. Add public manuals and guides only when the behavior they describe is stable.

## 8. Canonical Ownership Summary

When in doubt, follow this ownership map:

- public documentation: `/docs`
- C++ product code: `/src/cpp`
- Rust experiments or future SDK work: `/src/rust`
- tests and benchmarks: `/test`
- browser presentation plane: `/web`
- desktop presentation plane: `/gui`
- developer tooling: `/support`
- runtime configs and templates: `/config`
- upstream references: `/.reference`

This should be the default mental model for all future work in the repository.
