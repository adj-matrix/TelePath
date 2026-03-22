#include "telepath/replacer/lru_k_replacer.h"

#include <memory>

namespace telepath {

LruKReplacer::LruKReplacer(std::size_t capacity, std::size_t history_length)
    : capacity_(capacity),
      history_length_(history_length == 0 ? 1 : history_length) {}

void LruKReplacer::RecordAccess(FrameId frame_id) {
  std::lock_guard<std::mutex> guard(latch_);
  if (!IsValidFrame(frame_id)) {
    return;
  }

  auto [it, inserted] = entries_.try_emplace(frame_id);
  if (inserted && entries_.size() > capacity_) {
    entries_.erase(it);
    return;
  }

  ++current_timestamp_;
  it->second.access_history.push_back(current_timestamp_);
  while (it->second.access_history.size() > history_length_) {
    it->second.access_history.pop_front();
  }
}

void LruKReplacer::SetEvictable(FrameId frame_id, bool evictable) {
  std::lock_guard<std::mutex> guard(latch_);
  auto it = entries_.find(frame_id);
  if (it == entries_.end()) {
    return;
  }
  it->second.evictable = evictable;
}

bool LruKReplacer::Victim(FrameId *frame_id) {
  std::lock_guard<std::mutex> guard(latch_);
  if (frame_id == nullptr) {
    return false;
  }

  Entry *best_entry = nullptr;
  FrameId best_frame_id = kInvalidFrameId;
  for (auto &entry : entries_) {
    if (!entry.second.evictable) {
      continue;
    }
    if (PreferCandidate(entry.second, entry.first, best_entry, best_frame_id)) {
      best_entry = &entry.second;
      best_frame_id = entry.first;
    }
  }

  if (best_entry == nullptr) {
    return false;
  }

  entries_.erase(best_frame_id);
  *frame_id = best_frame_id;
  return true;
}

void LruKReplacer::Remove(FrameId frame_id) {
  std::lock_guard<std::mutex> guard(latch_);
  entries_.erase(frame_id);
}

std::size_t LruKReplacer::Size() const {
  std::lock_guard<std::mutex> guard(latch_);
  std::size_t count = 0;
  for (const auto &entry : entries_) {
    if (entry.second.evictable) {
      ++count;
    }
  }
  return count;
}

bool LruKReplacer::IsValidFrame(FrameId frame_id) const {
  return frame_id < capacity_;
}

bool LruKReplacer::PreferCandidate(const Entry &candidate,
                                   FrameId candidate_id,
                                   const Entry *current_best,
                                   FrameId current_best_id) const {
  if (current_best == nullptr) {
    return true;
  }

  const bool candidate_incomplete = candidate.access_history.size() < history_length_;
  const bool best_incomplete = current_best->access_history.size() < history_length_;

  if (candidate_incomplete != best_incomplete) {
    return candidate_incomplete;
  }

  if (candidate_incomplete) {
    const uint64_t candidate_recent = candidate.access_history.back();
    const uint64_t best_recent = current_best->access_history.back();
    if (candidate_recent != best_recent) {
      return candidate_recent < best_recent;
    }
    return candidate_id < current_best_id;
  }

  const uint64_t candidate_kth = candidate.access_history.front();
  const uint64_t best_kth = current_best->access_history.front();
  if (candidate_kth != best_kth) {
    return candidate_kth < best_kth;
  }
  return candidate_id < current_best_id;
}

std::unique_ptr<Replacer> MakeLruKReplacer(std::size_t capacity,
                                           std::size_t history_length) {
  return std::make_unique<LruKReplacer>(capacity, history_length);
}

}  // namespace telepath
