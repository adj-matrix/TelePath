#include "telepath/buffer/buffer_manager.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "buffer_descriptor_state.h"
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

namespace {

}  // namespace

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
  ResolveFlushOptions();

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

void BufferManager::ResolveFlushOptions() {
  const DiskBackendCapabilities capabilities = disk_backend_->GetCapabilities();
  options_.flush_worker_count = ResolveFlushWorkerCount(capabilities);
  options_.flush_submit_batch_size = ResolveFlushSubmitBatchSize(capabilities, options_.flush_worker_count);
  options_.flush_foreground_burst_limit = ResolveFlushForegroundBurstLimit();
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

void BufferManager::ScheduleCleanerFlush(FrameId frame_id) {
  bool was_busy = false;
  Result<std::shared_ptr<BufferManagerFlushTask>> schedule_result = TryScheduleFlushTask(frame_id, nullptr, &was_busy, true);
  if (!schedule_result.ok()) return;
}

auto BufferManager::StartFlushScheduler() -> Status {
  flush_scheduler_ = std::make_unique<BufferManagerFlushScheduler>(disk_backend_.get(), completion_dispatcher_.get(), page_size_, options_.flush_worker_count, options_.flush_submit_batch_size, options_.flush_foreground_burst_limit);
  return flush_scheduler_->Start(
    [this](const std::shared_ptr<BufferManagerFlushTask> &task) { BeginFlushTask(task); },
    [this](const std::shared_ptr<BufferManagerFlushTask> &task, const Status &status) { FinalizeFlushTask(task, status); });
}

auto BufferManager::StartCleanerController() -> Status {
  if (!options_.enable_background_cleaner) return Status::Ok();

  cleaner_controller_ = std::make_unique<BufferManagerCleanerController>(pool_size_, &dirty_page_count_, options_.dirty_page_high_watermark, options_.dirty_page_low_watermark);
  return cleaner_controller_->Start(
    [this]() { SeedCleanerCandidates(); },
    [this](FrameId frame_id) { ScheduleCleanerFlush(frame_id); });
}

void BufferManager::StopFlushPipeline(const Status &reason) {
  if (cleaner_controller_ != nullptr) cleaner_controller_->Shutdown();
  if (flush_scheduler_ != nullptr) flush_scheduler_->Shutdown();
  if (completion_dispatcher_ != nullptr) completion_dispatcher_->Shutdown(reason);
}

Result<BufferHandle> BufferManager::ReadBuffer(FileId file_id, BlockId block_id) {
  if (!init_status_.ok()) return init_status_;
  const BufferTag tag{file_id, block_id};

  auto resident_hit = TryReadResidentHit(tag);
  if (resident_hit.has_value()) return std::move(resident_hit.value());

  auto registration = miss_coordinator_->RegisterOrJoin(tag);
  if (!registration.is_owner) return AwaitJoinedMiss(tag, registration.state);

  auto owner_hit = TryServeMissOwnerHit(tag, registration.state);
  if (owner_hit.has_value()) return std::move(owner_hit.value());
  return LoadMissOwnerBuffer(tag, registration.state);
}

Status BufferManager::ReleaseBuffer(BufferHandle &&handle) {
  Status validate_status = ValidateOwnedHandle(handle);
  if (!validate_status.ok()) return validate_status;
  handle.Reset();
  return Status::Ok();
}

Status BufferManager::MarkBufferDirty(const BufferHandle &handle) {
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

Status BufferManager::FlushBuffer(const BufferHandle &handle) {
  Status validate_status = ValidateOwnedHandle(handle);
  if (!validate_status.ok()) return validate_status;

  if (HandleHasStableFlushSource(handle)) return FlushFrameWithStableSource(handle.frame_id(), handle.data_);
  return FlushFrame(handle.frame_id());
}

Status BufferManager::FlushAll() {
  if (!init_status_.ok()) {
    return init_status_;
  }
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

BufferPoolSnapshot BufferManager::ExportSnapshot() const {
  BufferPoolSnapshot snapshot;
  snapshot.pool_size = pool_size_;
  snapshot.page_size = page_size_;
  snapshot.frames.reserve(descriptors_.size());

  for (const BufferDescriptor &descriptor : descriptors_) {
    std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
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

Status BufferManager::ReleaseFrame(FrameId frame_id) {
  if (!ValidateFrame(frame_id)) return Status::InvalidArgument("invalid frame id");

  Result<bool> unpin_result = UnpinFrame(frame_id);
  if (!unpin_result.ok()) return unpin_result.status();
  if (unpin_result.value()) {
    MaybeEnqueueCleanerCandidate(frame_id);
    NotifyCleaner();
  }
  return Status::Ok();
}

auto BufferManager::BuildPinnedHandle(FrameId frame_id, const BufferTag &tag) -> BufferHandle {
  return BufferHandle(this, frame_id, tag, GetFrameData(frame_id), page_size_);
}

auto BufferManager::UnpinFrame(FrameId frame_id) -> Result<bool> {
  BufferDescriptor &descriptor = descriptors_[frame_id];
  std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
  if (descriptor.pin_count == 0) return Status::Internal("pin count underflow");
  if (descriptor.state != BufferFrameState::kResident || !descriptor.is_valid || descriptor.io_in_flight) return Status::Internal("attempted to release a non-resident frame");

  --descriptor.pin_count;
  if (descriptor.pin_count != 0) return false;

  replacer_->SetEvictable(frame_id, true);
  return descriptor.is_dirty;
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

auto BufferManager::TryReadResidentHit(const BufferTag &tag) -> std::optional<BufferHandle> {
  auto handle = TryReadResidentBuffer(tag);
  if (!handle.has_value()) return std::nullopt;

  observer_->RecordResidentHit(tag);
  return std::move(handle.value());
}

auto BufferManager::AwaitJoinedMiss(const BufferTag &tag, const std::shared_ptr<BufferManagerMissState> &state) -> Result<BufferHandle> {
  Result<FrameId> wait_result = miss_coordinator_->Wait(state);
  if (!wait_result.ok()) return wait_result.status();

  Result<BufferHandle> handle_result = AwaitResidentBuffer(wait_result.value(), tag);
  if (handle_result.ok()) observer_->RecordJoinedMissHit(tag);
  return handle_result;
}

auto BufferManager::TryServeMissOwnerHit(const BufferTag &tag, const std::shared_ptr<BufferManagerMissState> &state) -> std::optional<BufferHandle> {
  auto handle = TryReadResidentHit(tag);
  if (!handle.has_value()) return std::nullopt;

  CompleteMiss(tag, state, Status::Ok(), handle->frame_id());
  return std::move(handle.value());
}

auto BufferManager::LoadMissOwnerBuffer(const BufferTag &tag, const std::shared_ptr<BufferManagerMissState> &state) -> Result<BufferHandle> {
  observer_->RecordReadMiss(tag);

  Result<FrameReservation> reserve_result = ReserveFrameForTag(tag);
  if (!reserve_result.ok()) {
    CompleteMiss(tag, state, reserve_result.status(), kInvalidFrameId);
    return reserve_result.status();
  }

  Result<FrameId> frame_result = CompleteReservation(tag, reserve_result.value());
  if (!frame_result.ok()) {
    CompleteMiss(tag, state, frame_result.status(), kInvalidFrameId);
    return frame_result.status();
  }

  const FrameId frame_id = frame_result.value();
  CompleteMiss(tag, state, Status::Ok(), frame_id);
  return BuildPinnedHandle(frame_id, tag);
}

void BufferManager::CompleteMiss(const BufferTag &tag, const std::shared_ptr<BufferManagerMissState> &state, const Status &status, FrameId frame_id) {
  miss_coordinator_->Complete(tag, state, status, frame_id);
}

auto BufferManager::ValidateOwnedHandle(const BufferHandle &handle) const -> Status {
  if (!init_status_.ok()) return init_status_;
  if (!handle.valid()) return Status::InvalidArgument("buffer handle is not valid");
  if (handle.manager_ != this) return Status::InvalidArgument("buffer handle belongs to another manager");
  if (!ValidateHandle(handle)) return Status::InvalidArgument("invalid buffer handle");
  return Status::Ok();
}

auto BufferManager::MarkFrameDirty(FrameId frame_id, const BufferTag &tag) -> Result<bool> {
  BufferDescriptor &descriptor = descriptors_[frame_id];
  std::lock_guard<std::mutex> guard(descriptor.latch);
  if (!descriptor.is_valid || descriptor.state != BufferFrameState::kResident || descriptor.tag != tag) return Status::InvalidArgument("buffer handle refers to an invalid frame");

  const bool became_dirty = !descriptor.is_dirty;
  ++descriptor.dirty_generation;
  descriptor.is_dirty = true;
  return became_dirty;
}

bool BufferManager::HandleHasStableFlushSource(const BufferHandle &handle) const {
  if (handle.write_lock_.owns_lock()) return true;
  if (handle.read_lock_.owns_lock()) return true;
  return false;
}

auto BufferManager::LookupFrameId(const BufferTag &tag) -> std::optional<FrameId> {
  return page_table_->LookupFrameId(tag);
}

void BufferManager::RecordPinnedAccess(FrameId frame_id) {
  replacer_->RecordAccess(frame_id);
  replacer_->SetEvictable(frame_id, false);
}

auto BufferManager::TryPinResidentFrame(FrameId frame_id, const BufferTag &tag) -> std::optional<BufferHandle> {
  BufferDescriptor &descriptor = descriptors_[frame_id];
  {
    std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
    if (!buffer_descriptor_state::IsResidentFrameMatch(descriptor, tag)) return std::nullopt;
    ++descriptor.pin_count;
  }

  RecordPinnedAccess(frame_id);
  return BuildPinnedHandle(frame_id, tag);
}

auto BufferManager::WaitForFrameReady(FrameId frame_id) -> Status {
  if (!ValidateFrame(frame_id)) return Status::InvalidArgument("invalid frame id while awaiting buffer");

  BufferDescriptor &descriptor = descriptors_[frame_id];
  std::unique_lock<std::mutex> lock(descriptor.latch);
  while (descriptor.state == BufferFrameState::kLoading || descriptor.io_in_flight) {
    descriptor.io_cv.wait(lock);
  }
  return descriptor.last_io_status;
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
  std::memcpy(snapshot.data(), GetFrameData(frame_id), page_size_);
  return snapshot;
}

auto BufferManager::BuildDirtyFlushTask(FrameId frame_id, const BufferTag &tag, uint64_t dirty_generation, const std::byte *stable_data, bool cleaner_owned) -> std::shared_ptr<BufferManagerFlushTask> {
  auto task = std::make_shared<BufferManagerFlushTask>();
  task->frame_id = frame_id;
  task->tag = tag;
  task->generation = dirty_generation;
  task->clear_dirty_on_success = true;
  task->cleaner_owned = cleaner_owned;
  task->snapshot = CaptureFlushSnapshot(frame_id, stable_data);
  return task;
}

auto BufferManager::BuildEvictedFlushTask(const FrameReservation &reservation) -> std::shared_ptr<BufferManagerFlushTask> {
  auto task = std::make_shared<BufferManagerFlushTask>();
  task->frame_id = reservation.frame_id;
  task->tag = reservation.evicted_tag;
  task->generation = reservation.evicted_dirty_generation;
  task->clear_dirty_on_success = false;
  task->snapshot = CaptureFlushSnapshot(reservation.frame_id, nullptr);
  return task;
}

auto BufferManager::EnqueueFlushTask( const std::shared_ptr<BufferManagerFlushTask> &task) -> Status {
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

auto BufferManager::WaitForFlushTask(const std::shared_ptr<BufferManagerFlushTask> &task) -> Status {
  return flush_scheduler_->Wait(task);
}

auto BufferManager::WaitForScheduledFlushes(const std::vector<std::shared_ptr<BufferManagerFlushTask>> &tasks) -> Status {
  Status first_error = Status::Ok();
  for (const auto &task : tasks) {
    Status status = WaitForFlushTask(task);
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

Status BufferManager::FlushFrame(FrameId frame_id) {
  return FlushFrameWithStableSource(frame_id, nullptr);
}

void BufferManager::BeginFlushTask(const std::shared_ptr<BufferManagerFlushTask> &task) {
  if (task == nullptr || !task->clear_dirty_on_success) return;

  BufferDescriptor &descriptor = descriptors_[task->frame_id];
  std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
  descriptor.flush_queued = false;
  descriptor.flush_in_flight = true;
  descriptor.io_cv.notify_all();
}

void BufferManager::ResetCleanerCandidate(FrameId frame_id) {
  if (cleaner_controller_ == nullptr || !ValidateFrame(frame_id)) return;
  cleaner_controller_->ResetCandidate(frame_id);
}

void BufferManager::MaybeEnqueueCleanerCandidate(FrameId frame_id) {
  if (cleaner_controller_ == nullptr || !ValidateFrame(frame_id)) return;
  cleaner_controller_->EnqueueCandidate(frame_id);
}

bool BufferManager::ShouldSeedCleanerCandidate(FrameId frame_id) const {
  if (!ValidateFrame(frame_id)) return false;

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

auto BufferManager::TryBuildFlushTask(FrameId frame_id, const std::byte *stable_data, bool *was_busy, bool cleaner_owned) -> Result<std::shared_ptr<BufferManagerFlushTask>> {
  BufferTag tag;
  uint64_t dirty_generation = 0;
  Result<bool> prepare_result = PrepareFlushTask(frame_id, stable_data, was_busy, cleaner_owned, &tag, &dirty_generation);
  if (!prepare_result.ok()) return prepare_result.status();
  if (!prepare_result.value()) return std::shared_ptr<BufferManagerFlushTask>{};

  return BuildDirtyFlushTask(frame_id, tag, dirty_generation, stable_data, cleaner_owned);
}

Result<std::shared_ptr<BufferManagerFlushTask>>
BufferManager::TryScheduleFlushTask(FrameId frame_id, const std::byte *stable_data, bool *was_busy, bool cleaner_owned) {
  if (was_busy != nullptr) *was_busy = false;
  if (!ValidateFrame(frame_id)) return Status::InvalidArgument("invalid frame id");

  Result<std::shared_ptr<BufferManagerFlushTask>> task_result = TryBuildFlushTask(frame_id, stable_data, was_busy, cleaner_owned);
  if (!task_result.ok()) return task_result.status();
  if (task_result.value() == nullptr) return std::shared_ptr<BufferManagerFlushTask>{};

  Status enqueue_status = EnqueueFlushTask(task_result.value());
  if (!enqueue_status.ok()) return RollBackFlushTask(frame_id, cleaner_owned, enqueue_status);
  return task_result.value();
}

auto BufferManager::RunForegroundFlush(FrameId frame_id, const std::byte *stable_data) -> Status {
  while (true) {
    Result<std::shared_ptr<BufferManagerFlushTask>> schedule_result = TryScheduleFlushTask(frame_id, stable_data, nullptr, false);
    if (!schedule_result.ok()) return schedule_result.status();
    if (schedule_result.value() == nullptr) return Status::Ok();

    Status wait_status = WaitForFlushTask(schedule_result.value());
    if (!wait_status.ok()) return wait_status;
  }
}

Status BufferManager::FlushFrameWithStableSource(FrameId frame_id, const std::byte *stable_data) {
  if (!ValidateFrame(frame_id)) return Status::InvalidArgument("invalid frame id");
  return RunForegroundFlush(frame_id, stable_data);
}

Status BufferManager::FlushEvictedPage(const FrameReservation &reservation) {
  auto task = BuildEvictedFlushTask(reservation);
  Status enqueue_status = EnqueueFlushTask(task);
  if (!enqueue_status.ok()) return enqueue_status;
  return WaitForFlushTask(task);
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

  FinalizeCleanerFlush(task);
  if (flush_status.ok()) observer_->RecordSuccessfulFlush(task->tag);
  NotifyCleaner();
}

bool BufferManager::ValidateFrame(FrameId frame_id) const {
  return frame_id < pool_size_;
}

bool BufferManager::ValidateHandle(const BufferHandle &handle) const {
  if (!handle.valid() || handle.manager_ != this || !ValidateFrame(handle.frame_id())) return false;
  const BufferDescriptor &descriptor = descriptors_[handle.frame_id()];
  std::lock_guard<std::mutex> guard(descriptor.latch);
  return buffer_descriptor_state::IsResidentFrameMatch(descriptor, handle.tag());
}

Result<BufferHandle> BufferManager::AwaitResidentBuffer(FrameId frame_id,
                                                        const BufferTag &tag) {
  Status wait_status = WaitForFrameReady(frame_id);
  if (!wait_status.ok()) return wait_status;

  auto handle = TryPinResidentFrame(frame_id, tag);
  if (!handle.has_value()) return Status::NotFound("buffer not resident after load completion");
  return std::move(handle.value());
}

std::optional<BufferHandle> BufferManager::TryReadResidentBuffer(const BufferTag &tag) {
  auto frame_id = LookupFrameId(tag);
  if (!frame_id.has_value()) return std::nullopt;
  return TryPinResidentFrame(frame_id.value(), tag);
}

Status BufferManager::StartCompletionDispatcher() {
  if (completion_dispatcher_ == nullptr) return Status::InvalidArgument("completion dispatcher must not be null");
  return completion_dispatcher_->Start();
}

std::byte *BufferManager::GetFrameData(FrameId frame_id) {
  return frame_pool_->GetFrameData(frame_id);
}

const std::byte *BufferManager::GetFrameData(FrameId frame_id) const {
  return frame_pool_->GetFrameData(frame_id);
}

const std::byte *BufferManager::AcquireReadPointer(BufferHandle *handle) const {
  if (handle == nullptr || !ValidateHandle(*handle)) return nullptr;
  AcquireReadLatch(handle);
  return handle->data_;
}

std::byte *BufferManager::AcquireWritePointer(BufferHandle *handle) {
  if (handle == nullptr || !ValidateHandle(*handle)) return nullptr;
  AcquireWriteLatch(handle);
  return handle->data_;
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

void BufferManager::RequeueReservationCandidate(FrameId frame_id) {
  replacer_->SetEvictable(frame_id, true);
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
    PrepareLoadingFrame(tag, reservation, &descriptor);
    return reservation;
  }
  if (buffer_descriptor_state::ShouldRequeueReservationCandidate(descriptor)) RequeueReservationCandidate(frame_id);
  return std::nullopt;
}

void BufferManager::RemoveReservationEntry(FrameId frame_id, const BufferTag &tag) {
  page_table_->Remove(tag, frame_id);
}

void BufferManager::PrepareLoadingFrame(const BufferTag &tag, const FrameReservation &reservation, BufferDescriptor *descriptor) {
  ResetCleanerCandidate(reservation.frame_id);
  buffer_descriptor_state::EnterLoadingState(descriptor, tag);

  if (!reservation.had_evicted_page || !reservation.evicted_dirty) return;
  dirty_page_count_.fetch_sub(1, std::memory_order_acq_rel);
  NotifyCleaner();
}

void BufferManager::MarkFrameReadInFlight(FrameId frame_id) {
  BufferDescriptor &descriptor = descriptors_[frame_id];
  std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
  descriptor.io_in_flight = true;
  descriptor.last_io_status = Status::Ok();
}

void BufferManager::CompleteLoadedFrame(FrameId frame_id) {
  BufferDescriptor &descriptor = descriptors_[frame_id];
  std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
  buffer_descriptor_state::EnterResidentState(&descriptor);
  descriptor.io_cv.notify_all();
  ResetCleanerCandidate(frame_id);
}

void BufferManager::ResetFrameToFree(FrameId frame_id, BufferDescriptor *descriptor, const Status &status) {
  buffer_descriptor_state::EnterFreeState(descriptor, frame_id, status);
  descriptor->io_cv.notify_all();
}

void BufferManager::ReleaseFreeFrame(FrameId frame_id) {
  ResetCleanerCandidate(frame_id);
  std::lock_guard<std::mutex> free_list_guard(free_list_latch_);
  free_list_.push_back(frame_id);
}

auto BufferManager::AbortLoadReservation(FrameId frame_id, const BufferTag &tag, const Status &status) -> Status {
  BufferDescriptor &descriptor = descriptors_[frame_id];
  std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
  RemoveReservationEntry(frame_id, tag);
  ResetFrameToFree(frame_id, &descriptor, status);
  ReleaseFreeFrame(frame_id);
  return status;
}

auto BufferManager::FlushReservedVictim(const FrameReservation &reservation) -> Status {
  if (!reservation.had_evicted_page || !reservation.evicted_dirty) return Status::Ok();

  Status flush_status = FlushEvictedPage(reservation);
  if (!flush_status.ok()) return flush_status;
  observer_->RecordReservedVictimFlush(reservation.evicted_tag);
  return Status::Ok();
}

auto BufferManager::SubmitReadRequest(FrameId frame_id, const BufferTag &tag) -> Result<uint64_t> {
  MarkFrameReadInFlight(frame_id);
  return disk_backend_->SubmitRead(tag, GetFrameData(frame_id), page_size_);
}

auto BufferManager::WaitForReadRequest(uint64_t request_id) -> Status {
  completion_dispatcher_->Register(request_id);
  return completion_dispatcher_->Wait(request_id);
}

auto BufferManager::ReadReservedPage(FrameId frame_id, const BufferTag &tag) -> Status {
  Result<uint64_t> read_submit = SubmitReadRequest(frame_id, tag);
  if (!read_submit.ok()) return read_submit.status();
  return WaitForReadRequest(read_submit.value());
}

auto BufferManager::RestoreEvictionFailure(const FrameReservation &reservation, const Status &status) -> Status {
  RestoreFrameAfterFailedEviction(reservation.frame_id, reservation.evicted_tag, reservation.evicted_dirty, reservation.evicted_dirty_generation);
  return status;
}

void BufferManager::RestoreEvictedMapping(FrameId frame_id, const BufferTag &old_tag) {
  BufferDescriptor &descriptor = descriptors_[frame_id];
  page_table_->Replace(descriptor.tag, old_tag, frame_id);
}

void BufferManager::RestoreDirtyEviction(FrameId frame_id) {
  dirty_page_count_.fetch_add(1, std::memory_order_acq_rel);
  MaybeEnqueueCleanerCandidate(frame_id);
  NotifyCleaner();
}

void BufferManager::FinishEvictionFailureRecovery(FrameId frame_id) {
  ResetCleanerCandidate(frame_id);
  RecordPinnedAccess(frame_id);
  replacer_->SetEvictable(frame_id, true);
}

Result<FrameReservation> BufferManager::ReserveFrameForTag(const BufferTag &tag) {
  while (true) {
    Result<FrameId> frame_result = AcquireReservationFrame();
    if (!frame_result.ok()) return frame_result.status();

    auto reservation = TryReserveFrameForTag(frame_result.value(), tag);
    if (reservation.has_value()) return std::move(reservation.value());
  }
}

Result<FrameId> BufferManager::CompleteReservation(const BufferTag &tag, const FrameReservation &reservation) {
  const FrameId frame_id = reservation.frame_id;

  Status flush_status = FlushReservedVictim(reservation);
  if (!flush_status.ok()) return RestoreEvictionFailure(reservation, flush_status);

  Status read_status = ReadReservedPage(frame_id, tag);
  if (!read_status.ok()) return AbortLoadReservation(frame_id, tag, read_status);

  CompleteLoadedFrame(frame_id);
  RecordPinnedAccess(frame_id);
  if (!reservation.had_evicted_page) {
    observer_->RecordLoadCompletion(tag);
    return frame_id;
  }
  observer_->RecordLoadCompletion(tag, reservation.evicted_tag);
  return frame_id;
}

Status BufferManager::RestoreFrameAfterFailedEviction(FrameId frame_id, const BufferTag &old_tag, bool was_dirty, uint64_t dirty_generation) {
  BufferDescriptor &descriptor = descriptors_[frame_id];
  std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);

  RestoreEvictedMapping(frame_id, old_tag);
  buffer_descriptor_state::RestoreEvictedFrameDescriptor( &descriptor, old_tag, was_dirty, dirty_generation);
  FinishEvictionFailureRecovery(frame_id);
  if (was_dirty) RestoreDirtyEviction(frame_id);
  return descriptor.last_io_status;
}

void BufferManager::FinalizeCleanerFlush(const std::shared_ptr<BufferManagerFlushTask> &task) {
  if (task->cleaner_owned && cleaner_controller_ != nullptr) cleaner_controller_->OnFlushFinished();
}

void BufferManager::AcquireReadLatch(BufferHandle *handle) const {
  if (handle->write_lock_.owns_lock() || handle->read_lock_.owns_lock()) return;
  handle->read_lock_ = std::shared_lock<std::shared_mutex>(descriptors_[handle->frame_id_].content_latch);
}

void BufferManager::AcquireWriteLatch(BufferHandle *handle) {
  if (handle->write_lock_.owns_lock()) return;
  if (handle->read_lock_.owns_lock()) handle->read_lock_.unlock();
  handle->write_lock_ = std::unique_lock<std::shared_mutex>(descriptors_[handle->frame_id_].content_latch);
}

void BufferManager::NotifyCleaner() {
  if (cleaner_controller_ != nullptr) cleaner_controller_->Notify();
}

}  // namespace telepath
