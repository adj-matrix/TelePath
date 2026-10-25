#ifndef TELEPATH_REPLACER_LRU_K_REPLACER_H_
#define TELEPATH_REPLACER_LRU_K_REPLACER_H_

#include <cstddef>
#include <memory>

#include "telepath/replacer/replacer.h"

namespace telepath {

// LRU-K replacer. Frames with fewer than K recorded accesses are evicted
// first, least recently accessed first; among the rest, the frame whose K-th
// most recent access is oldest is evicted. Accesses are buffered per thread
// and applied in batches; Victim() applies all pending accesses before
// choosing.
class LruKReplacer : public Replacer {
 public:
  // Creates a replacer for frame ids in [0, capacity) with history length K
  // (`history_length`, minimum 1).
  LruKReplacer(std::size_t capacity, std::size_t history_length);
  ~LruKReplacer() override;

  // Appends a timestamped access to the frame's history.
  void RecordAccess(FrameId frame_id) override;
  // Tracks the frame with a fresh history holding one access.
  void RecordLoad(FrameId frame_id) override;
  // Nominates the evictable frame with the largest backward K-distance.
  bool Victim(const EvictionProbe &probe, FrameId *frame_id) override;
  // Stops tracking the frame.
  void Remove(FrameId frame_id) override;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace telepath

#endif  // TELEPATH_REPLACER_LRU_K_REPLACER_H_
