#include "telepath/replacer/lru_replacer.h"

#include <memory>

namespace telepath {

LruReplacer::LruReplacer(std::size_t capacity) : capacity_(capacity) {}

void LruReplacer::RecordAccess(FrameId frame_id) {
  std::lock_guard<std::mutex> guard(latch_);
  if (!IsValidFrame(frame_id)) return;

  auto it = entries_.find(frame_id);
  if (it != entries_.end()) {
    lru_list_.erase(it->second.position);
    lru_list_.push_front(frame_id);
    it->second.position = lru_list_.begin();
    return;
  }

  if (entries_.size() >= capacity_) return;

  lru_list_.push_front(frame_id);
  entries_.emplace(frame_id, Entry{false, lru_list_.begin()});
}

void LruReplacer::SetEvictable(FrameId frame_id, bool evictable) {
  std::lock_guard<std::mutex> guard(latch_);
  auto it = entries_.find(frame_id);
  if (it == entries_.end()) return;
  if (it->second.evictable == evictable) return;
  it->second.evictable = evictable;
  if (evictable) ++evictable_size_;
  else if (evictable_size_ > 0) --evictable_size_;
}

bool LruReplacer::Victim(FrameId *frame_id) {
  std::lock_guard<std::mutex> guard(latch_);
  if (frame_id == nullptr || evictable_size_ == 0) return false;

  for (auto it = lru_list_.rbegin(); it != lru_list_.rend(); ++it) {
    if (TryEvictFrame(*it, frame_id)) return true;
  }
  return false;
}

void LruReplacer::Remove(FrameId frame_id) {
  std::lock_guard<std::mutex> guard(latch_);
  auto it = entries_.find(frame_id);
  if (it == entries_.end()) return;
  if (it->second.evictable && evictable_size_ > 0) --evictable_size_;
  lru_list_.erase(it->second.position);
  entries_.erase(it);
}

auto LruReplacer::Size() const -> std::size_t {
  std::lock_guard<std::mutex> guard(latch_);
  return evictable_size_;
}

bool LruReplacer::IsValidFrame(FrameId frame_id) const {
  return frame_id < capacity_;
}

bool LruReplacer::TryEvictFrame(FrameId candidate, FrameId *frame_id) {
  auto entry_it = entries_.find(candidate);
  if (entry_it == entries_.end() || !entry_it->second.evictable) return false;

  *frame_id = candidate;
  lru_list_.erase(entry_it->second.position);
  entries_.erase(entry_it);
  --evictable_size_;
  return true;
}

auto MakeLruReplacer(std::size_t capacity) -> std::unique_ptr<Replacer> {
  return std::make_unique<LruReplacer>(capacity);
}

}  // namespace telepath
