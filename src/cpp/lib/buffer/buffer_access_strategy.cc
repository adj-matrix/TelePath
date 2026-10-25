#include "telepath/buffer/buffer_access_strategy.h"

namespace telepath {

BufferAccessStrategy::BufferAccessStrategy(BufferAccessStrategyType type, std::size_t ring_size) : type_(type) {
  if (type_ == BufferAccessStrategyType::kNormal) return;
  ring_.assign(ring_size == 0 ? kDefaultBulkReadRingSize : ring_size, kInvalidFrameId);
}

auto BufferAccessStrategy::NextRingFrame() -> FrameId {
  if (ring_.empty()) return kInvalidFrameId;
  current_ = (current_ + 1) % ring_.size();
  return ring_[current_];
}

void BufferAccessStrategy::RememberRingFrame(FrameId frame_id) {
  if (ring_.empty()) return;
  ring_[current_] = frame_id;
}

}  // namespace telepath
