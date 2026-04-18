#ifndef TELEPATH_REPLACER_LRU_REPLACER_H_
#define TELEPATH_REPLACER_LRU_REPLACER_H_

#include <list>
#include <mutex>
#include <unordered_map>

#include "telepath/replacer/replacer.h"

namespace telepath {

// Hash-and-list-backed least-recently-used replacer.
class LruReplacer : public Replacer {
 public:
  // Creates a replacer that can track up to `capacity` frame ids.
  explicit LruReplacer(std::size_t capacity);

  // Moves the accessed frame to the MRU position, inserting it on first use if
  // capacity permits.
  void RecordAccess(FrameId frame_id) override;
  // Toggles whether the frame may currently be chosen as a victim.
  void SetEvictable(FrameId frame_id, bool evictable) override;
  // Removes and returns the least-recently-used evictable frame.
  bool Victim(FrameId *frame_id) override;
  // Stops tracking the given frame if it is present.
  void Remove(FrameId frame_id) override;
  // Returns the current number of evictable tracked frames.
  auto Size() const -> std::size_t override;

 private:
  struct Entry {
    bool evictable{false};
    std::list<FrameId>::iterator position;
  };

  bool IsValidFrame(FrameId frame_id) const;
  bool TryEvictFrame(FrameId candidate, FrameId *frame_id);

  const std::size_t capacity_;
  mutable std::mutex latch_;
  std::unordered_map<FrameId, Entry> entries_;
  std::list<FrameId> lru_list_;
  std::size_t evictable_size_{0};
};

}  // namespace telepath

#endif  // TELEPATH_REPLACER_LRU_REPLACER_H_
