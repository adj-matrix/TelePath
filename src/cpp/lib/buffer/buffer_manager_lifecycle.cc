#include "telepath/buffer/buffer_manager.h"

#include <algorithm>

#include "cleaner_controller.h"
#include "completion_dispatcher.h"
#include "frame_memory_pool.h"
#include "flush_scheduler.h"
#include "miss_coordinator.h"
#include "page_table.h"
#include "telepath/io/disk_backend.h"
#include "telepath/replacer/replacer.h"

namespace telepath {

auto BufferManager::InitializeFramePool() -> Status {
  if (frame_pool_ == nullptr) return Status::InvalidArgument("frame pool must not be null");
  return frame_pool_->Initialize();
}

void BufferManager::SeedFreeFrames() {
  free_list_.reserve(pool_size_);
  for (FrameId frame_id = 0; frame_id < pool_size_; ++frame_id) {
    descriptors_[frame_id].frame_id = frame_id;
    free_list_.push_back(frame_id);
  }
}

auto BufferManager::ValidateRuntimeDependencies() const -> Status {
  if (disk_backend_ == nullptr) return Status::InvalidArgument("disk backend must not be null");
  if (replacer_ == nullptr) return Status::InvalidArgument("replacer must not be null");
  if (miss_coordinator_ == nullptr) return Status::InvalidArgument("miss coordinator must not be null");
  if (completion_dispatcher_ == nullptr) return Status::InvalidArgument("completion dispatcher must not be null");
  if (page_table_ == nullptr) return Status::InvalidArgument("page table must not be null");
  return Status::Ok();
}

void BufferManager::ResolveCleanerOptions() {
  if (!options_.enable_background_cleaner) return;

  options_.dirty_page_high_watermark = options_.ResolveDirtyPageHighWatermark();
  options_.dirty_page_low_watermark = options_.ResolveDirtyPageLowWatermark();
  if (options_.dirty_page_high_watermark == 0) options_.enable_background_cleaner = false;
}

auto BufferManager::ResolveFlushWorkerCount( const DiskBackendCapabilities &capabilities) const -> std::size_t {
  const std::size_t max_worker_count = std::max<std::size_t>(1, pool_size_);
  std::size_t derived_worker_count = capabilities.recommended_queue_depth;
  if (derived_worker_count == 0) derived_worker_count = 1;
  if (derived_worker_count > 4) derived_worker_count = 4;
  if (derived_worker_count > max_worker_count) derived_worker_count = max_worker_count;
  if (options_.flush_worker_count == 0) return derived_worker_count;

  return std::clamp(options_.flush_worker_count, std::size_t{1}, max_worker_count);
}

auto BufferManager::ResolveFlushSubmitBatchSize(const DiskBackendCapabilities &capabilities, std::size_t flush_worker_count) const -> std::size_t {
  if (!capabilities.supports_submit_batching) return 1;

  const std::size_t max_submit_batch_size = std::max<std::size_t>(1, capabilities.recommended_queue_depth);
  const std::size_t derived_submit_batch_size = std::max<std::size_t>(1, capabilities.recommended_queue_depth / flush_worker_count);
  if (options_.flush_submit_batch_size == 0) return derived_submit_batch_size;

  return std::clamp(options_.flush_submit_batch_size, std::size_t{1}, max_submit_batch_size);
}

auto BufferManager::ResolveFlushForegroundBurstLimit() const -> std::size_t {
  std::size_t derived_burst_limit = options_.flush_submit_batch_size * 4;
  if (derived_burst_limit < 4) derived_burst_limit = 4;
  if (options_.flush_foreground_burst_limit == 0) return derived_burst_limit;
  return std::max<std::size_t>(std::size_t{1}, options_.flush_foreground_burst_limit);
}

auto BufferManager::StartFlushScheduler() -> Status {
  flush_scheduler_ = std::make_unique<BufferManagerFlushScheduler>(disk_backend_.get(), completion_dispatcher_.get(), page_size_, options_.flush_worker_count, options_.flush_submit_batch_size, options_.flush_foreground_burst_limit);
  return flush_scheduler_->Start(
    [this](const std::shared_ptr<BufferManagerFlushTask> &task) { BeginFlushTask(task); },
    [this](const std::shared_ptr<BufferManagerFlushTask> &task, const Status &status) { FinalizeFlushTask(task, status); });
}

void BufferManager::StopFlushPipeline(const Status &reason) {
  if (cleaner_controller_ != nullptr) cleaner_controller_->Shutdown();
  if (flush_scheduler_ != nullptr) flush_scheduler_->Shutdown();
  if (completion_dispatcher_ != nullptr) completion_dispatcher_->Shutdown(reason);
}

auto BufferManager::StartCompletionDispatcher() -> Status{
  if (completion_dispatcher_ == nullptr) return Status::InvalidArgument("completion dispatcher must not be null");
  return completion_dispatcher_->Start();
}

}  // namespace telepath
