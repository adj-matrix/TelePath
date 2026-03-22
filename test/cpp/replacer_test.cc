#include <cassert>
#include <memory>

#include "telepath/replacer/replacer.h"

int main() {
  {
    std::unique_ptr<telepath::Replacer> replacer =
        telepath::MakeClockReplacer(3);
    replacer->RecordAccess(0);
    replacer->RecordAccess(1);
    replacer->SetEvictable(0, true);
    replacer->SetEvictable(1, true);
    telepath::FrameId victim = telepath::kInvalidFrameId;
    assert(replacer->Victim(&victim));
    assert(victim == 0 || victim == 1);
  }

  {
    std::unique_ptr<telepath::Replacer> replacer =
        telepath::MakeLruReplacer(3);
    replacer->RecordAccess(0);
    replacer->RecordAccess(1);
    replacer->RecordAccess(2);
    replacer->SetEvictable(0, true);
    replacer->SetEvictable(1, true);
    replacer->SetEvictable(2, true);
    replacer->RecordAccess(1);
    telepath::FrameId victim = telepath::kInvalidFrameId;
    assert(replacer->Victim(&victim));
    assert(victim == 0);
  }

  {
    std::unique_ptr<telepath::Replacer> replacer =
        telepath::MakeLruKReplacer(4, 2);
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
    telepath::FrameId victim = telepath::kInvalidFrameId;
    assert(replacer->Victim(&victim));
    assert(victim == 0);
  }

  {
    std::unique_ptr<telepath::Replacer> replacer =
        telepath::MakeLruKReplacer(4, 2);
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

  return 0;
}
