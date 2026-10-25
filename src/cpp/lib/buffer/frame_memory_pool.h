#ifndef TELEPATH_LIB_BUFFER_FRAME_MEMORY_POOL_H_
#define TELEPATH_LIB_BUFFER_FRAME_MEMORY_POOL_H_

#include <cstdlib>
#include <cstring>

#include "telepath/common/block_geometry.h"
#include "telepath/common/status.h"
#include "telepath/common/types.h"

namespace telepath {

class FrameMemoryPool {
 public:
  FrameMemoryPool(std::size_t pool_size, BlockGeometry geometry)
  : pool_size_(pool_size), geometry_(geometry) {}

  FrameMemoryPool(const FrameMemoryPool &) = delete;
  FrameMemoryPool &operator=(const FrameMemoryPool &) = delete;

  ~FrameMemoryPool() { std::free(data_); }

  Status Initialize() {
    if (pool_size_ == 0) return InvalidArgument("frame memory pool size must be non-zero");
    if (pool_size_ >= kInvalidFrameId) return InvalidArgument("frame memory pool size exceeds the frame id range");
    if (pool_size_ > geometry_.MaxFrameCount()) return InvalidArgument("frame memory pool size overflows the address space");

    const std::size_t arena_size = pool_size_ << geometry_.block_shift();
    void *raw = nullptr;
    if (posix_memalign(&raw, geometry_.block_size(), arena_size) != 0) return ResourceExhausted("failed to allocate contiguous frame memory pool");

    data_ = static_cast<std::byte *>(raw);
    std::memset(data_, 0, arena_size);
    return Ok();
  }

  std::byte *GetFrameData(FrameId frame_id) {
    return data_ + geometry_.FrameOffset(frame_id);
  }

  const std::byte *GetFrameData(FrameId frame_id) const {
    return data_ + geometry_.FrameOffset(frame_id);
  }

 private:
  std::size_t pool_size_{0};
  BlockGeometry geometry_;
  std::byte *data_{nullptr};
};

}  // namespace telepath

#endif  // TELEPATH_LIB_BUFFER_FRAME_MEMORY_POOL_H_
