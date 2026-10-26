#ifndef TELEPATH_REPLACER_SIEVE_REPLACER_H_
#define TELEPATH_REPLACER_SIEVE_REPLACER_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "telepath/replacer/replacer.h"

namespace telepath {

// SIEVE replacer (Zhang et al., NSDI 2024). Frames sit in a queue ordered by
// load time; an access only sets the frame's visited bit (lock-free). The hand
// moves from the oldest frame toward the newest, clearing visited bits and
// nominating the first evictable frame whose bit is clear. Unlike LRU, a hit
// never moves a frame, so frequently visited frames stay put while one-hit
// frames near the hand are evicted quickly.
class SieveReplacer : public Replacer {
 public:
  // Creates a replacer for frame ids in [0, capacity).
  explicit SieveReplacer(std::size_t capacity);
  ~SieveReplacer() override;

  // Sets the frame's visited bit.
  void RecordAccess(FrameId frame_id) override;
  // Moves the frame to the newest end of the queue with its visited bit clear.
  void RecordLoad(FrameId frame_id) override;
  // Advances the hand toward newer frames, clearing visited bits, until it
  // reaches an evictable frame that was not visited.
  bool Victim(const EvictionProbe &probe, FrameId *frame_id) override;
  // Unlinks the frame from the queue.
  void Remove(FrameId frame_id) override;
  auto SharedStateBytes() const -> std::size_t override;
  auto BindSharedState(std::span<std::byte> memory, bool initialize) -> Status override;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace telepath

#endif  // TELEPATH_REPLACER_SIEVE_REPLACER_H_
