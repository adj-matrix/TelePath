#ifndef TELEPATH_BUFFER_BUFFER_ACCESS_STRATEGY_H_
#define TELEPATH_BUFFER_BUFFER_ACCESS_STRATEGY_H_

#include <cstddef>
#include <vector>

#include "telepath/common/types.h"

namespace telepath {

class BufferManager;

// Access patterns with dedicated replacement behavior, modeled after
// PostgreSQL's BufferAccessStrategyType.
enum class BufferAccessStrategyType {
  // Ordinary access; equivalent to passing no strategy.
  kNormal = 0,
  // Large sequential reads: pages loaded through the strategy recycle a small
  // ring of frames, and hits do not raise replacement priority, so a scan does
  // not evict the rest of the cache.
  kBulkRead,
};

// A per-scan access strategy passed to ReadBuffer()/ReadBufferExclusive(),
// modeled after PostgreSQL's BufferAccessStrategy. It only remembers frame ids,
// so it never keeps pages pinned. Not thread-safe: use one strategy per scan
// or per thread.
class BufferAccessStrategy {
 public:
  // Ring size used by kBulkRead when `ring_size` is 0 (PostgreSQL uses 256 KiB
  // worth of buffers).
  static constexpr std::size_t kDefaultBulkReadRingSize = 32;

  explicit BufferAccessStrategy(BufferAccessStrategyType type, std::size_t ring_size = 0);

  auto type() const noexcept -> BufferAccessStrategyType { return type_; }
  // Number of frames the strategy recycles; 0 for kNormal.
  auto ring_size() const noexcept -> std::size_t { return ring_.size(); }

 private:
  friend class BufferManager;

  bool RecordsAccesses() const noexcept { return type_ == BufferAccessStrategyType::kNormal; }
  auto NextRingFrame() -> FrameId;
  void RememberRingFrame(FrameId frame_id);

  BufferAccessStrategyType type_;
  std::vector<FrameId> ring_;
  std::size_t current_{0};
};

}  // namespace telepath

#endif  // TELEPATH_BUFFER_BUFFER_ACCESS_STRATEGY_H_
