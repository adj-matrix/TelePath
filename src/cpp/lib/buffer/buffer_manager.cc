#include "telepath/buffer/buffer_manager.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "frame_memory_pool.h"
#include "telepath/io/disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace telepath {

namespace {

}  // namespace

BufferHandle::BufferHandle(BufferManager *manager, FrameId frame_id,
                           const BufferTag &tag, std::byte *data,
                           std::size_t size)
    : manager_(manager),
      frame_id_(frame_id),
      tag_(tag),
      data_(data),
      size_(size) {}

BufferHandle::BufferHandle(BufferHandle &&other) noexcept
    : manager_(other.manager_),
      frame_id_(other.frame_id_),
      tag_(other.tag_),
      data_(other.data_),
      size_(other.size_) {
  other.manager_ = nullptr;
  other.frame_id_ = kInvalidFrameId;
  other.data_ = nullptr;
  other.size_ = 0;
}

BufferHandle &BufferHandle::operator=(BufferHandle &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  Reset();
  manager_ = other.manager_;
  frame_id_ = other.frame_id_;
  tag_ = other.tag_;
  data_ = other.data_;
  size_ = other.size_;
  other.manager_ = nullptr;
  other.frame_id_ = kInvalidFrameId;
  other.data_ = nullptr;
  other.size_ = 0;
  return *this;
}

BufferHandle::~BufferHandle() { Reset(); }

const std::byte *BufferHandle::data() const {
  if (manager_ == nullptr) {
    return nullptr;
  }
  return manager_->AcquireReadPointer(const_cast<BufferHandle *>(this));
}

std::byte *BufferHandle::mutable_data() {
  if (manager_ == nullptr) {
    return nullptr;
  }
  return manager_->AcquireWritePointer(this);
}

void BufferHandle::Reset() {
  if (manager_ == nullptr) {
    return;
  }
  if (read_lock_.owns_lock()) {
    read_lock_.unlock();
  }
  if (write_lock_.owns_lock()) {
    write_lock_.unlock();
  }
  BufferManager *manager = manager_;
  manager_ = nullptr;
  std::ignore = manager->ReleaseFrame(frame_id_);
  frame_id_ = kInvalidFrameId;
  data_ = nullptr;
  size_ = 0;
}

BufferManager::BufferManager(const BufferManagerOptions &options,
                             std::unique_ptr<DiskBackend> disk_backend,
                             std::unique_ptr<Replacer> replacer,
                             std::shared_ptr<TelemetrySink> telemetry_sink)
    : pool_size_(options.pool_size),
      page_size_(options.page_size),
      options_(options),
      disk_backend_(std::move(disk_backend)),
      replacer_(std::move(replacer)),
      telemetry_sink_(std::move(telemetry_sink)),
      frame_pool_(std::make_unique<FrameMemoryPool>(options.pool_size,
                                                    options.page_size)),
      descriptors_(options.pool_size),
      page_table_latches_(options.ResolvePageTableStripeCount()) {
  options_.page_table_stripe_count = options.ResolvePageTableStripeCount();
  const Status init_status = frame_pool_->Initialize();
  if (!init_status.ok()) {
    init_status_ = init_status;
    return;
  }

  free_list_.reserve(pool_size_);
  for (FrameId frame_id = 0; frame_id < pool_size_; ++frame_id) {
    descriptors_[frame_id].frame_id = frame_id;
    free_list_.push_back(frame_id);
  }

  if (disk_backend_ == nullptr) {
    init_status_ = Status::InvalidArgument("disk backend must not be null");
    return;
  }

  try {
    completion_thread_ =
        std::thread(&BufferManager::CompletionDispatcherLoop, this);
  } catch (...) {
    init_status_ = Status::Unavailable("failed to start completion dispatcher");
  }
}

BufferManager::BufferManager(std::size_t pool_size, std::size_t page_size,
                             std::unique_ptr<DiskBackend> disk_backend,
                             std::unique_ptr<Replacer> replacer,
                             std::shared_ptr<TelemetrySink> telemetry_sink)
    : BufferManager(BufferManagerOptions{pool_size, page_size, 0},
                    std::move(disk_backend),
                    std::move(replacer),
                    std::move(telemetry_sink)) {}

BufferManager::~BufferManager() {
  {
    std::lock_guard<std::mutex> guard(completion_latch_);
    completion_shutdown_ = true;
    completion_shutdown_status_ =
        Status::Unavailable("buffer manager is shutting down");
  }
  completion_cv_.notify_all();
  if (disk_backend_ != nullptr) {
    disk_backend_->Shutdown();
  }
  if (completion_thread_.joinable()) {
    completion_thread_.join();
  }
}

Result<BufferHandle> BufferManager::ReadBuffer(FileId file_id, BlockId block_id) {
  if (!init_status_.ok()) {
    return init_status_;
  }
  const BufferTag tag{file_id, block_id};

  if (auto handle = TryReadResidentBuffer(tag); handle.has_value()) {
    telemetry_sink_->RecordHit(tag);
    return std::move(handle.value());
  }

  FrameId existing_frame = kInvalidFrameId;
  std::optional<FrameReservation> reservation;
  {
    std::lock_guard<std::mutex> miss_guard(miss_latch_);
    const std::size_t stripe = GetPageTableStripe(tag);
    {
      std::lock_guard<std::mutex> page_table_guard(page_table_latches_[stripe]);
      auto it = page_table_.find(tag);
      if (it != page_table_.end()) {
        existing_frame = it->second;
      }
    }

    if (existing_frame == kInvalidFrameId) {
      telemetry_sink_->RecordMiss(tag);
      Result<FrameReservation> reserve_result = ReserveFrameForTag(tag);
      if (!reserve_result.ok()) {
        return reserve_result.status();
      }
      reservation = reserve_result.value();
    }
  }

  if (existing_frame != kInvalidFrameId) {
    Result<BufferHandle> await_result = AwaitResidentBuffer(existing_frame, tag);
    if (await_result.ok()) {
      telemetry_sink_->RecordHit(tag);
    }
    return await_result;
  }

  Result<FrameId> frame_result = CompleteReservation(tag, reservation.value());
  if (!frame_result.ok()) {
    return frame_result.status();
  }

  const FrameId frame_id = frame_result.value();
  return BufferHandle(this, frame_id, tag, GetFrameData(frame_id), page_size_);
}

Status BufferManager::ReleaseBuffer(BufferHandle &&handle) {
  if (!init_status_.ok()) {
    return init_status_;
  }
  if (!handle.valid()) {
    return Status::InvalidArgument("buffer handle is not valid");
  }
  if (handle.manager_ != this) {
    return Status::InvalidArgument("buffer handle belongs to another manager");
  }
  handle.Reset();
  return Status::Ok();
}

Status BufferManager::MarkBufferDirty(const BufferHandle &handle) {
  if (!init_status_.ok()) {
    return init_status_;
  }
  if (!ValidateHandle(handle)) {
    return Status::InvalidArgument("invalid buffer handle");
  }
  BufferDescriptor &descriptor = descriptors_[handle.frame_id()];
  std::lock_guard<std::mutex> guard(descriptor.latch);
  if (!descriptor.is_valid || descriptor.state != BufferFrameState::kResident ||
      descriptor.tag != handle.tag()) {
    return Status::InvalidArgument("buffer handle refers to an invalid frame");
  }
  ++descriptor.dirty_generation;
  descriptor.is_dirty = true;
  return Status::Ok();
}

Status BufferManager::FlushBuffer(const BufferHandle &handle) {
  if (!init_status_.ok()) {
    return init_status_;
  }
  if (!ValidateHandle(handle)) {
    return Status::InvalidArgument("invalid buffer handle");
  }
  if (handle.write_lock_.owns_lock() || handle.read_lock_.owns_lock()) {
    return FlushFrameWithStableSource(handle.frame_id(), handle.data_);
  }
  return FlushFrame(handle.frame_id());
}

Status BufferManager::FlushAll() {
  if (!init_status_.ok()) {
    return init_status_;
  }
  for (FrameId frame_id = 0; frame_id < pool_size_; ++frame_id) {
    Status status = FlushFrame(frame_id);
    if (!status.ok()) {
      return status;
    }
  }
  return Status::Ok();
}

Status BufferManager::ReleaseFrame(FrameId frame_id) {
  if (!ValidateFrame(frame_id)) {
    return Status::InvalidArgument("invalid frame id");
  }
  BufferDescriptor &descriptor = descriptors_[frame_id];
  std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
  if (descriptor.pin_count == 0) {
    return Status::Internal("pin count underflow");
  }
  if (descriptor.state != BufferFrameState::kResident || !descriptor.is_valid ||
      descriptor.io_in_flight) {
    return Status::Internal("attempted to release a non-resident frame");
  }
  --descriptor.pin_count;
  if (descriptor.pin_count == 0) {
    replacer_->SetEvictable(frame_id, true);
  }
  return Status::Ok();
}

Status BufferManager::FlushFrame(FrameId frame_id) {
  return FlushFrameWithStableSource(frame_id, nullptr);
}

Status BufferManager::FlushFrameWithStableSource(FrameId frame_id,
                                                 const std::byte *stable_data) {
  if (!ValidateFrame(frame_id)) {
    return Status::InvalidArgument("invalid frame id");
  }

  BufferTag tag;
  bool should_flush = false;
  uint64_t dirty_generation = 0;
  {
    BufferDescriptor &descriptor = descriptors_[frame_id];
    std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
    if (!descriptor.is_valid || descriptor.state != BufferFrameState::kResident ||
        !descriptor.is_dirty) {
      return Status::Ok();
    }
    tag = descriptor.tag;
    dirty_generation = descriptor.dirty_generation;
    should_flush = true;
  }

  if (!should_flush) {
    return Status::Ok();
  }

  std::vector<std::byte> flush_snapshot(page_size_);
  if (stable_data != nullptr) {
    std::memcpy(flush_snapshot.data(), stable_data, page_size_);
  } else {
    const BufferDescriptor &descriptor = descriptors_[frame_id];
    std::shared_lock<std::shared_mutex> content_guard(descriptor.content_latch);
    std::memcpy(flush_snapshot.data(), GetFrameData(frame_id), page_size_);
  }

  Status status =
      [&]() -> Status {
        Result<uint64_t> submit_result =
            disk_backend_->SubmitWrite(tag, flush_snapshot.data(), page_size_);
        if (!submit_result.ok()) {
          return submit_result.status();
        }
        RegisterDiskRequest(submit_result.value());
        return WaitForDiskRequest(submit_result.value());
      }();
  if (!status.ok()) {
    return status;
  }

  {
    BufferDescriptor &descriptor = descriptors_[frame_id];
    std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
    if (descriptor.is_valid && descriptor.state == BufferFrameState::kResident &&
        descriptor.tag == tag &&
        descriptor.dirty_generation == dirty_generation) {
      descriptor.is_dirty = false;
    }
  }

  telemetry_sink_->RecordDiskWrite(tag);
  telemetry_sink_->RecordDirtyFlush(tag);
  return Status::Ok();
}

bool BufferManager::ValidateFrame(FrameId frame_id) const {
  return frame_id < pool_size_;
}

bool BufferManager::ValidateHandle(const BufferHandle &handle) const {
  if (!handle.valid() || handle.manager_ != this || !ValidateFrame(handle.frame_id())) {
    return false;
  }
  const BufferDescriptor &descriptor = descriptors_[handle.frame_id()];
  std::lock_guard<std::mutex> guard(descriptor.latch);
  return descriptor.is_valid && descriptor.state == BufferFrameState::kResident &&
         !descriptor.io_in_flight && descriptor.tag == handle.tag();
}

Result<BufferHandle> BufferManager::AwaitResidentBuffer(FrameId frame_id,
                                                        const BufferTag &tag) {
  if (!ValidateFrame(frame_id)) {
    return Status::InvalidArgument("invalid frame id while awaiting buffer");
  }

  BufferDescriptor &descriptor = descriptors_[frame_id];
  std::unique_lock<std::mutex> lock(descriptor.latch);
  while (descriptor.state == BufferFrameState::kLoading || descriptor.io_in_flight) {
    descriptor.io_cv.wait(lock);
  }

  if (!descriptor.last_io_status.ok()) {
    return descriptor.last_io_status;
  }
  if (!descriptor.is_valid || descriptor.state != BufferFrameState::kResident ||
      descriptor.tag != tag) {
    return Status::NotFound("buffer not resident after load completion");
  }

  ++descriptor.pin_count;
  replacer_->RecordAccess(descriptor.frame_id);
  replacer_->SetEvictable(descriptor.frame_id, false);
  return BufferHandle(this, descriptor.frame_id, tag,
                      GetFrameData(descriptor.frame_id), page_size_);
}

std::optional<BufferHandle> BufferManager::TryReadResidentBuffer(const BufferTag &tag) {
  const std::size_t stripe = GetPageTableStripe(tag);
  FrameId frame_id = kInvalidFrameId;
  {
    std::lock_guard<std::mutex> page_table_guard(page_table_latches_[stripe]);
    auto it = page_table_.find(tag);
    if (it == page_table_.end()) {
      return std::nullopt;
    }
    frame_id = it->second;
  }

  BufferDescriptor &descriptor = descriptors_[frame_id];
  std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
  if (descriptor.state != BufferFrameState::kResident || !descriptor.is_valid ||
      descriptor.io_in_flight ||
      descriptor.tag != tag) {
    return std::nullopt;
  }

  ++descriptor.pin_count;
  replacer_->RecordAccess(descriptor.frame_id);
  replacer_->SetEvictable(descriptor.frame_id, false);
  return BufferHandle(this, descriptor.frame_id, tag,
                      GetFrameData(descriptor.frame_id), page_size_);
}

std::size_t BufferManager::GetPageTableStripe(const BufferTag &tag) const {
  return BufferTagHash{}(tag) % page_table_latches_.size();
}

std::byte *BufferManager::GetFrameData(FrameId frame_id) {
  return frame_pool_->GetFrameData(frame_id);
}

const std::byte *BufferManager::GetFrameData(FrameId frame_id) const {
  return frame_pool_->GetFrameData(frame_id);
}

const std::byte *BufferManager::AcquireReadPointer(BufferHandle *handle) const {
  if (handle == nullptr || !ValidateHandle(*handle)) {
    return nullptr;
  }
  if (!handle->write_lock_.owns_lock() && !handle->read_lock_.owns_lock()) {
    handle->read_lock_ = std::shared_lock<std::shared_mutex>(
        descriptors_[handle->frame_id_].content_latch);
  }
  return handle->data_;
}

std::byte *BufferManager::AcquireWritePointer(BufferHandle *handle) {
  if (handle == nullptr || !ValidateHandle(*handle)) {
    return nullptr;
  }
  if (handle->write_lock_.owns_lock()) {
    return handle->data_;
  }
  if (handle->read_lock_.owns_lock()) {
    handle->read_lock_.unlock();
  }
  handle->write_lock_ = std::unique_lock<std::shared_mutex>(
      descriptors_[handle->frame_id_].content_latch);
  return handle->data_;
}

Result<FrameReservation> BufferManager::ReserveFrameForTag(const BufferTag &tag) {
  FrameId frame_id = kInvalidFrameId;
  BufferTag evicted_tag{};
  bool had_evicted_page = false;
  bool evicted_dirty = false;
  uint64_t evicted_dirty_generation = 0;

  while (true) {
    frame_id = kInvalidFrameId;
    {
      std::lock_guard<std::mutex> free_list_guard(free_list_latch_);
      if (!free_list_.empty()) {
        frame_id = free_list_.back();
        free_list_.pop_back();
      }
    }

    if (frame_id == kInvalidFrameId && !replacer_->Victim(&frame_id)) {
      return Status::ResourceExhausted("no evictable frame available");
    }

    BufferDescriptor &descriptor = descriptors_[frame_id];
    std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
    if (descriptor.pin_count != 0 || descriptor.io_in_flight) {
      continue;
    }

    had_evicted_page = descriptor.is_valid;
    if (had_evicted_page) {
      evicted_tag = descriptor.tag;
      evicted_dirty = descriptor.is_dirty;
      evicted_dirty_generation = descriptor.dirty_generation;

      const std::size_t old_stripe = GetPageTableStripe(descriptor.tag);
      std::lock_guard<std::mutex> page_table_guard(page_table_latches_[old_stripe]);
      page_table_.erase(descriptor.tag);
    } else {
      evicted_dirty = false;
      evicted_dirty_generation = 0;
    }

    descriptor.tag = tag;
    descriptor.pin_count = 1;
    descriptor.dirty_generation = 0;
    descriptor.is_dirty = false;
    descriptor.is_valid = false;
    descriptor.state = BufferFrameState::kLoading;
    descriptor.io_in_flight = false;
    descriptor.last_io_status = Status::Ok();

    const std::size_t stripe = GetPageTableStripe(tag);
    {
      std::lock_guard<std::mutex> page_table_guard(page_table_latches_[stripe]);
      page_table_[tag] = frame_id;
    }
    return FrameReservation{frame_id, had_evicted_page, evicted_tag, evicted_dirty,
                            evicted_dirty_generation};
  }
}

Result<FrameId> BufferManager::CompleteReservation(
    const BufferTag &tag, const FrameReservation &reservation) {
  const FrameId frame_id = reservation.frame_id;

  if (reservation.had_evicted_page && reservation.evicted_dirty) {
    std::vector<std::byte> evicted_snapshot(page_size_);
    {
      const BufferDescriptor &descriptor = descriptors_[frame_id];
      std::shared_lock<std::shared_mutex> content_guard(descriptor.content_latch);
      std::memcpy(evicted_snapshot.data(), GetFrameData(frame_id), page_size_);
    }
    {
      BufferDescriptor &descriptor = descriptors_[frame_id];
      std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
      descriptor.io_in_flight = true;
      descriptor.last_io_status = Status::Ok();
    }

    Result<uint64_t> write_submit = disk_backend_->SubmitWrite(
        reservation.evicted_tag, evicted_snapshot.data(), page_size_);
    if (!write_submit.ok()) {
      {
        BufferDescriptor &descriptor = descriptors_[frame_id];
        std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
        descriptor.io_in_flight = false;
      }
      RestoreFrameAfterFailedEviction(frame_id, reservation.evicted_tag,
                                      reservation.evicted_dirty,
                                      reservation.evicted_dirty_generation);
      return write_submit.status();
    }

    RegisterDiskRequest(write_submit.value());
    Status flush_status = WaitForDiskRequest(write_submit.value());
    {
      BufferDescriptor &descriptor = descriptors_[frame_id];
      std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
      descriptor.io_in_flight = false;
      descriptor.last_io_status = flush_status;
    }
    if (!flush_status.ok()) {
      RestoreFrameAfterFailedEviction(frame_id, reservation.evicted_tag,
                                      reservation.evicted_dirty,
                                      reservation.evicted_dirty_generation);
      return flush_status;
    }
    telemetry_sink_->RecordDiskWrite(reservation.evicted_tag);
    telemetry_sink_->RecordDirtyFlush(reservation.evicted_tag);
  }

  {
    BufferDescriptor &descriptor = descriptors_[frame_id];
    std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
    descriptor.io_in_flight = true;
    descriptor.last_io_status = Status::Ok();
  }

  Result<uint64_t> read_submit =
      disk_backend_->SubmitRead(tag, GetFrameData(frame_id), page_size_);
  if (!read_submit.ok()) {
    BufferDescriptor &descriptor = descriptors_[frame_id];
    std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
    const std::size_t stripe = GetPageTableStripe(tag);
    {
      std::lock_guard<std::mutex> page_table_guard(page_table_latches_[stripe]);
      auto it = page_table_.find(tag);
      if (it != page_table_.end() && it->second == frame_id) {
        page_table_.erase(it);
      }
    }
    descriptor.frame_id = frame_id;
    descriptor.tag = BufferTag{};
    descriptor.pin_count = 0;
    descriptor.dirty_generation = 0;
    descriptor.is_dirty = false;
    descriptor.is_valid = false;
    descriptor.state = BufferFrameState::kFree;
    descriptor.io_in_flight = false;
    descriptor.last_io_status = read_submit.status();
    descriptor.io_cv.notify_all();
    {
      std::lock_guard<std::mutex> free_list_guard(free_list_latch_);
      free_list_.push_back(frame_id);
    }
    return read_submit.status();
  }

  RegisterDiskRequest(read_submit.value());
  Status read_status = WaitForDiskRequest(read_submit.value());
  BufferDescriptor &descriptor = descriptors_[frame_id];
  std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
  if (!read_status.ok()) {
    const std::size_t stripe = GetPageTableStripe(tag);
    {
      std::lock_guard<std::mutex> page_table_guard(page_table_latches_[stripe]);
      auto it = page_table_.find(tag);
      if (it != page_table_.end() && it->second == frame_id) {
        page_table_.erase(it);
      }
    }
    descriptor.frame_id = frame_id;
    descriptor.tag = BufferTag{};
    descriptor.pin_count = 0;
    descriptor.dirty_generation = 0;
    descriptor.is_dirty = false;
    descriptor.is_valid = false;
    descriptor.state = BufferFrameState::kFree;
    descriptor.io_in_flight = false;
    descriptor.last_io_status = read_status;
    descriptor.io_cv.notify_all();
    {
      std::lock_guard<std::mutex> free_list_guard(free_list_latch_);
      free_list_.push_back(frame_id);
    }
    return read_status;
  }

  descriptor.pin_count = 1;
  descriptor.dirty_generation = 0;
  descriptor.is_dirty = false;
  descriptor.is_valid = true;
  descriptor.state = BufferFrameState::kResident;
  descriptor.io_in_flight = false;
  descriptor.last_io_status = Status::Ok();
  descriptor.io_cv.notify_all();

  replacer_->RecordAccess(frame_id);
  replacer_->SetEvictable(frame_id, false);
  telemetry_sink_->RecordDiskRead(tag);
  if (reservation.had_evicted_page) {
    telemetry_sink_->RecordEviction(reservation.evicted_tag);
  }
  return frame_id;
}

Status BufferManager::RestoreFrameAfterFailedEviction(FrameId frame_id,
                                                      const BufferTag &old_tag,
                                                      bool was_dirty,
                                                      uint64_t dirty_generation) {
  BufferDescriptor &descriptor = descriptors_[frame_id];
  std::lock_guard<std::mutex> miss_guard(miss_latch_);
  std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);

  const std::size_t new_stripe = GetPageTableStripe(descriptor.tag);
  {
    std::lock_guard<std::mutex> page_table_guard(page_table_latches_[new_stripe]);
    auto it = page_table_.find(descriptor.tag);
    if (it != page_table_.end() && it->second == frame_id) {
      page_table_.erase(it);
    }
  }

  descriptor.tag = old_tag;
  descriptor.pin_count = 0;
  descriptor.is_dirty = was_dirty;
  descriptor.dirty_generation = dirty_generation;
  descriptor.is_valid = true;
  descriptor.state = BufferFrameState::kResident;
  descriptor.io_in_flight = false;
  descriptor.last_io_status = Status::IoError("eviction flush failed");
  descriptor.io_cv.notify_all();

  const std::size_t old_stripe = GetPageTableStripe(old_tag);
  {
    std::lock_guard<std::mutex> page_table_guard(page_table_latches_[old_stripe]);
    page_table_[old_tag] = frame_id;
  }
  replacer_->RecordAccess(frame_id);
  replacer_->SetEvictable(frame_id, true);
  return descriptor.last_io_status;
}

Status BufferManager::WaitForDiskRequest(uint64_t request_id) {
  std::unique_lock<std::mutex> lock(completion_latch_);
  if (completion_states_.find(request_id) == completion_states_.end()) {
    return Status::InvalidArgument("disk request was not registered");
  }
  completion_cv_.wait(lock, [&]() {
    auto state_it = completion_states_.find(request_id);
    return completion_shutdown_ ||
           (state_it != completion_states_.end() && state_it->second.completed);
  });
  auto state_it = completion_states_.find(request_id);
  if (state_it == completion_states_.end()) {
    return Status::Internal("registered disk request disappeared");
  }
  if (!state_it->second.completed) {
    completion_states_.erase(state_it);
    return completion_shutdown_status_;
  }

  Status status = state_it->second.status;
  completion_states_.erase(state_it);
  return status;
}

void BufferManager::RegisterDiskRequest(uint64_t request_id) {
  std::lock_guard<std::mutex> guard(completion_latch_);
  completion_states_.try_emplace(request_id);
  ++outstanding_disk_requests_;
  completion_cv_.notify_all();
}

void BufferManager::CompletionDispatcherLoop() {
  while (true) {
    {
      std::unique_lock<std::mutex> lock(completion_latch_);
      completion_cv_.wait(lock, [this]() {
        return completion_shutdown_ || outstanding_disk_requests_ > 0;
      });
      if (completion_shutdown_) {
        return;
      }
    }

    Result<DiskCompletion> completion_result = disk_backend_->PollCompletion();
    std::lock_guard<std::mutex> guard(completion_latch_);
    if (!completion_result.ok()) {
      completion_shutdown_ = true;
      completion_shutdown_status_ = completion_result.status();
      completion_cv_.notify_all();
      return;
    }

    DiskCompletion completion = completion_result.value();
    auto &state = completion_states_[completion.request_id];
    state.completed = true;
    state.status = completion.status;
    if (outstanding_disk_requests_ > 0) {
      --outstanding_disk_requests_;
    }
    completion_cv_.notify_all();
  }
}

}  // namespace telepath
