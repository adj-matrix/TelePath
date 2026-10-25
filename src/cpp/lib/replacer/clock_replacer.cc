#include "telepath/replacer/clock_replacer.h"

namespace telepath {

ClockReplacer::ClockReplacer(std::size_t capacity)
  : capacity_(capacity),
    state_(std::make_unique<std::atomic<std::uint8_t>[]>(capacity)) {}

void ClockReplacer::RecordAccess(FrameId frame_id) {
  if (frame_id >= capacity_) return;
  std::atomic<std::uint8_t> &state = state_[frame_id];
  if (state.load(std::memory_order_relaxed) == kClear) state.store(kReferenced, std::memory_order_relaxed);
}

void ClockReplacer::RecordLoad(FrameId frame_id) {
  if (frame_id >= capacity_) return;
  state_[frame_id].store(kReferenced, std::memory_order_relaxed);
}

bool ClockReplacer::Victim(const EvictionProbe &probe, FrameId *frame_id) {
  if (frame_id == nullptr || capacity_ == 0) return false;
  for (std::size_t scanned = 0; scanned < 2 * capacity_; ++scanned) {
    const auto candidate = static_cast<FrameId>(hand_.fetch_add(1, std::memory_order_relaxed) % capacity_);
    std::atomic<std::uint8_t> &state = state_[candidate];
    const std::uint8_t observed = state.load(std::memory_order_relaxed);
    if (observed == kUntracked || !probe.IsEvictable(candidate)) continue;
    if (observed == kReferenced) {
      state.store(kClear, std::memory_order_relaxed);
      continue;
    }
    *frame_id = candidate;
    return true;
  }
  return false;
}

void ClockReplacer::Remove(FrameId frame_id) {
  if (frame_id >= capacity_) return;
  state_[frame_id].store(kUntracked, std::memory_order_relaxed);
}

auto MakeClockReplacer(std::size_t capacity) -> std::unique_ptr<Replacer> {
  return std::make_unique<ClockReplacer>(capacity);
}

}  // namespace telepath
