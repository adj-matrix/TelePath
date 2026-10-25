#include "telepath/replacer/lru_replacer.h"

#include <mutex>

#include "access_batcher.h"
#include "frame_list.h"

namespace telepath {

struct LruReplacer::State {
  explicit State(std::size_t capacity) : capacity(capacity), recency(capacity) {}

  void Touch(FrameId frame_id) {
    if (frame_id < capacity && recency.Contains(frame_id)) recency.MoveToFront(frame_id);
  }

  const std::size_t capacity;
  std::mutex latch;
  AccessBatcher batcher;
  FrameList recency;
};

LruReplacer::LruReplacer(std::size_t capacity) : state_(std::make_unique<State>(capacity)) {}

LruReplacer::~LruReplacer() = default;

void LruReplacer::RecordAccess(FrameId frame_id) {
  state_->batcher.Record(frame_id, state_->latch, [this](FrameId frame) { state_->Touch(frame); });
}

void LruReplacer::RecordLoad(FrameId frame_id) {
  if (frame_id >= state_->capacity) return;
  std::lock_guard<std::mutex> guard(state_->latch);
  state_->batcher.DrainCurrentThread([this](FrameId frame) { state_->Touch(frame); });
  if (state_->recency.Contains(frame_id)) state_->recency.MoveToFront(frame_id);
  else state_->recency.PushFront(frame_id);
}

bool LruReplacer::Victim(const EvictionProbe &probe, FrameId *frame_id) {
  if (frame_id == nullptr) return false;
  std::lock_guard<std::mutex> guard(state_->latch);
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
  std::lock_guard<std::mutex> guard(state_->latch);
  state_->batcher.DrainCurrentThread([this](FrameId frame) { state_->Touch(frame); });
  if (state_->recency.Contains(frame_id)) state_->recency.Unlink(frame_id);
}

auto MakeLruReplacer(std::size_t capacity) -> std::unique_ptr<Replacer> {
  return std::make_unique<LruReplacer>(capacity);
}

}  // namespace telepath
