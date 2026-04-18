#include <cassert>

#include "telepath/replacer/replacer.h"

namespace {

void AssertFramesBelowKUseOldestAccessOrder() {
  auto replacer = telepath::MakeLruKReplacer(6, 2);
  replacer->RecordAccess(0);
  replacer->RecordAccess(1);
  replacer->RecordAccess(2);
  replacer->RecordAccess(0);
  replacer->RecordAccess(1);
  replacer->RecordAccess(3);
  replacer->SetEvictable(0, true);
  replacer->SetEvictable(1, true);
  replacer->SetEvictable(2, true);
  replacer->SetEvictable(3, true);

  telepath::FrameId victim = telepath::kInvalidFrameId;
  assert(replacer->Victim(&victim));
  assert(victim == 2);
}

void AssertFullyQualifiedFramesUseBackwardKDistance() {
  auto replacer = telepath::MakeLruKReplacer(6, 2);
  replacer->RecordAccess(0);
  replacer->RecordAccess(1);
  replacer->RecordAccess(2);
  replacer->RecordAccess(0);
  replacer->RecordAccess(1);
  replacer->RecordAccess(2);
  replacer->SetEvictable(0, true);
  replacer->SetEvictable(1, true);
  replacer->SetEvictable(2, true);

  telepath::FrameId victim = telepath::kInvalidFrameId;
  assert(replacer->Victim(&victim));
  assert(victim == 0);
}

void AssertNonEvictableFramesAreSkipped() {
  auto replacer = telepath::MakeLruKReplacer(6, 2);
  replacer->RecordAccess(0);
  replacer->RecordAccess(1);
  replacer->RecordAccess(0);
  replacer->RecordAccess(1);
  replacer->SetEvictable(0, true);
  replacer->SetEvictable(1, false);

  telepath::FrameId victim = telepath::kInvalidFrameId;
  assert(replacer->Victim(&victim));
  assert(victim == 0);
  assert(replacer->Size() == 0);
}

void AssertHigherKStillPrefersOlderKthReference() {
  auto replacer = telepath::MakeLruKReplacer(6, 3);
  replacer->RecordAccess(0);
  replacer->RecordAccess(0);
  replacer->RecordAccess(1);
  replacer->RecordAccess(1);
  replacer->RecordAccess(1);
  replacer->RecordAccess(0);
  replacer->RecordAccess(0);
  replacer->SetEvictable(0, true);
  replacer->SetEvictable(1, true);

  telepath::FrameId victim = telepath::kInvalidFrameId;
  assert(replacer->Victim(&victim));
  assert(victim == 0);
}

}  // namespace

int main() {
  AssertFramesBelowKUseOldestAccessOrder();
  AssertFullyQualifiedFramesUseBackwardKDistance();
  AssertNonEvictableFramesAreSkipped();
  AssertHigherKStillPrefersOlderKthReference();
  return 0;
}
