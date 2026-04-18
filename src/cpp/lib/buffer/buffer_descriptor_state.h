#ifndef TELEPATH_LIB_BUFFER_BUFFER_DESCRIPTOR_STATE_H_
#define TELEPATH_LIB_BUFFER_BUFFER_DESCRIPTOR_STATE_H_

#include "telepath/buffer/buffer_descriptor.h"

namespace telepath {

struct BufferManagerFlushTask;
struct FrameReservation;

namespace buffer_descriptor_state {

bool IsResidentFrameMatch(
    const BufferDescriptor &descriptor,
    const BufferTag &tag);
bool HasPendingFlush(const BufferDescriptor &descriptor);
bool CanFlushResidentFrame(const BufferDescriptor &descriptor);
bool CleanerMustSkipFlush(const BufferDescriptor &descriptor);
void ReserveFlushSlot(
    BufferDescriptor *descriptor,
    BufferTag *tag,
    uint64_t *dirty_generation);
bool CanClearDirtyAfterFlush(
    const BufferDescriptor &descriptor,
    const BufferManagerFlushTask &task,
    const Status &flush_status);
bool CanReserveFrame(const BufferDescriptor &descriptor);
bool ShouldRequeueReservationCandidate(const BufferDescriptor &descriptor);
auto BuildFrameReservation(FrameId frame_id, const BufferDescriptor &descriptor)
  -> FrameReservation;
void EnterLoadingState(BufferDescriptor *descriptor, const BufferTag &tag);
void EnterResidentState(BufferDescriptor *descriptor);
void EnterFreeState(
    BufferDescriptor *descriptor,
    FrameId frame_id,
    const Status &status);
void RestoreEvictedFrameDescriptor(
    BufferDescriptor *descriptor,
    const BufferTag &old_tag, bool was_dirty,
    uint64_t dirty_generation);

}  // namespace buffer_descriptor_state

}  // namespace telepath

#endif  // TELEPATH_LIB_BUFFER_BUFFER_DESCRIPTOR_STATE_H_
