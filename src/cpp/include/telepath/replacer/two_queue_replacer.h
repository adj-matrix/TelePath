#ifndef TELEPATH_REPLACER_TWO_QUEUE_REPLACER_H_
#define TELEPATH_REPLACER_TWO_QUEUE_REPLACER_H_

#include <list>
#include <mutex>
#include <unordered_map>

#include "telepath/replacer/replacer.h"

namespace telepath {

// Two-queue approximation of history-aware replacement:
// first-touch pages enter a history queue, and second-touch pages are promoted
// into a cache queue. Victim selection prefers history pages before cache
// pages, while both queues support O(1) updates and eviction.
class TwoQueueReplacer : public Replacer {
 public:
  // Creates a replacer that can track up to `capacity` frame ids.
  explicit TwoQueueReplacer(std::size_t capacity);

  // Records an access. First-touch pages join the history segment, and pages
  // touched again are promoted into the cache segment.
  void RecordAccess(FrameId frame_id) override;
  // Toggles whether the frame may currently be chosen as a victim.
  void SetEvictable(FrameId frame_id, bool evictable) override;
  // Evicts the oldest history victim first, then falls back to the cache LRU.
  bool Victim(FrameId *frame_id) override;
  // Stops tracking the given frame if it is present.
  void Remove(FrameId frame_id) override;
  // Returns the current number of evictable tracked frames.
  auto Size() const -> std::size_t override;

 private:
  enum class Segment {
    kHistory = 0,
    kCache,
  };

  struct Entry {
    bool evictable{false};
    Segment segment{Segment::kHistory};
    std::list<FrameId>::iterator position;
  };

  bool IsValidFrame(FrameId frame_id) const;
  bool IsInCache(const Entry &entry) const;
  void InsertIntoSegment(FrameId frame_id, Entry *entry);
  void RemoveFromSegment(Entry *entry);
  void PromoteToCache(FrameId frame_id, Entry *entry);
  bool EvictFromSegment(std::list<FrameId> *segment, FrameId *frame_id);

  const std::size_t capacity_;
  mutable std::mutex latch_;
  std::unordered_map<FrameId, Entry> entries_;
  std::list<FrameId> history_;
  std::list<FrameId> cache_;
  std::size_t evictable_size_{0};
};

}  // namespace telepath

#endif  // TELEPATH_REPLACER_TWO_QUEUE_REPLACER_H_
