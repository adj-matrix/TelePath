#ifndef TELEPATH_LIB_BUFFER_KERNEL_REGION_H_
#define TELEPATH_LIB_BUFFER_KERNEL_REGION_H_

#include <atomic>
#include <cstddef>
#include <new>

#include "frame_free_list.h"
#include "frame_memory_pool.h"
#include "page_table.h"
#include "telepath/buffer/buffer_descriptor.h"
#include "telepath/common/block_geometry.h"
#include "../common/state_layout.h"

namespace telepath {

// Counters every process attached to the kernel updates.
struct KernelShared {
  std::atomic<std::size_t> dirty_page_count{0};
};

// Where each piece of kernel metadata and the frames live inside one region.
struct KernelLayout {
  std::size_t shared{0};
  std::size_t descriptors{0};
  BufferManagerPageTable::Offsets page_table{};
  FrameFreeList::Offsets free_list{};
  std::size_t frames{0};
  std::size_t bytes{0};

  static auto For(std::size_t pool_size, std::size_t stripe_count, BlockGeometry geometry) -> KernelLayout {
    StateLayout layout;
    KernelLayout result;
    result.shared = layout.Add<KernelShared>(1);
    result.descriptors = layout.Add<BufferDescriptor>(pool_size);
    result.page_table = BufferManagerPageTable::Reserve(&layout, pool_size, stripe_count);
    result.free_list = FrameFreeList::Reserve(&layout, pool_size);
    result.frames = FrameMemoryPool::Reserve(&layout, pool_size, geometry);
    result.bytes = layout.size();
    return result;
  }
};

// One block holding kernel metadata and frames, aligned for the frame area.
class KernelRegion {
 public:
  static constexpr std::size_t kAlignment = FrameMemoryPool::kAlignment;

  explicit KernelRegion(std::size_t bytes)
    : base_(static_cast<std::byte *>(::operator new(bytes, std::align_val_t{kAlignment}, std::nothrow))), bytes_(bytes) {}
  ~KernelRegion() { ::operator delete(base_, std::align_val_t{kAlignment}); }

  KernelRegion(const KernelRegion &) = delete;
  KernelRegion &operator=(const KernelRegion &) = delete;

  auto base() const -> std::byte * { return base_; }
  auto size() const -> std::size_t { return bytes_; }

 private:
  std::byte *base_;
  std::size_t bytes_;
};

}  // namespace telepath

#endif  // TELEPATH_LIB_BUFFER_KERNEL_REGION_H_
