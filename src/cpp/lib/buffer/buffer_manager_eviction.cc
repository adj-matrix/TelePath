#include "telepath/buffer/buffer_manager.h"

#include "buffer_descriptor_state.h"
#include "buffer_manager_observer.h"
#include "page_table.h"
#include "telepath/replacer/replacer.h"

namespace telepath {

auto BufferManager::AbortLoadReservation(FrameId frame_id, const BufferTag &tag, const Status &status) -> Status {
  BufferDescriptor &descriptor = descriptors_[frame_id];
  std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
  page_table_->Remove(tag, frame_id);
  buffer_descriptor_state::EnterFreeState(&descriptor, frame_id, status);
  descriptor.io_cv.notify_all();
  ReleaseFreeFrame(frame_id);
  return status;
}

void BufferManager::ReleaseFreeFrame(FrameId frame_id) {
  ResetCleanerCandidate(frame_id);
  std::lock_guard<std::mutex> free_list_guard(free_list_latch_);
  free_list_.push_back(frame_id);
}

auto BufferManager::AcquireReservationFrame() -> Result<FrameId> {
  {
    std::lock_guard<std::mutex> free_list_guard(free_list_latch_);
    if (!free_list_.empty()) {
      const FrameId frame_id = free_list_.back();
      free_list_.pop_back();
      return frame_id;
    }
  }

  FrameId frame_id = kInvalidFrameId;
  if (replacer_->Victim(&frame_id)) return frame_id;
  return Status::ResourceExhausted("no evictable frame available");
}

void BufferManager::InstallReservationEntry(const BufferTag &tag, const FrameReservation &reservation) {
  if (!reservation.had_evicted_page) {
    page_table_->Install(tag, reservation.frame_id);
    return;
  }
  page_table_->Replace(reservation.evicted_tag, tag, reservation.frame_id);
}

auto BufferManager::TryReserveFrameForTag(FrameId frame_id, const BufferTag &tag, bool allow_victim_flush, FrameReservation *reservation) -> FrameReservationOutcome {
  BufferDescriptor &descriptor = descriptors_[frame_id];
  std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
  if (!buffer_descriptor_state::CanReserveFrame(descriptor)) {
    ReturnUnreservedVictim(frame_id, descriptor);
    return FrameReservationOutcome::kRejected;
  }

  if (descriptor.is_valid && descriptor.is_dirty) {
    if (allow_victim_flush) return FrameReservationOutcome::kNeedsFlush;
    ReturnUnreservedVictim(frame_id, descriptor);
    return FrameReservationOutcome::kRejected;
  }

  *reservation = buffer_descriptor_state::BuildFrameReservation(frame_id, descriptor);
  InstallReservationEntry(tag, *reservation);
  ResetCleanerCandidate(frame_id);
  buffer_descriptor_state::EnterLoadingState(&descriptor, tag);
  return FrameReservationOutcome::kReserved;
}

void BufferManager::ReturnUnreservedVictim(FrameId frame_id, const BufferDescriptor &descriptor) {
  replacer_->RecordAccess(frame_id);
  if (descriptor.pin_count == 0) replacer_->SetEvictable(frame_id, true);
}

auto BufferManager::FlushVictimBeforeReuse(FrameId frame_id) -> Status {
  Status flush_status = FlushFrame(frame_id);
  if (flush_status.ok()) return Status::Ok();

  BufferDescriptor &descriptor = descriptors_[frame_id];
  BufferTag victim_tag;
  {
    std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
    victim_tag = descriptor.tag;
    ReturnUnreservedVictim(frame_id, descriptor);
  }
  observer_->RecordEvictionFailure(victim_tag);
  return flush_status;
}

}  // namespace telepath
