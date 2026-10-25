#ifndef TELEPATH_REPLACER_TWO_QUEUE_REPLACER_H_
#define TELEPATH_REPLACER_TWO_QUEUE_REPLACER_H_

#include <cstddef>
#include <memory>

#include "telepath/replacer/replacer.h"

namespace telepath {

// Two-queue replacer: newly loaded pages enter a history queue, and pages
// accessed again are promoted into a cache queue ordered by recency. Victim
// selection prefers the oldest history pages before the cache LRU. Accesses
// are buffered per thread and applied in batches; Victim() applies all pending
// accesses before choosing.
class TwoQueueReplacer : public Replacer {
 public:
  // Creates a replacer for frame ids in [0, capacity).
  explicit TwoQueueReplacer(std::size_t capacity);
  ~TwoQueueReplacer() override;

  // Records an access: a history page is promoted into the cache queue, a
  // cache page moves to its most-recent end.
  void RecordAccess(FrameId frame_id) override;
  // Tracks the frame at the newest end of the history queue.
  void RecordLoad(FrameId frame_id) override;
  // Nominates the oldest evictable history page, else the least-recently-used
  // evictable cache page.
  bool Victim(const EvictionProbe &probe, FrameId *frame_id) override;
  // Stops tracking the frame.
  void Remove(FrameId frame_id) override;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace telepath

#endif  // TELEPATH_REPLACER_TWO_QUEUE_REPLACER_H_
