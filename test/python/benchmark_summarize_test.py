#!/usr/bin/env python3

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> int:
    repo_root = Path(__file__).resolve().parents[2]
    summarize = repo_root / "scripts" / "bench" / "summarize.py"
    csv_payload = (
        "workload,replacer,requested_disk_backend,write_percent,threads,"
        "throughput_ops_per_sec,hit_rate,operation_latency_p95_ns,"
        "operation_latency_p99_ns,disk_writes,flush_tasks_scheduled\n"
        "hotspot,lru_k,posix,0,1,1000,0.50,100000,130000,2,3\n"
        "hotspot,lru_k,posix,0,2,1800,0.75,90000,120000,4,5\n"
    )

    with tempfile.TemporaryDirectory() as temp_dir:
        csv_path = Path(temp_dir) / "hotspot.csv"
        csv_path.write_text(csv_payload, encoding="utf-8")
        result = subprocess.run(
            [sys.executable, str(summarize), str(csv_path)],
            cwd=repo_root,
            text=True,
            capture_output=True,
            check=False,
        )

    assert result.returncode == 0, result.stderr
    assert "# TelePath Benchmark Summary" in result.stdout
    assert "Best throughput: 1,800 ops/s at 2 threads" in result.stdout
    assert "Best hit rate: 75.00% at 2 threads" in result.stdout
    assert "Lowest p95 latency: 90.00 us at 2 threads" in result.stdout
    assert "| hotspot | lru_k | posix | 0 | 2 | 1,800 | 75.00% | 90.00 | 120.00 | 4 | 5 |" in result.stdout
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
