#ifndef TELEPATH_REPLACER_LRU_REPLACER_H_
#define TELEPATH_REPLACER_LRU_REPLACER_H_

#include <cstddef>
#include <memory>

#include "telepath/replacer/replacer.h"

namespace telepath {

// Least-recently-used replacer. Accesses are buffered per thread and applied
// in batches under the policy latch; Victim() applies all pending accesses
// before choosing, so its order reflects every recorded access.
class LruReplacer : public Replacer {
 public:
  // Creates a replacer for frame ids in [0, capacity).
  explicit LruReplacer(std::size_t capacity);
  ~LruReplacer() override;

  // Records an access; the frame moves to the most-recent end when its batch
  // is applied.
  void RecordAccess(FrameId frame_id) override;
  // Tracks the frame at the most-recent end.
  void RecordLoad(FrameId frame_id) override;
  // Nominates the least-recently-used evictable frame.
  bool Victim(const EvictionProbe &probe, FrameId *frame_id) override;
  // Stops tracking the frame.
  void Remove(FrameId frame_id) override;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace telepath

#endif  // TELEPATH_REPLACER_LRU_REPLACER_H_
