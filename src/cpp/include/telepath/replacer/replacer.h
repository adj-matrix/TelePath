#ifndef TELEPATH_REPLACER_REPLACER_H_
#define TELEPATH_REPLACER_REPLACER_H_

#include <cstddef>
#include <memory>

#include "telepath/common/types.h"

namespace telepath {

// Pluggable page-replacement policy used by BufferManager to track resident
// frames and choose eviction victims among the currently evictable set.
class Replacer {
 public:
  virtual ~Replacer() = default;

  // Records an access to the given frame so the policy can update its
  // recency/history metadata.
  virtual void RecordAccess(FrameId frame_id) = 0;
  // Marks whether the given frame may currently be selected as an eviction
  // victim. Pinned frames are typically non-evictable.
  virtual void SetEvictable(FrameId frame_id, bool evictable) = 0;
  // Selects one evictable frame as the next victim. Returns false when no
  // evictable frame is available.
  virtual bool Victim(FrameId *frame_id) = 0;
  // Removes a frame from the replacer state if it is currently tracked.
  virtual void Remove(FrameId frame_id) = 0;
  // Returns the number of frames that are both tracked and evictable.
  virtual auto Size() const -> std::size_t = 0;
};

// Builds a Clock/Second-Chance replacer.
auto MakeClockReplacer(std::size_t capacity) -> std::unique_ptr<Replacer>;
// Builds a classic LRU replacer.
auto MakeLruReplacer(std::size_t capacity) -> std::unique_ptr<Replacer>;
// Builds an LRU-K replacer using the given access-history length K.
auto MakeLruKReplacer(
  std::size_t capacity,
  std::size_t history_length
) -> std::unique_ptr<Replacer>;
// Builds a two-queue replacer with O(1) victim selection.
auto MakeTwoQueueReplacer(std::size_t capacity) -> std::unique_ptr<Replacer>;

}  // namespace telepath

#endif  // TELEPATH_REPLACER_REPLACER_H_
