#include <cassert>
#include <memory>

#include "telepath/replacer/replacer.h"

namespace {

void AssertClockReplacerEvictsTrackedFrame() {
  auto replacer = telepath::MakeClockReplacer(3);
  replacer->RecordAccess(0);
  replacer->RecordAccess(1);
  replacer->SetEvictable(0, true);
  replacer->SetEvictable(1, true);
  assert(replacer->Size() == 2);

  telepath::FrameId victim = telepath::kInvalidFrameId;
  assert(replacer->Victim(&victim));
  assert(victim == 0 || victim == 1);
  assert(replacer->Size() == 1);
}

void AssertLruReplacerEvictsLeastRecentFrame() {
  auto replacer = telepath::MakeLruReplacer(3);
  replacer->RecordAccess(0);
  replacer->RecordAccess(1);
  replacer->RecordAccess(2);
  replacer->SetEvictable(0, true);
  replacer->SetEvictable(1, true);
  replacer->SetEvictable(2, true);
  assert(replacer->Size() == 3);

  replacer->RecordAccess(1);
  telepath::FrameId victim = telepath::kInvalidFrameId;
  assert(replacer->Victim(&victim));
  assert(victim == 0);
  assert(replacer->Size() == 2);
}

void AssertLruKReplacerEvictsOldestUnderKFrame() {
  auto replacer = telepath::MakeLruKReplacer(4, 2);
  replacer->RecordAccess(0);
  replacer->RecordAccess(1);
  replacer->RecordAccess(2);
  replacer->RecordAccess(1);
  replacer->RecordAccess(2);
  replacer->RecordAccess(3);
  replacer->SetEvictable(0, true);
  replacer->SetEvictable(1, true);
  replacer->SetEvictable(2, true);
  replacer->SetEvictable(3, true);
  assert(replacer->Size() == 4);

  telepath::FrameId victim = telepath::kInvalidFrameId;
  assert(replacer->Victim(&victim));
  assert(victim == 0);
  assert(replacer->Size() == 3);
}

void AssertLruKReplacerHandlesAllUnderKCase() {
  auto replacer = telepath::MakeLruKReplacer(4, 2);
  replacer->RecordAccess(0);
  replacer->RecordAccess(1);
  replacer->RecordAccess(2);
  replacer->SetEvictable(0, true);
  replacer->SetEvictable(1, true);
  replacer->SetEvictable(2, true);
  assert(replacer->Size() == 3);

  telepath::FrameId victim = telepath::kInvalidFrameId;
  assert(replacer->Victim(&victim));
  assert(victim == 0);
  assert(replacer->Size() == 2);
}

void AssertTwoQueueReplacerEvictsHistoryQueueFirst() {
  auto replacer = telepath::MakeTwoQueueReplacer(4);
  replacer->RecordAccess(0);
  replacer->RecordAccess(1);
  replacer->SetEvictable(0, true);
  replacer->SetEvictable(1, true);
  assert(replacer->Size() == 2);

  telepath::FrameId victim = telepath::kInvalidFrameId;
  assert(replacer->Victim(&victim));
  assert(victim == 0);
  assert(replacer->Size() == 1);
}

}  // namespace

int main() {
  AssertClockReplacerEvictsTrackedFrame();
  AssertLruReplacerEvictsLeastRecentFrame();
  AssertLruKReplacerEvictsOldestUnderKFrame();
  AssertLruKReplacerHandlesAllUnderKCase();
  AssertTwoQueueReplacerEvictsHistoryQueueFirst();
  return 0;
}
