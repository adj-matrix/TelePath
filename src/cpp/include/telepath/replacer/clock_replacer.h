#ifndef TELEPATH_REPLACER_CLOCK_REPLACER_H_
#define TELEPATH_REPLACER_CLOCK_REPLACER_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "telepath/replacer/replacer.h"

namespace telepath {

// Clock (second-chance) replacer with one reference bit per frame. Accesses
// and the sweeping hand are lock-free.
class ClockReplacer : public Replacer {
 public:
  // Creates a replacer for frame ids in [0, capacity).
  explicit ClockReplacer(std::size_t capacity);

  // Sets the frame's reference bit.
  void RecordAccess(FrameId frame_id) override;
  // Tracks the frame with its reference bit set.
  void RecordLoad(FrameId frame_id) override;
  // Sweeps the hand, clearing set reference bits, until it reaches a tracked
  // evictable frame whose bit is clear.
  bool Victim(const EvictionProbe &probe, FrameId *frame_id) override;
  // Stops tracking the frame.
  void Remove(FrameId frame_id) override;

 private:
  static constexpr std::uint8_t kUntracked = 0;
  static constexpr std::uint8_t kClear = 1;
  static constexpr std::uint8_t kReferenced = 2;

  const std::size_t capacity_;
  std::unique_ptr<std::atomic<std::uint8_t>[]> state_;
  std::atomic<std::size_t> hand_{0};
};

}  // namespace telepath

#endif  // TELEPATH_REPLACER_CLOCK_REPLACER_H_
