#include "telepath/replacer/two_queue_replacer.h"

#include <memory>

namespace telepath {

TwoQueueReplacer::TwoQueueReplacer(std::size_t capacity) : capacity_(capacity) {}

void TwoQueueReplacer::RecordAccess(FrameId frame_id) {
  std::lock_guard<std::mutex> guard(latch_);
  if (!IsValidFrame(frame_id)) return;

  auto [it, inserted] = entries_.try_emplace(frame_id);
  if (inserted && entries_.size() > capacity_) {
    entries_.erase(it);
    return;
  }
  if (inserted) return;
  if (IsInCache(it->second)) {
    if (!it->second.evictable) return;
    cache_.erase(it->second.position);
    cache_.push_front(frame_id);
    it->second.position = cache_.begin();
    return;
  }
  PromoteToCache(frame_id, &it->second);
}

void TwoQueueReplacer::SetEvictable(FrameId frame_id, bool evictable) {
  std::lock_guard<std::mutex> guard(latch_);
  auto it = entries_.find(frame_id);
  if (it == entries_.end()) return;
  if (it->second.evictable == evictable) return;

  it->second.evictable = evictable;
  if (evictable) {
    InsertIntoSegment(frame_id, &it->second);
    ++evictable_size_;
    return;
  }
  RemoveFromSegment(&it->second);
  if (evictable_size_ > 0) --evictable_size_;
}

bool TwoQueueReplacer::Victim(FrameId *frame_id) {
  std::lock_guard<std::mutex> guard(latch_);
  if (frame_id == nullptr || evictable_size_ == 0) return false;
  if (EvictFromSegment(&history_, frame_id)) return true;
  return EvictFromSegment(&cache_, frame_id);
}

void TwoQueueReplacer::Remove(FrameId frame_id) {
  std::lock_guard<std::mutex> guard(latch_);
  auto it = entries_.find(frame_id);
  if (it == entries_.end()) return;
  if (it->second.evictable) {
    RemoveFromSegment(&it->second);
    if (evictable_size_ > 0) --evictable_size_;
  }
  entries_.erase(it);
}

auto TwoQueueReplacer::Size() const -> std::size_t {
  std::lock_guard<std::mutex> guard(latch_);
  return evictable_size_;
}

bool TwoQueueReplacer::IsValidFrame(FrameId frame_id) const {
  return frame_id < capacity_;
}

bool TwoQueueReplacer::IsInCache(const Entry &entry) const {
  return entry.segment == Segment::kCache;
}

void TwoQueueReplacer::InsertIntoSegment(FrameId frame_id, Entry *entry) {
  if (entry == nullptr) return;
  auto *segment = IsInCache(*entry) ? &cache_ : &history_;
  segment->push_front(frame_id);
  entry->position = segment->begin();
}

void TwoQueueReplacer::RemoveFromSegment(Entry *entry) {
  if (entry == nullptr) return;
  auto *segment = IsInCache(*entry) ? &cache_ : &history_;
  segment->erase(entry->position);
}

void TwoQueueReplacer::PromoteToCache(FrameId frame_id, Entry *entry) {
  if (entry == nullptr) return;
  if (entry->evictable) {
    history_.erase(entry->position);
    cache_.push_front(frame_id);
    entry->position = cache_.begin();
  }
  entry->segment = Segment::kCache;
}

bool TwoQueueReplacer::EvictFromSegment(std::list<FrameId> *segment, FrameId *frame_id) {
  if (segment == nullptr || segment->empty()) return false;

  const FrameId victim = segment->back();
  segment->pop_back();
  entries_.erase(victim);
  --evictable_size_;
  *frame_id = victim;
  return true;
}

auto MakeTwoQueueReplacer(std::size_t capacity) -> std::unique_ptr<Replacer> {
  return std::make_unique<TwoQueueReplacer>(capacity);
}

}  // namespace telepath
