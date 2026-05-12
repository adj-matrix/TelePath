#include "telepath/buffer/buffer_manager.h"

#include "buffer_descriptor_state.h"
#include "buffer_manager_observer.h"
#include "flush_scheduler.h"
#include "page_table.h"
#include "telepath/replacer/replacer.h"

namespace telepath {

auto BufferManager::RestoreEvictionFailure(const FrameReservation &reservation, const Status &status) -> Status {
  FrameId frame_id = reservation.frame_id;
  bool was_dirty = reservation.evicted_dirty;
  uint64_t dirty_generation = reservation.evicted_dirty_generation;
  BufferDescriptor &descriptor = descriptors_[frame_id];
  std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
  page_table_->Replace(descriptor.tag, reservation.evicted_tag, frame_id);
  buffer_descriptor_state::RestoreEvictedFrameDescriptor( &descriptor, reservation.evicted_tag, was_dirty, dirty_generation);
  ResetCleanerCandidate(frame_id);
  replacer_->RecordAccess(frame_id);
  replacer_->SetEvictable(frame_id, true);
  if (was_dirty) RestoreDirtyEviction(frame_id);
  observer_->RecordEvictionFailure(reservation.evicted_tag);
  return status;
}

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

auto BufferManager::FlushEvictedPage(const FrameReservation &reservation) -> Status {
  auto task = std::make_shared<BufferManagerFlushTask>();
  task->frame_id = reservation.frame_id;
  task->tag = reservation.evicted_tag;
  task->generation = reservation.evicted_dirty_generation;
  task->clear_dirty_on_success = false;
  task->snapshot = CaptureFlushSnapshot(reservation.frame_id, nullptr);
  Status enqueue_status = EnqueueFlushTask(task);
  if (!enqueue_status.ok()) return enqueue_status;
  observer_->RecordFlushTaskScheduled(task->tag);
  return flush_scheduler_->Wait(task);
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

auto BufferManager::TryReserveFrameForTag(FrameId frame_id, const BufferTag &tag) -> std::optional<FrameReservation> {
  BufferDescriptor &descriptor = descriptors_[frame_id];
  std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
  if (buffer_descriptor_state::CanReserveFrame(descriptor)) {
    FrameReservation reservation = buffer_descriptor_state::BuildFrameReservation(frame_id, descriptor);
    InstallReservationEntry(tag, reservation);
    ResetCleanerCandidate(reservation.frame_id);
    buffer_descriptor_state::EnterLoadingState(&descriptor, tag);
    if (!reservation.had_evicted_page || !reservation.evicted_dirty) return reservation;
    dirty_page_count_.fetch_sub(1, std::memory_order_acq_rel);
    NotifyCleaner();
    return reservation;
  }
  if (buffer_descriptor_state::ShouldRequeueReservationCandidate(descriptor)) replacer_->SetEvictable(frame_id, true);
  return std::nullopt;
}

auto BufferManager::FlushReservedVictim(const FrameReservation &reservation) -> Status {
  if (!reservation.had_evicted_page || !reservation.evicted_dirty) return Status::Ok();

  Status flush_status = FlushEvictedPage(reservation);
  if (!flush_status.ok()) return flush_status;
  return Status::Ok();
}

void BufferManager::RestoreDirtyEviction(FrameId frame_id) {
  dirty_page_count_.fetch_add(1, std::memory_order_acq_rel);
  MaybeEnqueueCleanerCandidate(frame_id);
  NotifyCleaner();
}

}  // namespace telepath
