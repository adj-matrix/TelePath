#include "telepath/buffer/buffer_manager.h"

#include <algorithm>
#include <utility>

#include "telepath/io/disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace telepath {

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
      frames_(pool_size, std::vector<std::byte>(page_size)),
      descriptors_(pool_size) {
  free_list_.reserve(pool_size_);
  for (FrameId frame_id = 0; frame_id < pool_size_; ++frame_id) {
    descriptors_[frame_id].frame_id = frame_id;
    free_list_.push_back(frame_id);
  }
}

Result<BufferHandle> BufferManager::ReadBuffer(FileId file_id, BlockId block_id) {
  const BufferTag tag{file_id, block_id};

  {
    std::lock_guard<std::mutex> guard(manager_latch_);
    auto it = page_table_.find(tag);
    if (it != page_table_.end()) {
      BufferDescriptor &descriptor = descriptors_[it->second];
      std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
      if (descriptor.state != BufferFrameState::kResident || !descriptor.is_valid ||
          descriptor.tag != tag) {
        return Status::Internal("page table points to a non-resident frame");
      }
      ++descriptor.pin_count;
      replacer_->RecordAccess(descriptor.frame_id);
      replacer_->SetEvictable(descriptor.frame_id, false);
      telemetry_sink_->RecordHit(tag);
      return BufferHandle(this, descriptor.frame_id, tag,
                          frames_[descriptor.frame_id].data(), page_size_);
    }
  }

  telemetry_sink_->RecordMiss(tag);
  Result<FrameId> frame_result = AcquireFrame(tag);
  if (!frame_result.ok()) {
    return frame_result.status();
  }

  const FrameId frame_id = frame_result.value();
  return BufferHandle(this, frame_id, tag, frames_[frame_id].data(), page_size_);
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
  std::lock_guard<std::mutex> manager_guard(manager_latch_);
  std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);
  if (descriptor.pin_count == 0) {
    return Status::Internal("pin count underflow");
  }
  if (descriptor.state != BufferFrameState::kResident || !descriptor.is_valid) {
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
      disk_backend_->WriteBlock(tag, frames_[frame_id].data(), page_size_);
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
  std::lock_guard<std::mutex> manager_guard(manager_latch_);

  FrameId frame_id = kInvalidFrameId;
  if (!free_list_.empty()) {
    frame_id = free_list_.back();
    free_list_.pop_back();
  } else if (!replacer_->Victim(&frame_id)) {
    return Status::ResourceExhausted("no evictable frame available");
  }

  BufferDescriptor &descriptor = descriptors_[frame_id];
  std::lock_guard<std::mutex> descriptor_guard(descriptor.latch);

  if (descriptor.is_valid) {
    if (descriptor.is_dirty) {
      Status flush_status =
          disk_backend_->WriteBlock(descriptor.tag, frames_[frame_id].data(),
                                    page_size_);
      if (!flush_status.ok()) {
        return flush_status;
      }
      descriptor.is_dirty = false;
      telemetry_sink_->RecordDiskWrite(descriptor.tag);
      telemetry_sink_->RecordDirtyFlush(descriptor.tag);
    }
    page_table_.erase(descriptor.tag);
    telemetry_sink_->RecordEviction(descriptor.tag);
  }

  descriptor.state = BufferFrameState::kLoading;
  descriptor.is_valid = false;
  descriptor.is_dirty = false;
  descriptor.pin_count = 1;
  descriptor.tag = BufferTag{};

  Status read_status =
      disk_backend_->ReadBlock(tag, frames_[frame_id].data(), page_size_);
  if (!read_status.ok()) {
    descriptor.frame_id = frame_id;
    descriptor.tag = BufferTag{};
    descriptor.pin_count = 0;
    descriptor.is_dirty = false;
    descriptor.is_valid = false;
    descriptor.state = BufferFrameState::kFree;
    free_list_.push_back(frame_id);
    return read_status;
  }

  descriptor.tag = tag;
  descriptor.pin_count = 1;
  descriptor.is_dirty = false;
  descriptor.is_valid = true;
  descriptor.state = BufferFrameState::kResident;

  page_table_[tag] = frame_id;
  replacer_->RecordAccess(frame_id);
  replacer_->SetEvictable(frame_id, false);
  telemetry_sink_->RecordDiskRead(tag);
  return frame_id;
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
         descriptor.tag == handle.tag();
}

}  // namespace telepath
