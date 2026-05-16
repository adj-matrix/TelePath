#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import json
import mimetypes
import subprocess
import threading
import time
from collections import deque
from dataclasses import asdict, dataclass
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import urlparse


REPO_ROOT = Path(__file__).resolve().parents[2]
FRONTEND_ROOT = REPO_ROOT / "web" / "frontend"
LOCALIZATION_ROOT = REPO_ROOT / "localization" / "web-console"
BENCHMARK_BIN = REPO_ROOT / "build" / "debug" / "test" / "telepath_benchmark"
BUILD_SCRIPT = REPO_ROOT / "scripts" / "build" / "debug.sh"

WORKLOADS = [
    "hotspot",
    "uniform",
    "sequential_shared",
    "sequential_disjoint",
]

REPLACERS = [
    "lru_k",
    "clock",
    "lru",
    "two_queue",
]

DISK_BACKENDS = [
    "auto",
    "posix",
    "io_uring",
]

DEFAULTS = {
    "workload": "hotspot",
    "replacer": "lru_k",
    "disk_backend": "auto",
    "pool_size": 256,
    "block_count": 1024,
    "threads": 4,
    "ops_per_thread": 10000,
    "hotset_size": 64,
    "hot_access_percent": 80,
    "write_percent": 0,
    "flush_every_ops": 0,
    "background_cleaner": False,
    "dirty_page_high_watermark": 0,
    "dirty_page_low_watermark": 0,
    "flush_workers": 0,
    "flush_submit_batch_size": 0,
    "flush_foreground_burst_limit": 0,
    "queue_depth": 0,
    "max_open_files": 0,
}

DEFAULT_SWEEP = {
    "workload": "hotspot",
    "replacer": "lru_k",
    "disk_backend": "auto",
    "thread_candidates": [1, 2, 4, 8],
    "pool_size": 256,
    "block_count": 1024,
    "ops_per_thread": 10000,
    "hotset_size": 64,
    "hot_access_percent": 80,
    "write_percent": 0,
    "flush_every_ops": 0,
    "background_cleaner": False,
    "dirty_page_high_watermark": 0,
    "dirty_page_low_watermark": 0,
    "flush_workers": 0,
    "flush_submit_batch_size": 0,
    "flush_foreground_burst_limit": 0,
    "queue_depth": 0,
    "max_open_files": 0,
}

SUPPORTED_LANGUAGES = [
    {"code": "en", "name": "English"},
    {"code": "zh-CN", "name": "简体中文"},
]

_build_lock = threading.Lock()
_history_lock = threading.Lock()
_recent_runs: deque[dict[str, Any]] = deque(maxlen=12)
_recent_sweeps: deque[dict[str, Any]] = deque(maxlen=8)
_localization_cache: dict[str, dict[str, Any]] = {}


@dataclass(frozen=True)
class BenchmarkRequest:
    workload: str
    replacer: str
    disk_backend: str
    pool_size: int
    block_count: int
    threads: int
    ops_per_thread: int
    hotset_size: int
    hot_access_percent: int
    write_percent: int
    flush_every_ops: int
    background_cleaner: bool
    dirty_page_high_watermark: int
    dirty_page_low_watermark: int
    flush_workers: int
    flush_submit_batch_size: int
    flush_foreground_burst_limit: int
    queue_depth: int
    max_open_files: int


@dataclass(frozen=True)
class SweepRequest:
    workload: str
    replacer: str
    disk_backend: str
    pool_size: int
    block_count: int
    thread_candidates: list[int]
    ops_per_thread: int
    hotset_size: int
    hot_access_percent: int
    write_percent: int
    flush_every_ops: int
    background_cleaner: bool
    dirty_page_high_watermark: int
    dirty_page_low_watermark: int
    flush_workers: int
    flush_submit_batch_size: int
    flush_foreground_burst_limit: int
    queue_depth: int
    max_open_files: int


def now_epoch_ms() -> int:
    return int(time.time() * 1000)


def json_bytes(payload: dict[str, Any]) -> bytes:
    return json.dumps(payload, ensure_ascii=True, indent=2).encode("utf-8")


def load_localization(lang: str) -> dict[str, Any]:
    for candidate in SUPPORTED_LANGUAGES:
        if candidate["code"] == lang:
            break
    else:
        raise ValueError(f"unsupported language: {lang}")

    cached = _localization_cache.get(lang)
    if cached is not None:
        return cached

    file_path = LOCALIZATION_ROOT / f"{lang}.json"
    if not file_path.exists():
        raise RuntimeError(f"missing localization file: {file_path}")

    payload = json.loads(file_path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise RuntimeError(f"invalid localization payload: {file_path}")
    _localization_cache[lang] = payload
    return payload


def positive_int_from_payload(payload: dict[str, Any], name: str, fallback: int) -> int:
    value = payload.get(name, fallback)
    if isinstance(value, bool):
        raise ValueError(f"{name} must be a positive integer")
    try:
        parsed = int(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{name} must be a positive integer") from exc
    if parsed <= 0:
        raise ValueError(f"{name} must be a positive integer")
    return parsed


def non_negative_int_from_payload(
    payload: dict[str, Any], name: str, fallback: int
) -> int:
    value = payload.get(name, fallback)
    if isinstance(value, bool):
        raise ValueError(f"{name} must be a non-negative integer")
    try:
        parsed = int(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{name} must be a non-negative integer") from exc
    if parsed < 0:
        raise ValueError(f"{name} must be a non-negative integer")
    return parsed


def bool_from_payload(payload: dict[str, Any], name: str, fallback: bool) -> bool:
    value = payload.get(name, fallback)
    if isinstance(value, bool):
        return value
    if isinstance(value, int):
        if value == 0:
            return False
        if value == 1:
            return True
    if isinstance(value, str):
        normalized = value.strip().lower()
        if normalized in {"1", "true", "yes", "on"}:
            return True
        if normalized in {"0", "false", "no", "off"}:
            return False
    raise ValueError(f"{name} must be a boolean")


def enum_from_payload(
    payload: dict[str, Any],
    name: str,
    fallback: str,
    allowed: list[str],
) -> str:
    value = str(payload.get(name, fallback))
    if value not in allowed:
        raise ValueError(f"unsupported {name}: {value}")
    return value


def clamp_hot_fields(block_count: int, hotset_size: int, hot_access_percent: int) -> tuple[int, int]:
    return min(hotset_size, block_count), min(hot_access_percent, 100)


def parse_common_options(payload: dict[str, Any], defaults: dict[str, Any]) -> dict[str, Any]:
    workload = enum_from_payload(payload, "workload", defaults["workload"], WORKLOADS)
    replacer = enum_from_payload(payload, "replacer", defaults["replacer"], REPLACERS)
    disk_backend = enum_from_payload(
        payload, "disk_backend", defaults["disk_backend"], DISK_BACKENDS
    )
    pool_size = positive_int_from_payload(payload, "pool_size", defaults["pool_size"])
    block_count = positive_int_from_payload(
        payload, "block_count", defaults["block_count"]
    )
    ops_per_thread = positive_int_from_payload(
        payload, "ops_per_thread", defaults["ops_per_thread"]
    )
    hotset_size = positive_int_from_payload(
        payload, "hotset_size", defaults["hotset_size"]
    )
    hot_access_percent = non_negative_int_from_payload(
        payload, "hot_access_percent", defaults["hot_access_percent"]
    )
    hotset_size, hot_access_percent = clamp_hot_fields(
        block_count, hotset_size, hot_access_percent
    )
    write_percent = min(
        non_negative_int_from_payload(
            payload, "write_percent", defaults["write_percent"]
        ),
        100,
    )
    return {
        "workload": workload,
        "replacer": replacer,
        "disk_backend": disk_backend,
        "pool_size": pool_size,
        "block_count": block_count,
        "ops_per_thread": ops_per_thread,
        "hotset_size": hotset_size,
        "hot_access_percent": hot_access_percent,
        "write_percent": write_percent,
        "flush_every_ops": non_negative_int_from_payload(
            payload, "flush_every_ops", defaults["flush_every_ops"]
        ),
        "background_cleaner": bool_from_payload(
            payload, "background_cleaner", defaults["background_cleaner"]
        ),
        "dirty_page_high_watermark": non_negative_int_from_payload(
            payload,
            "dirty_page_high_watermark",
            defaults["dirty_page_high_watermark"],
        ),
        "dirty_page_low_watermark": non_negative_int_from_payload(
            payload,
            "dirty_page_low_watermark",
            defaults["dirty_page_low_watermark"],
        ),
        "flush_workers": non_negative_int_from_payload(
            payload, "flush_workers", defaults["flush_workers"]
        ),
        "flush_submit_batch_size": non_negative_int_from_payload(
            payload,
            "flush_submit_batch_size",
            defaults["flush_submit_batch_size"],
        ),
        "flush_foreground_burst_limit": non_negative_int_from_payload(
            payload,
            "flush_foreground_burst_limit",
            defaults["flush_foreground_burst_limit"],
        ),
        "queue_depth": non_negative_int_from_payload(
            payload, "queue_depth", defaults["queue_depth"]
        ),
        "max_open_files": non_negative_int_from_payload(
            payload, "max_open_files", defaults["max_open_files"]
        ),
    }


def ensure_benchmark_binary() -> None:
    if BENCHMARK_BIN.exists():
        return

    with _build_lock:
        if BENCHMARK_BIN.exists():
            return
        result = subprocess.run(
            [str(BUILD_SCRIPT)],
            cwd=REPO_ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        if result.returncode != 0 or not BENCHMARK_BIN.exists():
            raise RuntimeError(
                "failed to build TelePath debug benchmark binary\n"
                f"stdout:\n{result.stdout}\n"
                f"stderr:\n{result.stderr}"
            )


def parse_benchmark_request(payload: dict[str, Any]) -> BenchmarkRequest:
    options = parse_common_options(payload, DEFAULTS)
    threads = positive_int_from_payload(payload, "threads", DEFAULTS["threads"])

    return BenchmarkRequest(
        workload=options["workload"],
        replacer=options["replacer"],
        disk_backend=options["disk_backend"],
        pool_size=options["pool_size"],
        block_count=options["block_count"],
        threads=threads,
        ops_per_thread=options["ops_per_thread"],
        hotset_size=options["hotset_size"],
        hot_access_percent=options["hot_access_percent"],
        write_percent=options["write_percent"],
        flush_every_ops=options["flush_every_ops"],
        background_cleaner=options["background_cleaner"],
        dirty_page_high_watermark=options["dirty_page_high_watermark"],
        dirty_page_low_watermark=options["dirty_page_low_watermark"],
        flush_workers=options["flush_workers"],
        flush_submit_batch_size=options["flush_submit_batch_size"],
        flush_foreground_burst_limit=options["flush_foreground_burst_limit"],
        queue_depth=options["queue_depth"],
        max_open_files=options["max_open_files"],
    )


def parse_thread_candidates(payload: dict[str, Any]) -> list[int]:
    raw = payload.get("thread_candidates", DEFAULT_SWEEP["thread_candidates"])
    if isinstance(raw, str):
        tokens = [token.strip() for token in raw.split(",") if token.strip()]
    elif isinstance(raw, list):
        tokens = raw
    else:
        raise ValueError("thread_candidates must be a comma-separated string or list")

    candidates: list[int] = []
    for token in tokens:
        try:
            parsed = int(token)
        except (TypeError, ValueError) as exc:
            raise ValueError("thread_candidates must contain positive integers") from exc
        if parsed <= 0:
            raise ValueError("thread_candidates must contain positive integers")
        if parsed not in candidates:
            candidates.append(parsed)
    if not candidates:
        raise ValueError("thread_candidates must not be empty")
    return candidates


def parse_sweep_request(payload: dict[str, Any]) -> SweepRequest:
    options = parse_common_options(payload, DEFAULT_SWEEP)

    return SweepRequest(
        workload=options["workload"],
        replacer=options["replacer"],
        disk_backend=options["disk_backend"],
        pool_size=options["pool_size"],
        block_count=options["block_count"],
        thread_candidates=parse_thread_candidates(payload),
        ops_per_thread=options["ops_per_thread"],
        hotset_size=options["hotset_size"],
        hot_access_percent=options["hot_access_percent"],
        write_percent=options["write_percent"],
        flush_every_ops=options["flush_every_ops"],
        background_cleaner=options["background_cleaner"],
        dirty_page_high_watermark=options["dirty_page_high_watermark"],
        dirty_page_low_watermark=options["dirty_page_low_watermark"],
        flush_workers=options["flush_workers"],
        flush_submit_batch_size=options["flush_submit_batch_size"],
        flush_foreground_burst_limit=options["flush_foreground_burst_limit"],
        queue_depth=options["queue_depth"],
        max_open_files=options["max_open_files"],
    )


def parse_csv_output(stdout: str) -> dict[str, Any]:
    lines = [line.strip() for line in stdout.splitlines() if line.strip()]
    if len(lines) < 2:
        raise RuntimeError(f"unexpected benchmark output:\n{stdout}")

    header_index = -1
    for index, line in enumerate(lines):
        if line.startswith("workload,disk_backend,commit_sha"):
            header_index = index
    if header_index < 0 or header_index + 1 >= len(lines):
        raise RuntimeError(f"failed to find CSV payload in benchmark output:\n{stdout}")

    reader = csv.DictReader(lines[header_index : header_index + 2])
    row = next(reader, None)
    if row is None:
        raise RuntimeError(f"failed to parse benchmark CSV row:\n{stdout}")

    ints = {
        "threads",
        "pool_size",
        "block_count",
        "ops_per_thread",
        "hotset_size",
        "hot_access_percent",
        "total_ops",
        "buffer_hits",
        "buffer_misses",
        "disk_reads",
        "disk_writes",
        "evictions",
        "dirty_flushes",
        "write_percent",
        "flush_every_ops",
        "flush_workers",
        "flush_submit_batch_size",
        "flush_foreground_burst_limit",
        "dirty_page_high_watermark",
        "dirty_page_low_watermark",
        "queue_depth",
        "max_open_files",
        "telemetry_shm_capacity",
        "flush_tasks_scheduled",
        "flush_tasks_completed",
        "flush_failures",
        "cleaner_flushes_scheduled",
        "cleaner_flushes_finished",
        "cleaner_flushes_skipped",
        "eviction_failures",
        "writes_marked_dirty",
        "foreground_flushes_requested",
        "operation_latency_min_ns",
        "operation_latency_p50_ns",
        "operation_latency_p95_ns",
        "operation_latency_p99_ns",
        "operation_latency_max_ns",
    }
    floats = {
        "seconds",
        "throughput_ops_per_sec",
        "hit_rate",
        "operation_latency_avg_ns",
    }

    parsed: dict[str, Any] = {}
    for key, value in row.items():
        if value is None:
            continue
        if key in ints:
            parsed[key] = int(float(value))
        elif key in floats:
            parsed[key] = float(value)
        else:
            parsed[key] = value
    return parsed


def parse_json_output(stdout: str) -> dict[str, Any]:
    try:
        payload = json.loads(stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"failed to parse benchmark JSON output:\n{stdout}") from exc
    if not isinstance(payload, dict):
        raise RuntimeError(f"unexpected benchmark JSON payload:\n{stdout}")
    metrics = payload.get("metrics")
    snapshot = payload.get("snapshot")
    if not isinstance(metrics, dict):
        raise RuntimeError(f"benchmark JSON payload is missing metrics:\n{stdout}")
    if snapshot is not None and not isinstance(snapshot, dict):
        raise RuntimeError(f"benchmark JSON payload has invalid snapshot:\n{stdout}")
    return payload


def run_benchmark_once(request: BenchmarkRequest, output_format: str = "csv") -> dict[str, Any]:
    ensure_benchmark_binary()
    command = [
        str(BENCHMARK_BIN),
        "--output-format",
        output_format,
        "--workload",
        request.workload,
        "--replacer",
        request.replacer,
        "--disk-backend",
        request.disk_backend,
        "--pool-size",
        str(request.pool_size),
        "--block-count",
        str(request.block_count),
        "--threads",
        str(request.threads),
        "--ops-per-thread",
        str(request.ops_per_thread),
        "--hotset-size",
        str(request.hotset_size),
        "--hot-access-percent",
        str(request.hot_access_percent),
        "--write-percent",
        str(request.write_percent),
        "--flush-every-ops",
        str(request.flush_every_ops),
        "--background-cleaner",
        "true" if request.background_cleaner else "false",
        "--dirty-page-high-watermark",
        str(request.dirty_page_high_watermark),
        "--dirty-page-low-watermark",
        str(request.dirty_page_low_watermark),
        "--flush-workers",
        str(request.flush_workers),
        "--flush-submit-batch-size",
        str(request.flush_submit_batch_size),
        "--flush-foreground-burst-limit",
        str(request.flush_foreground_burst_limit),
        "--queue-depth",
        str(request.queue_depth),
        "--max-open-files",
        str(request.max_open_files),
    ]
    result = subprocess.run(
        command,
        cwd=REPO_ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(
            "benchmark execution failed\n"
            f"stdout:\n{result.stdout}\n"
            f"stderr:\n{result.stderr}"
        )
    if output_format == "json":
        return parse_json_output(result.stdout)
    return parse_csv_output(result.stdout)


def run_benchmark(request: BenchmarkRequest) -> dict[str, Any]:
    benchmark_payload = run_benchmark_once(request, "json")
    metrics = benchmark_payload["metrics"]
    response = {
        "kind": "single_run",
        "timestamp_ms": now_epoch_ms(),
        "request": asdict(request),
        "metrics": metrics,
        "snapshot": benchmark_payload.get("snapshot"),
        "access_profile": benchmark_payload.get("access_profile"),
    }
    with _history_lock:
        _recent_runs.appendleft(response)
    return response


def run_sweep(request: SweepRequest) -> dict[str, Any]:
    samples: list[dict[str, Any]] = []
    for thread_count in request.thread_candidates:
        sample_request = BenchmarkRequest(
            workload=request.workload,
            replacer=request.replacer,
            disk_backend=request.disk_backend,
            pool_size=request.pool_size,
            block_count=request.block_count,
            threads=thread_count,
            ops_per_thread=request.ops_per_thread,
            hotset_size=request.hotset_size,
            hot_access_percent=request.hot_access_percent,
            write_percent=request.write_percent,
            flush_every_ops=request.flush_every_ops,
            background_cleaner=request.background_cleaner,
            dirty_page_high_watermark=request.dirty_page_high_watermark,
            dirty_page_low_watermark=request.dirty_page_low_watermark,
            flush_workers=request.flush_workers,
            flush_submit_batch_size=request.flush_submit_batch_size,
            flush_foreground_burst_limit=request.flush_foreground_burst_limit,
            queue_depth=request.queue_depth,
            max_open_files=request.max_open_files,
        )
        samples.append(run_benchmark_once(sample_request, "csv"))

    best_throughput = max(
        samples, key=lambda sample: sample["throughput_ops_per_sec"]
    )
    best_hit_rate = max(samples, key=lambda sample: sample["hit_rate"])

    response = {
        "kind": "sweep",
        "timestamp_ms": now_epoch_ms(),
        "request": asdict(request),
        "samples": samples,
        "summary": {
            "sample_count": len(samples),
            "best_throughput_threads": best_throughput["threads"],
            "best_throughput_ops_per_sec": best_throughput["throughput_ops_per_sec"],
            "best_hit_rate_threads": best_hit_rate["threads"],
            "best_hit_rate": best_hit_rate["hit_rate"],
        },
    }
    with _history_lock:
        _recent_sweeps.appendleft(response)
    return response


def snapshot_session_state() -> dict[str, Any]:
    with _history_lock:
        recent_runs = list(_recent_runs)
        recent_sweeps = list(_recent_sweeps)

    latest_run = recent_runs[0] if recent_runs else None
    latest_sweep = recent_sweeps[0] if recent_sweeps else None
    latest_throughput = (
        latest_run["metrics"]["throughput_ops_per_sec"] if latest_run else None
    )
    latest_hit_rate = latest_run["metrics"]["hit_rate"] if latest_run else None
    return {
        "recent_runs": recent_runs,
        "recent_sweeps": recent_sweeps,
        "overview": {
            "run_count": len(recent_runs),
            "sweep_count": len(recent_sweeps),
            "latest_throughput_ops_per_sec": latest_throughput,
            "latest_hit_rate": latest_hit_rate,
        },
    }


class TelePathDemoHandler(BaseHTTPRequestHandler):
    server_version = "TelePathDemo/0.2"

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == "/api/health":
            self.respond_json(
                HTTPStatus.OK,
                {
                    "status": "ok",
                    "benchmark_ready": BENCHMARK_BIN.exists(),
                },
            )
            return

        if parsed.path == "/api/config":
            self.respond_json(
                HTTPStatus.OK,
                {
                    "workloads": WORKLOADS,
                    "replacers": REPLACERS,
                    "disk_backends": DISK_BACKENDS,
                    "defaults": DEFAULTS,
                    "sweep_defaults": DEFAULT_SWEEP,
                    "default_language": "en",
                    "supported_languages": SUPPORTED_LANGUAGES,
                    "backend": {
                        "type": "single-process demo server",
                        "benchmark_binary": str(BENCHMARK_BIN.relative_to(REPO_ROOT)),
                    },
                },
            )
            return

        if parsed.path.startswith("/api/i18n/"):
            lang = parsed.path.removeprefix("/api/i18n/")
            try:
                payload = load_localization(lang)
            except ValueError as exc:
                self.respond_json(HTTPStatus.NOT_FOUND, {"error": str(exc)})
                return
            except RuntimeError as exc:
                self.respond_json(HTTPStatus.INTERNAL_SERVER_ERROR, {"error": str(exc)})
                return
            self.respond_json(HTTPStatus.OK, payload)
            return

        if parsed.path == "/api/session":
            self.respond_json(HTTPStatus.OK, snapshot_session_state())
            return

        self.serve_static(parsed.path)

    def do_POST(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path not in {"/api/benchmark/run", "/api/benchmark/sweep"}:
            self.respond_json(
                HTTPStatus.NOT_FOUND, {"error": f"unknown endpoint: {parsed.path}"}
            )
            return

        try:
            content_length = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(content_length) if content_length > 0 else b"{}"
            payload = json.loads(body.decode("utf-8"))
            if not isinstance(payload, dict):
                raise ValueError("request body must be a JSON object")

            if parsed.path == "/api/benchmark/run":
                request = parse_benchmark_request(payload)
                result = run_benchmark(request)
            else:
                request = parse_sweep_request(payload)
                result = run_sweep(request)
        except ValueError as exc:
            self.respond_json(HTTPStatus.BAD_REQUEST, {"error": str(exc)})
            return
        except RuntimeError as exc:
            self.respond_json(HTTPStatus.INTERNAL_SERVER_ERROR, {"error": str(exc)})
            return

        self.respond_json(HTTPStatus.OK, result)

    def serve_static(self, path: str) -> None:
        candidate = path.lstrip("/") or "index.html"
        file_path = (FRONTEND_ROOT / candidate).resolve()
        try:
            file_path.relative_to(FRONTEND_ROOT.resolve())
        except ValueError:
            self.respond_json(HTTPStatus.FORBIDDEN, {"error": "forbidden"})
            return

        if not file_path.exists() or file_path.is_dir():
            file_path = FRONTEND_ROOT / "index.html"

        try:
            body = file_path.read_bytes()
        except OSError as exc:
            self.respond_json(
                HTTPStatus.INTERNAL_SERVER_ERROR,
                {"error": f"failed to read static asset: {exc}"},
            )
            return

        content_type, _ = mimetypes.guess_type(str(file_path))
        if content_type is None:
            if file_path.suffix == ".js":
                content_type = "application/javascript"
            elif file_path.suffix == ".css":
                content_type = "text/css"
            else:
                content_type = "text/html"
        if content_type.startswith("text/") and "charset=" not in content_type:
            content_type = f"{content_type}; charset=utf-8"

        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            return

    def respond_json(self, status: HTTPStatus, payload: dict[str, Any]) -> None:
        body = json_bytes(payload)
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format: str, *args: Any) -> None:
        return


def main() -> None:
    parser = argparse.ArgumentParser(description="Serve the TelePath web console.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", default=8080, type=int)
    args = parser.parse_args()

    httpd = ThreadingHTTPServer((args.host, args.port), TelePathDemoHandler)
    print(f"telepath_web_demo listening on http://{args.host}:{args.port}")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()


if __name__ == "__main__":
    main()
