#include "telepath/buffer/buffer_manager.h"

#include <algorithm>
#include <typeinfo>

#include "cleaner_controller.h"
#include "frame_memory_pool.h"
#include "io_engine.h"
#include "flush_scheduler.h"
#include "frame_free_list.h"
#include "kernel_region.h"
#include "page_table.h"
#include "shared_segment.h"
#include "telepath/io/disk_backend.h"
#include "telepath/replacer/replacer.h"

namespace telepath {

auto BufferManager::Initialize() -> Status {
  if (Status dependency_status = ValidateRuntimeDependencies(); !dependency_status.has_value()) return dependency_status;
  if (Status memory_status = InitializeKernelMemory(); !memory_status.has_value()) return memory_status;

  if (segment_ == nullptr || segment_->created()) SeedFreeFrames();
  if (segment_ != nullptr && segment_->created()) segment_->MarkReady();
  ResolveCleanerOptions();
  const DiskBackendCapabilities capabilities = disk_backend_->GetCapabilities();
  options_.flush_worker_count = ResolveFlushWorkerCount(capabilities);
  options_.flush_submit_batch_size = ResolveFlushSubmitBatchSize(capabilities, options_.flush_worker_count);
  options_.flush_foreground_burst_limit = ResolveFlushForegroundBurstLimit();

  if (Status io_status = StartIoEngine(); !io_status.has_value()) return io_status;

  if (Status flush_status = StartFlushScheduler(); !flush_status.has_value()) {
    StopFlushPipeline(Unavailable("failed to start flush scheduler"));
    return flush_status;
  }

  if (Status cleaner_status = StartCleanerController(); !cleaner_status.has_value()) {
    StopFlushPipeline(Unavailable("failed to start background cleaner"));
    return cleaner_status;
  }

  if (Status observation_status = StartObservation(); !observation_status.has_value()) {
    StopFlushPipeline(Unavailable("failed to start the observation plane"));
    return observation_status;
  }
  return Ok();
}

auto BufferManager::CheckUsable() const -> Status {
  if (failed_.load(std::memory_order_acquire)) return Failed("buffer manager is in the failed state");
  if (closed_.load(std::memory_order_acquire)) return Unavailable("buffer manager is closed");
  return Ok();
}

void BufferManager::EnterFailedState(const Error &cause) {
  std::lock_guard<std::mutex> failure_guard(failure_latch_);
  if (failure_.has_value()) return;
  failure_ = cause;
  failed_.store(true, std::memory_order_release);
}

auto BufferManager::failure() const -> std::optional<Error> {
  if (!failed_.load(std::memory_order_acquire)) return std::nullopt;
  std::lock_guard<std::mutex> failure_guard(failure_latch_);
  return failure_;
}

auto BufferManager::InitializeKernelMemory() -> Status {
  if (Status valid = FrameMemoryPool::Validate(pool_size_, geometry_); !valid.has_value()) return valid;
  const KernelLayout layout = KernelLayout::For(pool_size_, options_.page_table_stripe_count, geometry_);
  std::byte *base = nullptr;
  bool initialize = true;
  if (options_.shared_memory.name.empty()) {
    region_ = std::make_unique<KernelRegion>(layout.bytes);
    if (region_->base() == nullptr) return ResourceExhausted("failed to allocate kernel memory");
    base = region_->base();
  } else {
    const std::size_t replacer_bytes = replacer_->SharedStateBytes();
    if (replacer_bytes == 0) return InvalidArgument("replacer does not support cross-process mode");
    const Replacer &replacer = *replacer_;
    const SharedSegment::Shape shape{geometry_.block_size(), pool_size_, options_.page_table_stripe_count, replacer_bytes, typeid(replacer).name(), layout.bytes};
    Result<std::unique_ptr<SharedSegment>> segment = SharedSegment::Open(options_.shared_memory, shape);
    if (!segment.has_value()) return PropagateError(segment);
    segment_ = std::move(segment.value());
    initialize = segment_->created();
    if (Status bound = replacer_->BindSharedState(segment_->replacer_state(), initialize); !bound.has_value()) return bound;
    base = segment_->kernel_base();
  }

  KernelShared *shared = initialize ? ConstructState<KernelShared>(base, layout.shared, 1) : StateAt<KernelShared>(base, layout.shared);
  dirty_page_count_ = &shared->dirty_page_count;
  BufferDescriptor *descriptors = initialize ? ConstructState<BufferDescriptor>(base, layout.descriptors, pool_size_) : StateAt<BufferDescriptor>(base, layout.descriptors);
  descriptors_ = std::span<BufferDescriptor>(descriptors, pool_size_);
  page_table_ = std::make_unique<BufferManagerPageTable>(pool_size_, options_.page_table_stripe_count, base, layout.page_table, initialize);
  free_list_ = std::make_unique<FrameFreeList>(pool_size_, base, layout.free_list, initialize);
  frame_pool_ = std::make_unique<FrameMemoryPool>(geometry_, base + layout.frames);
  return Ok();
}

void BufferManager::SeedFreeFrames() {
  for (FrameId frame_id = 0; frame_id < pool_size_; ++frame_id) {
    descriptors_[frame_id].frame_id = frame_id;
    free_list_->Push(frame_id);
  }
}

auto BufferManager::ValidateRuntimeDependencies() const -> Status {
  if (disk_backend_ == nullptr) return InvalidArgument("disk backend must not be null");
  if (replacer_ == nullptr) return InvalidArgument("replacer must not be null");
  if (io_engine_ == nullptr) return InvalidArgument("io engine must not be null");
  return Ok();
}

void BufferManager::ResolveCleanerOptions() {
  if (!options_.enable_background_cleaner) return;

  options_.dirty_page_high_watermark = options_.ResolveDirtyPageHighWatermark();
  options_.dirty_page_low_watermark = options_.ResolveDirtyPageLowWatermark();
  if (options_.dirty_page_high_watermark == 0) options_.enable_background_cleaner = false;
}

auto BufferManager::ResolveFlushWorkerCount( const DiskBackendCapabilities &capabilities) const -> std::size_t {
  const std::size_t max_worker_count = std::max<std::size_t>(1, pool_size_);
  std::size_t derived_worker_count = capabilities.queue_depth;
  if (derived_worker_count == 0) derived_worker_count = 1;
  if (derived_worker_count > 4) derived_worker_count = 4;
  if (derived_worker_count > max_worker_count) derived_worker_count = max_worker_count;
  if (options_.flush_worker_count == 0) return derived_worker_count;

  return std::clamp(options_.flush_worker_count, std::size_t{1}, max_worker_count);
}

auto BufferManager::ResolveFlushSubmitBatchSize(const DiskBackendCapabilities &capabilities, std::size_t flush_worker_count) const -> std::size_t {
  const std::size_t max_submit_batch_size = std::max<std::size_t>(1, capabilities.queue_depth);
  const std::size_t derived_submit_batch_size = std::max<std::size_t>(1, capabilities.queue_depth / flush_worker_count);
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
  flush_scheduler_ = std::make_unique<BufferManagerFlushScheduler>(io_engine_.get(), options_.flush_worker_count, options_.flush_submit_batch_size, options_.flush_foreground_burst_limit);
  return flush_scheduler_->Start(
    [this](const std::shared_ptr<BufferManagerFlushTask> &task) { BeginFlushTask(task); },
    [this](const std::shared_ptr<BufferManagerFlushTask> &task, const Status &status) { FinalizeFlushTask(task, status); });
}

void BufferManager::StopFlushPipeline(const Status &reason) {
  if (cleaner_controller_ != nullptr) cleaner_controller_->Shutdown();
  if (flush_scheduler_ != nullptr) flush_scheduler_->Shutdown();
  if (io_engine_ != nullptr) io_engine_->Shutdown(reason);
}

void BufferManager::NoteWrittenFile(FileId file_id) {
  if (segment_ != nullptr) {
    SharedWrittenFiles &shared = segment_->written_files();
    std::lock_guard<sync::FutexMutex> shared_guard(shared.latch);
    const auto end = shared.files.begin() + shared.count;
    if (std::find(shared.files.begin(), end, file_id) != end) return;
    if (shared.count < SharedWrittenFiles::kCapacity) {
      shared.files[shared.count++] = file_id;
      return;
    }
  }
  std::lock_guard<std::mutex> guard(written_files_latch_);
  if (std::find(written_files_.begin(), written_files_.end(), file_id) == written_files_.end()) written_files_.push_back(file_id);
}

auto BufferManager::SyncWrittenFiles() -> Status {
  std::vector<FileId> files;
  {
    std::lock_guard<std::mutex> guard(written_files_latch_);
    files.swap(written_files_);
  }
  if (segment_ != nullptr) {
    SharedWrittenFiles &shared = segment_->written_files();
    std::lock_guard<sync::FutexMutex> shared_guard(shared.latch);
    for (std::uint32_t index = 0; index < shared.count; ++index) {
      if (std::find(files.begin(), files.end(), shared.files[index]) == files.end()) files.push_back(shared.files[index]);
    }
    shared.count = 0;
  }
  return SyncFiles(std::move(files));
}

auto BufferManager::SyncFiles(std::vector<FileId> files) -> Status {
  if (files.empty()) return Ok();

  IoWaiter waiter(static_cast<std::uint32_t>(files.size()));
  std::vector<DiskRequest> requests(files.size());
  for (std::size_t i = 0; i < files.size(); ++i) {
    requests[i].token = reinterpret_cast<uint64_t>(static_cast<IoCompletion *>(&waiter));
    requests[i].operation = DiskOperation::kSync;
    requests[i].tag = BufferTag{files[i], 0};
  }
  io_engine_->Submit(requests, DiskSubmitMode::kWait);
  Status status = io_engine_->Wait(waiter);
  if (!status.has_value()) {
    for (FileId file_id : files) NoteWrittenFile(file_id);
  }
  return status;
}

auto BufferManager::StartIoEngine() -> Status {
  return io_engine_->Start([this](const Error &cause) { EnterFailedState(cause); });
}

}  // namespace telepath
