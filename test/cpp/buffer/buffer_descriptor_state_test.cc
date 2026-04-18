#include <cassert>

#include "buffer_descriptor_state.h"
#include "flush_scheduler.h"
#include "telepath/buffer/buffer_manager.h"

namespace {

void AssertReserveFlushSlotCapturesMetadataAndMarksQueued() {
  telepath::BufferDescriptor descriptor;
  descriptor.tag = {2, 11};
  descriptor.dirty_generation = 9;
  descriptor.flush_queued = false;
  descriptor.last_flush_status = telepath::Status::IoError("old error");

  telepath::BufferTag reserved_tag;
  uint64_t reserved_generation = 0;
  telepath::buffer_descriptor_state::ReserveFlushSlot(&descriptor, &reserved_tag, &reserved_generation);

  assert(descriptor.flush_queued);
  assert(descriptor.last_flush_status.ok());
  assert(reserved_tag == descriptor.tag);
  assert(reserved_generation == 9);
}

void AssertCanClearDirtyAfterFlushRequiresMatchingResidentState() {
  telepath::BufferDescriptor descriptor;
  descriptor.tag = {4, 19};
  descriptor.state = telepath::BufferFrameState::kResident;
  descriptor.is_valid = true;
  descriptor.is_dirty = true;
  descriptor.dirty_generation = 7;

  telepath::BufferManagerFlushTask task;
  task.tag = descriptor.tag;
  task.generation = 7;
  assert(telepath::buffer_descriptor_state::CanClearDirtyAfterFlush(descriptor, task, telepath::Status::Ok()));

  task.generation = 8;
  assert(!telepath::buffer_descriptor_state::CanClearDirtyAfterFlush(descriptor, task, telepath::Status::Ok()));

  task.generation = 7;
  task.tag = {4, 20};
  assert(!telepath::buffer_descriptor_state::CanClearDirtyAfterFlush(descriptor, task, telepath::Status::Ok()));

  task.tag = descriptor.tag;
  assert(!telepath::buffer_descriptor_state::CanClearDirtyAfterFlush(descriptor, task, telepath::Status::IoError("flush failed")));
}

void AssertBuildFrameReservationReflectsEvictedPageMetadata() {
  telepath::BufferDescriptor descriptor;
  descriptor.is_valid = true;
  descriptor.tag = {6, 13};
  descriptor.is_dirty = true;
  descriptor.dirty_generation = 5;

  auto reservation = telepath::buffer_descriptor_state::BuildFrameReservation(12, descriptor);
  assert(reservation.frame_id == 12);
  assert(reservation.had_evicted_page);
  assert(reservation.evicted_tag == descriptor.tag);
  assert(reservation.evicted_dirty);
  assert(reservation.evicted_dirty_generation == 5);

  descriptor.is_valid = false;
  reservation = telepath::buffer_descriptor_state::BuildFrameReservation(12, descriptor);
  assert(reservation.frame_id == 12);
  assert(!reservation.had_evicted_page);
}

void AssertStateTransitionsResetAndRestoreDescriptorFields() {
  telepath::BufferDescriptor descriptor;
  const telepath::BufferTag loading_tag{8, 21};
  const telepath::BufferTag restored_tag{9, 31};
  telepath::buffer_descriptor_state::EnterLoadingState(&descriptor, {8, 21});
  assert(descriptor.state == telepath::BufferFrameState::kLoading);
  assert(descriptor.tag == loading_tag);
  assert(descriptor.pin_count == 1);
  assert(!descriptor.is_valid);

  telepath::buffer_descriptor_state::EnterResidentState(&descriptor);
  assert(descriptor.state == telepath::BufferFrameState::kResident);
  assert(descriptor.pin_count == 1);
  assert(descriptor.is_valid);
  assert(!descriptor.is_dirty);

  telepath::buffer_descriptor_state::EnterFreeState(&descriptor, 4, telepath::Status::IoError("load failed"));
  assert(descriptor.state == telepath::BufferFrameState::kFree);
  assert(descriptor.frame_id == 4);
  assert(descriptor.pin_count == 0);
  assert(!descriptor.is_valid);
  assert(descriptor.last_io_status.code() == telepath::StatusCode::kIoError);

  telepath::buffer_descriptor_state::RestoreEvictedFrameDescriptor(&descriptor, restored_tag, true, 14);
  assert(descriptor.state == telepath::BufferFrameState::kResident);
  assert(descriptor.tag == restored_tag);
  assert(descriptor.pin_count == 0);
  assert(descriptor.is_valid);
  assert(descriptor.is_dirty);
  assert(descriptor.dirty_generation == 14);
  assert(descriptor.last_io_status.code() == telepath::StatusCode::kIoError);
}

}  // namespace

int main() {
  AssertReserveFlushSlotCapturesMetadataAndMarksQueued();
  AssertCanClearDirtyAfterFlushRequiresMatchingResidentState();
  AssertBuildFrameReservationReflectsEvictedPageMetadata();
  AssertStateTransitionsResetAndRestoreDescriptorFields();
  return 0;
}
