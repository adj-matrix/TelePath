#ifndef TELEPATH_REPLACER_CLOCK_REPLACER_H_
#define TELEPATH_REPLACER_CLOCK_REPLACER_H_

#include <mutex>
#include <vector>

#include "telepath/replacer/replacer.h"

namespace telepath {

// Array-backed Clock/Second-Chance replacer.
class ClockReplacer : public Replacer {
 public:
  // Creates a replacer that can track up to `capacity` frame ids.
  explicit ClockReplacer(std::size_t capacity);

  // Sets the reference bit for the accessed frame, inserting tracking state on
  // first use when the frame id is valid.
  void RecordAccess(FrameId frame_id) override;
  // Toggles whether the frame may currently be chosen as a victim.
  void SetEvictable(FrameId frame_id, bool evictable) override;
  // Advances the clock hand until it finds and removes one evictable victim.
  bool Victim(FrameId *frame_id) override;
  // Stops tracking the given frame if it is present.
  void Remove(FrameId frame_id) override;
  // Returns the current number of evictable tracked frames.
  auto Size() const -> std::size_t override;

 private:
  struct Entry {
    bool referenced{false};
    bool evictable{false};
    bool present{false};
  };

  bool IsValidFrame(FrameId frame_id) const;
  bool TryVictimAtHand(FrameId *frame_id);
  void AdvanceHand();

  const std::size_t capacity_;
  mutable std::mutex latch_;
  std::vector<Entry> entries_;
  std::size_t hand_{0};
  std::size_t size_{0};
};

}  // namespace telepath

#endif  // TELEPATH_REPLACER_CLOCK_REPLACER_H_
