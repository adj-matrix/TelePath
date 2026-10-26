#ifndef TELEPATH_LIB_BUFFER_FRAME_MEMORY_POOL_H_
#define TELEPATH_LIB_BUFFER_FRAME_MEMORY_POOL_H_

#include <cstddef>

#include "telepath/common/block_geometry.h"
#include "telepath/common/status.h"
#include "telepath/common/types.h"
#include "../common/state_layout.h"

namespace telepath {

// Fixed-size frames placed contiguously in memory owned by the caller.
class FrameMemoryPool {
 public:
  // Alignment of the frame area; page-aligned so frames suit direct I/O.
  static constexpr std::size_t kAlignment = 4096;

  static auto Reserve(StateLayout *layout, std::size_t pool_size, BlockGeometry geometry) -> std::size_t {
    return layout->Add<std::byte>(pool_size << geometry.block_shift(), kAlignment);
  }

  static auto Validate(std::size_t pool_size, BlockGeometry geometry) -> Status {
    if (pool_size == 0) return InvalidArgument("frame memory pool size must be non-zero");
    if (pool_size >= kInvalidFrameId) return InvalidArgument("frame memory pool size exceeds the frame id range");
    if (pool_size > geometry.MaxFrameCount()) return InvalidArgument("frame memory pool size overflows the address space");
    return Ok();
  }

  FrameMemoryPool(BlockGeometry geometry, std::byte *frames) : geometry_(geometry), data_(frames) {}

  FrameMemoryPool(const FrameMemoryPool &) = delete;
  FrameMemoryPool &operator=(const FrameMemoryPool &) = delete;

  std::byte *GetFrameData(FrameId frame_id) {
    return data_ + geometry_.FrameOffset(frame_id);
  }

  const std::byte *GetFrameData(FrameId frame_id) const {
    return data_ + geometry_.FrameOffset(frame_id);
  }

 private:
  BlockGeometry geometry_;
  std::byte *data_{nullptr};
};

}  // namespace telepath

#endif  // TELEPATH_LIB_BUFFER_FRAME_MEMORY_POOL_H_
