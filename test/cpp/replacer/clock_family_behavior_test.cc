#include <cassert>

#include "replacer_test_support.h"
#include "telepath/replacer/replacer.h"

namespace {

using telepath::replacer_test_support::NextVictim;
using telepath::replacer_test_support::SetProbe;

void AssertClockGivesReferencedFramesASecondChance() {
  auto replacer = telepath::MakeClockReplacer(2);
  replacer->RecordLoad(0);
  replacer->RecordLoad(1);
  assert(NextVictim(replacer.get(), SetProbe{}) == 0);

  replacer->RecordAccess(0);
  assert(NextVictim(replacer.get(), SetProbe{}) == 1);
}

void AssertClockSweepEvictsLowestUsageFirst() {
  auto replacer = telepath::MakeClockSweepReplacer(2);
  replacer->RecordLoad(0);
  replacer->RecordLoad(1);
  for (int access = 0; access < 4; ++access) replacer->RecordAccess(0);

  assert(NextVictim(replacer.get(), SetProbe{}) == 1);
}

void AssertClockSweepDecaysSaturatedUsageUntilEvictable() {
  auto replacer = telepath::MakeClockSweepReplacer(2);
  replacer->RecordLoad(0);
  replacer->RecordLoad(1);
  for (int access = 0; access < 20; ++access) replacer->RecordAccess(0);

  assert(NextVictim(replacer.get(), SetProbe{1}) == 0);
}

void AssertSieveKeepsVisitedFramesAndEvictsOneHitFrames() {
  auto replacer = telepath::MakeSieveReplacer(4);
  for (telepath::FrameId frame = 0; frame < 4; ++frame) replacer->RecordLoad(frame);
  replacer->RecordAccess(0);
  replacer->RecordAccess(1);

  assert(NextVictim(replacer.get(), SetProbe{}) == 2);
  assert(NextVictim(replacer.get(), SetProbe{}) == 3);
  assert(NextVictim(replacer.get(), SetProbe{}) == 0);
}

void AssertSieveHandWrapsFromNewestToOldest() {
  auto replacer = telepath::MakeSieveReplacer(3);
  for (telepath::FrameId frame = 0; frame < 3; ++frame) replacer->RecordLoad(frame);
  replacer->RecordAccess(0);

  assert(NextVictim(replacer.get(), SetProbe{}) == 1);
  assert(NextVictim(replacer.get(), SetProbe{}) == 2);
  assert(NextVictim(replacer.get(), SetProbe{}) == 0);
}

void AssertSieveReloadMovesFrameToNewestEnd() {
  auto replacer = telepath::MakeSieveReplacer(3);
  for (telepath::FrameId frame = 0; frame < 3; ++frame) replacer->RecordLoad(frame);
  replacer->RecordLoad(0);

  assert(NextVictim(replacer.get(), SetProbe{}) == 1);
  assert(NextVictim(replacer.get(), SetProbe{}) == 2);
  assert(NextVictim(replacer.get(), SetProbe{}) == 0);
}

}  // namespace

int main() {
  AssertClockGivesReferencedFramesASecondChance();
  AssertClockSweepEvictsLowestUsageFirst();
  AssertClockSweepDecaysSaturatedUsageUntilEvictable();
  AssertSieveKeepsVisitedFramesAndEvictsOneHitFrames();
  AssertSieveHandWrapsFromNewestToOldest();
  AssertSieveReloadMovesFrameToNewestEnd();
  return 0;
}
