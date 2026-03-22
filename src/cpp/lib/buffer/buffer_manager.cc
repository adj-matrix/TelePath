#include "telepath/buffer/buffer_manager.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "frame_memory_pool.h"
#include "telepath/io/disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace telepath {

namespace {

constexpr std::size_t kPageTableStripeCount = 64;

Status WaitForDiskRequest(DiskBackend *disk_backend, uint64_t request_id) {
  while (true) {
    Result<DiskCompletion> completion_result = disk_backend->PollCompletion();
    if (!completion_result.ok()) {
      return completion_result.status();
    }
    DiskCompletion completion = completion_result.value();
    if (completion.request_id == request_id) {
      return completion.status;
    }
  }
}

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

void BufferHandle::Reset() {
  if (manager_ == nullptr) {
    return;
  }
  BufferManager *manager = manager_;
  manager_ = nullptr;
  std::ignore = manager->ReleaseFrame(frame_id_);
  frame_id_ = kInvalidFrameId;
  data_ = nullptr;
  size_ = 0;
}

BufferManager::BufferManager(std::size_t pool_size, std::size_t page_size,
                             std::unique_ptr<DiskBackend> disk_backend,
                             std::unique_ptr<Replacer> replacer,
                             std::shared_ptr<TelemetrySink> telemetry_sink)
    : pool_size_(pool_size),
      page_size_(page_size),
      disk_backend_(std::move(disk_backend)),
      replacer_(std::move(replacer)),
      telemetry_sink_(std::move(telemetry_sink)),
      frame_pool_(std::make_unique<FrameMemoryPool>(pool_size, page_size)),
      descriptors_(pool_size),
      page_table_latches_(kPageTableStripeCount) {
  const Status init_status = frame_pool_->Initialize();
  if (!init_status.ok()) {
    throw std::runtime_error(init_status.message());
  }

  free_list_.reserve(pool_size_);
  for (FrameId frame_id = 0; frame_id < pool_size_; ++frame_id) {
    descriptors_[frame_id].frame_id = frame_id;
    free_list_.push_back(frame_id);
  }
}

BufferManager::~BufferManager() = default;

Result<BufferHandle> BufferManager::ReadBuffer(FileId file_id, BlockId block_id) {
  const BufferTag tag{file_id, block_id};

  if (auto handle = TryReadResidentBuffer(tag); handle.has_value()) {
    telemetry_sink_->RecordHit(tag);
    return std::move(handle.value());
  }

  {
    std::lock_guard<std::mutex> miss_guard(miss_latch_);
    const std::size_t stripe = GetPageTableStripe(tag);
    FrameId existing_frame = kInvalidFrameId;
    {
      std::lock_guard<std::mutex> page_table_guard(page_table_latches_[stripe]);
      auto it = page_table_.find(tag);
      if (it != page_table_.end()) {
        existing_frame = it->second;
      }
    }

    if (existing_frame != kInvalidFrameId) {
      Result<BufferHandle> await_result = AwaitResidentBuffer(existing_frame, tag);
      if (await_result.ok()) {
        telemetry_sink_->RecordHit(tag);
      }
      return await_result;
    }

    telemetry_sink_->RecordMiss(tag);
    Result<FrameId> frame_result = AcquireFrame(tag);
    if (!frame_result.ok()) {
      return frame_result.status();
    }

    const FrameId frame_id = frame_result.value();
    return BufferHandle(this, frame_id, tag, GetFrameData(frame_id), page_size_);
  }
}

Status BufferManager::ReleaseBuffer(BufferHandle &&handle) {
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
  if (!ValidateHandle(handle)) {
    return Status::InvalidArgument("invalid buffer handle");
  }
  BufferDescriptor &descriptor = descriptors_[handle.frame_id()];
  std::lock_guard<std::mutex> guard(descriptor.latch);
  if (!descriptor.is_valid || descriptor.state != BufferFrameState::kResident ||
      descriptor.tag != handle.tag()) {
    return Status::InvalidArgument("buffer handle refers to an invalid frame");
  }
  descriptor.is_dirty = true;
  return Status::Ok();
}

Status BufferManager::FlushBuffer(const BufferHandle &handle) {
  if (!ValidateHandle(handle)) {
    return Status::InvalidArgument("invalid buffer handle");
  }
  return FlushFrame(handle.frame_id());
}

Status BufferManager::FlushAll() {
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
  if (!ValidateFrame(frame_id)) {
    return Status::InvalidArgument("invalid frame id");
  }

  BufferTag tag;
  bool should_flush = false;
  {
    BufferDescriptor &descriptor = descriptors_[frame_id];
    std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
    if (!descriptor.is_valid || descriptor.state != BufferFrameState::kResident ||
        !descriptor.is_dirty) {
      return Status::Ok();
    }
    tag = descriptor.tag;
    should_flush = true;
  }

  if (!should_flush) {
    return Status::Ok();
  }

  Status status =
      [&]() -> Status {
        Result<uint64_t> submit_result =
            disk_backend_->SubmitWrite(tag, GetFrameData(frame_id), page_size_);
        if (!submit_result.ok()) {
          return submit_result.status();
        }
        return WaitForDiskRequest(disk_backend_.get(), submit_result.value());
      }();
  if (!status.ok()) {
    return status;
  }

  {
    BufferDescriptor &descriptor = descriptors_[frame_id];
    std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
    descriptor.is_dirty = false;
  }

  telemetry_sink_->RecordDiskWrite(tag);
  telemetry_sink_->RecordDirtyFlush(tag);
  return Status::Ok();
}

Result<FrameId> BufferManager::AcquireFrame(const BufferTag &tag) {
  FrameId frame_id = kInvalidFrameId;

  while (true) {
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

    if (descriptor.is_valid && descriptor.pin_count != 0) {
      frame_id = kInvalidFrameId;
      continue;
    }

    if (descriptor.is_valid) {
      if (descriptor.is_dirty) {
        Status flush_status =
            [&]() -> Status {
              Result<uint64_t> submit_result = disk_backend_->SubmitWrite(
                  descriptor.tag, GetFrameData(frame_id), page_size_);
              if (!submit_result.ok()) {
                return submit_result.status();
              }
              return WaitForDiskRequest(disk_backend_.get(),
                                        submit_result.value());
            }();
        if (!flush_status.ok()) {
          return flush_status;
        }
        descriptor.is_dirty = false;
        telemetry_sink_->RecordDiskWrite(descriptor.tag);
        telemetry_sink_->RecordDirtyFlush(descriptor.tag);
      }

      const std::size_t old_stripe = GetPageTableStripe(descriptor.tag);
      std::lock_guard<std::mutex> page_table_guard(page_table_latches_[old_stripe]);
      page_table_.erase(descriptor.tag);
      telemetry_sink_->RecordEviction(descriptor.tag);
    }

    descriptor.state = BufferFrameState::kLoading;
    descriptor.is_valid = false;
    descriptor.is_dirty = false;
    descriptor.pin_count = 1;
    descriptor.io_in_flight = true;
    descriptor.last_io_status = Status::Ok();
    descriptor.tag = tag;

    const std::size_t stripe = GetPageTableStripe(tag);
    {
      std::lock_guard<std::mutex> page_table_guard(page_table_latches_[stripe]);
      page_table_[tag] = frame_id;
    }

    Status read_status =
        [&]() -> Status {
          Result<uint64_t> submit_result =
              disk_backend_->SubmitRead(tag, GetFrameData(frame_id), page_size_);
          if (!submit_result.ok()) {
            return submit_result.status();
          }
          return WaitForDiskRequest(disk_backend_.get(), submit_result.value());
        }();
    if (!read_status.ok()) {
      {
        const std::size_t stripe = GetPageTableStripe(tag);
        std::lock_guard<std::mutex> page_table_guard(page_table_latches_[stripe]);
        auto it = page_table_.find(tag);
        if (it != page_table_.end() && it->second == frame_id) {
          page_table_.erase(it);
        }
      }
      descriptor.frame_id = frame_id;
      descriptor.tag = BufferTag{};
      descriptor.pin_count = 0;
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
    descriptor.is_dirty = false;
    descriptor.is_valid = true;
    descriptor.state = BufferFrameState::kResident;
    descriptor.io_in_flight = false;
    descriptor.last_io_status = Status::Ok();
    descriptor.io_cv.notify_all();
    replacer_->RecordAccess(frame_id);
    replacer_->SetEvictable(frame_id, false);
    telemetry_sink_->RecordDiskRead(tag);
    return frame_id;
  }
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

}  // namespace telepath
