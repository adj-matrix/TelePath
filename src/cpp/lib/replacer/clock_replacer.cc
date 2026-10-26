#include "telepath/replacer/clock_replacer.h"

#include "../common/state_layout.h"

namespace telepath {

namespace {

struct Offsets {
  std::size_t hand;
  std::size_t state;
  std::size_t bytes;
};

auto LayoutFor(std::size_t capacity) -> Offsets {
  StateLayout layout;
  Offsets offsets{};
  offsets.hand = layout.Add<std::atomic<std::size_t>>(1);
  offsets.state = layout.Add<std::atomic<std::uint8_t>>(capacity);
  offsets.bytes = layout.size();
  return offsets;
}

}  // namespace

struct ClockReplacer::State {
  explicit State(std::size_t capacity) : capacity(capacity), offsets(LayoutFor(capacity)), owned(std::make_unique<std::byte[]>(offsets.bytes)) { Place(owned.get(), true); }

  void Place(std::byte *base, bool initialize) {
    if (initialize) {
      hand = ConstructState<std::atomic<std::size_t>>(base, offsets.hand, 1, std::size_t{0});
      state = ConstructState<std::atomic<std::uint8_t>>(base, offsets.state, capacity, kUntracked);
      return;
    }
    hand = StateAt<std::atomic<std::size_t>>(base, offsets.hand);
    state = StateAt<std::atomic<std::uint8_t>>(base, offsets.state);
  }

  const std::size_t capacity;
  const Offsets offsets;
  std::unique_ptr<std::byte[]> owned;
  std::atomic<std::size_t> *hand{nullptr};
  std::atomic<std::uint8_t> *state{nullptr};
};

ClockReplacer::ClockReplacer(std::size_t capacity) : state_(std::make_unique<State>(capacity)) {}

ClockReplacer::~ClockReplacer() = default;

auto ClockReplacer::SharedStateBytes() const -> std::size_t { return state_->offsets.bytes; }

auto ClockReplacer::BindSharedState(std::span<std::byte> memory, bool initialize) -> Status {
  if (Status valid = CheckStateMemory(memory, state_->offsets.bytes); !valid.has_value()) return valid;
  state_->Place(memory.data(), initialize);
  state_->owned.reset();
  return Ok();
}

void ClockReplacer::RecordAccess(FrameId frame_id) {
  if (frame_id >= state_->capacity) return;
  std::atomic<std::uint8_t> &state = state_->state[frame_id];
  if (state.load(std::memory_order_relaxed) == kClear) state.store(kReferenced, std::memory_order_relaxed);
}

void ClockReplacer::RecordLoad(FrameId frame_id) {
  if (frame_id >= state_->capacity) return;
  state_->state[frame_id].store(kReferenced, std::memory_order_relaxed);
}

bool ClockReplacer::Victim(const EvictionProbe &probe, FrameId *frame_id) {
  if (frame_id == nullptr || state_->capacity == 0) return false;
  for (std::size_t scanned = 0; scanned < 2 * state_->capacity; ++scanned) {
    const auto candidate = static_cast<FrameId>(state_->hand->fetch_add(1, std::memory_order_relaxed) % state_->capacity);
    std::atomic<std::uint8_t> &state = state_->state[candidate];
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
  if (frame_id >= state_->capacity) return;
  state_->state[frame_id].store(kUntracked, std::memory_order_relaxed);
}

auto MakeClockReplacer(std::size_t capacity) -> std::unique_ptr<Replacer> {
  return std::make_unique<ClockReplacer>(capacity);
}

}  // namespace telepath
