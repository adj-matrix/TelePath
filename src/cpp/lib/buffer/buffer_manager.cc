#include "telepath/buffer/buffer_manager.h"

#include <utility>

#include "buffer_manager_observer.h"
#include "cleaner_controller.h"
#include "completion_dispatcher.h"
#include "frame_memory_pool.h"
#include "flush_scheduler.h"
#include "miss_coordinator.h"
#include "page_table.h"
#include "telepath/io/disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace telepath {

BufferManager::BufferManager(
  const BufferManagerOptions &options,
  std::unique_ptr<DiskBackend> disk_backend,
  std::unique_ptr<Replacer> replacer,
  std::shared_ptr<TelemetrySink> telemetry_sink)
  : pool_size_(options.pool_size),
    page_size_(options.page_size),
    options_(options),
    disk_backend_(std::move(disk_backend)),
    replacer_(std::move(replacer)),
    observer_(std::make_unique<BufferManagerObserver>(std::move(telemetry_sink))),
    frame_pool_(std::make_unique<FrameMemoryPool>(options.pool_size, options.page_size)),
    miss_coordinator_(std::make_unique<BufferManagerMissCoordinator>( options.ResolvePageTableStripeCount())),
    completion_dispatcher_(std::make_unique<BufferManagerCompletionDispatcher>(disk_backend_.get())), page_table_(std::make_unique<BufferManagerPageTable>(options.ResolvePageTableStripeCount())),
    descriptors_(options.pool_size) {
  options_.page_table_stripe_count = options.ResolvePageTableStripeCount();
  Status dependency_status = ValidateRuntimeDependencies();
  if (!dependency_status.ok()) {
    init_status_ = dependency_status;
    return;
  }

  Status frame_status = InitializeFramePool();
  if (!frame_status.ok()) {
    init_status_ = frame_status;
    return;
  }

  SeedFreeFrames();
  ResolveCleanerOptions();
  const DiskBackendCapabilities capabilities = disk_backend_->GetCapabilities();
  options_.flush_worker_count = ResolveFlushWorkerCount(capabilities);
  options_.flush_submit_batch_size = ResolveFlushSubmitBatchSize(capabilities, options_.flush_worker_count);
  options_.flush_foreground_burst_limit = ResolveFlushForegroundBurstLimit();

  Status completion_status = StartCompletionDispatcher();
  if (!completion_status.ok()) {
    init_status_ = completion_status;
    return;
  }

  Status flush_status = StartFlushScheduler();
  if (!flush_status.ok()) {
    StopFlushPipeline(Status::Unavailable("failed to start flush scheduler"));
    init_status_ = flush_status;
    return;
  }

  Status cleaner_status = StartCleanerController();
  if (!cleaner_status.ok()) {
    StopFlushPipeline(Status::Unavailable("failed to start background cleaner"));
    init_status_ = cleaner_status;
    return;
  }
}

BufferManager::BufferManager(
  std::size_t pool_size, std::size_t page_size,
  std::unique_ptr<DiskBackend> disk_backend,
  std::unique_ptr<Replacer> replacer,
  std::shared_ptr<TelemetrySink> telemetry_sink)
  : BufferManager(
    BufferManagerOptions{pool_size, page_size, 0, {}},
    std::move(disk_backend),
    std::move(replacer),
    std::move(telemetry_sink)) {}

BufferManager::~BufferManager() {
  if (cleaner_controller_ != nullptr) cleaner_controller_->Shutdown();
  if (flush_scheduler_ != nullptr) flush_scheduler_->Shutdown();

  if (completion_dispatcher_ != nullptr) completion_dispatcher_->Shutdown(Status::Unavailable("buffer manager is shutting down"));

  // Destroy the backend while frame memory is still alive so any backend-owned
  // worker thread drains and joins before frame_pool_ and descriptors_ are
  // released.
  disk_backend_.reset();
}

auto BufferManager::ReadBuffer(FileId file_id, BlockId block_id) -> Result<BufferHandle> {
  if (!init_status_.ok()) return init_status_;
  const BufferTag tag{file_id, block_id};

  auto resident_hit = TryReadResidentHit(tag);
  if (resident_hit.has_value()) return std::move(resident_hit.value());

  auto registration = miss_coordinator_->RegisterOrJoin(tag);
  if (!registration.is_owner) return AwaitJoinedMiss(tag, registration.state);

  auto handle = TryReadResidentHit(tag);
  if (!handle.has_value()) return LoadMissOwnerBuffer(tag, registration.state);
  miss_coordinator_->Complete(tag, registration.state, Status::Ok(), handle->frame_id());
  return std::move(handle.value());
}

auto BufferManager::ReleaseBuffer(BufferHandle &&handle) -> Status {
  Status validate_status = ValidateOwnedHandle(handle);
  if (!validate_status.ok()) return validate_status;
  handle.Reset();
  return Status::Ok();
}

auto BufferManager::MarkBufferDirty(const BufferHandle &handle) -> Status {
  Status validate_status = ValidateOwnedHandle(handle);
  if (!validate_status.ok()) return validate_status;

  Result<bool> dirty_result = MarkFrameDirty(handle.frame_id(), handle.tag());
  if (!dirty_result.ok()) return dirty_result.status();
  if (dirty_result.value()) {
    dirty_page_count_.fetch_add(1, std::memory_order_acq_rel);
    NotifyCleaner();
  }
  return Status::Ok();
}

auto BufferManager::FlushBuffer(const BufferHandle &handle) -> Status {
  Status validate_status = ValidateOwnedHandle(handle);
  if (!validate_status.ok()) return validate_status;

  if (handle.write_lock_.owns_lock() || handle.read_lock_.owns_lock()) return FlushFrameWithStableSource(handle.frame_id(), handle.data_);
  return FlushFrame(handle.frame_id());
}

auto BufferManager::FlushAll() -> Status {
  if (!init_status_.ok()) return init_status_;
  std::vector<std::shared_ptr<BufferManagerFlushTask>> queued_tasks;
  queued_tasks.reserve(pool_size_);
  std::vector<FrameId> busy_frames;
  busy_frames.reserve(pool_size_);

  for (FrameId frame_id = 0; frame_id < pool_size_; ++frame_id) {
    bool was_busy = false;
    Result<std::shared_ptr<BufferManagerFlushTask>> schedule_result = TryScheduleFlushTask(frame_id, nullptr, &was_busy, false);
    if (!schedule_result.ok()) return schedule_result.status();
    if (was_busy) {
      busy_frames.push_back(frame_id);
      continue;
    }
    if (schedule_result.value() != nullptr) queued_tasks.push_back(std::move(schedule_result.value()));
  }

  Status wait_status = WaitForScheduledFlushes(queued_tasks);
  Status flush_status = FlushBusyFrames(busy_frames);
  if (!wait_status.ok()) return wait_status;
  return flush_status;
}

auto BufferManager::ExportSnapshot() const -> BufferPoolSnapshot{
  BufferPoolSnapshot snapshot;
  snapshot.pool_size = pool_size_;
  snapshot.page_size = page_size_;
  snapshot.dirty_page_count = dirty_page_count_.load(std::memory_order_acquire);
  snapshot.frames.reserve(descriptors_.size());

  for (const BufferDescriptor &descriptor : descriptors_) {
    std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
    if (descriptor.flush_queued) ++snapshot.flush_queued_count;
    if (descriptor.flush_in_flight) ++snapshot.flush_in_flight_count;
    snapshot.frames.push_back(FrameSnapshot{
      descriptor.frame_id,
      descriptor.state,
      descriptor.tag,
      descriptor.pin_count,
      descriptor.dirty_generation,
      descriptor.is_valid,
      descriptor.is_dirty,
      descriptor.io_in_flight,
      descriptor.flush_queued,
      descriptor.flush_in_flight,
    });
  }

  return snapshot;
}

}  // namespace telepath
