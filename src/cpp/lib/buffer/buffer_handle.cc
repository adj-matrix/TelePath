#include "telepath/buffer/buffer_handle.h"

#include <utility>

#include "telepath/buffer/buffer_manager.h"

namespace telepath {

SharedBufferHandle::SharedBufferHandle(BufferManager *manager, FrameId frame_id, const BufferTag &tag, std::byte *data, std::size_t size) noexcept
  : manager_(manager), frame_id_(frame_id), tag_(tag), data_(data), size_(size) {}

SharedBufferHandle::SharedBufferHandle(SharedBufferHandle &&other) noexcept
  : manager_(std::exchange(other.manager_, nullptr)),
    frame_id_(std::exchange(other.frame_id_, kInvalidFrameId)),
    tag_(other.tag_),
    data_(std::exchange(other.data_, nullptr)),
    size_(std::exchange(other.size_, 0)) {}

SharedBufferHandle &SharedBufferHandle::operator=(SharedBufferHandle &&other) noexcept {
  if (this == &other) return *this;
  Reset();
  manager_ = std::exchange(other.manager_, nullptr);
  frame_id_ = std::exchange(other.frame_id_, kInvalidFrameId);
  tag_ = other.tag_;
  data_ = std::exchange(other.data_, nullptr);
  size_ = std::exchange(other.size_, 0);
  return *this;
}

SharedBufferHandle::~SharedBufferHandle() { Reset(); }

void SharedBufferHandle::Reset() noexcept {
  if (manager_ == nullptr) return;
  std::exchange(manager_, nullptr)->ReleaseSharedHandle(frame_id_);
  frame_id_ = kInvalidFrameId;
  data_ = nullptr;
  size_ = 0;
}

ExclusiveBufferHandle::ExclusiveBufferHandle(BufferManager *manager, FrameId frame_id, const BufferTag &tag, std::byte *data, std::size_t size) noexcept
  : manager_(manager), frame_id_(frame_id), tag_(tag), data_(data), size_(size) {}

ExclusiveBufferHandle::ExclusiveBufferHandle(ExclusiveBufferHandle &&other) noexcept
  : manager_(std::exchange(other.manager_, nullptr)),
    frame_id_(std::exchange(other.frame_id_, kInvalidFrameId)),
    tag_(other.tag_),
    data_(std::exchange(other.data_, nullptr)),
    size_(std::exchange(other.size_, 0)),
    marked_dirty_(std::exchange(other.marked_dirty_, false)) {}

ExclusiveBufferHandle &ExclusiveBufferHandle::operator=(ExclusiveBufferHandle &&other) noexcept {
  if (this == &other) return *this;
  Reset();
  manager_ = std::exchange(other.manager_, nullptr);
  frame_id_ = std::exchange(other.frame_id_, kInvalidFrameId);
  tag_ = other.tag_;
  data_ = std::exchange(other.data_, nullptr);
  size_ = std::exchange(other.size_, 0);
  marked_dirty_ = std::exchange(other.marked_dirty_, false);
  return *this;
}

ExclusiveBufferHandle::~ExclusiveBufferHandle() { Reset(); }

auto ExclusiveBufferHandle::mutable_data() noexcept -> std::span<std::byte> {
  if (manager_ == nullptr) return {};
  if (!marked_dirty_) {
    manager_->MarkHandleDirty(frame_id_);
    marked_dirty_ = true;
  }
  return {data_, size_};
}

void ExclusiveBufferHandle::MarkDirty(Lsn lsn) noexcept {
  if (manager_ == nullptr) return;
  manager_->MarkHandleDirty(frame_id_, lsn);
  marked_dirty_ = true;
}

void ExclusiveBufferHandle::Reset() noexcept {
  if (manager_ == nullptr) return;
  std::exchange(manager_, nullptr)->ReleaseExclusiveHandle(frame_id_);
  frame_id_ = kInvalidFrameId;
  data_ = nullptr;
  size_ = 0;
  marked_dirty_ = false;
}

}  // namespace telepath
