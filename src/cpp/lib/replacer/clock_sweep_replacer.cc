#include "telepath/replacer/clock_sweep_replacer.h"

namespace telepath {

ClockSweepReplacer::ClockSweepReplacer(std::size_t capacity)
  : capacity_(capacity),
    usage_count_(std::make_unique<std::atomic<std::uint8_t>[]>(capacity)) {
  for (std::size_t frame = 0; frame < capacity_; ++frame) usage_count_[frame].store(kUntracked, std::memory_order_relaxed);
}

void ClockSweepReplacer::RecordAccess(FrameId frame_id) {
  if (frame_id >= capacity_) return;
  std::atomic<std::uint8_t> &usage = usage_count_[frame_id];
  std::uint8_t observed = usage.load(std::memory_order_relaxed);
  if (observed >= kMaxUsageCount) return;
  usage.compare_exchange_weak(observed, static_cast<std::uint8_t>(observed + 1), std::memory_order_relaxed);
}

void ClockSweepReplacer::RecordLoad(FrameId frame_id) {
  if (frame_id >= capacity_) return;
  usage_count_[frame_id].store(1, std::memory_order_relaxed);
}

bool ClockSweepReplacer::Victim(const EvictionProbe &probe, FrameId *frame_id) {
  if (frame_id == nullptr || capacity_ == 0) return false;
  for (std::size_t scanned = 0; scanned < (kMaxUsageCount + 1) * capacity_; ++scanned) {
    const auto candidate = static_cast<FrameId>(hand_.fetch_add(1, std::memory_order_relaxed) % capacity_);
    std::atomic<std::uint8_t> &usage = usage_count_[candidate];
    std::uint8_t observed = usage.load(std::memory_order_relaxed);
    if (observed == kUntracked || !probe.IsEvictable(candidate)) continue;
    if (observed > 0) {
      usage.compare_exchange_weak(observed, static_cast<std::uint8_t>(observed - 1), std::memory_order_relaxed);
      continue;
    }
    *frame_id = candidate;
    return true;
  }
  return false;
}

void ClockSweepReplacer::Remove(FrameId frame_id) {
  if (frame_id >= capacity_) return;
  usage_count_[frame_id].store(kUntracked, std::memory_order_relaxed);
}

auto MakeClockSweepReplacer(std::size_t capacity) -> std::unique_ptr<Replacer> {
  return std::make_unique<ClockSweepReplacer>(capacity);
}

}  // namespace telepath
