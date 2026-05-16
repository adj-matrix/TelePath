#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path
from typing import Iterable


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Summarize TelePath benchmark CSV files as Markdown."
    )
    parser.add_argument(
        "csv_files",
        nargs="+",
        help="Benchmark CSV files to summarize. Use '-' to read one CSV from stdin.",
    )
    return parser.parse_args()


def read_rows(path: str) -> list[dict[str, str]]:
    if path == "-":
        return list(csv.DictReader(sys.stdin))
    with Path(path).open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def parse_float(row: dict[str, str], name: str) -> float | None:
    value = row.get(name)
    if value is None or value == "":
        return None
    try:
        return float(value)
    except ValueError:
        return None


def parse_int(row: dict[str, str], name: str) -> int | None:
    value = parse_float(row, name)
    if value is None:
        return None
    return int(value)


def label(row: dict[str, str], name: str, fallback: str = "-") -> str:
    value = row.get(name)
    if value is None or value == "":
        return fallback
    return value.replace("|", "\\|")


def format_number(value: float | None, digits: int = 0) -> str:
    if value is None:
        return "-"
    return f"{value:,.{digits}f}"


def format_percent(value: float | None) -> str:
    if value is None:
        return "-"
    return f"{value * 100.0:.2f}%"


def format_latency_us(row: dict[str, str], name: str) -> str:
    value = parse_float(row, name)
    if value is None:
        return "-"
    return f"{value / 1000.0:.2f}"


def sort_key(row: dict[str, str]) -> tuple[str, str, str, int, int]:
    return (
        label(row, "workload"),
        label(row, "replacer"),
        label(row, "requested_disk_backend"),
        parse_int(row, "write_percent") or 0,
        parse_int(row, "threads") or 0,
    )


def best_by(
    rows: Iterable[dict[str, str]],
    field: str,
    *,
    highest: bool,
) -> dict[str, str] | None:
    best_row: dict[str, str] | None = None
    best_value: float | None = None
    for row in rows:
        value = parse_float(row, field)
        if value is None:
            continue
        if best_value is None:
            best_row = row
            best_value = value
            continue
        if highest and value > best_value:
            best_row = row
            best_value = value
        if not highest and value < best_value:
            best_row = row
            best_value = value
    return best_row


def describe_row(row: dict[str, str] | None, value_text: str) -> str:
    if row is None:
        return "not available"
    return (
        f"{value_text} at {parse_int(row, 'threads') or 0} threads, "
        f"{label(row, 'replacer')} / {label(row, 'requested_disk_backend')}, "
        f"{parse_int(row, 'write_percent') or 0}% writes"
    )


def print_table(rows: list[dict[str, str]]) -> None:
    print(
        "| workload | replacer | backend | write % | threads | throughput ops/s | "
        "hit rate | p95 us | p99 us | disk writes | flush tasks |"
    )
    print("| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
    for row in sorted(rows, key=sort_key):
        print(
            "| "
            f"{label(row, 'workload')} | "
            f"{label(row, 'replacer')} | "
            f"{label(row, 'requested_disk_backend')} | "
            f"{parse_int(row, 'write_percent') or 0} | "
            f"{parse_int(row, 'threads') or 0} | "
            f"{format_number(parse_float(row, 'throughput_ops_per_sec'), 0)} | "
            f"{format_percent(parse_float(row, 'hit_rate'))} | "
            f"{format_latency_us(row, 'operation_latency_p95_ns')} | "
            f"{format_latency_us(row, 'operation_latency_p99_ns')} | "
            f"{format_number(parse_float(row, 'disk_writes'), 0)} | "
            f"{format_number(parse_float(row, 'flush_tasks_scheduled'), 0)} |"
        )


def print_summary(path: str, rows: list[dict[str, str]]) -> None:
    name = "stdin" if path == "-" else Path(path).stem
    print(f"## {name}")
    print()
    if not rows:
        print("No rows found.")
        print()
        return

    best_throughput = best_by(rows, "throughput_ops_per_sec", highest=True)
    best_hit_rate = best_by(rows, "hit_rate", highest=True)
    lowest_p95 = best_by(rows, "operation_latency_p95_ns", highest=False)
    throughput_text = format_number(
        parse_float(best_throughput or {}, "throughput_ops_per_sec"), 0
    )
    hit_rate_text = format_percent(parse_float(best_hit_rate or {}, "hit_rate"))
    p95_text = format_latency_us(lowest_p95 or {}, "operation_latency_p95_ns")
    print(f"- Best throughput: {describe_row(best_throughput, throughput_text + ' ops/s')}")
    print(f"- Best hit rate: {describe_row(best_hit_rate, hit_rate_text)}")
    print(f"- Lowest p95 latency: {describe_row(lowest_p95, p95_text + ' us')}")
    print()
    print_table(rows)
    print()


def main() -> int:
    args = parse_args()
    print("# TelePath Benchmark Summary")
    print()
    print("Shared-runner results are trend data; final claims still need controlled hardware.")
    print()
    for path in args.csv_files:
        print_summary(path, read_rows(path))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
