#include "telepath/replacer/lru_k_replacer.h"

#include <memory>

namespace telepath {

LruKReplacer::LruKReplacer(
  std::size_t capacity,
  std::size_t history_length
) : capacity_(capacity),
    history_length_(history_length == 0 ? 1 : history_length) {}

void LruKReplacer::RecordAccess(FrameId frame_id) {
  std::lock_guard<std::mutex> guard(latch_);
  if (!IsValidFrame(frame_id)) return;

  auto [it, inserted] = entries_.try_emplace(frame_id);
  if (inserted && entries_.size() > capacity_) {
    entries_.erase(it);
    return;
  }

  if (it->second.evictable) EraseEvictableEntry(frame_id, it->second);

  ++current_timestamp_;
  it->second.access_history.push_back(current_timestamp_);
  while (it->second.access_history.size() > history_length_) {
    it->second.access_history.pop_front();
  }

  if (it->second.evictable) InsertEvictableEntry(frame_id, it->second);
}

void LruKReplacer::SetEvictable(FrameId frame_id, bool evictable) {
  std::lock_guard<std::mutex> guard(latch_);
  auto it = entries_.find(frame_id);
  if (it == entries_.end()) return;
  if (it->second.evictable == evictable) return;

  if (evictable) {
    it->second.evictable = true;
    InsertEvictableEntry(frame_id, it->second);
    ++evictable_size_;
  } else {
    EraseEvictableEntry(frame_id, it->second);
    it->second.evictable = false;
    if (evictable_size_ > 0) --evictable_size_;
  }
}

bool LruKReplacer::Victim(FrameId *frame_id) {
  std::lock_guard<std::mutex> guard(latch_);
  if (frame_id == nullptr || evictable_size_ == 0) return false;
  if (TakeVictimFromQueue(&history_queue_, frame_id)) return true;
  return TakeVictimFromQueue(&cache_queue_, frame_id);
}

void LruKReplacer::Remove(FrameId frame_id) {
  std::lock_guard<std::mutex> guard(latch_);
  auto it = entries_.find(frame_id);
  if (it == entries_.end()) return;
  if (it->second.evictable && evictable_size_ > 0) {
    EraseEvictableEntry(frame_id, it->second);
    --evictable_size_;
  } else if (it->second.evictable) {
    EraseEvictableEntry(frame_id, it->second);
  }
  entries_.erase(it);
}

auto LruKReplacer::Size() const -> std::size_t {
  std::lock_guard<std::mutex> guard(latch_);
  return evictable_size_;
}

bool LruKReplacer::IsValidFrame(FrameId frame_id) const {
  return frame_id < capacity_;
}

bool LruKReplacer::HasIncompleteHistory(const Entry &entry) const {
  return entry.access_history.size() < history_length_;
}

auto LruKReplacer::HistoryQueueKey(const Entry &entry, FrameId frame_id) const -> QueueKey {
  return {entry.access_history.back(), frame_id};
}

auto LruKReplacer::CacheQueueKey(const Entry &entry, FrameId frame_id) const -> QueueKey {
  return {entry.access_history.front(), frame_id};
}

void LruKReplacer::InsertEvictableEntry(FrameId frame_id, const Entry &entry) {
  if (HasIncompleteHistory(entry)) {
    history_queue_.emplace(HistoryQueueKey(entry, frame_id));
    return;
  }
  cache_queue_.emplace(CacheQueueKey(entry, frame_id));
}

void LruKReplacer::EraseEvictableEntry(FrameId frame_id, const Entry &entry) {
  if (HasIncompleteHistory(entry)) {
    history_queue_.erase(HistoryQueueKey(entry, frame_id));
    return;
  }
  cache_queue_.erase(CacheQueueKey(entry, frame_id));
}

bool LruKReplacer::TakeVictimFromQueue(std::set<QueueKey> *queue, FrameId *frame_id) {
  if (queue == nullptr || queue->empty()) return false;

  const FrameId victim = queue->begin()->second;
  queue->erase(queue->begin());
  entries_.erase(victim);
  --evictable_size_;
  *frame_id = victim;
  return true;
}

auto MakeLruKReplacer(std::size_t capacity, std::size_t history_length) -> std::unique_ptr<Replacer> {
  return std::make_unique<LruKReplacer>(capacity, history_length);
}

}  // namespace telepath
