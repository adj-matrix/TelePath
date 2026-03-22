#ifndef TELEPATH_BUFFER_BUFFER_DESCRIPTOR_H_
#define TELEPATH_BUFFER_BUFFER_DESCRIPTOR_H_

#include <condition_variable>
#include <mutex>

#include "telepath/common/status.h"
#include "telepath/common/types.h"

namespace telepath {

enum class BufferFrameState {
  kFree = 0,
  kLoading,
  kResident,
};

struct alignas(kCacheLineSize) BufferDescriptor {
  mutable std::condition_variable io_cv;
  mutable std::mutex latch;
  BufferTag tag{};
  FrameId frame_id{kInvalidFrameId};
  uint32_t pin_count{0};
  bool is_dirty{false};
  bool is_valid{false};
  bool io_in_flight{false};
  Status last_io_status{};
  BufferFrameState state{BufferFrameState::kFree};
};

}  // namespace telepath

#endif  // TELEPATH_BUFFER_BUFFER_DESCRIPTOR_H_
