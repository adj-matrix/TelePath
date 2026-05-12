#include "telepath/buffer/buffer_manager.h"

#include "buffer_descriptor_state.h"
#include "telepath/replacer/replacer.h"

namespace telepath {

auto BufferManager::ValidateOwnedHandle(const BufferHandle &handle) const -> Status {
  if (!init_status_.ok()) return init_status_;
  if (!handle.valid()) return Status::InvalidArgument("buffer handle is not valid");
  if (handle.manager_ != this) return Status::InvalidArgument("buffer handle belongs to another manager");
  if (!ValidateHandle(handle)) return Status::InvalidArgument("invalid buffer handle");
  return Status::Ok();
}

auto BufferManager::ReleaseFrame(FrameId frame_id) -> Status {
  if (frame_id >= pool_size_) return Status::InvalidArgument("invalid frame id");

  Result<bool> unpin_result = UnpinFrame(frame_id);
  if (!unpin_result.ok()) return unpin_result.status();
  if (unpin_result.value()) {
    MaybeEnqueueCleanerCandidate(frame_id);
    NotifyCleaner();
  }
  return Status::Ok();
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

bool BufferManager::ValidateHandle(const BufferHandle &handle) const {
  if (!handle.valid() || handle.manager_ != this || handle.frame_id() >= pool_size_) return false;
  const BufferDescriptor &descriptor = descriptors_[handle.frame_id()];
  std::lock_guard<std::mutex> guard(descriptor.latch);
  return buffer_descriptor_state::IsResidentFrameMatch(descriptor, handle.tag());
}

auto BufferManager::AcquireReadPointer(BufferHandle *handle) const -> const std::byte *{
  if (handle == nullptr || !ValidateHandle(*handle)) return nullptr;
  AcquireReadLatch(handle);
  return handle->data_;
}

auto BufferManager::AcquireWritePointer(BufferHandle *handle) -> std::byte *{
  if (handle == nullptr || !ValidateHandle(*handle)) return nullptr;
  AcquireWriteLatch(handle);
  return handle->data_;
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

}  // namespace telepath
