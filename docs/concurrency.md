# Concurrency Invariants

## Purpose

This document records the concurrency rules of the TelePath buffer manager that are not visible from the code surface. Violating any of them compiles cleanly but reintroduces data loss, duplicate residency, frame leaks, or deadlocks.

Implementation files stay lightly commented by convention. When a change touches locking, frame state, eviction, or writeback, check it against this document and update the document in the same change.

Regression coverage for the rules below lives in `test/cpp/buffer/eviction_race_test.cc`.

## 1. Lock Inventory

| Lock | Scope | Protects |
| --- | --- | --- |
| `BufferDescriptor::content_latch` (`shared_mutex`) | per frame | page bytes in the frame |
| `BufferDescriptor::latch` (`mutex`) + `io_cv` | per frame | descriptor metadata: tag, state, pin count, dirty state, I/O and flush flags |
| page table stripe latches | per stripe | `BufferTag -> FrameId` mapping |
| miss coordinator stripe latches, `BufferManagerMissState::latch` | per stripe / per miss | same-page miss ownership and outcome |
| replacer `latch_` | global | replacement-policy state |
| cleaner controller `latch_` | global | cleaner candidate queue and in-flight count |
| `free_list_latch_` | global | free frame list |
| flush scheduler `latch_`, `BufferManagerFlushTask::latch` | global / per task | flush queues and task completion |
| completion dispatcher `latch_` | global | request-id completion states |

## 2. Lock Order

Acquire locks only in this order:

```
content_latch  ->  descriptor latch  ->  { page table stripe, replacer, cleaner controller, free list }
```

- Handle holders already follow this order: they hold `content_latch` (through `data()` / `mutable_data()`) and then call `MarkBufferDirty()` or `FlushBuffer()`, which take the descriptor latch.
- `PrepareFlushTask()` takes `content_latch` first, then the descriptor latch.
- Never acquire `content_latch` while holding a descriptor latch.
- Cross-stripe page table updates lock the lower stripe index first.
- The miss coordinator, flush scheduler, completion dispatcher, and backend locks are never held while acquiring a descriptor latch or `content_latch`.

## 3. Waiting Rules

Threads block on `io_cv` while holding a descriptor latch (`WaitForFrameReady()`, `WaitForPendingFlush()`). A blocking flush caller may also hold `content_latch` in shared mode while waiting.

This is safe only because a queued flush never needs `content_latch` to make progress: its page snapshot is copied before the flush becomes visible (rule 4.1), and the flush worker only takes the descriptor latch, the backend, and the dispatcher.

Any new wait on `io_cv` must keep that property: the condition being waited for must not depend on a lock the waiter holds.

## 4. Writeback Invariants

### 4.1 Snapshot Before Publishing a Flush

`flush_queued` is set only together with copying the page snapshot, inside one critical section that holds `content_latch` (shared) and the descriptor latch.

If `flush_queued` were published first and the snapshot copied afterwards, the copy could block behind a writer that is itself waiting for that flush in `FlushBuffer()`, which is an ABBA deadlock.

### 4.2 Opportunistic Callers Never Block on a Writer

Callers that pass `was_busy` (the background cleaner and the first pass of `FlushAll()`) use `try_lock` on `content_latch` and report the frame as busy instead of waiting. `FlushAll()` retries busy frames through the blocking path.

### 4.3 Dirty Generation

A completed flush clears `is_dirty` only if the frame still holds the same tag and the same `dirty_generation` that was captured with the snapshot. A page re-dirtied during writeback stays dirty and is flushed again later.

### 4.4 One Writer per Page

At any time, a page has at most one resident frame, and each frame has at most one flush queued or in flight. Writes for one page therefore cannot be reordered on disk.

## 5. Eviction Invariants

### 5.1 Write Back Before Remapping

A dirty victim is written back while it is still mapped under its old tag. The page table is remapped to the new tag only when the frame is clean, unpinned, and has no I/O or flush in flight.

Remapping first would let a concurrent reader of the old page miss, load stale bytes from disk, and lose the update. If that writeback then failed, restoring the old mapping would leave two resident copies of the same page.

During the victim writeback the frame stays resident, so readers of the old page hit it normally. If the frame is pinned or re-dirtied by the time the writeback finishes, it is handed back to the replacer and the reservation moves on to another victim.

### 5.2 Writeback Failure

When a victim writeback fails, the frame keeps its tag, data, and dirty state, returns to the replacer, and the failing `ReadBuffer()` returns the I/O error. Eviction failures are counted in telemetry (`eviction_failures`).

## 6. Replacer Contract

`Replacer::Victim()` stops tracking the frame it returns. The LRU, LRU-K, and 2Q policies drop the entry; Clock re-registers on `SetEvictable()`, but callers must not rely on that.

A victim that is not reused (pinned, busy with I/O or flush, or still dirty) must be handed back through `ReturnUnreservedVictim()`, which calls `RecordAccess()` and, for unpinned frames, `SetEvictable(true)`. Calling `SetEvictable(true)` alone silently removes the frame from the evictable set for the policies that drop entries.

## 7. Known Limits

- If the only evictable frame is busy with a flush, `ReserveFrameForTag()` spins on `Victim()` until the flush completes.
- A thread that holds `content_latch` on a page must not call `FlushAll()`, and must not call `FlushBuffer()` through a different handle to the same page: the blocking path would wait on a latch the thread already holds.
- Upgrading a handle from read to write releases the shared latch before taking the exclusive one, so another writer can run in between.
