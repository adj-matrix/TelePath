#include "telepath/buffer/buffer_manager.h"

#include <utility>

#include "buffer_manager_observer.h"
#include "completion_dispatcher.h"
#include "frame_control.h"
#include "frame_memory_pool.h"
#include "miss_coordinator.h"
#include "page_table.h"
#include "telepath/io/disk_backend.h"
#include "telepath/replacer/replacer.h"

namespace telepath {

auto BufferManager::PinPage(const BufferTag &tag) -> Result<FrameId> {
  if (Status usable = CheckUsable(); !usable.has_value()) return PropagateError(usable);

  if (auto resident_hit = TryReadResidentHit(tag); resident_hit.has_value()) return resident_hit.value();

  auto registration = miss_coordinator_->RegisterOrJoin(tag);
  if (!registration.is_owner) return AwaitJoinedMiss(tag, registration.state);

  if (auto owner_hit = TryReadResidentHit(tag); owner_hit.has_value()) {
    miss_coordinator_->Complete(tag, registration.state, Ok(), owner_hit.value());
    return owner_hit.value();
  }
  return LoadMissOwnerBuffer(tag, registration.state);
}

auto BufferManager::TryReadResidentHit(const BufferTag &tag) -> std::optional<FrameId> {
  auto frame_id = page_table_->LookupFrameId(tag);
  if (!frame_id.has_value()) return std::nullopt;
  if (!TryPinResidentFrame(frame_id.value(), tag)) return std::nullopt;

  observer_->RecordResidentHit(tag);
  return frame_id.value();
}

auto BufferManager::AwaitJoinedMiss(const BufferTag &tag, const std::shared_ptr<BufferManagerMissState> &state) -> Result<FrameId> {
  auto joined_state = state;
  while (true) {
    Result<FrameId> wait_result = miss_coordinator_->Wait(joined_state);
    if (!wait_result.has_value()) return PropagateError(wait_result);

    if (TryPinResidentFrame(wait_result.value(), tag)) {
      observer_->RecordJoinedMissHit(tag);
      return wait_result.value();
    }

    if (auto resident_hit = TryReadResidentHit(tag); resident_hit.has_value()) return resident_hit.value();

    auto registration = miss_coordinator_->RegisterOrJoin(tag);
    if (!registration.is_owner) {
      joined_state = registration.state;
      continue;
    }

    if (auto owner_hit = TryReadResidentHit(tag); owner_hit.has_value()) {
      miss_coordinator_->Complete(tag, registration.state, Ok(), owner_hit.value());
      return owner_hit.value();
    }
    return LoadMissOwnerBuffer(tag, registration.state);
  }
}

auto BufferManager::LoadMissOwnerBuffer(const BufferTag &tag, const std::shared_ptr<BufferManagerMissState> &state) -> Result<FrameId> {
  observer_->RecordReadMiss(tag);

  Result<FrameReservation> reserve_result = ReserveFrameForTag(tag);
  if (!reserve_result.has_value()) {
    miss_coordinator_->Complete(tag, state, PropagateError(reserve_result), kInvalidFrameId);
    return PropagateError(reserve_result);
  }

  Result<FrameId> frame_result = CompleteReservation(tag, reserve_result.value());
  miss_coordinator_->Complete(tag, state, frame_result.has_value() ? Ok() : PropagateError(frame_result), frame_result.value_or(kInvalidFrameId));
  return frame_result;
}

bool BufferManager::TryPinResidentFrame(FrameId frame_id, const BufferTag &tag) {
  BufferDescriptor &descriptor = descriptors_[frame_id];
  if (!frame_control::TryPinResident(descriptor.control)) return false;
  if (LoadTag(descriptor) != tag) {
    std::ignore = frame_control::Unpin(descriptor.control);
    return false;
  }

  replacer_->RecordAccess(frame_id);
  replacer_->SetEvictable(frame_id, false);
  return true;
}

auto BufferManager::ReserveFrameForTag(const BufferTag &tag) -> Result<FrameReservation> {
  while (true) {
    Result<FrameId> frame_result = AcquireReservationFrame();
    if (!frame_result.has_value()) return PropagateError(frame_result);
    const FrameId frame_id = frame_result.value();

    FrameReservation reservation;
    FrameReservationOutcome outcome = TryReserveFrameForTag(frame_id, tag, true, &reservation);
    if (outcome == FrameReservationOutcome::kNeedsFlush) {
      Status flush_status = FlushVictimBeforeReuse(frame_id);
      if (!flush_status.has_value()) return PropagateError(flush_status);
      outcome = TryReserveFrameForTag(frame_id, tag, false, &reservation);
    }
    if (outcome == FrameReservationOutcome::kReserved) return reservation;
  }
}

auto BufferManager::CompleteReservation(const BufferTag &tag, const FrameReservation &reservation) -> Result<FrameId>{
  const FrameId frame_id = reservation.frame_id;

  Status read_status = ReadReservedPage(frame_id, tag);
  if (!read_status.has_value()) return PropagateError(AbortLoadReservation(frame_id, tag, read_status));

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
  ResetCleanerCandidate(frame_id);
  frame_control::PublishResident(descriptors_[frame_id].control);
}

auto BufferManager::ReadReservedPage(FrameId frame_id, const BufferTag &tag) -> Status {
  Result<uint64_t> read_submit = disk_backend_->SubmitRead(tag, frame_pool_->GetFrameData(frame_id), geometry_.block_size());
  if (!read_submit.has_value()) return PropagateError(read_submit);
  completion_dispatcher_->Register(read_submit.value());
  return completion_dispatcher_->Wait(read_submit.value());
}

auto BufferManager::LoadTag(const BufferDescriptor &descriptor) const -> BufferTag {
  return BufferTag{
    descriptor.tag_file_id.load(std::memory_order_relaxed),
    descriptor.tag_block_id.load(std::memory_order_relaxed),
  };
}

void BufferManager::StoreTag(BufferDescriptor *descriptor, const BufferTag &tag) {
  descriptor->tag_file_id.store(tag.file_id, std::memory_order_relaxed);
  descriptor->tag_block_id.store(tag.block_id, std::memory_order_relaxed);
}

}  // namespace telepath
