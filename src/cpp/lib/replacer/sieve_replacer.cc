#include "telepath/replacer/sieve_replacer.h"

namespace telepath {

SieveReplacer::SieveReplacer(std::size_t capacity)
  : capacity_(capacity),
    visited_(std::make_unique<std::atomic<std::uint8_t>[]>(capacity)),
    newer_(std::make_unique<FrameId[]>(capacity)),
    older_(std::make_unique<FrameId[]>(capacity)),
    linked_(std::make_unique<bool[]>(capacity)) {}

void SieveReplacer::RecordAccess(FrameId frame_id) {
  if (frame_id >= capacity_) return;
  std::atomic<std::uint8_t> &visited = visited_[frame_id];
  if (visited.load(std::memory_order_relaxed) == 0) visited.store(1, std::memory_order_relaxed);
}

void SieveReplacer::RecordLoad(FrameId frame_id) {
  if (frame_id >= capacity_) return;
  std::lock_guard<std::mutex> guard(latch_);
  if (linked_[frame_id]) Unlink(frame_id);
  visited_[frame_id].store(0, std::memory_order_relaxed);
  PushNewest(frame_id);
}

bool SieveReplacer::Victim(const EvictionProbe &probe, FrameId *frame_id) {
  if (frame_id == nullptr) return false;
  std::lock_guard<std::mutex> guard(latch_);
  FrameId candidate = hand_ != kNone ? hand_ : oldest_;
  for (std::size_t scanned = 0; candidate != kNone && scanned < 2 * linked_count_; ++scanned) {
    const FrameId next = newer_[candidate] != kNone ? newer_[candidate] : oldest_;
    if (!probe.IsEvictable(candidate)) {
      candidate = next;
      continue;
    }
    std::atomic<std::uint8_t> &visited = visited_[candidate];
    if (visited.load(std::memory_order_relaxed) != 0) {
      visited.store(0, std::memory_order_relaxed);
      candidate = next;
      continue;
    }
    hand_ = next;
    *frame_id = candidate;
    return true;
  }
  hand_ = candidate;
  return false;
}

void SieveReplacer::Remove(FrameId frame_id) {
  if (frame_id >= capacity_) return;
  std::lock_guard<std::mutex> guard(latch_);
  if (linked_[frame_id]) Unlink(frame_id);
  visited_[frame_id].store(0, std::memory_order_relaxed);
}

void SieveReplacer::Unlink(FrameId frame_id) {
  const FrameId newer = newer_[frame_id];
  const FrameId older = older_[frame_id];
  if (hand_ == frame_id) hand_ = newer;
  if (newer != kNone) older_[newer] = older;
  else newest_ = older;
  if (older != kNone) newer_[older] = newer;
  else oldest_ = newer;
  linked_[frame_id] = false;
  --linked_count_;
}

void SieveReplacer::PushNewest(FrameId frame_id) {
  newer_[frame_id] = kNone;
  older_[frame_id] = newest_;
  if (newest_ != kNone) newer_[newest_] = frame_id;
  newest_ = frame_id;
  if (oldest_ == kNone) oldest_ = frame_id;
  linked_[frame_id] = true;
  ++linked_count_;
}

auto MakeSieveReplacer(std::size_t capacity) -> std::unique_ptr<Replacer> {
  return std::make_unique<SieveReplacer>(capacity);
}

}  // namespace telepath
