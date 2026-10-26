#ifndef TELEPATH_REPLACER_PLAN_REPLACER_H_
#define TELEPATH_REPLACER_PLAN_REPLACER_H_

#include <cstddef>
#include <cstdint>
#include <memory>

#include "telepath/replacer/replacer.h"

namespace telepath {

// Belady replacer for workloads whose future accesses are known in advance,
// such as an epoch of ML training. The owner tells it, for each frame, the
// plan position at which the frame's page is next needed; Victim() evicts the
// evictable frame needed furthest in the future, preferring pages that are
// not needed again. A newly loaded frame counts as needed immediately until
// its next use is set, so prefetched pages are kept until they are consumed.
class PlanReplacer : public Replacer {
 public:
  // Next-use value of a page that the current plan does not need again.
  static constexpr std::uint64_t kNeverUsed = UINT64_MAX;

  // Creates a replacer for frame ids in [0, capacity).
  explicit PlanReplacer(std::size_t capacity);
  ~PlanReplacer() override;

  // Ignored: plan positions, not recency, order the frames.
  void RecordAccess(FrameId frame_id) override;
  // Tracks the frame as needed immediately.
  void RecordLoad(FrameId frame_id) override;
  // Nominates the evictable frame with the largest next use.
  bool Victim(const EvictionProbe &probe, FrameId *frame_id) override;
  // Stops tracking the frame.
  void Remove(FrameId frame_id) override;
  auto SharedStateBytes() const -> std::size_t override;
  auto BindSharedState(std::span<std::byte> memory, bool initialize) -> Status override;

  // Sets the plan position at which the tracked frame is next needed, or
  // kNeverUsed. Ignored for an untracked frame.
  void SetNextUse(FrameId frame_id, std::uint64_t next_use);
  // Marks every tracked frame as not needed again, for a new plan.
  void ForgetPlan();

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace telepath

#endif  // TELEPATH_REPLACER_PLAN_REPLACER_H_
