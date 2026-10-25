#include "telepath/replacer/two_queue_replacer.h"

#include <mutex>

#include "access_batcher.h"
#include "frame_list.h"

namespace telepath {

struct TwoQueueReplacer::State {
  explicit State(std::size_t capacity) : capacity(capacity), history(capacity), cache(capacity) {}

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
  std::mutex latch;
  AccessBatcher batcher;
  FrameList history;
  FrameList cache;
};

TwoQueueReplacer::TwoQueueReplacer(std::size_t capacity) : state_(std::make_unique<State>(capacity)) {}

TwoQueueReplacer::~TwoQueueReplacer() = default;

void TwoQueueReplacer::RecordAccess(FrameId frame_id) {
  state_->batcher.Record(frame_id, state_->latch, [this](FrameId frame) { state_->Touch(frame); });
}

void TwoQueueReplacer::RecordLoad(FrameId frame_id) {
  if (frame_id >= state_->capacity) return;
  std::lock_guard<std::mutex> guard(state_->latch);
  state_->batcher.DrainCurrentThread([this](FrameId frame) { state_->Touch(frame); });
  state_->Untrack(frame_id);
  state_->history.PushFront(frame_id);
}

bool TwoQueueReplacer::Victim(const EvictionProbe &probe, FrameId *frame_id) {
  if (frame_id == nullptr) return false;
  std::lock_guard<std::mutex> guard(state_->latch);
  state_->batcher.DrainAll([this](FrameId frame) { state_->Touch(frame); });
  return State::PickFrom(state_->history, probe, frame_id) || State::PickFrom(state_->cache, probe, frame_id);
}

void TwoQueueReplacer::Remove(FrameId frame_id) {
  if (frame_id >= state_->capacity) return;
  std::lock_guard<std::mutex> guard(state_->latch);
  state_->batcher.DrainCurrentThread([this](FrameId frame) { state_->Touch(frame); });
  state_->Untrack(frame_id);
}

auto MakeTwoQueueReplacer(std::size_t capacity) -> std::unique_ptr<Replacer> {
  return std::make_unique<TwoQueueReplacer>(capacity);
}

}  // namespace telepath
