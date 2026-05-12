#include "telepath/buffer/buffer_manager.h"

#include "buffer_descriptor_state.h"
#include "buffer_manager_observer.h"
#include "cleaner_controller.h"

namespace telepath {

auto BufferManager::MarkFrameDirty(FrameId frame_id, const BufferTag &tag) -> Result<bool> {
  BufferDescriptor &descriptor = descriptors_[frame_id];
  std::lock_guard<std::mutex> guard(descriptor.latch);
  if (!descriptor.is_valid || descriptor.state != BufferFrameState::kResident || descriptor.tag != tag) return Status::InvalidArgument("buffer handle refers to an invalid frame");

  const bool became_dirty = !descriptor.is_dirty;
  ++descriptor.dirty_generation;
  descriptor.is_dirty = true;
  return became_dirty;
}

void BufferManager::ResetCleanerCandidate(FrameId frame_id) {
  if (cleaner_controller_ == nullptr || frame_id >= pool_size_) return;
  cleaner_controller_->ResetCandidate(frame_id);
}

void BufferManager::NotifyCleaner() {
  if (cleaner_controller_ != nullptr) cleaner_controller_->Notify();
}

void BufferManager::ScheduleCleanerFlush(FrameId frame_id) {
  bool was_busy = false;
  Result<std::shared_ptr<BufferManagerFlushTask>> schedule_result = TryScheduleFlushTask(frame_id, nullptr, &was_busy, true);
  if (!schedule_result.ok()) {
    observer_->RecordCleanerFlushSkipped();
    return;
  }
  if (schedule_result.value() == nullptr) observer_->RecordCleanerFlushSkipped();
}

auto BufferManager::StartCleanerController() -> Status {
  if (!options_.enable_background_cleaner) return Status::Ok();

  cleaner_controller_ = std::make_unique<BufferManagerCleanerController>(pool_size_, &dirty_page_count_, options_.dirty_page_high_watermark, options_.dirty_page_low_watermark);
  return cleaner_controller_->Start(
    [this]() { SeedCleanerCandidates(); },
    [this](FrameId frame_id) { ScheduleCleanerFlush(frame_id); });
}

bool BufferManager::ShouldQueueCleanerCandidate( const BufferDescriptor &descriptor) const {
  if (!descriptor.is_valid) return false;
  if (descriptor.state != BufferFrameState::kResident) return false;
  if (!descriptor.is_dirty) return false;
  if (descriptor.pin_count != 0) return false;
  if (descriptor.flush_queued) return false;
  if (descriptor.flush_in_flight) return false;
  return true;
}

void BufferManager::MaybeEnqueueCleanerCandidate(FrameId frame_id) {
  if (cleaner_controller_ == nullptr || frame_id >= pool_size_) return;
  cleaner_controller_->EnqueueCandidate(frame_id);
}

bool BufferManager::ShouldSeedCleanerCandidate(FrameId frame_id) const {
  if (frame_id >= pool_size_) return false;

  const BufferDescriptor &descriptor = descriptors_[frame_id];
  std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
  return ShouldQueueCleanerCandidate(descriptor);
}

void BufferManager::SeedCleanerCandidates() {
  if (!options_.enable_background_cleaner) return;

  for (FrameId frame_id = 0; frame_id < pool_size_; ++frame_id) {
    if (!ShouldSeedCleanerCandidate(frame_id)) continue;
    MaybeEnqueueCleanerCandidate(frame_id);
  }
}

}  // namespace telepath
