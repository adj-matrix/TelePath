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
        "flush_submit_batch_size,flush_foreground_burst_limit,flush_all_after_run,background_cleaner,"
        "dirty_page_high_watermark,dirty_page_low_watermark,queue_depth,max_open_files,"
        "sync_on_write,telemetry_export_enabled,telemetry_shm_enabled,telemetry_shm_name,"
        "telemetry_shm_capacity,total_ops,seconds,throughput_ops_per_sec,"
        "operation_latency_min_ns,operation_latency_avg_ns,operation_latency_p50_ns,"
        "operation_latency_p95_ns,operation_latency_p99_ns,operation_latency_max_ns,"
        "buffer_hits,buffer_misses,hit_rate,disk_reads,disk_writes,evictions,"
        "dirty_flushes,flush_tasks_scheduled,flush_tasks_completed,flush_failures,"
        "cleaner_flushes_scheduled,cleaner_flushes_finished,cleaner_flushes_skipped,"
        "eviction_failures,writes_marked_dirty,foreground_flushes_requested,"
        "final_flush_all_requested,final_flush_all_ok,final_flush_all_latency_ns\n"
        "hotspot,posix,local,local,unknown,2,16,32,12,8,80,lru_k,posix,25,6,2,2,0,true,"
        "true,12,4,0,8,false,true,true,/telepath_web_parser_test,65536,24,0.125,192.0,1000,2500.5,1500,9000,12000,13000,"
        "15,9,0.625,9,4,9,4,4,4,0,2,2,1,0,8,4,true,true,123456\n"
    )

    parsed = server.parse_csv_output(csv_payload)
    assert parsed["threads"] == 2
    assert parsed["write_percent"] == 25
    assert parsed["background_cleaner"] == "true"
    assert parsed["flush_all_after_run"] == "true"
    assert parsed["telemetry_export_enabled"] == "true"
    assert parsed["sync_on_write"] == "false"
    assert parsed["telemetry_shm_enabled"] == "true"
    assert parsed["telemetry_shm_name"] == "/telepath_web_parser_test"
    assert parsed["telemetry_shm_capacity"] == 65536
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
    assert parsed["final_flush_all_requested"] == "true"
    assert parsed["final_flush_all_ok"] == "true"
    assert parsed["final_flush_all_latency_ns"] == 123456

    json_payload = """
    {
      "metrics": {"throughput_ops_per_sec": 10.0},
      "snapshot": {"frames": []},
      "sampled_snapshots": [
        {
          "reason": "after_mark_dirty",
          "snapshot": {"frames": [{"frame_id": 0, "is_dirty": true}]}
        }
      ]
    }
    """
    parsed_json = server.parse_json_output(json_payload)
    assert len(parsed_json["sampled_snapshots"]) == 1
    assert parsed_json["sampled_snapshots"][0]["reason"] == "after_mark_dirty"

    sweep_request = server.parse_sweep_request(
        {
            "workload": "scan_pollution",
            "replacer": "lru_k",
            "disk_backend": "posix",
            "thread_candidates": [1, 2],
            "flush_all_after_run": True,
            "sync_on_write": False,
            "bulk_read_scans": True,
        }
    )
    captured_flush_flags = []
    captured_sync_flags = []
    captured_bulk_flags = []
    original_run_benchmark_once = server.run_benchmark_once

    def fake_run_benchmark_once(request, output_format="csv"):
        assert output_format == "csv"
        captured_flush_flags.append(request.flush_all_after_run)
        captured_sync_flags.append(request.sync_on_write)
        captured_bulk_flags.append(request.bulk_read_scans)
        return {
            "threads": request.threads,
            "throughput_ops_per_sec": float(request.threads),
            "hit_rate": 0.5 + (request.threads * 0.01),
        }

    try:
        server.run_benchmark_once = fake_run_benchmark_once
        sweep_result = server.run_sweep(sweep_request)
    finally:
        server.run_benchmark_once = original_run_benchmark_once

    assert captured_flush_flags == [True, True]
    assert captured_sync_flags == [False, False]
    assert captured_bulk_flags == [True, True]
    assert sweep_result["request"]["flush_all_after_run"] is True
    assert sweep_result["request"]["sync_on_write"] is False
    assert len(sweep_result["samples"]) == 2
    default_request = server.parse_sweep_request({})
    assert default_request.bulk_read_scans is False
    assert default_request.replacer == "clock_sweep"
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
