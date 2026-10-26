#include "telepath/replacer/two_queue_replacer.h"

#include <mutex>

#include "access_batcher.h"
#include "frame_list.h"
#include "../common/state_layout.h"
#include "../sync/futex.h"

namespace telepath {

namespace {

struct Offsets {
  std::size_t latch;
  FrameList::Offsets history;
  FrameList::Offsets cache;
  std::size_t bytes;
};

auto LayoutFor(std::size_t capacity) -> Offsets {
  StateLayout layout;
  Offsets offsets{};
  offsets.latch = layout.Add<sync::FutexMutex>(1);
  offsets.history = FrameList::Reserve(&layout, capacity);
  offsets.cache = FrameList::Reserve(&layout, capacity);
  offsets.bytes = layout.size();
  return offsets;
}

}  // namespace

struct TwoQueueReplacer::State {
  explicit State(std::size_t capacity) : capacity(capacity), offsets(LayoutFor(capacity)), owned(std::make_unique<std::byte[]>(offsets.bytes)) { Place(owned.get(), true); }

  void Place(std::byte *base, bool initialize) {
    latch = initialize ? ConstructState<sync::FutexMutex>(base, offsets.latch, 1) : StateAt<sync::FutexMutex>(base, offsets.latch);
    history.Place(base, offsets.history, capacity, initialize);
    cache.Place(base, offsets.cache, capacity, initialize);
  }

  void Touch(FrameId frame_id) {
    if (frame_id >= capacity) return;
    if (history.Contains(frame_id)) {
      history.Unlink(frame_id);
      cache.PushFront(frame_id);
      return;
    }
    if (cache.Contains(frame_id)) cache.MoveToFront(frame_id);
  }

  void Untrack(FrameId frame_id) {
    if (history.Contains(frame_id)) history.Unlink(frame_id);
    if (cache.Contains(frame_id)) cache.Unlink(frame_id);
  }

  static bool PickFrom(const FrameList &segment, const EvictionProbe &probe, FrameId *frame_id) {
    for (FrameId candidate = segment.back(); candidate != FrameList::kNone; candidate = segment.TowardFront(candidate)) {
      if (!probe.IsEvictable(candidate)) continue;
      *frame_id = candidate;
      return true;
    }
    return false;
  }

  const std::size_t capacity;
  const Offsets offsets;
  std::unique_ptr<std::byte[]> owned;
  sync::FutexMutex *latch{nullptr};
  AccessBatcher batcher;
  FrameList history;
  FrameList cache;
};

TwoQueueReplacer::TwoQueueReplacer(std::size_t capacity) : state_(std::make_unique<State>(capacity)) {}

TwoQueueReplacer::~TwoQueueReplacer() = default;

auto TwoQueueReplacer::SharedStateBytes() const -> std::size_t { return state_->offsets.bytes; }

auto TwoQueueReplacer::BindSharedState(std::span<std::byte> memory, bool initialize) -> Status {
  if (Status valid = CheckStateMemory(memory, state_->offsets.bytes); !valid.has_value()) return valid;
  state_->Place(memory.data(), initialize);
  state_->owned.reset();
  return Ok();
}

void TwoQueueReplacer::RecordAccess(FrameId frame_id) {
  state_->batcher.Record(frame_id, *state_->latch, [this](FrameId frame) { state_->Touch(frame); });
}

void TwoQueueReplacer::RecordLoad(FrameId frame_id) {
  if (frame_id >= state_->capacity) return;
  std::lock_guard<sync::FutexMutex> guard(*state_->latch);
  state_->batcher.DrainCurrentThread([this](FrameId frame) { state_->Touch(frame); });
  state_->Untrack(frame_id);
  state_->history.PushFront(frame_id);
}

bool TwoQueueReplacer::Victim(const EvictionProbe &probe, FrameId *frame_id) {
  if (frame_id == nullptr) return false;
  std::lock_guard<sync::FutexMutex> guard(*state_->latch);
  state_->batcher.DrainAll([this](FrameId frame) { state_->Touch(frame); });
  return State::PickFrom(state_->history, probe, frame_id) || State::PickFrom(state_->cache, probe, frame_id);
}

void TwoQueueReplacer::Remove(FrameId frame_id) {
  if (frame_id >= state_->capacity) return;
  std::lock_guard<sync::FutexMutex> guard(*state_->latch);
  state_->batcher.DrainCurrentThread([this](FrameId frame) { state_->Touch(frame); });
  state_->Untrack(frame_id);
}

auto MakeTwoQueueReplacer(std::size_t capacity) -> std::unique_ptr<Replacer> {
  return std::make_unique<TwoQueueReplacer>(capacity);
}

}  // namespace telepath
