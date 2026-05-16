#!/usr/bin/env python3

from __future__ import annotations

import sys
from pathlib import Path


def main() -> int:
    repo_root = Path(__file__).resolve().parents[2]
    sys.path.insert(0, str(repo_root))

    from web.backend import server

    csv_payload = (
        "build log on stdout that should be ignored\n"
        "workload,disk_backend,commit_sha,runner_os,runner_arch,threads,pool_size,"
        "block_count,ops_per_thread,hotset_size,hot_access_percent,replacer,"
        "requested_disk_backend,write_percent,flush_every_ops,flush_workers,"
        "flush_submit_batch_size,flush_foreground_burst_limit,background_cleaner,"
        "dirty_page_high_watermark,dirty_page_low_watermark,queue_depth,max_open_files,"
        "telemetry_export_enabled,total_ops,seconds,throughput_ops_per_sec,"
        "operation_latency_min_ns,operation_latency_avg_ns,operation_latency_p50_ns,"
        "operation_latency_p95_ns,operation_latency_p99_ns,operation_latency_max_ns,"
        "buffer_hits,buffer_misses,hit_rate,disk_reads,disk_writes,evictions,"
        "dirty_flushes,flush_tasks_scheduled,flush_tasks_completed,flush_failures,"
        "cleaner_flushes_scheduled,cleaner_flushes_finished,cleaner_flushes_skipped,"
        "eviction_failures,writes_marked_dirty,foreground_flushes_requested\n"
        "hotspot,posix,local,local,unknown,2,16,32,12,8,80,lru_k,posix,25,6,2,2,0,"
        "true,12,4,0,8,true,24,0.125,192.0,1000,2500.5,1500,9000,12000,13000,"
        "15,9,0.625,9,4,9,4,4,4,0,2,2,1,0,8,4\n"
    )

    parsed = server.parse_csv_output(csv_payload)
    assert parsed["threads"] == 2
    assert parsed["write_percent"] == 25
    assert parsed["background_cleaner"] == "true"
    assert parsed["telemetry_export_enabled"] == "true"
    assert parsed["seconds"] == 0.125
    assert parsed["throughput_ops_per_sec"] == 192.0
    assert parsed["operation_latency_min_ns"] == 1000
    assert parsed["operation_latency_avg_ns"] == 2500.5
    assert parsed["operation_latency_p50_ns"] == 1500
    assert parsed["operation_latency_p95_ns"] == 9000
    assert parsed["operation_latency_p99_ns"] == 12000
    assert parsed["operation_latency_max_ns"] == 13000
    assert parsed["flush_tasks_scheduled"] == 4
    assert parsed["cleaner_flushes_skipped"] == 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
