#ifndef TELEPATH_REPLACER_CLOCK_SWEEP_REPLACER_H_
#define TELEPATH_REPLACER_CLOCK_SWEEP_REPLACER_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "telepath/replacer/replacer.h"

namespace telepath {

// PostgreSQL-style clock-sweep (generalized clock) replacer. Each frame has a
// usage count that accesses raise up to kMaxUsageCount and the sweeping hand
// lowers by one per pass; the hand evicts the first evictable frame it finds
// at zero. Accesses and the hand are lock-free.
class ClockSweepReplacer : public Replacer {
 public:
  static constexpr std::uint8_t kMaxUsageCount = 5;

  // Creates a replacer for frame ids in [0, capacity).
  explicit ClockSweepReplacer(std::size_t capacity);

  // Raises the frame's usage count by one, saturating at kMaxUsageCount.
  void RecordAccess(FrameId frame_id) override;
  // Tracks the frame with a usage count of one.
  void RecordLoad(FrameId frame_id) override;
  // Sweeps the hand, decrementing non-zero usage counts of evictable frames,
  // until it reaches a tracked evictable frame at zero.
  bool Victim(const EvictionProbe &probe, FrameId *frame_id) override;
  // Stops tracking the frame.
  void Remove(FrameId frame_id) override;

 private:
  static constexpr std::uint8_t kUntracked = 0xFF;

  const std::size_t capacity_;
  std::unique_ptr<std::atomic<std::uint8_t>[]> usage_count_;
  std::atomic<std::size_t> hand_{0};
};

}  // namespace telepath

#endif  // TELEPATH_REPLACER_CLOCK_SWEEP_REPLACER_H_
