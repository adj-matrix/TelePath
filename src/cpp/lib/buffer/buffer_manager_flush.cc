#include "telepath/buffer/buffer_manager.h"

#include <cstring>
#include <span>

#include "buffer_manager_observer.h"
#include "cleaner_controller.h"
#include "flush_scheduler.h"
#include "frame_control.h"
#include "frame_memory_pool.h"
#include "io_engine.h"

namespace telepath {

auto BufferManager::FlushFrameWithStableSource(FrameId frame_id, const std::byte *stable_data) -> Status {
  if (frame_id >= pool_size_) return InvalidArgument("invalid frame id");
  return RunForegroundFlush(frame_id, stable_data);
}

auto BufferManager::FlushFrame(FrameId frame_id) -> Status{
  return FlushFrameWithStableSource(frame_id, nullptr);
}

auto BufferManager::RunForegroundFlush(FrameId frame_id, const std::byte *stable_data) -> Status {
  while (true) {
    Result<bool> wrote = ForegroundFlushRound(frame_id, stable_data);
    if (!wrote.has_value()) return PropagateError(wrote);
    if (!wrote.value()) return Ok();
  }
}

auto BufferManager::FlushFrameOnce(FrameId frame_id, const std::byte *stable_data) -> Status {
  Result<bool> wrote = ForegroundFlushRound(frame_id, stable_data);
  if (!wrote.has_value()) return PropagateError(wrote);
  return Ok();
}

auto BufferManager::ForegroundFlushRound(FrameId frame_id, const std::byte *stable_data) -> Result<bool> {
  const bool queued = options_.foreground_writeback == ForegroundWriteback::kQueued;
  Result<std::shared_ptr<BufferManagerFlushTask>> task = queued
    ? TryScheduleFlushTask(frame_id, stable_data, nullptr, false)
    : CreateFlushTask(frame_id, stable_data, nullptr, false);
  if (!task.has_value()) return PropagateError(task);
  if (task.value() == nullptr) return false;

  Status written = queued ? flush_scheduler_->Wait(task.value()) : WriteFlushTaskDirect(task.value());
  if (!written.has_value()) return PropagateError(written);
  return true;
}

auto BufferManager::WriteFlushTaskDirect(const std::shared_ptr<BufferManagerFlushTask> &task) -> Status {
  observer_->RecordFlushTaskScheduled(task->tag);
  BeginFlushTask(task);
  IoWaiter waiter;
  DiskRequest request;
  request.token = reinterpret_cast<uint64_t>(static_cast<IoCompletion *>(&waiter));
  request.operation = DiskOperation::kWrite;
  request.tag = task->tag;
  request.buffers[0] = task->snapshot.data();
  io_engine_->Submit(std::span<const DiskRequest>(&request, 1), DiskSubmitMode::kWait);
  Status status = io_engine_->Wait(waiter);
  FinalizeFlushTask(task, status);
  return status;
}

auto BufferManager::WaitForScheduledFlushes(const std::vector<std::shared_ptr<BufferManagerFlushTask>> &tasks) -> Status {
  Status first_error = Ok();
  for (const auto &task : tasks) {
    Status status = flush_scheduler_->Wait(task);
    if (!status.has_value() && first_error.has_value()) first_error = status;
  }
  return first_error;
}

auto BufferManager::FlushBusyFrames(const std::vector<FrameId> &frame_ids) -> Status {
  Status first_error = Ok();
  for (FrameId frame_id : frame_ids) {
    Status status = FlushFrame(frame_id);
    if (!status.has_value() && first_error.has_value()) first_error = status;
  }
  return first_error;
}

auto BufferManager::TryScheduleFlushTask(FrameId frame_id, const std::byte *stable_data, bool *was_busy, bool cleaner_owned) -> Result<std::shared_ptr<BufferManagerFlushTask>> {
  Result<std::shared_ptr<BufferManagerFlushTask>> task = CreateFlushTask(frame_id, stable_data, was_busy, cleaner_owned);
  if (!task.has_value() || task.value() == nullptr) return task;

  Status enqueue_status = EnqueueFlushTask(task.value());
  if (!enqueue_status.has_value()) return PropagateError(RollBackFlushTask(frame_id, cleaner_owned, enqueue_status));
  observer_->RecordFlushTaskScheduled(task.value()->tag);
  if (cleaner_owned) observer_->RecordCleanerFlushScheduled(task.value()->tag);
  return task;
}

auto BufferManager::CreateFlushTask(FrameId frame_id, const std::byte *stable_data, bool *was_busy, bool cleaner_owned) -> Result<std::shared_ptr<BufferManagerFlushTask>> {
  if (was_busy != nullptr) *was_busy = false;
  if (frame_id >= pool_size_) return InvalidArgument("invalid frame id");

  BufferTag tag;
  std::vector<std::byte> snapshot;

  Result<bool> prepare_result = PrepareFlushTask(frame_id, stable_data, was_busy, cleaner_owned, &tag, &snapshot);
  if (!prepare_result.has_value()) return PropagateError(prepare_result);
  if (!prepare_result.value()) return std::shared_ptr<BufferManagerFlushTask>{};

  auto task = std::make_shared<BufferManagerFlushTask>();
  task->frame_id = frame_id;
  task->tag = tag;
  task->clear_dirty_on_success = true;
  task->cleaner_owned = cleaner_owned;
  task->snapshot = std::move(snapshot);
  if (options_.pre_write_hook) {
    Status hook_status = options_.pre_write_hook(task->tag, std::span<const std::byte>(task->snapshot));
    if (!hook_status.has_value()) return PropagateError(RollBackFlushTask(frame_id, cleaner_owned, hook_status));
  }
  return task;
}

auto BufferManager::PrepareFlushTask(FrameId frame_id, const std::byte *stable_data, bool *was_busy, bool cleaner_owned, BufferTag *tag, std::vector<std::byte> *snapshot) -> Result<bool> {
  BufferDescriptor &descriptor = descriptors_[frame_id];
  const bool opportunistic = was_busy != nullptr;
  bool shared_latch_held = false;
  if (stable_data == nullptr) {
    if (!opportunistic) {
      frame_control::AcquireShared(descriptor.control);
    } else if (!frame_control::TryAcquireShared(descriptor.control)) {
      *was_busy = true;
      return false;
    }
    shared_latch_held = true;
  }

  Result<bool> result = false;
  while (true) {
    const frame_control::QueueOutcome outcome = frame_control::TryQueueWriteback(descriptor.control, cleaner_owned);
    if (outcome == frame_control::QueueOutcome::kQueued) {
      descriptor.last_writeback_error.store(0, std::memory_order_relaxed);
      *tag = LoadTag(descriptor);
      *snapshot = CaptureFlushSnapshot(frame_id, stable_data);
      result = true;
      break;
    }
    if (outcome == frame_control::QueueOutcome::kNotDirty) break;
    if (outcome == frame_control::QueueOutcome::kPinned || (stable_data == nullptr && opportunistic)) {
      *was_busy = true;
      break;
    }
    Status wait_status = WaitForPendingFlush(&descriptor);
    if (!wait_status.has_value()) {
      result = PropagateError(wait_status);
      break;
    }
  }

  if (shared_latch_held) frame_control::ReleaseShared(descriptor.control);
  return result;
}

auto BufferManager::CaptureFlushSnapshot(FrameId frame_id, const std::byte *stable_data) -> std::vector<std::byte> {
  std::vector<std::byte> snapshot(geometry_.block_size());
  const std::byte *source = stable_data != nullptr ? stable_data : frame_pool_->GetFrameData(frame_id);
  std::memcpy(snapshot.data(), source, geometry_.block_size());
  return snapshot;
}

auto BufferManager::EnqueueFlushTask(const std::shared_ptr<BufferManagerFlushTask> &task) -> Status {
  if (task->cleaner_owned && cleaner_controller_ != nullptr) cleaner_controller_->OnFlushScheduled();
  return flush_scheduler_->Enqueue(task);
}

auto BufferManager::RollBackFlushTask(FrameId frame_id, bool cleaner_owned, const Status &status) -> Status {
  if (cleaner_owned && cleaner_controller_ != nullptr) cleaner_controller_->OnFlushFinished();

  BufferDescriptor &descriptor = descriptors_[frame_id];
  descriptor.last_writeback_error.store(frame_control::PackWritebackError(status), std::memory_order_relaxed);
  frame_control::CancelQueuedWriteback(descriptor.control);
  observer_->RecordFlushTaskCompletion(LoadTag(descriptor), status);
  return status;
}

auto BufferManager::WaitForPendingFlush(BufferDescriptor *descriptor) -> Status {
  frame_control::WaitWhile(descriptor->control, [](std::uint32_t word) { return frame_control::HasWriteback(word); });
  return frame_control::UnpackWritebackError(descriptor->last_writeback_error.load(std::memory_order_relaxed));
}

void BufferManager::BeginFlushTask(const std::shared_ptr<BufferManagerFlushTask> &task) {
  if (task == nullptr || !task->clear_dirty_on_success) return;
  frame_control::BeginWriteback(descriptors_[task->frame_id].control);
}

void BufferManager::FinalizeFlushTask(const std::shared_ptr<BufferManagerFlushTask> &task, const Status &flush_status) {
  observer_->RecordFlushTaskCompletion(task->tag, flush_status);
  if (task->clear_dirty_on_success) {
    BufferDescriptor &descriptor = descriptors_[task->frame_id];
    descriptor.last_writeback_error.store(frame_control::PackWritebackError(flush_status), std::memory_order_relaxed);
    const frame_control::WritebackCompletion completion = frame_control::CompleteWriteback(descriptor.control, flush_status.has_value());
    if (completion.cleared_dirty) dirty_page_count_.fetch_sub(1, std::memory_order_acq_rel);
    if (ShouldQueueCleanerCandidate(completion.word)) MaybeEnqueueCleanerCandidate(task->frame_id);
    else if (completion.cleared_dirty) ResetCleanerCandidate(task->frame_id);
  }

  if (task->cleaner_owned && cleaner_controller_ != nullptr) {
    cleaner_controller_->OnFlushFinished();
    observer_->RecordCleanerFlushFinished(task->tag);
  }
  if (flush_status.has_value()) {
    NoteWrittenFile(task->tag.file_id);
    observer_->RecordSuccessfulFlush(task->tag);
  }
  NotifyCleaner();
}

}  // namespace telepath
