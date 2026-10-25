#include <cassert>

#include "replacer_test_support.h"
#include "telepath/replacer/replacer.h"

namespace {

using telepath::replacer_test_support::NextVictim;
using telepath::replacer_test_support::SetProbe;

void AssertFramesBelowKUseOldestAccessOrder() {
  auto replacer = telepath::MakeLruKReplacer(6, 2);
  replacer->RecordLoad(0);
  replacer->RecordLoad(1);
  replacer->RecordLoad(2);
  replacer->RecordAccess(0);
  replacer->RecordAccess(1);
  replacer->RecordLoad(3);

  assert(NextVictim(replacer.get(), SetProbe{}) == 2);
}

void AssertFullyQualifiedFramesUseBackwardKDistance() {
  auto replacer = telepath::MakeLruKReplacer(6, 2);
  replacer->RecordLoad(0);
  replacer->RecordLoad(1);
  replacer->RecordLoad(2);
  replacer->RecordAccess(0);
  replacer->RecordAccess(1);
  replacer->RecordAccess(2);

  assert(NextVictim(replacer.get(), SetProbe{}) == 0);
}

void AssertNonEvictableFramesAreSkipped() {
  auto replacer = telepath::MakeLruKReplacer(6, 2);
  replacer->RecordLoad(0);
  replacer->RecordLoad(1);
  replacer->RecordAccess(0);
  replacer->RecordAccess(1);

  assert(NextVictim(replacer.get(), SetProbe{1}) == 0);
}

void AssertHigherKStillPrefersOlderKthReference() {
  auto replacer = telepath::MakeLruKReplacer(6, 3);
  replacer->RecordLoad(0);
  replacer->RecordAccess(0);
  replacer->RecordLoad(1);
  replacer->RecordAccess(1);
  replacer->RecordAccess(1);
  replacer->RecordAccess(0);
  replacer->RecordAccess(0);

  assert(NextVictim(replacer.get(), SetProbe{}) == 0);
}

}  // namespace

int main() {
  AssertFramesBelowKUseOldestAccessOrder();
  AssertFullyQualifiedFramesUseBackwardKDistance();
  AssertNonEvictableFramesAreSkipped();
  AssertHigherKStillPrefersOlderKthReference();
  return 0;
}
