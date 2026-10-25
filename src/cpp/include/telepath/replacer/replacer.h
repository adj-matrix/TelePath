#ifndef TELEPATH_REPLACER_REPLACER_H_
#define TELEPATH_REPLACER_REPLACER_H_

#include <cstddef>
#include <memory>

#include "telepath/common/types.h"

namespace telepath {

// Answers whether a frame may be evicted right now. The buffer manager
// implements it from the frame's control word (resident, unpinned, unlatched,
// no writeback); replacement policies never track evictability themselves.
class EvictionProbe {
 public:
  virtual ~EvictionProbe() = default;
  virtual bool IsEvictable(FrameId frame_id) const = 0;
};

// Pluggable page-replacement policy. A policy only orders frames by its notion
// of coldness; the buffer manager decides whether a nominated frame can be
// reused. All methods may be called concurrently from many threads.
class Replacer {
 public:
  virtual ~Replacer() = default;

  // Records a hit on a resident frame. Called on every pin, so
  // implementations keep it lock-free or batch it.
  virtual void RecordAccess(FrameId frame_id) = 0;
  // Records that the frame now holds a newly loaded page and resets its
  // policy state (reference bits, usage count, history, queue position).
  virtual void RecordLoad(FrameId frame_id) = 0;
  // Nominates the coldest frame for which `probe.IsEvictable()` holds. The
  // frame stays tracked: the caller still has to claim it, which can fail
  // under concurrency, and calls RecordLoad() once it is reused. Returns false
  // when a full pass finds no evictable frame.
  virtual bool Victim(const EvictionProbe &probe, FrameId *frame_id) = 0;
  // Stops tracking a frame that went back to the free list.
  virtual void Remove(FrameId frame_id) = 0;
};

// Builds a Clock (second-chance) replacer: one reference bit per frame.
auto MakeClockReplacer(std::size_t capacity) -> std::unique_ptr<Replacer>;
// Builds a PostgreSQL-style clock-sweep replacer: a saturating usage count per
// frame (maximum 5), decremented by the sweeping hand.
auto MakeClockSweepReplacer(std::size_t capacity) -> std::unique_ptr<Replacer>;
// Builds a SIEVE replacer: insertion-ordered queue with one visited bit per
// frame and a hand that moves from the oldest entry toward the newest.
auto MakeSieveReplacer(std::size_t capacity) -> std::unique_ptr<Replacer>;
// Builds a classic LRU replacer. Accesses are recorded in per-thread batches.
auto MakeLruReplacer(std::size_t capacity) -> std::unique_ptr<Replacer>;
// Builds an LRU-K replacer using the given access-history length K. Accesses
// are recorded in per-thread batches.
auto MakeLruKReplacer(
  std::size_t capacity,
  std::size_t history_length
) -> std::unique_ptr<Replacer>;
// Builds a two-queue (2Q) replacer. Accesses are recorded in per-thread
// batches.
auto MakeTwoQueueReplacer(std::size_t capacity) -> std::unique_ptr<Replacer>;

}  // namespace telepath

#endif  // TELEPATH_REPLACER_REPLACER_H_
