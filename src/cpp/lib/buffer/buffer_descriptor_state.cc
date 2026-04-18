#include "buffer_descriptor_state.h"

#include "flush_scheduler.h"
#include "telepath/buffer/buffer_manager.h"

namespace telepath {

namespace buffer_descriptor_state {

bool IsResidentFrameMatch(const BufferDescriptor &descriptor, const BufferTag &tag) {
  if (descriptor.state != BufferFrameState::kResident) return false;
  if (!descriptor.is_valid) return false;
  if (descriptor.io_in_flight) return false;
  if (descriptor.tag != tag) return false;
  return true;
}

bool HasPendingFlush(const BufferDescriptor &descriptor) {
  if (descriptor.flush_queued) return true;
  if (descriptor.flush_in_flight) return true;
  return false;
}

bool CanFlushResidentFrame(const BufferDescriptor &descriptor) {
  if (!descriptor.is_valid) return false;
  if (descriptor.state != BufferFrameState::kResident) return false;
  if (!descriptor.is_dirty) return false;
  return true;
}

bool CleanerMustSkipFlush(const BufferDescriptor &descriptor) {
  if (descriptor.pin_count != 0) return true;
  return false;
}

void ReserveFlushSlot(BufferDescriptor *descriptor, BufferTag *tag, uint64_t *dirty_generation) {
  *tag = descriptor->tag;
  *dirty_generation = descriptor->dirty_generation;
  descriptor->flush_queued = true;
  descriptor->last_flush_status = Status::Ok();
}

bool CanClearDirtyAfterFlush(const BufferDescriptor &descriptor, const BufferManagerFlushTask &task, const Status &flush_status) {
  if (!flush_status.ok()) return false;
  if (!descriptor.is_valid) return false;
  if (descriptor.state != BufferFrameState::kResident) return false;
  if (descriptor.tag != task.tag) return false;
  if (descriptor.dirty_generation != task.generation) return false;
  if (!descriptor.is_dirty) return false;
  return true;
}

bool CanReserveFrame(const BufferDescriptor &descriptor) {
  if (descriptor.pin_count != 0) return false;
  if (descriptor.io_in_flight) return false;
  if (descriptor.flush_queued) return false;
  if (descriptor.flush_in_flight) return false;
  return true;
}

bool ShouldRequeueReservationCandidate(const BufferDescriptor &descriptor) {
  if (descriptor.pin_count != 0) return false;
  if (descriptor.io_in_flight) return true;
  if (descriptor.flush_queued) return true;
  if (descriptor.flush_in_flight) return true;
  return false;
}

auto BuildFrameReservation(FrameId frame_id, const BufferDescriptor &descriptor) -> FrameReservation {
  if (!descriptor.is_valid) return FrameReservation{frame_id};

  return FrameReservation{frame_id, true, descriptor.tag, descriptor.is_dirty, descriptor.dirty_generation};
}

void EnterLoadingState(BufferDescriptor *descriptor, const BufferTag &tag) {
  descriptor->tag = tag;
  descriptor->pin_count = 1;
  descriptor->dirty_generation = 0;
  descriptor->is_dirty = false;
  descriptor->is_valid = false;
  descriptor->io_in_flight = false;
  descriptor->flush_queued = false;
  descriptor->flush_in_flight = false;
  descriptor->last_io_status = Status::Ok();
  descriptor->last_flush_status = Status::Ok();
  descriptor->state = BufferFrameState::kLoading;
}

void EnterResidentState(BufferDescriptor *descriptor) {
  descriptor->pin_count = 1;
  descriptor->dirty_generation = 0;
  descriptor->is_dirty = false;
  descriptor->is_valid = true;
  descriptor->io_in_flight = false;
  descriptor->flush_queued = false;
  descriptor->flush_in_flight = false;
  descriptor->last_io_status = Status::Ok();
  descriptor->last_flush_status = Status::Ok();
  descriptor->state = BufferFrameState::kResident;
}

void EnterFreeState(BufferDescriptor *descriptor, FrameId frame_id, const Status &status) {
  descriptor->frame_id = frame_id;
  descriptor->tag = BufferTag{};
  descriptor->pin_count = 0;
  descriptor->dirty_generation = 0;
  descriptor->is_dirty = false;
  descriptor->is_valid = false;
  descriptor->io_in_flight = false;
  descriptor->flush_queued = false;
  descriptor->flush_in_flight = false;
  descriptor->last_io_status = status;
  descriptor->last_flush_status = Status::Ok();
  descriptor->state = BufferFrameState::kFree;
}

void RestoreEvictedFrameDescriptor(BufferDescriptor *descriptor, const BufferTag &old_tag, bool was_dirty, uint64_t dirty_generation) {
  EnterResidentState(descriptor);
  descriptor->tag = old_tag;
  descriptor->pin_count = 0;
  descriptor->is_dirty = was_dirty;
  descriptor->dirty_generation = dirty_generation;
  descriptor->last_io_status = Status::IoError("eviction flush failed");
  descriptor->io_cv.notify_all();
}

}  // namespace buffer_descriptor_state

}  // namespace telepath
