#ifndef TELEPATH_BUFFER_BUFFER_DESCRIPTOR_H_
#define TELEPATH_BUFFER_BUFFER_DESCRIPTOR_H_

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <shared_mutex>

#include "telepath/common/status.h"
#include "telepath/common/types.h"

namespace telepath {

enum class BufferFrameState {
  kFree = 0,
  kLoading,
  kResident,
};

struct alignas(kCacheLineSize) BufferDescriptor {
  // Per-frame coordination used by the data plane.
  mutable std::condition_variable io_cv;
  mutable std::mutex latch;
  mutable std::shared_mutex content_latch;

  // Frame identity and ownership.
  BufferTag tag{};
  FrameId frame_id{kInvalidFrameId};

  // Resident page metadata.
  uint32_t pin_count{0};
  uint64_t dirty_generation{0};
  bool is_dirty{false};
  bool is_valid{false};

  // I/O and flush lifecycle flags.
  bool io_in_flight{false};
  bool flush_queued{false};
  bool flush_in_flight{false};
  Status last_io_status{};
  Status last_flush_status{};

  // Residency state machine.
  BufferFrameState state{BufferFrameState::kFree};
};

}  // namespace telepath

#endif  // TELEPATH_BUFFER_BUFFER_DESCRIPTOR_H_
