#include "telepath/buffer/buffer_manager.h"

#include <chrono>
#include <utility>

#include "buffer_manager_observer.h"
#include "cleaner_controller.h"
#include "frame_control.h"
#include "shared_segment.h"

namespace telepath {

namespace {

// How often the cleaner of a cross-process cache rechecks the shared
// dirty-page count and, in a follower, tries to take over leadership.
constexpr std::chrono::milliseconds kSharedCleanerPollInterval{10};

}  // namespace

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
  if (!schedule_result.has_value()) {
    observer_->RecordCleanerFlushSkipped();
    return;
  }
  if (schedule_result.value() == nullptr) observer_->RecordCleanerFlushSkipped();
}

auto BufferManager::StartCleanerController() -> Status {
  if (!options_.enable_background_cleaner) return Ok();

  BufferManagerCleanerController::LeadCallback lead;
  std::chrono::milliseconds poll_interval{0};
  if (segment_ != nullptr) {
    lead = [this]() { return segment_->TryLeadCleaner(); };
    poll_interval = kSharedCleanerPollInterval;
  }
  cleaner_controller_ = std::make_unique<BufferManagerCleanerController>(pool_size_, dirty_page_count_, options_.dirty_page_high_watermark, options_.dirty_page_low_watermark, std::move(lead), poll_interval);
  return cleaner_controller_->Start(
    [this]() { SeedCleanerCandidates(); },
    [this](FrameId frame_id) { ScheduleCleanerFlush(frame_id); });
}

bool BufferManager::ShouldQueueCleanerCandidate(std::uint32_t control_word) {
  if (!frame_control::IsResident(control_word)) return false;
  if (!frame_control::IsDirty(control_word)) return false;
  if (frame_control::Pins(control_word) != 0) return false;
  if (frame_control::HasWriteback(control_word)) return false;
  return true;
}

void BufferManager::MaybeEnqueueCleanerCandidate(FrameId frame_id) {
  if (cleaner_controller_ == nullptr || frame_id >= pool_size_) return;
  cleaner_controller_->EnqueueCandidate(frame_id);
}

bool BufferManager::ShouldSeedCleanerCandidate(FrameId frame_id) const {
  if (frame_id >= pool_size_) return false;

  return ShouldQueueCleanerCandidate(descriptors_[frame_id].control.load(std::memory_order_acquire));
}

void BufferManager::SeedCleanerCandidates() {
  if (!options_.enable_background_cleaner) return;

  for (FrameId frame_id = 0; frame_id < pool_size_; ++frame_id) {
    if (!ShouldSeedCleanerCandidate(frame_id)) continue;
    MaybeEnqueueCleanerCandidate(frame_id);
  }
}

}  // namespace telepath
