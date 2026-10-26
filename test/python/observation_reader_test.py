#!/usr/bin/env python3
"""Tests web/backend/observation.py.

Without arguments it decodes a hand-built observation area. With
`--live NAME EXPECTED_JSON` it reads a live area published by the C++
observation_test and checks that every key in EXPECTED_JSON matches. With
`--consistency NAME SECONDS` it reads a live area that is being republished
continuously and checks that every snapshot is internally consistent.
"""

from __future__ import annotations

import json
import struct
import sys
import tempfile
from pathlib import Path


def load_reader():
    repo_root = Path(__file__).resolve().parents[2]
    sys.path.insert(0, str(repo_root))
    from web.backend import observation

    return observation


def build_area(observation, sequence: int) -> bytes:
    pool_size = 3
    processes_offset = observation.HEADER.size
    frames_offset = processes_offset + 64 * observation.PROCESS.size
    header = observation.HEADER.pack(
        observation.MAGIC, observation.VERSION, observation.HEADER.size,
        sequence, 1700000000000, 4242, observation.FRAME.size,
        pool_size, 4096, frames_offset, processes_offset,
        observation.PROCESS.size, 64, 1,
        1, 0, 2, 1, 1, 0, 1, 1,
        77,
    )
    processes = bytearray(64 * observation.PROCESS.size)
    observation.PROCESS.pack_into(processes, 0, 4242, 0, 2, 1700000000000, *range(1, 14))
    observation.PROCESS.pack_into(processes, observation.PROCESS.size * 5, 4343, 0, 4, 1700000000001, *([10] * 13))
    frames = b"".join(
        [
            observation.FRAME.pack(0, 2, observation.FRAME_DIRTY | observation.FRAME_WRITEBACK_ACTIVE, 1, 7, 0, 9, 3),
            observation.FRAME.pack(1, 0, 0, 0, 0, 0, 0, 0),
            observation.FRAME.pack(2, 2, 0, 0, 7, 0, 10, 1),
        ]
    )
    return header + bytes(processes) + frames


def test_decodes_hand_built_area(observation) -> None:
    with tempfile.TemporaryDirectory() as root:
        (Path(root) / "obs").write_bytes(build_area(observation, 2))
        result = observation.read_observation("/obs", shm_root=root)
        assert result["pool_size"] == 3 and result["page_size"] == 4096
        assert result["cross_process"] is True
        assert result["publisher_pid"] == 4242
        assert result["summary"]["resident_frames"] == 2
        assert result["summary"]["dirty_page_count"] == 1
        assert result["min_rec_lsn"] == 77
        assert [p["pid"] for p in result["processes"]] == [4242, 4343]
        assert result["counters"]["buffer_hits"] == 11
        assert result["counters"]["eviction_failures"] == 23
        frame = result["frames"][0]
        assert frame["state"] == "resident" and frame["dirty"] and frame["writeback_active"]
        assert not frame["writeback_queued"]
        assert (frame["file_id"], frame["block_id"], frame["pins"], frame["version"]) == (7, 9, 1, 3)
        assert result["frames"][1]["state"] == "free"
        assert "frames" not in observation.read_observation("/obs", shm_root=root, include_frames=False)


def test_rejects_bad_areas(observation) -> None:
    with tempfile.TemporaryDirectory() as root:
        (Path(root) / "torn").write_bytes(build_area(observation, 3))
        try:
            observation.read_observation("/torn", shm_root=root, timeout=0.05)
            raise AssertionError("a snapshot being written must not be returned")
        except observation.ObservationError:
            pass

        unpublished = bytearray(build_area(observation, 0))
        struct.pack_into("<Q", unpublished, 16, 0)
        (Path(root) / "unpublished").write_bytes(bytes(unpublished))
        try:
            observation.read_observation("/unpublished", shm_root=root)
            raise AssertionError("an area without a snapshot must be rejected")
        except observation.ObservationError:
            pass

        (Path(root) / "blank").write_bytes(bytes(4096))
        try:
            observation.read_observation("/blank", shm_root=root)
            raise AssertionError("an uninitialized area must be rejected")
        except observation.ObservationError:
            pass

        try:
            observation.read_observation("/missing", shm_root=root)
            raise AssertionError("a missing area must be rejected")
        except observation.ObservationError:
            pass

        try:
            observation.read_observation("nested/name", shm_root=root)
            raise AssertionError("a malformed name must be rejected")
        except ValueError:
            pass


def test_web_route(observation) -> None:
    import os
    from http import HTTPStatus

    from web.backend import server

    name = f"/telepath_web_route_{os.getpid()}"
    path = observation.shm_path(name)
    try:
        path.write_bytes(build_area(observation, 2))
        status, payload = server.observation_response({"name": [name]})
        assert status == HTTPStatus.OK and payload["pool_size"] == 3 and len(payload["frames"]) == 3
        status, payload = server.observation_response({"name": [name], "frames": ["0"]})
        assert status == HTTPStatus.OK and "frames" not in payload

        unpublished = bytearray(build_area(observation, 0))
        struct.pack_into("<Q", unpublished, 16, 0)
        path.write_bytes(bytes(unpublished))
        status, payload = server.observation_response({"name": [name]})
        assert status == HTTPStatus.NOT_FOUND and "no snapshot" in payload["error"]
    finally:
        path.unlink(missing_ok=True)
    status, _ = server.observation_response({"name": [name]})
    assert status == HTTPStatus.NOT_FOUND
    status, _ = server.observation_response({"name": ["no_slash"]})
    assert status == HTTPStatus.BAD_REQUEST
    status, _ = server.observation_response({})
    assert status == HTTPStatus.BAD_REQUEST


def check_live(observation, name: str, expected: dict) -> None:
    result = observation.read_observation(name)

    def check(actual, wanted, path):
        if isinstance(wanted, dict):
            for key, value in wanted.items():
                check(actual[key], value, f"{path}.{key}")
        elif isinstance(wanted, list):
            assert len(actual) >= len(wanted), path
            for index, value in enumerate(wanted):
                check(actual[index], value, f"{path}[{index}]")
        else:
            assert actual == wanted, f"{path}: expected {wanted!r}, got {actual!r}"

    check(result, expected, "observation")


def check_consistency(observation, name: str, seconds: float) -> None:
    import time

    deadline = time.monotonic() + seconds
    snapshots = 0
    sequences = set()
    while time.monotonic() < deadline:
        try:
            result = observation.read_observation(name)
        except observation.ObservationError as exc:
            assert "no snapshot yet" in str(exc), exc
            continue
        frames = result["frames"]
        summary = result["summary"]
        assert len(frames) == result["pool_size"]
        counted = {
            "free_frames": sum(f["state"] == "free" for f in frames),
            "loading_frames": sum(f["state"] == "loading" for f in frames),
            "resident_frames": sum(f["state"] == "resident" for f in frames),
            "dirty_frames": sum(f["dirty"] for f in frames),
            "pinned_frames": sum(f["pins"] != 0 for f in frames),
            "writeback_queued_frames": sum(f["writeback_queued"] for f in frames),
            "writeback_active_frames": sum(f["writeback_active"] for f in frames),
        }
        for key, value in counted.items():
            assert summary[key] == value, f"torn snapshot {result['sequence']}: {key} {summary[key]} != {value}"
        assert [f["frame_id"] for f in frames] == list(range(result["pool_size"]))
        assert result["sequence"] % 2 == 0
        sequences.add(result["sequence"])
        snapshots += 1
    assert len(sequences) > 10, f"only {len(sequences)} distinct snapshots in {snapshots} reads"


def main() -> int:
    observation = load_reader()
    if len(sys.argv) == 4 and sys.argv[1] == "--live":
        check_live(observation, sys.argv[2], json.loads(sys.argv[3]))
        return 0
    if len(sys.argv) == 4 and sys.argv[1] == "--consistency":
        check_consistency(observation, sys.argv[2], float(sys.argv[3]))
        return 0
    test_decodes_hand_built_area(observation)
    test_rejects_bad_areas(observation)
    test_web_route(observation)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
