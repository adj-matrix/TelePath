# Concurrency Invariants

## Purpose

This document records the concurrency rules of the TelePath buffer manager that are not visible from the code surface. Violating any of them compiles cleanly but reintroduces data loss, duplicate residency, frame leaks, or deadlocks.

Implementation files stay lightly commented by convention. When a change touches locking, frame state, eviction, or writeback, check it against this document and update the document in the same change.

Regression coverage:

- `test/cpp/buffer/frame_control_test.cc`: control-word transitions, latch exclusion, dirty clearing, futex wake-ups.
- `test/cpp/buffer/eviction_race_test.cc`: eviction/writeback interleavings and the replacer contract.
- `test/cpp/buffer/stress_integrity_test.cc`: sustained mixed load with torn-page and lost-update detection after a reopen.
- `test/cpp/buffer/page_table_test.cc`: conditional remap and newest-mapping-first lookups.
- `test/cpp/replacer/replacer_concurrency_test.cc`: every policy under concurrent access, nomination, and reload.
- `test/cpp/buffer/io_engine_test.cc`: completion delivery, self-reaping waiters, rejection, fatal reaper failure.
- `test/cpp/buffer/async_read_test.cc`: prefetch and `StartReadBuffers()` racing ordinary readers on the same pages.

## 1. Synchronization Inventory

| Primitive | Scope | Protects |
| --- | --- | --- |
| `BufferDescriptor::control` (32-bit atomic, futex word) | per frame | state, dirty and writeback flags, content latch, pin count |
| `BufferDescriptor::version` (64-bit atomic) | per frame | write/reuse counter (reserved for optimistic reads) |
| `BufferDescriptor::tag_file_id` / `tag_block_id` (atomics) | per frame | current tag; written only while the frame is `Loading` and owned by its loader |
| page table stripe spinlock + sequence counter | per stripe | `BufferTag -> FrameId` chains; writers lock, lookups validate the sequence without locking |
| replacer policy latch (`sync::FutexMutex`, stored with the policy state) | global | replacement-policy state |
| cleaner controller `latch_` | global | cleaner candidate queue and in-flight count |
| `FrameFreeList` head (64-bit atomic: index and version) | global | lock-free stack of free frames |
| flush scheduler `latch_`, `BufferManagerFlushTask::latch` | global / per task | flush queues and task completion |
| `IoWaiter::pending_` (32-bit atomic, futex word) | per wait | outstanding requests of one waiter; the first error is written before the count drops |
| `IoEngine::demand_` (32-bit atomic, futex word) | global | outstanding asynchronous requests plus sleeping waiters; the reaper thread sleeps on it |
| POSIX backend queue latch, io_uring per-ring submit and reap latches, descriptor cache latch | per backend / per ring | request and completion queues, ring access, open descriptors |

The control-word bit layout is specified in `docs/design/kernel-refactor.md` (section 3.3) and implemented in `src/cpp/lib/buffer/frame_control.h`. Every control-word transition is a single CAS.

## 2. Ordering Rules

- No mutex is ever held while blocking on a frame's control word, except the waits listed in section 3.
- The content latch (shared or exclusive) is the only control-word "lock" a thread holds across other operations; a `SharedBufferHandle` or `ExclusiveBufferHandle` holds it for its whole lifetime. Handle holders mark the page dirty (`ExclusiveBufferHandle::mutable_data()`) and call `FlushBuffer(handle)` while holding it; those calls only perform CAS transitions on the same word and never wait for the latch.
- Page table, replacer, and cleaner locks are leaf locks: no other lock is acquired while holding one, and none is held while waiting on a control word.
- Cross-stripe page table updates lock the lower stripe address first.
- Backend latches are leaf locks private to the backend; `Submit()` and `Reap()` return with none held. The io engine therefore runs completion handlers and the fatal callback with no lock held; the fatal callback only takes `BufferManager::failure_latch_`.

## 3. Waiting Rules

All futexes are process-shared (`src/cpp/lib/sync/futex.h`), so the words may live in memory several processes map. Threads sleep on a frame's control word through `frame_control::WaitWhile()`: set the `waiters` bit, then `futex_wait` on the observed value. Every transition that can unblock a waiter (latch release, `Loading` → `Resident`/`Free`, clearing writeback flags) clears `waiters` in its CAS and wakes all sleepers, which re-check their condition.

Current waits:

| Wait | Condition | May hold |
| --- | --- | --- |
| `AcquireShared` | exclusive held, or 255 readers | nothing |
| `AcquireExclusive` | any latch holder | nothing |
| `WaitForPendingFlush` | `writeback_queued` or `writeback_active` | the frame's shared latch, or the caller's own handle latch |
| `WaitForLoad` | frame is `Loading` with the wanted tag | nothing |

A pending writeback never needs the content latch to finish: its snapshot is copied before the writeback becomes visible (4.1), and the writer only performs `BeginWriteback`/`CompleteWriteback` CAS transitions. The writer is a flush worker, or with `ForegroundWriteback::kDirect` the thread that requested a single-page flush; either way it holds no latch while the write is in flight. That is why waiting for a writeback while holding a latch is safe. Any new wait must keep the property that its condition does not depend on a latch the waiter holds.

The content latch prefers readers, like the glibc rwlock it replaced: a steady stream of readers can delay a writer.

## 4. Writeback Invariants

### 4.1 Snapshot Before Publishing a Writeback

`writeback_queued` is published by `TryQueueWriteback` while the snapshot source is stable: either the flusher holds the shared latch, or the caller's handle holds a latch and passes its data as the stable source. The snapshot is copied before the shared latch is released.

Publishing the writeback first and copying afterwards would let the copy block behind a writer that is itself waiting for that writeback in `FlushBuffer()`, which is an ABBA deadlock.

### 4.2 Opportunistic Callers Never Block on a Writer

Callers that pass `was_busy` (the background cleaner and the first pass of `FlushAll()`) use `TryAcquireShared` and report the frame as busy instead of waiting. `FlushAll()` retries busy frames through the blocking path. The cleaner also skips pinned frames.

### 4.3 Dirty Clearing

`CompleteWriteback` clears `dirty` only when the write succeeded, `redirtied` is clear, and nobody holds the exclusive latch, in the same CAS that clears the writeback flags. `redirtied` is set when, while a writeback is queued or active:

- the exclusive latch is released, or
- the page is marked dirty (`ExclusiveBufferHandle::mutable_data()`).

Every modification happens through an `ExclusiveBufferHandle`, which holds the exclusive latch and marks the page dirty when it hands out the writable view. A page with an active writer is therefore never considered clean, and any change made after the snapshot keeps the page dirty until a later round. Completion never takes a latch, so a writer waiting in `FlushBuffer()` cannot deadlock with it.

Consequences for the flush entry points:

- `FlushBuffer(ExclusiveBufferHandle&)` runs exactly one round: it persists the current bytes, and the page stays dirty while the handle lives (the holder may still write through a view obtained earlier).
- `FlushBuffer(const SharedBufferHandle&)`, `FlushBuffer(file, block)`, `FlushAll()`, and victim writeback loop until the page is clean. With a shared holder or no holder, writers can only add rounds, so the loop terminates once writes stop.

### 4.4 recLSN

Each descriptor carries `rec_lsn` (the earliest LSN reported through `ExclusiveBufferHandle::MarkDirty(lsn)` since the last writeback snapshot) and `writeback_rec_lsn` (the `rec_lsn` that the writeback in flight covers). A page's recLSN is the smaller valid one of the two.

- `MarkDirty(lsn)` lowers `rec_lsn` with a CAS after marking the page dirty, under the exclusive latch.
- Taking a writeback snapshot (under the shared latch, or by the exclusive holder for `FlushBuffer(ExclusiveBufferHandle&)`) stores `rec_lsn` into `writeback_rec_lsn`, then resets `rec_lsn`. The latch keeps writers out, and a reader loads `rec_lsn` before `writeback_rec_lsn`, so it never sees the recLSN in neither field.
- Before the completion CAS (while the writeback flag still blocks a new snapshot): success resets `writeback_rec_lsn`; failure and a cancelled writeback lower `rec_lsn` to `writeback_rec_lsn` and leave `writeback_rec_lsn` until the next snapshot overwrites it. A change made during the writeback is already in `rec_lsn`, so the page's recLSN moves to that change's LSN instead of staying at the old one.
- Dropping a page resets both fields. Every other way a frame is freed or reused requires the page to be clean, which a successful writeback reached.

`CheckpointBuffers(lsn)` writes back pages whose recLSN is below `lsn` until each is at or above `lsn` or untracked, then syncs every written file.

### 4.5 One Writer per Page

At any time, a page has at most one resident frame, and each frame has at most one writeback queued or active. Writes for one page therefore cannot be reordered on disk.

## 5. Eviction Invariants

### 5.1 Pin and Reservation Share One CAS

`TryReserve` moves a frame to `Loading` (with one pin for the loader) only if, in the same CAS, it is `Free`, or `Resident` with zero pins, no latch holders, no writeback flags, and clean. `TryPinResident` succeeds only on `Resident`. A pin and a reservation can therefore never both succeed on the same frame state.

A `Free` frame belongs to the free list: only the thread that popped it there may reserve it (`TryReserve(..., popped_from_free_list)`), and if that fails because a flusher holds the frame's shared latch for a moment, the thread pushes it back. Victim nominations and strategy ring frames can only claim `Resident` frames. Otherwise a stale nomination could reserve a frame between the moment its releaser marked it `Free` (dropping a page, abandoning a load, losing an install race) and the moment the releaser pushes it onto the free list: the releaser would then overwrite the new owner's tag and push a frame in use, which a later pop hands out a second time (found by `chaos_test`).

After pinning, the reader compares the frame's tag with the requested tag and unpins on a mismatch. The tag is stable while the frame is `Resident` because it is only written by the loader during `Loading`, before `PublishResident`'s release CAS.

### 5.2 Write Back Before Remapping

A dirty victim is written back while it is still mapped under its old tag. The page table is remapped to the new tag only after a successful reservation, which requires a clean frame.

Remapping first would let a concurrent reader of the old page miss, load stale bytes from disk, and lose the update. If that writeback then failed, restoring the old mapping would leave two resident copies of the same page.

During the victim writeback the frame stays resident, so readers of the old page hit it normally. If the frame is pinned or re-dirtied by the time the writeback finishes, it is handed back to the replacer and the reservation moves on to another victim.

### 5.3 Conditional Remap

Between a successful reservation and the page table remap, a reader of the old tag can fail to pin the now-`Loading` frame, miss, and load the old page into a different frame. `BufferManagerPageTable::Replace` therefore erases the old mapping only if it still points to the reserved frame.

### 5.4 Newest Mapping First

The page table's chains are intrusive (frames are the nodes) and `Link` always inserts at the chain head. In the window of 5.3, two frames can carry the same key: the reserved frame (now `Loading`) and the frame that reloaded the old page. Head insertion keeps the newer mapping ahead of the older one, so lookups find the frame that can actually be pinned. Returning the stale frame first would make every later reader miss, become the miss owner, and load yet another copy.

Lookups are lock-free: a reader samples the stripe's sequence counter, walks the chain with relaxed loads (bounded by the frame count, so a chain rewritten mid-walk cannot loop forever), and retries if the counter changed or was odd. The table keeps its own copy of each frame's key, written only under the stripe lock, so a validated walk sees a consistent snapshot; the descriptor's tag is updated separately and is not used for lookups.

### 5.5 One Load per Page

There is no separate miss table. A thread that misses reserves a frame and, in the same page table operation that remaps it (`ReplaceUnlessLive`, under the stripe lock), checks whether another frame already maps the tag and is *live* for it: not `Free` and carrying that tag in its descriptor. If one is, the thread drops its reservation (the victim's old mapping is already gone, so the frame goes back to the free list) and retries. A stale entry from the window of 5.3 is not live, so it never blocks the install.

A reader that finds a frame `Loading` with its tag sleeps on that frame's control word until the state changes, then retries the lookup. If the load failed, the loader releases the frame to `Free`, the waiters wake, and one of them loads the page itself, so each waiter sees its own read result rather than another thread's error.

Every load publishes its page with the loader's pin, and no other thread can see the page before that. A `kZeroAndLock` load also takes the exclusive latch while the frame is still `Loading`, so the caller receives the zero-filled page already latched and no reader ever sees the zeros it is about to overwrite. A prefetch (`PrefetchBuffer()`), whose loader does not use the page, drops its pin in the same CAS that publishes it, so nobody can mistake that pin for a user's (5.7).

`ReleaseToFree` keeps the latch bits: `FlushAll()` may hold the shared latch of any frame, including one whose load is being abandoned, and clearing that count would make the later release underflow.

### 5.6 Writeback Failure

When a victim writeback fails, the frame keeps its tag, data, and dirty state, returns to the replacer, and the failing `ReadBuffer()`/`ReadBufferExclusive()` returns the I/O error. Eviction failures are counted in telemetry (`eviction_failures`).

### 5.7 Dropping Pages

`DropFileBuffers()` first pins a candidate frame and checks its tag under the pin, so the frame cannot be reused for another page while it decides. It then moves the frame from `Resident` to `Free` with one CAS that requires its own pin to be the only one, no latch holder, and no writeback; dirty contents are discarded. A loading page is waited for, a page under writeback is waited for and retried, and a latch held while its own pin is the only one belongs to a flusher taking a snapshot (users latch only pinned pages), so that latch is waited for as well. Any other pin means someone still uses the page: the call leaves it and reports `kInvalidArgument`, because the caller promised nobody uses the dropped range.

## 6. Replacer Contract

Evictability lives only in the frame's control word. Replacement policies order frames by coldness and never learn about pins, latches, or writebacks:

- `RecordAccess()` is called on every pin of a resident frame, outside any frame lock. Policies must keep it lock-free (Clock, clock-sweep, SIEVE) or batch it (LRU, LRU-K, 2Q).
- `RecordLoad()` is called after a frame is reused for a newly loaded page; `Remove()` when a frame returns to the free list.
- `Victim()` asks the buffer manager's `EvictionProbe` for each candidate (resident, unpinned, unlatched, no writeback) and nominates the first that passes. The frame **stays tracked**. The manager then claims it with `TryReserve`; if the claim fails because the frame changed in between, the manager simply asks for another victim. Nothing has to be handed back, so no frame can drop out of the evictable set.
- A caller that holds reservations whose reads it has not submitted yet (`StartReadBuffers()`, `PrefetchBuffer()`) must not wait for a frame: its own `Loading` frames would keep it retrying forever, and every other thread with it. These callers reserve without waiting; a prefetch stops early and a read operation reads the remaining blocks one by one in `WaitReadBuffers()`, after its reads are submitted.
- If `Victim()` nominates nothing while some frame is unpinned or `Loading`, `ReserveFrameForTag()` yields and retries; it returns `kResourceExhausted` only when every frame is pinned by a resident page. An unpinned frame may be latched, under writeback, or simply missed: concurrent callers advance a shared clock hand, so one caller's bounded scan does not visit every frame, and a frame between `ReleaseToFree` and the free list is invisible to the replacer. A `Loading` frame is pinned only until its load publishes it or, for a reservation that lost the install race (5.5), until it returns to the free list: concurrent misses on one page each reserve a frame before learning they lost, which can briefly pin a small pool completely. When the pool is exhausted because another thread just loaded the very page being read, `PinPage()` goes back to the lookup and pins that copy instead of failing.

Batched policies buffer accesses in per-thread slots (`AccessBatcher`) and apply a full slot under the policy latch. Lock order is policy latch, then slot latch; recording never holds a slot latch while taking the policy latch. `RecordLoad()` and `Remove()` first apply the calling thread's own slot, so a thread's accesses and loads keep their program order; accesses from different threads have no order to preserve. `Victim()` applies every slot before choosing.

Hits through a `BufferAccessStrategy` of type `kBulkRead` skip `RecordAccess()`, and misses first try to reclaim the strategy's ring frame through the same `TryReserve` CAS (clean frames only), falling back to `Victim()`.

## 7. I/O Completion

- Every submitted request completes exactly once. A request the backend rejects is completed by `IoEngine::Submit()` on the submitting thread; the others by whichever thread reaps them: a waiter reaping its own completions, another waiter, or the reaper thread.
- `IoCompletion::Complete()` runs with no lock held and must not block. Completing an asynchronous load publishes each frame (`Loading` → `Resident`, which wakes waiting readers), records the load in the replacer, and for a prefetch drops the loader's pin; a failed load aborts the reservation instead. These only take leaf locks.
- A frame being loaded asynchronously stays `Loading` and pinned by the load, exactly like a synchronous miss, so eviction, lookups, and waiting readers treat both alike (5.5).
- The reaper sleeps on `demand_` while it is zero. `Submit(kAsync)` raises it per request before submitting, and a waiter raises it before its first futex sleep; the transition from zero wakes the reaper. Completions of asynchronous requests carry a tag bit in the token and lower `demand_` when delivered. A waiter sleeps on its own `pending_` with the value it observed, so a completion between the check and the sleep is not lost; the sleep is bounded (10 ms) only to notice an engine stop.
- `WaitReadBuffers()` waits for every read of the operation before touching its frames. If the engine stops with reads in flight (fatal error or shutdown), the operation's state is leaked on purpose, because pending completions still reference it.

## 8. Known Limits

- While no frame can be nominated but some frame is unpinned or `Loading`, `ReserveFrameForTag()` yields and retries instead of sleeping.
- A thread that holds an `ExclusiveBufferHandle` must not call `FlushAll()` or `FlushBuffer(file, block)` for that page: the blocking path would wait for the shared latch the thread blocks itself.
- A thread must not hold two handles on the same page; `ReadBuffer` followed by `ReadBufferExclusive` on the same page in one thread deadlocks. Debug detection is not implemented yet.
- There is no in-place upgrade: release the `SharedBufferHandle`, acquire an `ExclusiveBufferHandle`, and re-check.
- Pin counts saturate at 32767 per frame; further pins yield until one is released. A page admits 255 shared latch holders; a further `ReadBuffer()` of it waits until one leaves, so one thread must not hold more than 255 shared handles on one page.
- `Close()` does not wait for calls already under way on other threads; a change such a call makes after `Close()`'s `FlushAll()` is not written back. Stop other threads first.
- A process that dies releases its observation-plane locks when the kernel closes its files, which can be some tens of milliseconds after `waitpid()` returns; another process takes over publishing at its next publish after that. Until then the dead process still counts as a user, so a last user leaving in that window does not remove the name.
- Cross-process mode (`SharedMemoryOptions`) assumes every attached process stays alive while it holds a pin, latch, or `Loading` frame. A process that dies there leaves the frame pinned, latched, or loading for the others, and only removing the segment recovers; crash recovery is designed separately (phase 6e).
- In cross-process mode a frame waits only on its control word, so a process may wait for a load or writeback another process started. Per-process state (I/O tokens, flush tasks, access batches, the cleaner) never crosses processes.
