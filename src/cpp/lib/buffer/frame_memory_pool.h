#ifndef TELEPATH_LIB_BUFFER_FRAME_MEMORY_POOL_H_
#define TELEPATH_LIB_BUFFER_FRAME_MEMORY_POOL_H_

#include <cstdlib>
#include <cstring>

#include "telepath/common/status.h"
#include "telepath/common/types.h"

namespace telepath {

class FrameMemoryPool {
 public:
  FrameMemoryPool(std::size_t pool_size, std::size_t page_size)
  : pool_size_(pool_size), page_size_(page_size) {}

  FrameMemoryPool(const FrameMemoryPool &) = delete;
  FrameMemoryPool &operator=(const FrameMemoryPool &) = delete;

  ~FrameMemoryPool() { std::free(data_); }

  Status Initialize() {
    if (pool_size_ == 0 || page_size_ == 0) return Status::InvalidArgument("frame memory pool size must be non-zero");

    const std::size_t alignment = page_size_ > kCacheLineSize ? page_size_ : kCacheLineSize;
    void *raw = nullptr;
    if (posix_memalign(&raw, alignment, pool_size_ * page_size_) != 0) return Status::ResourceExhausted("failed to allocate contiguous frame memory pool");

    data_ = static_cast<std::byte *>(raw);
    std::memset(data_, 0, pool_size_ * page_size_);
    return Status::Ok();
  }

  std::byte *GetFrameData(FrameId frame_id) {
    return data_ + static_cast<std::size_t>(frame_id) * page_size_;
  }

  const std::byte *GetFrameData(FrameId frame_id) const {
    return data_ + static_cast<std::size_t>(frame_id) * page_size_;
  }

 private:
  std::size_t pool_size_{0};
  std::size_t page_size_{0};
  std::byte *data_{nullptr};
};

}  // namespace telepath

#endif  // TELEPATH_LIB_BUFFER_FRAME_MEMORY_POOL_H_
