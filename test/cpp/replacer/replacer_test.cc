#include <cassert>
#include <memory>

#include "replacer_test_support.h"
#include "telepath/replacer/replacer.h"

namespace {

using telepath::replacer_test_support::NextVictim;
using telepath::replacer_test_support::SetProbe;

void AssertClockReplacerEvictsTrackedFrame() {
  auto replacer = telepath::MakeClockReplacer(3);
  replacer->RecordLoad(0);
  replacer->RecordLoad(1);

  const telepath::FrameId victim = NextVictim(replacer.get(), SetProbe{});
  assert(victim == 0 || victim == 1);
}

void AssertLruReplacerEvictsLeastRecentFrame() {
  auto replacer = telepath::MakeLruReplacer(3);
  replacer->RecordLoad(0);
  replacer->RecordLoad(1);
  replacer->RecordLoad(2);
  replacer->RecordAccess(1);

  assert(NextVictim(replacer.get(), SetProbe{}) == 0);
}

void AssertLruKReplacerEvictsOldestUnderKFrame() {
  auto replacer = telepath::MakeLruKReplacer(4, 2);
  replacer->RecordLoad(0);
  replacer->RecordLoad(1);
  replacer->RecordLoad(2);
  replacer->RecordAccess(1);
  replacer->RecordAccess(2);
  replacer->RecordLoad(3);

  assert(NextVictim(replacer.get(), SetProbe{}) == 0);
}

void AssertLruKReplacerHandlesAllUnderKCase() {
  auto replacer = telepath::MakeLruKReplacer(4, 2);
  replacer->RecordLoad(0);
  replacer->RecordLoad(1);
  replacer->RecordLoad(2);

  assert(NextVictim(replacer.get(), SetProbe{}) == 0);
}

void AssertTwoQueueReplacerEvictsHistoryQueueFirst() {
  auto replacer = telepath::MakeTwoQueueReplacer(4);
  replacer->RecordLoad(0);
  replacer->RecordLoad(1);

  assert(NextVictim(replacer.get(), SetProbe{}) == 0);
}

void AssertEveryPolicyHonorsProbeAndKeepsVictimTracked() {
  for (auto make : {+[] { return telepath::MakeClockReplacer(4); }, +[] { return telepath::MakeClockSweepReplacer(4); },
                    +[] { return telepath::MakeSieveReplacer(4); }, +[] { return telepath::MakeLruReplacer(4); },
                    +[] { return telepath::MakeLruKReplacer(4, 2); }, +[] { return telepath::MakeTwoQueueReplacer(4); }}) {
    auto replacer = make();
    for (telepath::FrameId frame = 0; frame < 3; ++frame) replacer->RecordLoad(frame);

    assert(NextVictim(replacer.get(), SetProbe{0, 1, 2}) == telepath::kInvalidFrameId);
    const telepath::FrameId only = NextVictim(replacer.get(), SetProbe{0, 2});
    assert(only == 1);
    assert(NextVictim(replacer.get(), SetProbe{0, 2}) == 1);

    replacer->Remove(1);
    assert(NextVictim(replacer.get(), SetProbe{0, 2}) == telepath::kInvalidFrameId);
    assert(NextVictim(replacer.get(), SetProbe{3}) != 3);
  }
}

}  // namespace

int main() {
  AssertClockReplacerEvictsTrackedFrame();
  AssertLruReplacerEvictsLeastRecentFrame();
  AssertLruKReplacerEvictsOldestUnderKFrame();
  AssertLruKReplacerHandlesAllUnderKCase();
  AssertTwoQueueReplacerEvictsHistoryQueueFirst();
  AssertEveryPolicyHonorsProbeAndKeepsVictimTracked();
  return 0;
}
