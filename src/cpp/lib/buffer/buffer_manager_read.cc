#include "telepath/buffer/buffer_manager.h"

#include <thread>
#include <tuple>
#include <utility>

#include "buffer_manager_observer.h"
#include "frame_control.h"
#include "frame_memory_pool.h"
#include "io_engine.h"
#include "page_table.h"
#include "telepath/io/disk_backend.h"
#include "telepath/replacer/replacer.h"

namespace telepath {

auto BufferManager::PinPage(const BufferTag &tag, BufferAccessStrategy *strategy) -> Result<FrameId> {
  if (Status usable = CheckUsable(); !usable.has_value()) return PropagateError(usable);
  const bool record_access = strategy == nullptr || strategy->RecordsAccesses();

  bool waited = false;
  while (true) {
    FrameId frame_id = kInvalidFrameId;
    const ResidentProbe probe = ProbeResident(tag, record_access, &frame_id);
    if (probe == ResidentProbe::kPinned) {
      if (waited) observer_->RecordJoinedMissHit(tag);
      else observer_->RecordResidentHit(tag);
      return frame_id;
    }
    if (probe == ResidentProbe::kLoading) {
      WaitForLoad(frame_id, tag);
      waited = true;
      continue;
    }

    Result<std::optional<FrameId>> loaded = LoadPage(tag, strategy);
    if (!loaded.has_value()) return PropagateError(loaded);
    if (loaded->has_value()) return loaded->value();
  }
}

auto BufferManager::ProbeResident(const BufferTag &tag, bool record_access, FrameId *frame_id) -> ResidentProbe {
  const std::optional<FrameId> mapped = page_table_->LookupFrameId(tag);
  if (!mapped.has_value()) return ResidentProbe::kAbsent;
  *frame_id = mapped.value();
  if (TryPinResidentFrame(*frame_id, tag, record_access)) return ResidentProbe::kPinned;
  if (IsLoadingTag(*frame_id, tag)) return ResidentProbe::kLoading;
  return ResidentProbe::kAbsent;
}

auto BufferManager::TryReadResidentHit(const BufferTag &tag, bool record_access) -> std::optional<FrameId> {
  FrameId frame_id = kInvalidFrameId;
  if (ProbeResident(tag, record_access, &frame_id) != ResidentProbe::kPinned) return std::nullopt;
  observer_->RecordResidentHit(tag);
  return frame_id;
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

bool BufferManager::IsLoadingTag(FrameId frame_id, const BufferTag &tag) const {
  const BufferDescriptor &descriptor = descriptors_[frame_id];
  return frame_control::State(descriptor.control.load(std::memory_order_acquire)) == BufferFrameState::kLoading && LoadTag(descriptor) == tag;
}

void BufferManager::WaitForLoad(FrameId frame_id, const BufferTag &tag) {
  frame_control::WaitWhile(descriptors_[frame_id].control, [&](std::uint32_t) { return IsLoadingTag(frame_id, tag); });
}

bool BufferManager::IsLiveFor(FrameId frame_id, const BufferTag &tag) const {
  const BufferDescriptor &descriptor = descriptors_[frame_id];
  return frame_control::State(descriptor.control.load(std::memory_order_acquire)) != BufferFrameState::kFree && LoadTag(descriptor) == tag;
}

auto BufferManager::LoadPage(const BufferTag &tag, BufferAccessStrategy *strategy) -> Result<std::optional<FrameId>> {
  Result<std::optional<FrameReservation>> reserved = ReserveFrameForTag(tag, strategy);
  if (!reserved.has_value()) return PropagateError(reserved);
  if (!reserved->has_value()) return std::optional<FrameId>{};

  observer_->RecordReadMiss(tag);
  Result<FrameId> frame = CompleteReservation(tag, reserved->value());
  if (!frame.has_value()) return PropagateError(frame);
  return std::optional<FrameId>{frame.value()};
}

auto BufferManager::ReserveFrameForTag(const BufferTag &tag, BufferAccessStrategy *strategy) -> Result<std::optional<FrameReservation>> {
  FrameReservation reservation;
  const FrameReservationOutcome ring_outcome = TryReserveRingFrame(tag, strategy, &reservation);
  if (ring_outcome == FrameReservationOutcome::kReserved) return std::optional<FrameReservation>{reservation};
  if (ring_outcome == FrameReservationOutcome::kTagExists) return std::optional<FrameReservation>{};

  while (true) {
    Result<FrameId> victim = AcquireVictimFrame();
    if (!victim.has_value()) return PropagateError(victim);
    Result<FrameReservationOutcome> outcome = ReserveVictim(victim.value(), tag, &reservation);
    if (!outcome.has_value()) return PropagateError(outcome);

    if (outcome.value() == FrameReservationOutcome::kTagExists) return std::optional<FrameReservation>{};
    if (outcome.value() != FrameReservationOutcome::kReserved) continue;
    if (strategy != nullptr) strategy->RememberRingFrame(victim.value());
    return std::optional<FrameReservation>{reservation};
  }
}

auto BufferManager::AcquireVictimFrame() -> Result<FrameId> {
  while (true) {
    Result<FrameId> frame = AcquireReservationFrame();
    if (frame.has_value() || frame.error().code() != ErrorCode::kResourceExhausted || !HasTransientFrame()) return frame;
    std::this_thread::yield();
  }
}

auto BufferManager::ReserveVictim(FrameId frame_id, const BufferTag &tag, FrameReservation *reservation) -> Result<FrameReservationOutcome> {
  const FrameReservationOutcome outcome = TryReserveFrameForTag(frame_id, tag, true, reservation);
  if (outcome != FrameReservationOutcome::kNeedsFlush) return outcome;
  if (Status flushed = FlushVictimBeforeReuse(frame_id); !flushed.has_value()) return PropagateError(flushed);
  return TryReserveFrameForTag(frame_id, tag, false, reservation);
}

auto BufferManager::TryReserveRingFrame(const BufferTag &tag, BufferAccessStrategy *strategy, FrameReservation *reservation) -> FrameReservationOutcome {
  if (strategy == nullptr) return FrameReservationOutcome::kRejected;
  const FrameId ring_frame = strategy->NextRingFrame();
  if (ring_frame == kInvalidFrameId || ring_frame >= pool_size_) return FrameReservationOutcome::kRejected;
  return TryReserveFrameForTag(ring_frame, tag, false, reservation);
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
