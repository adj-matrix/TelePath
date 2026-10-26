#include "telepath/replacer/clock_sweep_replacer.h"

#include "../common/state_layout.h"

namespace telepath {

namespace {

struct Offsets {
  std::size_t hand;
  std::size_t usage_count;
  std::size_t bytes;
};

auto LayoutFor(std::size_t capacity) -> Offsets {
  StateLayout layout;
  Offsets offsets{};
  offsets.hand = layout.Add<std::atomic<std::size_t>>(1);
  offsets.usage_count = layout.Add<std::atomic<std::uint8_t>>(capacity);
  offsets.bytes = layout.size();
  return offsets;
}

}  // namespace

struct ClockSweepReplacer::State {
  explicit State(std::size_t capacity) : capacity(capacity), offsets(LayoutFor(capacity)), owned(std::make_unique<std::byte[]>(offsets.bytes)) { Place(owned.get(), true); }

  void Place(std::byte *base, bool initialize) {
    if (initialize) {
      hand = ConstructState<std::atomic<std::size_t>>(base, offsets.hand, 1, std::size_t{0});
      usage_count = ConstructState<std::atomic<std::uint8_t>>(base, offsets.usage_count, capacity, kUntracked);
      return;
    }
    hand = StateAt<std::atomic<std::size_t>>(base, offsets.hand);
    usage_count = StateAt<std::atomic<std::uint8_t>>(base, offsets.usage_count);
  }

  const std::size_t capacity;
  const Offsets offsets;
  std::unique_ptr<std::byte[]> owned;
  std::atomic<std::size_t> *hand{nullptr};
  std::atomic<std::uint8_t> *usage_count{nullptr};
};

ClockSweepReplacer::ClockSweepReplacer(std::size_t capacity) : state_(std::make_unique<State>(capacity)) {}

ClockSweepReplacer::~ClockSweepReplacer() = default;

auto ClockSweepReplacer::SharedStateBytes() const -> std::size_t { return state_->offsets.bytes; }

auto ClockSweepReplacer::BindSharedState(std::span<std::byte> memory, bool initialize) -> Status {
  if (Status valid = CheckStateMemory(memory, state_->offsets.bytes); !valid.has_value()) return valid;
  state_->Place(memory.data(), initialize);
  state_->owned.reset();
  return Ok();
}

void ClockSweepReplacer::RecordAccess(FrameId frame_id) {
  if (frame_id >= state_->capacity) return;
  std::atomic<std::uint8_t> &usage = state_->usage_count[frame_id];
  std::uint8_t observed = usage.load(std::memory_order_relaxed);
  if (observed >= kMaxUsageCount) return;
  usage.compare_exchange_weak(observed, static_cast<std::uint8_t>(observed + 1), std::memory_order_relaxed);
}

void ClockSweepReplacer::RecordLoad(FrameId frame_id) {
  if (frame_id >= state_->capacity) return;
  state_->usage_count[frame_id].store(1, std::memory_order_relaxed);
}

bool ClockSweepReplacer::Victim(const EvictionProbe &probe, FrameId *frame_id) {
  if (frame_id == nullptr || state_->capacity == 0) return false;
  for (std::size_t scanned = 0; scanned < (kMaxUsageCount + 1) * state_->capacity; ++scanned) {
    const auto candidate = static_cast<FrameId>(state_->hand->fetch_add(1, std::memory_order_relaxed) % state_->capacity);
    std::atomic<std::uint8_t> &usage = state_->usage_count[candidate];
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
  if (frame_id >= state_->capacity) return;
  state_->usage_count[frame_id].store(kUntracked, std::memory_order_relaxed);
}

auto MakeClockSweepReplacer(std::size_t capacity) -> std::unique_ptr<Replacer> {
  return std::make_unique<ClockSweepReplacer>(capacity);
}

}  // namespace telepath
