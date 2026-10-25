#include "telepath/buffer/buffer_manager.h"

#include <thread>
#include <utility>

#include "buffer_manager_observer.h"
#include "frame_control.h"
#include "frame_memory_pool.h"
#include "io_engine.h"
#include "miss_coordinator.h"
#include "page_table.h"
#include "telepath/io/disk_backend.h"
#include "telepath/replacer/replacer.h"

namespace telepath {

auto BufferManager::PinPage(const BufferTag &tag, BufferAccessStrategy *strategy) -> Result<FrameId> {
  if (Status usable = CheckUsable(); !usable.has_value()) return PropagateError(usable);
  const bool record_access = strategy == nullptr || strategy->RecordsAccesses();

  if (auto resident_hit = TryReadResidentHit(tag, record_access); resident_hit.has_value()) return resident_hit.value();

  auto registration = miss_coordinator_->RegisterOrJoin(tag);
  if (!registration.is_owner) return AwaitJoinedMiss(tag, registration.state, strategy);

  if (auto owner_hit = TryReadResidentHit(tag, record_access); owner_hit.has_value()) {
    miss_coordinator_->Complete(tag, registration.state, Ok(), owner_hit.value());
    return owner_hit.value();
  }
  return LoadMissOwnerBuffer(tag, registration.state, strategy);
}

auto BufferManager::TryReadResidentHit(const BufferTag &tag, bool record_access) -> std::optional<FrameId> {
  auto frame_id = page_table_->LookupFrameId(tag);
  if (!frame_id.has_value()) return std::nullopt;
  if (!TryPinResidentFrame(frame_id.value(), tag, record_access)) return std::nullopt;

  observer_->RecordResidentHit(tag);
  return frame_id.value();
}

auto BufferManager::AwaitJoinedMiss(const BufferTag &tag, const std::shared_ptr<BufferManagerMissState> &state, BufferAccessStrategy *strategy) -> Result<FrameId> {
  const bool record_access = strategy == nullptr || strategy->RecordsAccesses();
  auto joined_state = state;
  while (true) {
    Result<FrameId> wait_result = miss_coordinator_->Wait(joined_state);
    if (!wait_result.has_value()) return PropagateError(wait_result);

    if (wait_result.value() != kInvalidFrameId && TryPinResidentFrame(wait_result.value(), tag, record_access)) {
      observer_->RecordJoinedMissHit(tag);
      return wait_result.value();
    }

    if (auto resident_hit = TryReadResidentHit(tag, record_access); resident_hit.has_value()) return resident_hit.value();

    auto registration = miss_coordinator_->RegisterOrJoin(tag);
    if (!registration.is_owner) {
      joined_state = registration.state;
      continue;
    }

    if (auto owner_hit = TryReadResidentHit(tag, record_access); owner_hit.has_value()) {
      miss_coordinator_->Complete(tag, registration.state, Ok(), owner_hit.value());
      return owner_hit.value();
    }
    return LoadMissOwnerBuffer(tag, registration.state, strategy);
  }
}

auto BufferManager::LoadMissOwnerBuffer(const BufferTag &tag, const std::shared_ptr<BufferManagerMissState> &state, BufferAccessStrategy *strategy) -> Result<FrameId> {
  observer_->RecordReadMiss(tag);

  Result<FrameReservation> reserve_result = ReserveFrameForTag(tag, strategy);
  if (!reserve_result.has_value()) {
    miss_coordinator_->Complete(tag, state, PropagateError(reserve_result), kInvalidFrameId);
    return PropagateError(reserve_result);
  }

  Result<FrameId> frame_result = CompleteReservation(tag, reserve_result.value());
  miss_coordinator_->Complete(tag, state, frame_result.has_value() ? Ok() : PropagateError(frame_result), frame_result.value_or(kInvalidFrameId));
  return frame_result;
}

bool BufferManager::TryPinResidentFrame(FrameId frame_id, const BufferTag &tag, bool record_access) {
  BufferDescriptor &descriptor = descriptors_[frame_id];
  if (!frame_control::TryPinResident(descriptor.control)) return false;
  if (LoadTag(descriptor) != tag) {
    std::ignore = frame_control::Unpin(descriptor.control);
    return false;
  }

  if (record_access) replacer_->RecordAccess(frame_id);
  return true;
}

auto BufferManager::ReserveFrameForTag(const BufferTag &tag, BufferAccessStrategy *strategy) -> Result<FrameReservation> {
  if (auto ring_reservation = TryReserveRingFrame(tag, strategy); ring_reservation.has_value()) return ring_reservation.value();

  while (true) {
    Result<FrameId> frame_result = AcquireReservationFrame();
    if (!frame_result.has_value()) {
      if (frame_result.error().code() != ErrorCode::kResourceExhausted || !HasUnpinnedFrame()) return PropagateError(frame_result);
      std::this_thread::yield();
      continue;
    }
    const FrameId frame_id = frame_result.value();

    FrameReservation reservation;
    FrameReservationOutcome outcome = TryReserveFrameForTag(frame_id, tag, true, &reservation);
    if (outcome == FrameReservationOutcome::kNeedsFlush) {
      Status flush_status = FlushVictimBeforeReuse(frame_id);
      if (!flush_status.has_value()) return PropagateError(flush_status);
      outcome = TryReserveFrameForTag(frame_id, tag, false, &reservation);
    }
    if (outcome != FrameReservationOutcome::kReserved) continue;
    if (strategy != nullptr) strategy->RememberRingFrame(frame_id);
    return reservation;
  }
}

auto BufferManager::TryReserveRingFrame(const BufferTag &tag, BufferAccessStrategy *strategy) -> std::optional<FrameReservation> {
  if (strategy == nullptr) return std::nullopt;
  const FrameId ring_frame = strategy->NextRingFrame();
  if (ring_frame == kInvalidFrameId || ring_frame >= pool_size_) return std::nullopt;
  FrameReservation reservation;
  if (TryReserveFrameForTag(ring_frame, tag, false, &reservation) != FrameReservationOutcome::kReserved) return std::nullopt;
  return reservation;
}

auto BufferManager::CompleteReservation(const BufferTag &tag, const FrameReservation &reservation) -> Result<FrameId>{
  const FrameId frame_id = reservation.frame_id;

  Status read_status = ReadReservedPage(frame_id, tag);
  if (!read_status.has_value()) return PropagateError(AbortLoadReservation(frame_id, tag, read_status));

  PublishLoadedFrame(tag, reservation);
  return frame_id;
}

void BufferManager::PublishLoadedFrame(const BufferTag &tag, const FrameReservation &reservation) {
  ResetCleanerCandidate(reservation.frame_id);
  frame_control::PublishResident(descriptors_[reservation.frame_id].control);
  replacer_->RecordLoad(reservation.frame_id);
  if (reservation.had_evicted_page) observer_->RecordLoadCompletion(tag, reservation.evicted_tag);
  else observer_->RecordLoadCompletion(tag);
}

auto BufferManager::ReadReservedPage(FrameId frame_id, const BufferTag &tag) -> Status {
  IoWaiter waiter;
  DiskRequest request;
  request.token = reinterpret_cast<uint64_t>(static_cast<IoCompletion *>(&waiter));
  request.operation = DiskOperation::kRead;
  request.tag = tag;
  request.buffers[0] = frame_pool_->GetFrameData(frame_id);
  io_engine_->Submit(std::span<const DiskRequest>(&request, 1), DiskSubmitMode::kWait);
  return io_engine_->Wait(waiter);
}

bool BufferManager::IsResidentTag(const BufferTag &tag) const {
  std::optional<FrameId> frame_id = page_table_->LookupFrameId(tag);
  if (!frame_id.has_value()) return false;
  const BufferDescriptor &descriptor = descriptors_[frame_id.value()];
  return frame_control::State(descriptor.control.load(std::memory_order_acquire)) != BufferFrameState::kFree && LoadTag(descriptor) == tag;
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
