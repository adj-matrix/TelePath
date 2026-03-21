#ifndef TELEPATH_BUFFER_BUFFER_DESCRIPTOR_H_
#define TELEPATH_BUFFER_BUFFER_DESCRIPTOR_H_

#include <mutex>

#include "telepath/common/types.h"

namespace telepath {

enum class BufferFrameState {
  kFree = 0,
  kLoading,
  kResident,
};

struct BufferDescriptor {
  mutable std::mutex latch;
  BufferTag tag{};
  FrameId frame_id{kInvalidFrameId};
  uint32_t pin_count{0};
  bool is_dirty{false};
  bool is_valid{false};
  BufferFrameState state{BufferFrameState::kFree};
};

}  // namespace telepath

#endif  // TELEPATH_BUFFER_BUFFER_DESCRIPTOR_H_
