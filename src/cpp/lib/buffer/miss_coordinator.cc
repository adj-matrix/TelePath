#include "miss_coordinator.h"

namespace telepath {

BufferManagerMissCoordinator::BufferManagerMissCoordinator(std::size_t stripe_count)
  : latches_(stripe_count), state_shards_(stripe_count) {}

auto BufferManagerMissCoordinator::RegisterOrJoin(const BufferTag &tag) -> BufferManagerMissRegistration {
  const std::size_t stripe = StripeForTag(tag);
  std::lock_guard<std::mutex> guard(latches_[stripe]);
  auto &shard = state_shards_[stripe];
  auto it = shard.find(tag);
  if (it != shard.end()) return BufferManagerMissRegistration{it->second, false};

  auto state = std::make_shared<BufferManagerMissState>();
  shard.emplace(tag, state);
  return BufferManagerMissRegistration{std::move(state), true};
}

void BufferManagerMissCoordinator::Complete(const BufferTag &tag, const std::shared_ptr<BufferManagerMissState> &state, const Status &status, FrameId frame_id) {
  if (state == nullptr) return;

  {
    std::lock_guard<std::mutex> state_guard(state->latch);
    state->status = status;
    state->frame_id = frame_id;
    state->completed = true;
  }
  state->cv.notify_all();

  const std::size_t stripe = StripeForTag(tag);
  std::lock_guard<std::mutex> guard(latches_[stripe]);
  auto &shard = state_shards_[stripe];
  auto it = shard.find(tag);
  if (it != shard.end() && it->second == state) shard.erase(it);
}

auto BufferManagerMissCoordinator::Wait(const std::shared_ptr<BufferManagerMissState> &state) -> Result<FrameId> {
  if (state == nullptr) return Status::InvalidArgument("miss state must not be null");

  std::unique_lock<std::mutex> lock(state->latch);
  state->cv.wait(lock, [&state]() { return state->completed; });
  if (!state->status.ok()) return state->status;
  if (state->frame_id == kInvalidFrameId) return Status::Internal("completed miss is missing a frame id");
  return state->frame_id;
}

auto BufferManagerMissCoordinator::StripeForTag(const BufferTag &tag) const -> std::size_t {
  return BufferTagHash{}(tag) % latches_.size();
}

}  // namespace telepath
