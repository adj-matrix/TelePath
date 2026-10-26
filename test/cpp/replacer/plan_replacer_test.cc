#include <cassert>
#include <set>
#include <utility>

#include "telepath/replacer/plan_replacer.h"

namespace {

class SetProbe : public telepath::EvictionProbe {
 public:
  explicit SetProbe(std::set<telepath::FrameId> blocked = {}) : blocked_(std::move(blocked)) {}
  bool IsEvictable(telepath::FrameId frame_id) const override { return blocked_.count(frame_id) == 0; }

 private:
  std::set<telepath::FrameId> blocked_;
};

void ExpectFurthestNextUseIsEvicted() {
  telepath::PlanReplacer replacer(4);
  for (telepath::FrameId frame = 0; frame < 4; ++frame) replacer.RecordLoad(frame);
  replacer.SetNextUse(0, 10);
  replacer.SetNextUse(1, 30);
  replacer.SetNextUse(2, 20);
  // Frame 3 keeps the "needed now" value of a fresh load.
  telepath::FrameId victim = telepath::kInvalidFrameId;
  assert(replacer.Victim(SetProbe(), &victim) && victim == 1);
  assert(replacer.Victim(SetProbe({1}), &victim) && victim == 2);

  replacer.SetNextUse(0, telepath::PlanReplacer::kNeverUsed);
  assert(replacer.Victim(SetProbe(), &victim) && victim == 0);
  assert(replacer.Victim(SetProbe({0, 1, 2}), &victim) && victim == 3);
  assert(!replacer.Victim(SetProbe({0, 1, 2, 3}), &victim));
}

void ExpectRemoveAndForgetPlan() {
  telepath::PlanReplacer replacer(3);
  replacer.RecordLoad(0);
  replacer.RecordLoad(1);
  replacer.SetNextUse(0, 5);
  replacer.SetNextUse(1, 7);
  replacer.SetNextUse(2, 99);
  replacer.Remove(1);
  telepath::FrameId victim = telepath::kInvalidFrameId;
  assert(replacer.Victim(SetProbe(), &victim) && victim == 0);

  replacer.RecordLoad(2);
  replacer.ForgetPlan();
  replacer.SetNextUse(2, 1);
  assert(replacer.Victim(SetProbe(), &victim) && victim == 0);
}

}  // namespace

int main() {
  ExpectFurthestNextUseIsEvicted();
  ExpectRemoveAndForgetPlan();
  return 0;
}
