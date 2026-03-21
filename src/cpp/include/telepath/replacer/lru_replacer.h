#ifndef TELEPATH_REPLACER_LRU_REPLACER_H_
#define TELEPATH_REPLACER_LRU_REPLACER_H_

#include <list>
#include <mutex>
#include <unordered_map>

#include "telepath/replacer/replacer.h"

namespace telepath {

class LruReplacer : public Replacer {
 public:
  explicit LruReplacer(std::size_t capacity);

  void RecordAccess(FrameId frame_id) override;
  void SetEvictable(FrameId frame_id, bool evictable) override;
  bool Victim(FrameId *frame_id) override;
  void Remove(FrameId frame_id) override;
  std::size_t Size() const override;

 private:
  struct Entry {
    bool evictable{false};
    std::list<FrameId>::iterator position;
  };

  bool IsValidFrame(FrameId frame_id) const;

  const std::size_t capacity_;
  mutable std::mutex latch_;
  std::unordered_map<FrameId, Entry> entries_;
  std::list<FrameId> lru_list_;
};

}  // namespace telepath

#endif  // TELEPATH_REPLACER_LRU_REPLACER_H_
