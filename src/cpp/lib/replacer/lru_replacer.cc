#include "telepath/replacer/lru_replacer.h"

#include <memory>

namespace telepath {

LruReplacer::LruReplacer(std::size_t capacity) : capacity_(capacity) {}

void LruReplacer::RecordAccess(FrameId frame_id) {
  std::lock_guard<std::mutex> guard(latch_);
  if (!IsValidFrame(frame_id)) {
    return;
  }

  auto it = entries_.find(frame_id);
  if (it != entries_.end()) {
    lru_list_.erase(it->second.position);
    lru_list_.push_front(frame_id);
    it->second.position = lru_list_.begin();
    return;
  }

  if (entries_.size() >= capacity_) {
    return;
  }

  lru_list_.push_front(frame_id);
  entries_.emplace(frame_id, Entry{false, lru_list_.begin()});
}

void LruReplacer::SetEvictable(FrameId frame_id, bool evictable) {
  std::lock_guard<std::mutex> guard(latch_);
  auto it = entries_.find(frame_id);
  if (it == entries_.end()) {
    return;
  }
  it->second.evictable = evictable;
}

bool LruReplacer::Victim(FrameId *frame_id) {
  std::lock_guard<std::mutex> guard(latch_);
  if (frame_id == nullptr) {
    return false;
  }

  for (auto it = lru_list_.rbegin(); it != lru_list_.rend(); ++it) {
    auto entry_it = entries_.find(*it);
    if (entry_it != entries_.end() && entry_it->second.evictable) {
      *frame_id = *it;
      lru_list_.erase(entry_it->second.position);
      entries_.erase(entry_it);
      return true;
    }
  }
  return false;
}

void LruReplacer::Remove(FrameId frame_id) {
  std::lock_guard<std::mutex> guard(latch_);
  auto it = entries_.find(frame_id);
  if (it == entries_.end()) {
    return;
  }
  lru_list_.erase(it->second.position);
  entries_.erase(it);
}

std::size_t LruReplacer::Size() const {
  std::lock_guard<std::mutex> guard(latch_);
  std::size_t count = 0;
  for (const auto &entry : entries_) {
    if (entry.second.evictable) {
      ++count;
    }
  }
  return count;
}

bool LruReplacer::IsValidFrame(FrameId frame_id) const {
  return frame_id < capacity_;
}

std::unique_ptr<Replacer> MakeLruReplacer(std::size_t capacity) {
  return std::make_unique<LruReplacer>(capacity);
}

}  // namespace telepath
