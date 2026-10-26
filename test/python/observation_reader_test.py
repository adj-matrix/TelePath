#!/usr/bin/env python3
"""Tests web/backend/observation.py.

Without arguments it decodes a hand-built observation area. With
`--live NAME EXPECTED_JSON` it reads a live area published by the C++
observation_test and checks that every key in EXPECTED_JSON matches.
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
            observation.read_observation("/torn", shm_root=root, retries=5)
            raise AssertionError("a snapshot being written must not be returned")
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


def main() -> int:
    observation = load_reader()
    if len(sys.argv) == 4 and sys.argv[1] == "--live":
        check_live(observation, sys.argv[2], json.loads(sys.argv[3]))
        return 0
    test_decodes_hand_built_area(observation)
    test_rejects_bad_areas(observation)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
