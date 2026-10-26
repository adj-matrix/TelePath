"""Reader for the TelePath observation area.

Mirrors src/cpp/include/telepath/observation/observation_format.h, which is the
authoritative definition of the format.
"""

from __future__ import annotations

import mmap
import os
import struct
import time
from pathlib import Path
from typing import Any

MAGIC = 0x424F5054
VERSION = 1

HEADER = struct.Struct("<IHHQQII4Q2IB7x8QQ104x")
PROCESS = struct.Struct("<IIQQ13Q")
FRAME = struct.Struct("<IBBHIIQQ")

FRAME_DIRTY = 1 << 0
FRAME_WRITEBACK_QUEUED = 1 << 1
FRAME_WRITEBACK_ACTIVE = 1 << 2
FRAME_STATES = {0: "free", 1: "loading", 2: "resident"}

SUMMARY_FIELDS = (
    "free_frames",
    "loading_frames",
    "resident_frames",
    "dirty_frames",
    "pinned_frames",
    "writeback_queued_frames",
    "writeback_active_frames",
    "dirty_page_count",
)

COUNTER_FIELDS = (
    "buffer_hits",
    "buffer_misses",
    "disk_reads",
    "disk_writes",
    "evictions",
    "dirty_flushes",
    "flush_tasks_scheduled",
    "flush_tasks_completed",
    "flush_failures",
    "cleaner_flushes_scheduled",
    "cleaner_flushes_finished",
    "cleaner_flushes_skipped",
    "eviction_failures",
)

SEQUENCE_OFFSET = 8
PROCESS_SEQUENCE_OFFSET = 8

assert HEADER.size == 256 and PROCESS.size == 128 and FRAME.size == 32


class ObservationError(Exception):
    """The observation area is missing, unreadable, or in an unknown format."""


def shm_path(name: str, shm_root: str = "/dev/shm") -> Path:
    if len(name) < 2 or not name.startswith("/") or "/" in name[1:]:
        raise ValueError('observation name must look like "/name"')
    return Path(shm_root) / name[1:]


def _read_consistent(view: mmap.mmap, sequence_offset: int, start: int, end: int, timeout: float) -> bytes:
    # Bounded by time rather than attempts: a writer descheduled mid-snapshot
    # keeps the sequence odd for as long as it waits.
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        (before,) = struct.unpack_from("<Q", view, sequence_offset)
        if before % 2 == 1:
            time.sleep(0)
            continue
        data = view[start:end]
        (after,) = struct.unpack_from("<Q", view, sequence_offset)
        if before == after:
            return data
    raise ObservationError("observation area kept changing while being read")


def _decode_frames(data: bytes, count: int) -> list[dict[str, Any]]:
    frames = []
    for frame_id, state, flags, pins, file_id, namespace_id, block_id, version in FRAME.iter_unpack(data[: count * FRAME.size]):
        frames.append(
            {
                "frame_id": frame_id,
                "state": FRAME_STATES.get(state, "unknown"),
                "dirty": bool(flags & FRAME_DIRTY),
                "writeback_queued": bool(flags & FRAME_WRITEBACK_QUEUED),
                "writeback_active": bool(flags & FRAME_WRITEBACK_ACTIVE),
                "pins": pins,
                "file_id": file_id,
                "block_id": block_id,
                "namespace_id": namespace_id,
                "version": version,
            }
        )
    return frames


def read_observation(name: str, *, include_frames: bool = True, shm_root: str = "/dev/shm", timeout: float = 2.0) -> dict[str, Any]:
    """Returns one consistent snapshot of the observation area `name`."""
    path = shm_path(name, shm_root)
    try:
        fd = os.open(path, os.O_RDONLY)
    except FileNotFoundError as exc:
        raise ObservationError(f"no observation area named {name}") from exc
    try:
        size = os.fstat(fd).st_size
        if size < HEADER.size:
            raise ObservationError("observation area is not initialized")
        view = mmap.mmap(fd, size, prot=mmap.PROT_READ)
    finally:
        os.close(fd)

    try:
        fixed = HEADER.unpack_from(view, 0)
        magic, version, header_bytes = fixed[0], fixed[1], fixed[2]
        if magic != MAGIC:
            raise ObservationError("observation area is not initialized or has an unknown magic")
        if version != VERSION or header_bytes != HEADER.size:
            raise ObservationError(f"unsupported observation format version {version}")
        frame_record_bytes, pool_size, page_size, frames_offset, processes_offset = fixed[6], fixed[7], fixed[8], fixed[9], fixed[10]
        process_record_bytes, process_capacity, cross_process = fixed[11], fixed[12], fixed[13]
        if frame_record_bytes != FRAME.size or process_record_bytes != PROCESS.size:
            raise ObservationError("observation record sizes do not match this reader")
        if frames_offset + pool_size * FRAME.size > size:
            raise ObservationError("observation area is truncated")

        frames_end = frames_offset + pool_size * FRAME.size if include_frames else HEADER.size
        snapshot = _read_consistent(view, SEQUENCE_OFFSET, 0, frames_end, timeout)
        header = HEADER.unpack_from(snapshot, 0)
        sequence, published_unix_ms, publisher_pid = header[3], header[4], header[5]
        if published_unix_ms == 0:
            raise ObservationError("observation area has no snapshot yet")
        summary = dict(zip(SUMMARY_FIELDS, header[14:22]))
        min_rec_lsn = header[22]

        processes = []
        totals = dict.fromkeys(COUNTER_FIELDS, 0)
        for index in range(process_capacity):
            start = processes_offset + index * PROCESS.size
            record = PROCESS.unpack(_read_consistent(view, start + PROCESS_SEQUENCE_OFFSET, start, start + PROCESS.size, timeout))
            if record[0] == 0:
                continue
            counters = dict(zip(COUNTER_FIELDS, record[4:]))
            for field, value in counters.items():
                totals[field] += value
            processes.append({"pid": record[0], "updated_unix_ms": record[3], "counters": counters})
    finally:
        view.close()

    result: dict[str, Any] = {
        "name": name,
        "version": version,
        "pool_size": pool_size,
        "page_size": page_size,
        "cross_process": bool(cross_process),
        "sequence": sequence,
        "published_unix_ms": published_unix_ms,
        "publisher_pid": publisher_pid,
        "summary": summary,
        "min_rec_lsn": min_rec_lsn,
        "processes": processes,
        "counters": totals,
    }
    if include_frames:
        result["frames"] = _decode_frames(snapshot[frames_offset:], pool_size)
    return result
