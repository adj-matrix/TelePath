#include <cassert>

#include "telepath/replacer/replacer.h"

namespace {

void AssertHistoryQueueEvictsOldestFrame() {
  auto replacer = telepath::MakeTwoQueueReplacer(6);
  replacer->RecordAccess(0);
  replacer->RecordAccess(1);
  replacer->SetEvictable(0, true);
  replacer->SetEvictable(1, true);

  telepath::FrameId victim = telepath::kInvalidFrameId;
  assert(replacer->Victim(&victim));
  assert(victim == 0);
}

void AssertHistoryQueueBeatsCacheQueueForEviction() {
  auto replacer = telepath::MakeTwoQueueReplacer(6);
  replacer->RecordAccess(0);
  replacer->RecordAccess(1);
  replacer->SetEvictable(0, true);
  replacer->SetEvictable(1, true);
  replacer->RecordAccess(0);

  telepath::FrameId victim = telepath::kInvalidFrameId;
  assert(replacer->Victim(&victim));
  assert(victim == 1);
}

void AssertCacheQueueBehavesLikeLruAmongPromotedFrames() {
  auto replacer = telepath::MakeTwoQueueReplacer(6);
  replacer->RecordAccess(0);
  replacer->RecordAccess(0);
  replacer->RecordAccess(1);
  replacer->RecordAccess(1);
  replacer->SetEvictable(0, true);
  replacer->SetEvictable(1, true);
  replacer->RecordAccess(0);

  telepath::FrameId victim = telepath::kInvalidFrameId;
  assert(replacer->Victim(&victim));
  assert(victim == 1);
}

void AssertEvictableFlagUpdatesTrackedSize() {
  auto replacer = telepath::MakeTwoQueueReplacer(6);
  replacer->RecordAccess(2);
  replacer->SetEvictable(2, true);
  assert(replacer->Size() == 1);

  replacer->SetEvictable(2, false);
  assert(replacer->Size() == 0);
}

}  // namespace

int main() {
  AssertHistoryQueueEvictsOldestFrame();
  AssertHistoryQueueBeatsCacheQueueForEviction();
  AssertCacheQueueBehavesLikeLruAmongPromotedFrames();
  AssertEvictableFlagUpdatesTrackedSize();
  return 0;
}
