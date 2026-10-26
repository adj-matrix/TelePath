#include "telepath/replacer/lru_replacer.h"

#include <mutex>

#include "access_batcher.h"
#include "frame_list.h"
#include "../common/state_layout.h"
#include "../sync/futex.h"

namespace telepath {

namespace {

struct Offsets {
  std::size_t latch;
  FrameList::Offsets recency;
  std::size_t bytes;
};

auto LayoutFor(std::size_t capacity) -> Offsets {
  StateLayout layout;
  Offsets offsets{};
  offsets.latch = layout.Add<sync::FutexMutex>(1);
  offsets.recency = FrameList::Reserve(&layout, capacity);
  offsets.bytes = layout.size();
  return offsets;
}

}  // namespace

struct LruReplacer::State {
  explicit State(std::size_t capacity) : capacity(capacity), offsets(LayoutFor(capacity)), owned(std::make_unique<std::byte[]>(offsets.bytes)) { Place(owned.get(), true); }

  void Place(std::byte *base, bool initialize) {
    latch = initialize ? ConstructState<sync::FutexMutex>(base, offsets.latch, 1) : StateAt<sync::FutexMutex>(base, offsets.latch);
    recency.Place(base, offsets.recency, capacity, initialize);
  }

  void Touch(FrameId frame_id) {
    if (frame_id < capacity && recency.Contains(frame_id)) recency.MoveToFront(frame_id);
  }

  const std::size_t capacity;
  const Offsets offsets;
  std::unique_ptr<std::byte[]> owned;
  sync::FutexMutex *latch{nullptr};
  AccessBatcher batcher;
  FrameList recency;
};

LruReplacer::LruReplacer(std::size_t capacity) : state_(std::make_unique<State>(capacity)) {}

LruReplacer::~LruReplacer() = default;

auto LruReplacer::SharedStateBytes() const -> std::size_t { return state_->offsets.bytes; }

auto LruReplacer::BindSharedState(std::span<std::byte> memory, bool initialize) -> Status {
  if (Status valid = CheckStateMemory(memory, state_->offsets.bytes); !valid.has_value()) return valid;
  state_->Place(memory.data(), initialize);
  state_->owned.reset();
  return Ok();
}

void LruReplacer::RecordAccess(FrameId frame_id) {
  state_->batcher.Record(frame_id, *state_->latch, [this](FrameId frame) { state_->Touch(frame); });
}

void LruReplacer::RecordLoad(FrameId frame_id) {
  if (frame_id >= state_->capacity) return;
  std::lock_guard<sync::FutexMutex> guard(*state_->latch);
  state_->batcher.DrainCurrentThread([this](FrameId frame) { state_->Touch(frame); });
  if (state_->recency.Contains(frame_id)) state_->recency.MoveToFront(frame_id);
  else state_->recency.PushFront(frame_id);
}

bool LruReplacer::Victim(const EvictionProbe &probe, FrameId *frame_id) {
  if (frame_id == nullptr) return false;
  std::lock_guard<sync::FutexMutex> guard(*state_->latch);
  state_->batcher.DrainAll([this](FrameId frame) { state_->Touch(frame); });
  for (FrameId candidate = state_->recency.back(); candidate != FrameList::kNone; candidate = state_->recency.TowardFront(candidate)) {
    if (!probe.IsEvictable(candidate)) continue;
    *frame_id = candidate;
    return true;
  }
  return false;
}

void LruReplacer::Remove(FrameId frame_id) {
  if (frame_id >= state_->capacity) return;
  std::lock_guard<sync::FutexMutex> guard(*state_->latch);
  state_->batcher.DrainCurrentThread([this](FrameId frame) { state_->Touch(frame); });
  if (state_->recency.Contains(frame_id)) state_->recency.Unlink(frame_id);
}

auto MakeLruReplacer(std::size_t capacity) -> std::unique_ptr<Replacer> {
  return std::make_unique<LruReplacer>(capacity);
}

}  // namespace telepath
