#include "telepath/buffer/buffer_manager.h"

#include <utility>

#include "buffer_manager_observer.h"
#include "cleaner_controller.h"
#include "completion_dispatcher.h"
#include "frame_memory_pool.h"
#include "flush_scheduler.h"
#include "frame_control.h"
#include "miss_coordinator.h"
#include "page_table.h"
#include "telepath/io/disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace telepath {

auto BufferManager::Create(
  const BufferManagerOptions &options,
  std::unique_ptr<DiskBackend> disk_backend,
  std::unique_ptr<Replacer> replacer,
  std::shared_ptr<TelemetrySink> telemetry_sink) -> Result<std::unique_ptr<BufferManager>> {
  auto geometry = BlockGeometry::Create(options.page_size);
  if (!geometry.has_value()) return PropagateError(geometry);
  if (disk_backend != nullptr) {
    const std::size_t backend_block_size = disk_backend->GetCapabilities().block_size;
    if (backend_block_size != 0 && backend_block_size != geometry->block_size()) return InvalidArgument("disk backend block size does not match the buffer manager page size");
  }

  std::unique_ptr<BufferManager> manager(new BufferManager(options, geometry.value(), std::move(disk_backend), std::move(replacer), std::move(telemetry_sink)));
  Status init_status = manager->Initialize();
  if (!init_status.has_value()) return PropagateError(init_status);
  return manager;
}

auto BufferManager::Create(
  std::size_t pool_size, std::size_t page_size,
  std::unique_ptr<DiskBackend> disk_backend,
  std::unique_ptr<Replacer> replacer,
  std::shared_ptr<TelemetrySink> telemetry_sink) -> Result<std::unique_ptr<BufferManager>> {
  return Create(BufferManagerOptions{pool_size, page_size, 0, {}}, std::move(disk_backend), std::move(replacer), std::move(telemetry_sink));
}

BufferManager::BufferManager(
  const BufferManagerOptions &options,
  BlockGeometry geometry,
  std::unique_ptr<DiskBackend> disk_backend,
  std::unique_ptr<Replacer> replacer,
  std::shared_ptr<TelemetrySink> telemetry_sink)
  : pool_size_(options.pool_size),
    geometry_(geometry),
    options_(options),
    disk_backend_(std::move(disk_backend)),
    replacer_(std::move(replacer)),
    observer_(std::make_unique<BufferManagerObserver>(std::move(telemetry_sink))),
    frame_pool_(std::make_unique<FrameMemoryPool>(options.pool_size, geometry)),
    miss_coordinator_(std::make_unique<BufferManagerMissCoordinator>(options.ResolvePageTableStripeCount())),
    completion_dispatcher_(std::make_unique<BufferManagerCompletionDispatcher>(disk_backend_.get())),
    page_table_(std::make_unique<BufferManagerPageTable>(options.pool_size, options.ResolvePageTableStripeCount())),
    descriptors_(options.pool_size) {
  options_.page_table_stripe_count = options.ResolvePageTableStripeCount();
}

BufferManager::~BufferManager() {
  if (cleaner_controller_ != nullptr) cleaner_controller_->Shutdown();
  if (flush_scheduler_ != nullptr) flush_scheduler_->Shutdown();

  if (completion_dispatcher_ != nullptr) completion_dispatcher_->Shutdown(Unavailable("buffer manager is shutting down"));

  // Destroy the backend while frame memory is still alive so any backend-owned
  // worker thread drains and joins before frame_pool_ and descriptors_ are
  // released.
  disk_backend_.reset();
}

auto BufferManager::ReadBuffer(FileId file_id, BlockId block_id) -> Result<SharedBufferHandle> {
  const BufferTag tag{file_id, block_id};
  Result<FrameId> frame = PinPage(tag);
  if (!frame.has_value()) return PropagateError(frame);
  return MakeSharedHandle(frame.value(), tag);
}

auto BufferManager::ReadBufferExclusive(FileId file_id, BlockId block_id) -> Result<ExclusiveBufferHandle> {
  const BufferTag tag{file_id, block_id};
  Result<FrameId> frame = PinPage(tag);
  if (!frame.has_value()) return PropagateError(frame);
  return MakeExclusiveHandle(frame.value(), tag);
}

auto BufferManager::ReleaseBuffer(SharedBufferHandle &&handle) -> Status {
  if (Status usable = CheckUsable(); !usable.has_value()) return usable;
  if (handle.manager_ != this) return InvalidArgument("buffer handle does not belong to this buffer manager");
  handle.Reset();
  return Ok();
}

auto BufferManager::ReleaseBuffer(ExclusiveBufferHandle &&handle) -> Status {
  if (Status usable = CheckUsable(); !usable.has_value()) return usable;
  if (handle.manager_ != this) return InvalidArgument("buffer handle does not belong to this buffer manager");
  handle.Reset();
  return Ok();
}

auto BufferManager::FlushBuffer(const SharedBufferHandle &handle) -> Status {
  if (Status usable = CheckUsable(); !usable.has_value()) return usable;
  if (handle.manager_ != this) return InvalidArgument("buffer handle does not belong to this buffer manager");
  return FlushFrameWithStableSource(handle.frame_id_, handle.data_);
}

auto BufferManager::FlushBuffer(FileId file_id, BlockId block_id) -> Status {
  if (Status usable = CheckUsable(); !usable.has_value()) return usable;
  const BufferTag tag{file_id, block_id};
  std::optional<FrameId> frame_id = page_table_->LookupFrameId(tag);
  if (!frame_id.has_value() || !TryPinResidentFrame(frame_id.value(), tag)) return Ok();
  Status flush_status = FlushFrame(frame_id.value());
  ReleaseFrame(frame_id.value());
  return flush_status;
}

auto BufferManager::FlushBuffer(ExclusiveBufferHandle &handle) -> Status {
  if (Status usable = CheckUsable(); !usable.has_value()) return usable;
  if (handle.manager_ != this) return InvalidArgument("buffer handle does not belong to this buffer manager");
  return FlushFrameOnce(handle.frame_id_, handle.data_);
}

auto BufferManager::FlushAll() -> Status {
  if (Status usable = CheckUsable(); !usable.has_value()) return usable;
  std::vector<std::shared_ptr<BufferManagerFlushTask>> queued_tasks;
  queued_tasks.reserve(pool_size_);
  std::vector<FrameId> busy_frames;
  busy_frames.reserve(pool_size_);

  for (FrameId frame_id = 0; frame_id < pool_size_; ++frame_id) {
    bool was_busy = false;
    Result<std::shared_ptr<BufferManagerFlushTask>> schedule_result = TryScheduleFlushTask(frame_id, nullptr, &was_busy, false);
    if (!schedule_result.has_value()) return PropagateError(schedule_result);
    if (was_busy) {
      busy_frames.push_back(frame_id);
      continue;
    }
    if (schedule_result.value() != nullptr) queued_tasks.push_back(std::move(schedule_result.value()));
  }

  Status wait_status = WaitForScheduledFlushes(queued_tasks);
  Status flush_status = FlushBusyFrames(busy_frames);
  if (!wait_status.has_value()) return wait_status;
  return flush_status;
}

auto BufferManager::ExportSnapshot() const -> BufferPoolSnapshot{
  BufferPoolSnapshot snapshot;
  snapshot.pool_size = pool_size_;
  snapshot.page_size = geometry_.block_size();
  snapshot.dirty_page_count = dirty_page_count_.load(std::memory_order_acquire);
  snapshot.frames.reserve(descriptors_.size());

  for (const BufferDescriptor &descriptor : descriptors_) {
    const std::uint32_t word = descriptor.control.load(std::memory_order_acquire);
    const bool flush_queued = (word & frame_control::kWritebackQueued) != 0;
    const bool flush_in_flight = (word & frame_control::kWritebackActive) != 0;
    if (flush_queued) ++snapshot.flush_queued_count;
    if (flush_in_flight) ++snapshot.flush_in_flight_count;
    snapshot.frames.push_back(FrameSnapshot{
      descriptor.frame_id,
      frame_control::State(word),
      LoadTag(descriptor),
      frame_control::Pins(word),
      descriptor.version.load(std::memory_order_relaxed),
      frame_control::IsResident(word),
      frame_control::IsDirty(word),
      frame_control::State(word) == BufferFrameState::kLoading,
      flush_queued,
      flush_in_flight,
    });
  }

  return snapshot;
}

}  // namespace telepath
