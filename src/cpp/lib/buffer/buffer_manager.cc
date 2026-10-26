#include "telepath/buffer/buffer_manager.h"

#include <algorithm>
#include <utility>

#include "buffer_manager_observer.h"
#include "cleaner_controller.h"
#include "frame_memory_pool.h"
#include "flush_scheduler.h"
#include "frame_control.h"
#include "io_engine.h"
#include "frame_free_list.h"
#include "kernel_region.h"
#include "observation_publisher.h"
#include "page_table.h"
#include "shared_segment.h"
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

auto BufferManager::RemoveSharedMemory(const std::string &name) -> Status { return SharedSegment::Remove(name); }

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
    io_engine_(std::make_unique<IoEngine>(disk_backend_.get())),
    page_table_(nullptr) {
  options_.page_table_stripe_count = options.ResolvePageTableStripeCount();
}

BufferManager::~BufferManager() {
  observation_.reset();
  if (cleaner_controller_ != nullptr) cleaner_controller_->Shutdown();
  if (flush_scheduler_ != nullptr) flush_scheduler_->Shutdown();

  if (io_engine_ != nullptr) io_engine_->Shutdown(Unavailable("buffer manager is shutting down"));

  // Destroy the backend while frame memory is still alive so any backend-owned
  // worker thread drains and joins before frame_pool_ and descriptors_ are
  // released.
  disk_backend_.reset();
}

auto BufferManager::ReadBuffer(FileId file_id, BlockId block_id, BufferAccessStrategy *strategy) -> Result<SharedBufferHandle> {
  const BufferTag tag{file_id, block_id};
  Result<FrameId> frame = PinPage(tag, strategy);
  if (!frame.has_value()) return PropagateError(frame);
  return MakeSharedHandle(frame.value(), tag);
}

auto BufferManager::ReadBufferExclusive(FileId file_id, BlockId block_id, BufferAccessStrategy *strategy, ReadBufferMode mode) -> Result<ExclusiveBufferHandle> {
  const BufferTag tag{file_id, block_id};
  Result<FrameId> frame = PinPage(tag, strategy, mode);
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
  if (!frame_id.has_value() || !TryPinResidentFrame(frame_id.value(), tag, false)) return Ok();
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
  if (Status written = WriteBackFrames([](FrameId) { return true; }); !written.has_value()) return written;
  return SyncWrittenFiles();
}

auto BufferManager::FlushFileBuffers(FileId file_id) -> Status {
  if (Status usable = CheckUsable(); !usable.has_value()) return usable;
  return WriteBackFrames([this, file_id](FrameId frame_id) { return LoadTag(descriptors_[frame_id]).file_id == file_id; });
}

auto BufferManager::SyncFile(FileId file_id) -> Status {
  if (Status usable = CheckUsable(); !usable.has_value()) return usable;
  {
    std::lock_guard<std::mutex> guard(written_files_latch_);
    std::erase(written_files_, file_id);
  }
  if (segment_ != nullptr) {
    SharedWrittenFiles &shared = segment_->written_files();
    std::lock_guard<sync::FutexMutex> shared_guard(shared.latch);
    const auto end = std::remove(shared.files.begin(), shared.files.begin() + shared.count, file_id);
    shared.count = static_cast<std::uint32_t>(end - shared.files.begin());
  }
  return SyncFiles({file_id});
}

auto BufferManager::WriteBackFrames(const std::function<bool(FrameId)> &selected) -> Status {
  std::vector<std::shared_ptr<BufferManagerFlushTask>> queued_tasks;
  queued_tasks.reserve(pool_size_);
  std::vector<FrameId> busy_frames;
  busy_frames.reserve(pool_size_);

  for (FrameId frame_id = 0; frame_id < pool_size_; ++frame_id) {
    if (!selected(frame_id)) continue;
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

auto BufferManager::MinRecLsn() const -> std::optional<Lsn> {
  Lsn oldest = kInvalidLsn;
  for (const BufferDescriptor &descriptor : descriptors_) {
    const Lsn rec_lsn = FrameRecLsn(descriptor);
    if (rec_lsn != kInvalidLsn && (oldest == kInvalidLsn || rec_lsn < oldest)) oldest = rec_lsn;
  }
  if (oldest == kInvalidLsn) return std::nullopt;
  return oldest;
}

auto BufferManager::CheckpointBuffers(Lsn lsn) -> Status {
  if (Status usable = CheckUsable(); !usable.has_value()) return usable;
  const auto before = [this, lsn](FrameId frame_id) {
    const Lsn rec_lsn = FrameRecLsn(descriptors_[frame_id]);
    return rec_lsn != kInvalidLsn && rec_lsn < lsn;
  };
  if (Status written = WriteBackFrames(before); !written.has_value()) return written;
  for (FrameId frame_id = 0; frame_id < pool_size_; ++frame_id) {
    while (before(frame_id)) {
      Result<bool> wrote = ForegroundFlushRound(frame_id, nullptr);
      if (!wrote.has_value()) return PropagateError(wrote);
      if (!wrote.value()) break;
    }
  }
  return SyncWrittenFiles();
}

auto BufferManager::FrameRecLsn(const BufferDescriptor &descriptor) -> Lsn {
  const Lsn pending = descriptor.rec_lsn.load(std::memory_order_acquire);
  const Lsn writing = descriptor.writeback_rec_lsn.load(std::memory_order_acquire);
  if (pending == kInvalidLsn) return writing;
  if (writing == kInvalidLsn) return pending;
  return std::min(pending, writing);
}

auto BufferManager::Close() -> Status {
  if (closed_.load(std::memory_order_acquire)) return Ok();
  // Prefetches still in flight pin their frames only until they complete.
  for (std::uint32_t pending = prefetches_in_flight_.load(std::memory_order_acquire); pending != 0; pending = prefetches_in_flight_.load(std::memory_order_acquire)) {
    sync::FutexWait(prefetches_in_flight_, pending);
  }
  for (FrameId frame_id = 0; frame_id < pool_size_; ++frame_id) {
    if (IsLoadingTag(frame_id, LoadTag(descriptors_[frame_id]))) WaitForLoad(frame_id, LoadTag(descriptors_[frame_id]));
  }
  if (segment_ == nullptr && HasPinnedFrame()) return InvalidArgument("pages are still pinned by handles or read operations");
  Status status = CheckUsable();
  if (status.has_value()) status = FlushAll();
  if (observation_ != nullptr) {
    observation_->Publish();
    observation_->Stop();
  }
  closed_.store(true, std::memory_order_release);
  StopFlushPipeline(Unavailable("buffer manager is closed"));
  return status;
}

auto BufferManager::ExportSnapshot() const -> BufferPoolSnapshot{
  BufferPoolSnapshot snapshot;
  snapshot.pool_size = pool_size_;
  snapshot.page_size = geometry_.block_size();
  snapshot.dirty_page_count = dirty_page_count_->load(std::memory_order_acquire);
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
      FrameRecLsn(descriptor),
    });
  }

  return snapshot;
}

auto BufferManager::PublishObservation() -> Status {
  if (observation_ == nullptr) return Unavailable("observation is disabled");
  observation_->Publish();
  return Ok();
}

auto BufferManager::StartObservation() -> Status {
  if (!options_.observation.enabled) return Ok();
  Result<std::unique_ptr<ObservationPublisher>> publisher = ObservationPublisher::Open(options_.observation, pool_size_, geometry_.block_size(), segment_ != nullptr);
  if (!publisher.has_value()) return PropagateError(publisher);
  observation_ = std::move(publisher.value());
  observation_->Start(ObservationPublisher::Sources{
    [this](observation::ObservationSummary *summary, Lsn *min_rec_lsn, std::span<observation::ObservationFrameRecord> frames) {
      *summary = {};
      for (std::size_t index = 0; index < descriptors_.size(); ++index) {
        const BufferDescriptor &descriptor = descriptors_[index];
        const std::uint32_t word = descriptor.control.load(std::memory_order_acquire);
        const BufferTag tag = LoadTag(descriptor);
        observation::ObservationFrameRecord &record = frames[index];
        record.frame_id = static_cast<std::uint32_t>(index);
        record.state = static_cast<std::uint8_t>(frame_control::State(word));
        record.flags = 0;
        if (frame_control::IsDirty(word)) record.flags |= observation::kFrameDirty;
        if ((word & frame_control::kWritebackQueued) != 0) record.flags |= observation::kFrameWritebackQueued;
        if ((word & frame_control::kWritebackActive) != 0) record.flags |= observation::kFrameWritebackActive;
        record.pins = static_cast<std::uint16_t>(frame_control::Pins(word));
        record.file_id = tag.file_id;
        record.namespace_id = 0;
        record.block_id = tag.block_id;
        record.version = descriptor.version.load(std::memory_order_relaxed);
        switch (frame_control::State(word)) {
          case BufferFrameState::kFree: ++summary->free_frames; break;
          case BufferFrameState::kLoading: ++summary->loading_frames; break;
          case BufferFrameState::kResident: ++summary->resident_frames; break;
        }
        if ((record.flags & observation::kFrameDirty) != 0) ++summary->dirty_frames;
        if (record.pins != 0) ++summary->pinned_frames;
        if ((record.flags & observation::kFrameWritebackQueued) != 0) ++summary->writeback_queued_frames;
        if ((record.flags & observation::kFrameWritebackActive) != 0) ++summary->writeback_active_frames;
      }
      summary->dirty_page_count = dirty_page_count_->load(std::memory_order_relaxed);
      *min_rec_lsn = MinRecLsn().value_or(kInvalidLsn);
    },
    [this] { return observer_->Snapshot(); },
  });
  return Ok();
}

}  // namespace telepath
