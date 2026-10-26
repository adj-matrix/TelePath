#include "telepath/buffer/buffer_manager.h"

#include "frame_control.h"
#include "frame_memory_pool.h"

namespace telepath {

auto BufferManager::MakeSharedHandle(FrameId frame_id, const BufferTag &tag) -> SharedBufferHandle {
  frame_control::AcquireShared(descriptors_[frame_id].control);
  return SharedBufferHandle(this, frame_id, tag, frame_pool_->GetFrameData(frame_id), geometry_.block_size());
}

auto BufferManager::MakeExclusiveHandle(FrameId frame_id, const BufferTag &tag) -> ExclusiveBufferHandle {
  frame_control::AcquireExclusive(descriptors_[frame_id].control);
  return ExclusiveBufferHandle(this, frame_id, tag, frame_pool_->GetFrameData(frame_id), geometry_.block_size());
}

void BufferManager::ReleaseSharedHandle(FrameId frame_id) noexcept {
  frame_control::ReleaseShared(descriptors_[frame_id].control);
  ReleaseFrame(frame_id);
}

void BufferManager::ReleaseExclusiveHandle(FrameId frame_id) noexcept {
  BufferDescriptor &descriptor = descriptors_[frame_id];
  frame_control::ReleaseExclusive(descriptor.control, descriptor.version);
  ReleaseFrame(frame_id);
}

void BufferManager::MarkHandleDirty(FrameId frame_id, Lsn lsn) noexcept {
  const std::optional<bool> became_dirty = frame_control::MarkDirty(descriptors_[frame_id].control);
  frame_control::LowerRecLsn(descriptors_[frame_id].rec_lsn, lsn);
  if (!became_dirty.value_or(false)) return;
  dirty_page_count_->fetch_add(1, std::memory_order_acq_rel);
  NotifyCleaner();
}

void BufferManager::ReleaseFrame(FrameId frame_id) noexcept {
  const std::optional<std::uint32_t> previous = frame_control::Unpin(descriptors_[frame_id].control);
  if (!previous.has_value() || frame_control::Pins(*previous) != 1) return;
  if (!frame_control::IsDirty(*previous)) return;
  MaybeEnqueueCleanerCandidate(frame_id);
  NotifyCleaner();
}

}  // namespace telepath
