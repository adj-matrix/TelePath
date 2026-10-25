#include "telepath/buffer/buffer_manager.h"

#include <tuple>
#include <utility>

#include "buffer_manager_observer.h"
#include "frame_memory_pool.h"
#include "io_engine.h"
#include "miss_coordinator.h"

namespace telepath {

struct ReadBuffersState {
  enum class Slot : std::uint8_t {
    kPinned,
    kLoading,
    kFailed,
    kDeferred,
  };

  BufferAccessStrategy *strategy{nullptr};
  std::vector<BufferTag> tags;
  std::vector<FrameId> frames;
  std::vector<Slot> slots;
  IoWaiter waiter{0};
};

struct BufferManager::AsyncLoad final : public IoCompletion {
  struct Entry {
    BufferTag tag{};
    FrameReservation reservation{};
    std::shared_ptr<BufferManagerMissState> miss_state;
    std::size_t slot{0};
  };

  void Complete(const Status &status) override {
    manager->FinishAsyncLoad(*this, status);
    delete this;
  }

  BufferManager *manager{nullptr};
  ReadBuffersState *operation{nullptr};
  std::vector<Entry> entries;
};

ReadBuffersOperation::ReadBuffersOperation() = default;

ReadBuffersOperation::~ReadBuffersOperation() {
  if (state_ == nullptr || manager_ == nullptr) return;
  Status ignored = Ok();
  manager_->FinishReadBuffers(*this, false, &ignored);
}

auto BufferManager::PrefetchBuffer(FileId file_id, BlockId block_id, std::size_t block_count) -> Status {
  if (Status usable = CheckUsable(); !usable.has_value()) return usable;

  std::vector<std::unique_ptr<AsyncLoad>> loads;
  Status status = Ok();
  for (std::size_t i = 0; i < block_count; ++i) {
    const BufferTag tag{file_id, block_id + i};
    if (IsResidentTag(tag)) continue;
    auto registration = miss_coordinator_->RegisterOrJoin(tag);
    if (!registration.is_owner) continue;
    if (IsResidentTag(tag)) {
      miss_coordinator_->Complete(tag, registration.state, Ok(), kInvalidFrameId);
      continue;
    }

    Result<FrameReservation> reservation = ReserveFrameForTag(tag, nullptr);
    if (!reservation.has_value()) {
      miss_coordinator_->Complete(tag, registration.state, Ok(), kInvalidFrameId);
      if (reservation.error().code() != ErrorCode::kResourceExhausted) status = PropagateError(reservation);
      break;
    }
    AppendAsyncLoad(&loads, nullptr, tag, reservation.value(), std::move(registration.state), i);
  }
  SubmitAsyncLoads(&loads);
  return status;
}

auto BufferManager::StartReadBuffers(ReadBuffersOperation &operation, FileId file_id, BlockId block_id, std::size_t block_count, BufferAccessStrategy *strategy) -> Status {
  if (Status usable = CheckUsable(); !usable.has_value()) return usable;
  if (operation.in_progress()) return InvalidArgument("read buffers operation is still in progress");

  operation.handles_.clear();
  auto state = std::make_unique<ReadBuffersState>();
  state->strategy = strategy;
  state->tags.resize(block_count);
  state->frames.assign(block_count, kInvalidFrameId);
  state->slots.assign(block_count, ReadBuffersState::Slot::kDeferred);
  const bool record_access = strategy == nullptr || strategy->RecordsAccesses();

  std::vector<std::unique_ptr<AsyncLoad>> loads;
  for (std::size_t i = 0; i < block_count; ++i) {
    const BufferTag tag{file_id, block_id + i};
    state->tags[i] = tag;
    if (auto hit = TryReadResidentHit(tag, record_access); hit.has_value()) {
      state->frames[i] = hit.value();
      state->slots[i] = ReadBuffersState::Slot::kPinned;
      continue;
    }

    auto registration = miss_coordinator_->RegisterOrJoin(tag);
    if (!registration.is_owner) continue;
    if (auto hit = TryReadResidentHit(tag, record_access); hit.has_value()) {
      miss_coordinator_->Complete(tag, registration.state, Ok(), hit.value());
      state->frames[i] = hit.value();
      state->slots[i] = ReadBuffersState::Slot::kPinned;
      continue;
    }

    observer_->RecordReadMiss(tag);
    Result<FrameReservation> reservation = ReserveFrameForTag(tag, strategy);
    if (!reservation.has_value()) {
      miss_coordinator_->Complete(tag, registration.state, Ok(), kInvalidFrameId);
      continue;
    }
    state->frames[i] = reservation->frame_id;
    state->slots[i] = ReadBuffersState::Slot::kLoading;
    AppendAsyncLoad(&loads, state.get(), tag, reservation.value(), std::move(registration.state), i);
  }

  operation.manager_ = this;
  operation.state_ = std::move(state);
  SubmitAsyncLoads(&loads);
  return Ok();
}

auto BufferManager::WaitReadBuffers(ReadBuffersOperation &operation) -> Status {
  if (!operation.in_progress()) return InvalidArgument("read buffers operation is not in progress");
  if (operation.manager_ != this) return InvalidArgument("read buffers operation belongs to another buffer manager");
  Status status = Ok();
  FinishReadBuffers(operation, true, &status);
  return status;
}

void BufferManager::AppendAsyncLoad(
  std::vector<std::unique_ptr<AsyncLoad>> *loads,
  ReadBuffersState *operation,
  const BufferTag &tag,
  const FrameReservation &reservation,
  std::shared_ptr<BufferManagerMissState> miss_state,
  std::size_t slot) {
  AsyncLoad *last = loads->empty() ? nullptr : loads->back().get();
  const bool extends_last = last != nullptr
    && last->entries.size() < io_engine_->max_request_blocks()
    && last->entries.back().tag.file_id == tag.file_id
    && last->entries.back().tag.block_id + 1 == tag.block_id;
  if (!extends_last) {
    auto load = std::make_unique<AsyncLoad>();
    load->manager = this;
    load->operation = operation;
    loads->push_back(std::move(load));
  }
  loads->back()->entries.push_back(AsyncLoad::Entry{tag, reservation, std::move(miss_state), slot});
}

void BufferManager::SubmitAsyncLoads(std::vector<std::unique_ptr<AsyncLoad>> *loads) {
  if (loads->empty()) return;
  std::vector<DiskRequest> requests(loads->size());
  for (std::size_t i = 0; i < loads->size(); ++i) {
    AsyncLoad *load = (*loads)[i].release();
    if (load->operation != nullptr) load->operation->waiter.Add(1);
    requests[i].token = reinterpret_cast<uint64_t>(static_cast<IoCompletion *>(load));
    requests[i].operation = DiskOperation::kRead;
    requests[i].tag = load->entries.front().tag;
    requests[i].block_count = static_cast<std::uint32_t>(load->entries.size());
    for (std::size_t j = 0; j < load->entries.size(); ++j) requests[i].buffers[j] = frame_pool_->GetFrameData(load->entries[j].reservation.frame_id);
  }
  loads->clear();
  io_engine_->Submit(requests, DiskSubmitMode::kAsync);
}

void BufferManager::FinishAsyncLoad(AsyncLoad &load, const Status &status) {
  for (const AsyncLoad::Entry &entry : load.entries) {
    const FrameId frame_id = entry.reservation.frame_id;
    if (!status.has_value()) {
      AbortLoadReservation(frame_id, entry.tag, status);
      miss_coordinator_->Complete(entry.tag, entry.miss_state, status, kInvalidFrameId);
      if (load.operation != nullptr) load.operation->slots[entry.slot] = ReadBuffersState::Slot::kFailed;
      continue;
    }
    PublishLoadedFrame(entry.tag, entry.reservation);
    miss_coordinator_->Complete(entry.tag, entry.miss_state, Ok(), frame_id);
    if (load.operation == nullptr) ReleaseFrame(frame_id);
  }
  if (load.operation != nullptr) load.operation->waiter.Complete(status);
}

void BufferManager::FinishReadBuffers(ReadBuffersOperation &operation, bool make_handles, Status *status) {
  ReadBuffersState &state = *operation.state_;
  Status io_status = io_engine_->Wait(state.waiter);
  if (!state.waiter.done()) {
    // The engine stopped with reads in flight; their completions may still
    // reference the state, so it is deliberately leaked.
    std::ignore = operation.state_.release();
    *status = io_status;
    return;
  }

  Status result = io_status;
  for (std::size_t i = 0; result.has_value() && make_handles && i < state.slots.size(); ++i) {
    if (state.slots[i] != ReadBuffersState::Slot::kDeferred) continue;
    Result<FrameId> frame = PinPage(state.tags[i], state.strategy);
    if (!frame.has_value()) {
      result = PropagateError(frame);
      break;
    }
    state.frames[i] = frame.value();
    state.slots[i] = ReadBuffersState::Slot::kPinned;
  }

  const bool keep = result.has_value() && make_handles;
  if (keep) operation.handles_.reserve(state.slots.size());
  for (std::size_t i = 0; i < state.slots.size(); ++i) {
    const ReadBuffersState::Slot slot = state.slots[i];
    if (slot != ReadBuffersState::Slot::kPinned && slot != ReadBuffersState::Slot::kLoading) continue;
    if (keep) operation.handles_.push_back(MakeSharedHandle(state.frames[i], state.tags[i]));
    else ReleaseFrame(state.frames[i]);
  }
  operation.state_.reset();
  *status = result;
}

}  // namespace telepath
