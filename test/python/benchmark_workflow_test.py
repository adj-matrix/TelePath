#!/usr/bin/env python3

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def read_text(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def assert_contains(text: str, needle: str) -> None:
    assert needle in text, f"missing expected benchmark workflow contract: {needle}"


def main() -> int:
    benchmark_workflow = read_text(".github/workflows/benchmark.yml")
    bench_run = read_text("scripts/bench/run.sh")
    bench_sweep = read_text("scripts/bench/sweep.sh")
    bench_matrix = read_text("scripts/bench/matrix.sh")

    assert_contains(benchmark_workflow, "io_uring_release")
    assert_contains(benchmark_workflow, "for backend in posix io_uring")
    assert_contains(benchmark_workflow, "scan_pollution")

    assert_contains(bench_run, "BENCHMARK_PRESET")
    assert_contains(bench_sweep, "FLUSH_ALL_AFTER_RUN")
    assert_contains(bench_sweep, "--flush-all-after-run")
    assert_contains(bench_sweep, "SYNC_ON_WRITE")
    assert_contains(bench_sweep, "--sync-on-write")
    assert_contains(bench_matrix, "scan_pollution")
    assert_contains(bench_matrix, "FLUSH_ALL_AFTER_RUN")
    assert_contains(bench_matrix, "SYNC_ON_WRITE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
