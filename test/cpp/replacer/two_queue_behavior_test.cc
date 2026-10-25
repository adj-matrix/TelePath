#include <cassert>

#include "replacer_test_support.h"
#include "telepath/replacer/replacer.h"

namespace {

using telepath::replacer_test_support::NextVictim;
using telepath::replacer_test_support::SetProbe;

void AssertHistoryQueueEvictsOldestFrame() {
  auto replacer = telepath::MakeTwoQueueReplacer(6);
  replacer->RecordLoad(0);
  replacer->RecordLoad(1);

  assert(NextVictim(replacer.get(), SetProbe{}) == 0);
}

void AssertHistoryQueueBeatsCacheQueueForEviction() {
  auto replacer = telepath::MakeTwoQueueReplacer(6);
  replacer->RecordLoad(0);
  replacer->RecordLoad(1);
  replacer->RecordAccess(0);

  assert(NextVictim(replacer.get(), SetProbe{}) == 1);
}

void AssertCacheQueueBehavesLikeLruAmongPromotedFrames() {
  auto replacer = telepath::MakeTwoQueueReplacer(6);
  replacer->RecordLoad(0);
  replacer->RecordAccess(0);
  replacer->RecordLoad(1);
  replacer->RecordAccess(1);
  replacer->RecordAccess(0);

  assert(NextVictim(replacer.get(), SetProbe{}) == 1);
}

void AssertBlockedHistoryFrameFallsBackToCacheQueue() {
  auto replacer = telepath::MakeTwoQueueReplacer(6);
  replacer->RecordLoad(2);
  replacer->RecordLoad(3);
  replacer->RecordAccess(3);

  assert(NextVictim(replacer.get(), SetProbe{2}) == 3);
}

}  // namespace

int main() {
  AssertHistoryQueueEvictsOldestFrame();
  AssertHistoryQueueBeatsCacheQueueForEviction();
  AssertCacheQueueBehavesLikeLruAmongPromotedFrames();
  AssertBlockedHistoryFrameFallsBackToCacheQueue();
  return 0;
}
