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
from dataclasses import dataclass
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

DEFAULTS = {
    "workload": "hotspot",
    "pool_size": 256,
    "block_count": 1024,
    "threads": 4,
    "ops_per_thread": 10000,
    "hotset_size": 64,
    "hot_access_percent": 80,
}

DEFAULT_SWEEP = {
    "thread_candidates": [1, 2, 4, 8],
    "pool_size": 256,
    "block_count": 1024,
    "ops_per_thread": 10000,
    "hotset_size": 64,
    "hot_access_percent": 80,
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
    pool_size: int
    block_count: int
    threads: int
    ops_per_thread: int
    hotset_size: int
    hot_access_percent: int


@dataclass(frozen=True)
class SweepRequest:
    workload: str
    pool_size: int
    block_count: int
    thread_candidates: list[int]
    ops_per_thread: int
    hotset_size: int
    hot_access_percent: int


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


def clamp_hot_fields(block_count: int, hotset_size: int, hot_access_percent: int) -> tuple[int, int]:
    return min(hotset_size, block_count), min(hot_access_percent, 100)


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
    workload = str(payload.get("workload", DEFAULTS["workload"]))
    if workload not in WORKLOADS:
        raise ValueError(f"unsupported workload: {workload}")

    pool_size = positive_int_from_payload(payload, "pool_size", DEFAULTS["pool_size"])
    block_count = positive_int_from_payload(
        payload, "block_count", DEFAULTS["block_count"]
    )
    threads = positive_int_from_payload(payload, "threads", DEFAULTS["threads"])
    ops_per_thread = positive_int_from_payload(
        payload, "ops_per_thread", DEFAULTS["ops_per_thread"]
    )
    hotset_size = positive_int_from_payload(
        payload, "hotset_size", DEFAULTS["hotset_size"]
    )
    hot_access_percent = positive_int_from_payload(
        payload, "hot_access_percent", DEFAULTS["hot_access_percent"]
    )
    hotset_size, hot_access_percent = clamp_hot_fields(
        block_count, hotset_size, hot_access_percent
    )

    return BenchmarkRequest(
        workload=workload,
        pool_size=pool_size,
        block_count=block_count,
        threads=threads,
        ops_per_thread=ops_per_thread,
        hotset_size=hotset_size,
        hot_access_percent=hot_access_percent,
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
    workload = str(payload.get("workload", DEFAULTS["workload"]))
    if workload not in WORKLOADS:
        raise ValueError(f"unsupported workload: {workload}")

    pool_size = positive_int_from_payload(
        payload, "pool_size", DEFAULT_SWEEP["pool_size"]
    )
    block_count = positive_int_from_payload(
        payload, "block_count", DEFAULT_SWEEP["block_count"]
    )
    ops_per_thread = positive_int_from_payload(
        payload, "ops_per_thread", DEFAULT_SWEEP["ops_per_thread"]
    )
    hotset_size = positive_int_from_payload(
        payload, "hotset_size", DEFAULT_SWEEP["hotset_size"]
    )
    hot_access_percent = positive_int_from_payload(
        payload, "hot_access_percent", DEFAULT_SWEEP["hot_access_percent"]
    )
    hotset_size, hot_access_percent = clamp_hot_fields(
        block_count, hotset_size, hot_access_percent
    )

    return SweepRequest(
        workload=workload,
        pool_size=pool_size,
        block_count=block_count,
        thread_candidates=parse_thread_candidates(payload),
        ops_per_thread=ops_per_thread,
        hotset_size=hotset_size,
        hot_access_percent=hot_access_percent,
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
    }
    floats = {"seconds", "throughput_ops_per_sec", "hit_rate"}

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


def run_benchmark_once(request: BenchmarkRequest) -> dict[str, Any]:
    ensure_benchmark_binary()
    command = [
        str(BENCHMARK_BIN),
        "--output-format",
        "csv",
        "--workload",
        request.workload,
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
    return parse_csv_output(result.stdout)


def run_benchmark(request: BenchmarkRequest) -> dict[str, Any]:
    metrics = run_benchmark_once(request)
    response = {
        "kind": "single_run",
        "timestamp_ms": now_epoch_ms(),
        "request": {
            "workload": request.workload,
            "pool_size": request.pool_size,
            "block_count": request.block_count,
            "threads": request.threads,
            "ops_per_thread": request.ops_per_thread,
            "hotset_size": request.hotset_size,
            "hot_access_percent": request.hot_access_percent,
        },
        "metrics": metrics,
    }
    with _history_lock:
        _recent_runs.appendleft(response)
    return response


def run_sweep(request: SweepRequest) -> dict[str, Any]:
    samples: list[dict[str, Any]] = []
    for thread_count in request.thread_candidates:
        sample_request = BenchmarkRequest(
            workload=request.workload,
            pool_size=request.pool_size,
            block_count=request.block_count,
            threads=thread_count,
            ops_per_thread=request.ops_per_thread,
            hotset_size=request.hotset_size,
            hot_access_percent=request.hot_access_percent,
        )
        samples.append(run_benchmark_once(sample_request))

    best_throughput = max(
        samples, key=lambda sample: sample["throughput_ops_per_sec"]
    )
    best_hit_rate = max(samples, key=lambda sample: sample["hit_rate"])

    response = {
        "kind": "sweep",
        "timestamp_ms": now_epoch_ms(),
        "request": {
            "workload": request.workload,
            "pool_size": request.pool_size,
            "block_count": request.block_count,
            "thread_candidates": request.thread_candidates,
            "ops_per_thread": request.ops_per_thread,
            "hotset_size": request.hotset_size,
            "hot_access_percent": request.hot_access_percent,
        },
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

        content_type, _ = mimetypes.guess_type(str(file_path))
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", content_type or "text/html; charset=utf-8")
        self.end_headers()
        self.wfile.write(file_path.read_bytes())

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
