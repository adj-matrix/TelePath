#ifndef TELEPATH_BUFFER_BUFFER_DESCRIPTOR_H_
#define TELEPATH_BUFFER_BUFFER_DESCRIPTOR_H_

#include <atomic>
#include <cstdint>

#include "telepath/common/types.h"

namespace telepath {

// Residency state of a frame, as reported in FrameSnapshot.
enum class BufferFrameState {
  kFree = 0,
  kLoading,
  kResident,
};

// Per-frame metadata. All synchronization goes through `control`, a 32-bit
// word holding state, flags, the content latch, and the pin count; its layout
// is documented in docs/design/kernel-refactor.md. Every field is either an
// atomic or written only before the frame is published, so the struct contains
// no pointers or heap-owning members and fits in one cache line.
struct alignas(kCacheLineSize) BufferDescriptor {
  std::atomic<std::uint32_t> control{0};
  // Packed ErrorCode and errno of the most recent writeback; 0 when it succeeded.
  std::atomic<std::uint32_t> last_writeback_error{0};
  std::atomic<std::uint64_t> version{0};
  std::atomic<FileId> tag_file_id{0};
  FrameId frame_id{kInvalidFrameId};
  std::atomic<BlockId> tag_block_id{0};
  // LSN of the first logged change since the page's last writeback snapshot,
  // or kInvalidLsn.
  std::atomic<Lsn> rec_lsn{kInvalidLsn};
  // rec_lsn as of the snapshot of the writeback in flight, or kInvalidLsn.
  std::atomic<Lsn> writeback_rec_lsn{kInvalidLsn};
  // Reserved for the multi-tenant policy module.
  std::uint16_t tenant_id{0};
  // Reserved for cross-process pin ownership.
  std::uint32_t holder_slot{0};
};

static_assert(sizeof(BufferDescriptor) == kCacheLineSize, "BufferDescriptor must occupy exactly one cache line");

}  // namespace telepath

#endif  // TELEPATH_BUFFER_BUFFER_DESCRIPTOR_H_
