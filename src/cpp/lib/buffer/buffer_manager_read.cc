#include "telepath/buffer/buffer_manager.h"

#include <utility>

#include "buffer_descriptor_state.h"
#include "buffer_manager_observer.h"
#include "completion_dispatcher.h"
#include "frame_memory_pool.h"
#include "miss_coordinator.h"
#include "page_table.h"
#include "telepath/io/disk_backend.h"
#include "telepath/replacer/replacer.h"

namespace telepath {

auto BufferManager::TryReadResidentHit(const BufferTag &tag) -> std::optional<BufferHandle> {
  auto frame_id = page_table_->LookupFrameId(tag);
  if (!frame_id.has_value()) return std::nullopt;
  auto handle = TryPinResidentFrame(frame_id.value(), tag);
  if (!handle.has_value()) return std::nullopt;

  observer_->RecordResidentHit(tag);
  return std::move(handle.value());
}

auto BufferManager::AwaitJoinedMiss(const BufferTag &tag, const std::shared_ptr<BufferManagerMissState> &state) -> Result<BufferHandle> {
  Result<FrameId> wait_result = miss_coordinator_->Wait(state);
  if (!wait_result.ok()) return wait_result.status();

  FrameId frame_id = wait_result.value();
  Status wait_status = WaitForFrameReady(frame_id);
  if (!wait_status.ok()) return wait_status;

  auto handle = TryPinResidentFrame(frame_id, tag);
  if (!handle.has_value()) return Status::NotFound("buffer not resident after load completion");
  observer_->RecordJoinedMissHit(tag);
  return std::move(handle.value());
}

auto BufferManager::LoadMissOwnerBuffer(const BufferTag &tag, const std::shared_ptr<BufferManagerMissState> &state) -> Result<BufferHandle> {
  observer_->RecordReadMiss(tag);

  Result<FrameReservation> reserve_result = ReserveFrameForTag(tag);
  if (!reserve_result.ok()) {
    miss_coordinator_->Complete(tag, state, reserve_result.status(), kInvalidFrameId);
    return reserve_result.status();
  }

  Result<FrameId> frame_result = CompleteReservation(tag, reserve_result.value());
  if (!frame_result.ok()) {
    miss_coordinator_->Complete(tag, state, frame_result.status(), kInvalidFrameId);
    return frame_result.status();
  }

  const FrameId frame_id = frame_result.value();
  miss_coordinator_->Complete(tag, state, Status::Ok(), frame_id);
  return BufferHandle(this, frame_id, tag, frame_pool_->GetFrameData(frame_id), page_size_);
}

auto BufferManager::TryPinResidentFrame(FrameId frame_id, const BufferTag &tag) -> std::optional<BufferHandle> {
  BufferDescriptor &descriptor = descriptors_[frame_id];
  {
    std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
    if (!buffer_descriptor_state::IsResidentFrameMatch(descriptor, tag)) return std::nullopt;
    ++descriptor.pin_count;
  }

  replacer_->RecordAccess(frame_id);
  replacer_->SetEvictable(frame_id, false);
  return BufferHandle(this, frame_id, tag, frame_pool_->GetFrameData(frame_id), page_size_);
}

auto BufferManager::WaitForFrameReady(FrameId frame_id) -> Status {
  if (frame_id >= pool_size_) return Status::InvalidArgument("invalid frame id while awaiting buffer");

  BufferDescriptor &descriptor = descriptors_[frame_id];
  std::unique_lock<std::mutex> lock(descriptor.latch);
  while (descriptor.state == BufferFrameState::kLoading || descriptor.io_in_flight) {
    descriptor.io_cv.wait(lock);
  }
  return descriptor.last_io_status;
}

auto BufferManager::ReserveFrameForTag(const BufferTag &tag) -> Result<FrameReservation> {
  while (true) {
    Result<FrameId> frame_result = AcquireReservationFrame();
    if (!frame_result.ok()) return frame_result.status();

    auto reservation = TryReserveFrameForTag(frame_result.value(), tag);
    if (reservation.has_value()) return std::move(reservation.value());
  }
}

auto BufferManager::CompleteReservation(const BufferTag &tag, const FrameReservation &reservation) -> Result<FrameId>{
  const FrameId frame_id = reservation.frame_id;

  Status flush_status = FlushReservedVictim(reservation);
  if (!flush_status.ok()) return RestoreEvictionFailure(reservation, flush_status);

  Status read_status = ReadReservedPage(frame_id, tag);
  if (!read_status.ok()) return AbortLoadReservation(frame_id, tag, read_status);

  CompleteLoadedFrame(frame_id);
  replacer_->RecordAccess(frame_id);
  replacer_->SetEvictable(frame_id, false);
  if (!reservation.had_evicted_page) {
    observer_->RecordLoadCompletion(tag);
    return frame_id;
  }
  observer_->RecordLoadCompletion(tag, reservation.evicted_tag);
  return frame_id;
}

void BufferManager::CompleteLoadedFrame(FrameId frame_id) {
  BufferDescriptor &descriptor = descriptors_[frame_id];
  std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
  buffer_descriptor_state::EnterResidentState(&descriptor);
  descriptor.io_cv.notify_all();
  ResetCleanerCandidate(frame_id);
}

void BufferManager::MarkFrameReadInFlight(FrameId frame_id) {
  BufferDescriptor &descriptor = descriptors_[frame_id];
  std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
  descriptor.io_in_flight = true;
  descriptor.last_io_status = Status::Ok();
}

auto BufferManager::ReadReservedPage(FrameId frame_id, const BufferTag &tag) -> Status {
  MarkFrameReadInFlight(frame_id);
  Result<uint64_t> read_submit = disk_backend_->SubmitRead(tag, frame_pool_->GetFrameData(frame_id), page_size_);
  if (!read_submit.ok()) return read_submit.status();
  completion_dispatcher_->Register(read_submit.value());
  return completion_dispatcher_->Wait(read_submit.value());
}

}  // namespace telepath
