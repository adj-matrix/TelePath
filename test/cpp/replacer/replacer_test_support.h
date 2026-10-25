#ifndef TELEPATH_TEST_CPP_REPLACER_REPLACER_TEST_SUPPORT_H_
#define TELEPATH_TEST_CPP_REPLACER_REPLACER_TEST_SUPPORT_H_

#include <initializer_list>
#include <set>

#include "telepath/replacer/replacer.h"

namespace telepath::replacer_test_support {

class SetProbe final : public EvictionProbe {
 public:
  SetProbe() = default;
  SetProbe(std::initializer_list<FrameId> blocked) : blocked_(blocked) {}

  void Block(FrameId frame_id) { blocked_.insert(frame_id); }
  bool IsEvictable(FrameId frame_id) const override { return blocked_.count(frame_id) == 0; }

 private:
  std::set<FrameId> blocked_;
};

inline auto NextVictim(Replacer *replacer, const EvictionProbe &probe) -> FrameId {
  FrameId victim = kInvalidFrameId;
  if (!replacer->Victim(probe, &victim)) return kInvalidFrameId;
  return victim;
}

}  // namespace telepath::replacer_test_support

#endif  // TELEPATH_TEST_CPP_REPLACER_REPLACER_TEST_SUPPORT_H_
