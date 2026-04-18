#include "telepath/buffer/buffer_manager.h"

#include <tuple>
#include <utility>

namespace telepath {

BufferHandle::BufferHandle(
  BufferManager *manager,
  FrameId frame_id,
  const BufferTag &tag,
  std::byte *data,
  std::size_t size
) : manager_(manager),
    frame_id_(frame_id),
    tag_(tag),
    data_(data),
    size_(size) {}

BufferHandle::BufferHandle(BufferHandle &&other) noexcept
  : manager_(other.manager_),
    frame_id_(other.frame_id_),
    tag_(other.tag_),
    data_(other.data_),
    size_(other.size_),
    read_lock_(std::move(other.read_lock_)),
    write_lock_(std::move(other.write_lock_)) {
  other.manager_ = nullptr;
  other.frame_id_ = kInvalidFrameId;
  other.data_ = nullptr;
  other.size_ = 0;
}

BufferHandle &BufferHandle::operator=(BufferHandle &&other) noexcept {
  if (this == &other) return *this;

  Reset();
  manager_ = other.manager_;
  frame_id_ = other.frame_id_;
  tag_ = other.tag_;
  data_ = other.data_;
  size_ = other.size_;
  read_lock_ = std::move(other.read_lock_);
  write_lock_ = std::move(other.write_lock_);
  other.manager_ = nullptr;
  other.frame_id_ = kInvalidFrameId;
  other.data_ = nullptr;
  other.size_ = 0;
  return *this;
}

BufferHandle::~BufferHandle() { Reset(); }

auto BufferHandle::data() const -> const std::byte * {
  if (manager_ == nullptr) return nullptr;
  return manager_->AcquireReadPointer(const_cast<BufferHandle *>(this));
}

auto BufferHandle::mutable_data() -> std::byte * {
  if (manager_ == nullptr) return nullptr;
  return manager_->AcquireWritePointer(this);
}

void BufferHandle::Reset() {
  if (manager_ == nullptr) return;
  if (read_lock_.owns_lock()) read_lock_.unlock();
  if (write_lock_.owns_lock()) write_lock_.unlock();

  BufferManager *manager = manager_;
  manager_ = nullptr;
  std::ignore = manager->ReleaseFrame(frame_id_);
  frame_id_ = kInvalidFrameId;
  data_ = nullptr;
  size_ = 0;
}

}  // namespace telepath
