#include "telepath/buffer/buffer_manager.h"

#include <cstring>

#include "buffer_descriptor_state.h"
#include "buffer_manager_observer.h"
#include "cleaner_controller.h"
#include "flush_scheduler.h"
#include "frame_memory_pool.h"

namespace telepath {

auto BufferManager::FlushFrameWithStableSource(FrameId frame_id, const std::byte *stable_data) -> Status {
  if (frame_id >= pool_size_) return Status::InvalidArgument("invalid frame id");
  return RunForegroundFlush(frame_id, stable_data);
}

auto BufferManager::FlushFrame(FrameId frame_id) -> Status{
  return FlushFrameWithStableSource(frame_id, nullptr);
}

auto BufferManager::RunForegroundFlush(FrameId frame_id, const std::byte *stable_data) -> Status {
  while (true) {
    Result<std::shared_ptr<BufferManagerFlushTask>> schedule_result = TryScheduleFlushTask(frame_id, stable_data, nullptr, false);
    if (!schedule_result.ok()) return schedule_result.status();
    if (schedule_result.value() == nullptr) return Status::Ok();

    Status wait_status = flush_scheduler_->Wait(schedule_result.value());
    if (!wait_status.ok()) return wait_status;
  }
}

auto BufferManager::WaitForScheduledFlushes(const std::vector<std::shared_ptr<BufferManagerFlushTask>> &tasks) -> Status {
  Status first_error = Status::Ok();
  for (const auto &task : tasks) {
    Status status = flush_scheduler_->Wait(task);
    if (!status.ok() && first_error.ok()) first_error = status;
  }
  return first_error;
}

auto BufferManager::FlushBusyFrames(const std::vector<FrameId> &frame_ids) -> Status {
  Status first_error = Status::Ok();
  for (FrameId frame_id : frame_ids) {
    Status status = FlushFrame(frame_id);
    if (!status.ok() && first_error.ok()) first_error = status;
  }
  return first_error;
}

auto BufferManager::TryScheduleFlushTask(FrameId frame_id, const std::byte *stable_data, bool *was_busy, bool cleaner_owned) -> Result<std::shared_ptr<BufferManagerFlushTask>> {
  if (was_busy != nullptr) *was_busy = false;
  if (frame_id >= pool_size_) return Status::InvalidArgument("invalid frame id");

  BufferTag tag;
  uint64_t dirty_generation = 0;

  Result<bool> prepare_result = PrepareFlushTask(frame_id, stable_data, was_busy, cleaner_owned, &tag, &dirty_generation);
  if (!prepare_result.ok()) return prepare_result.status();
  if (!prepare_result.value()) return std::shared_ptr<BufferManagerFlushTask>{};

  auto task = std::make_shared<BufferManagerFlushTask>();
  task->frame_id = frame_id;
  task->tag = tag;
  task->generation = dirty_generation;
  task->clear_dirty_on_success = true;
  task->cleaner_owned = cleaner_owned;
  task->snapshot = CaptureFlushSnapshot(frame_id, stable_data);
  Status enqueue_status = EnqueueFlushTask(task);
  if (!enqueue_status.ok()) return RollBackFlushTask(frame_id, cleaner_owned, enqueue_status);
  return task;
}

auto BufferManager::PrepareFlushTask(FrameId frame_id, const std::byte *stable_data, bool *was_busy, bool cleaner_owned, BufferTag *tag, uint64_t *dirty_generation) -> Result<bool> {
  BufferDescriptor &descriptor = descriptors_[frame_id];
  std::unique_lock<std::mutex> descriptor_guard(descriptor.latch);
  Result<bool> wait_result = WaitForPendingFlush(&descriptor, &descriptor_guard, stable_data, was_busy);
  if (!wait_result.ok()) return wait_result.status();
  if (!wait_result.value()) return false;
  if (!buffer_descriptor_state::CanFlushResidentFrame(descriptor)) return false;
  if (cleaner_owned && buffer_descriptor_state::CleanerMustSkipFlush(descriptor)) {
    if (was_busy != nullptr) *was_busy = true;
    return false;
  }

  buffer_descriptor_state::ReserveFlushSlot(&descriptor, tag, dirty_generation);
  return true;
}

auto BufferManager::CaptureFlushSnapshot(FrameId frame_id, const std::byte *stable_data) -> std::vector<std::byte> {
  std::vector<std::byte> snapshot(page_size_);
  if (stable_data != nullptr) {
    std::memcpy(snapshot.data(), stable_data, page_size_);
    return snapshot;
  }

  const BufferDescriptor &descriptor = descriptors_[frame_id];
  std::shared_lock<std::shared_mutex> content_guard(descriptor.content_latch);
  std::memcpy(snapshot.data(), frame_pool_->GetFrameData(frame_id), page_size_);
  return snapshot;
}

auto BufferManager::EnqueueFlushTask(const std::shared_ptr<BufferManagerFlushTask> &task) -> Status {
  if (task->cleaner_owned && cleaner_controller_ != nullptr) cleaner_controller_->OnFlushScheduled();
  return flush_scheduler_->Enqueue(task);
}

auto BufferManager::RollBackFlushTask(FrameId frame_id, bool cleaner_owned, const Status &status) -> Status {
  if (cleaner_owned && cleaner_controller_ != nullptr) cleaner_controller_->OnFlushFinished();

  BufferDescriptor &descriptor = descriptors_[frame_id];
  std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
  descriptor.flush_queued = false;
  descriptor.last_flush_status = status;
  descriptor.io_cv.notify_all();
  return status;
}

auto BufferManager::WaitForPendingFlush(BufferDescriptor *descriptor, std::unique_lock<std::mutex> *descriptor_guard, const std::byte *stable_data, bool *was_busy) -> Result<bool> {
  if (!buffer_descriptor_state::HasPendingFlush(*descriptor)) return true;
  if (stable_data == nullptr && was_busy != nullptr) {
    *was_busy = true;
    return false;
  }

  descriptor->io_cv.wait(*descriptor_guard, [descriptor]() {
    return !descriptor->flush_queued && !descriptor->flush_in_flight;
  });
  if (!descriptor->last_flush_status.ok()) return descriptor->last_flush_status;
  return true;
}

void BufferManager::FinishFlushCompletion(BufferDescriptor *descriptor, const BufferManagerFlushTask &task, const Status &flush_status, bool *cleared_dirty, bool *should_requeue_cleaner) {
  descriptor->flush_in_flight = false;
  descriptor->last_flush_status = flush_status;
  if (buffer_descriptor_state::CanClearDirtyAfterFlush(*descriptor, task, flush_status)) {
    descriptor->is_dirty = false;
    *cleared_dirty = true;
  }
  *should_requeue_cleaner = ShouldQueueCleanerCandidate(*descriptor);
  descriptor->io_cv.notify_all();
}

void BufferManager::BeginFlushTask(const std::shared_ptr<BufferManagerFlushTask> &task) {
  if (task == nullptr || !task->clear_dirty_on_success) return;

  BufferDescriptor &descriptor = descriptors_[task->frame_id];
  std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
  descriptor.flush_queued = false;
  descriptor.flush_in_flight = true;
  descriptor.io_cv.notify_all();
}

void BufferManager::FinalizeFlushTask( const std::shared_ptr<BufferManagerFlushTask> &task, const Status &flush_status) {
  if (task->clear_dirty_on_success) {
    BufferDescriptor &descriptor = descriptors_[task->frame_id];
    bool cleared_dirty = false;
    bool should_requeue_cleaner = false;
    {
      std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
      FinishFlushCompletion(&descriptor, *task, flush_status, &cleared_dirty, &should_requeue_cleaner);
    }
    if (cleared_dirty) dirty_page_count_.fetch_sub(1, std::memory_order_acq_rel);
    if (should_requeue_cleaner) MaybeEnqueueCleanerCandidate(task->frame_id);
    else if (cleared_dirty) ResetCleanerCandidate(task->frame_id);
  }

  if (task->cleaner_owned && cleaner_controller_ != nullptr) cleaner_controller_->OnFlushFinished();
  if (flush_status.ok()) observer_->RecordSuccessfulFlush(task->tag);
  NotifyCleaner();
}

}  // namespace telepath
