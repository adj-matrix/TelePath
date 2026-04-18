#include <cassert>
#include <memory>

#include "telepath/replacer/replacer.h"

int main() {
  {
    std::unique_ptr<telepath::Replacer> replacer =
        telepath::MakeTwoQueueReplacer(6);

    replacer->RecordAccess(0);
    replacer->RecordAccess(1);
    replacer->SetEvictable(0, true);
    replacer->SetEvictable(1, true);

    telepath::FrameId victim = telepath::kInvalidFrameId;
    assert(replacer->Victim(&victim));
    assert(victim == 0);
  }

  {
    std::unique_ptr<telepath::Replacer> replacer =
        telepath::MakeTwoQueueReplacer(6);

    replacer->RecordAccess(0);
    replacer->RecordAccess(1);
    replacer->SetEvictable(0, true);
    replacer->SetEvictable(1, true);
    replacer->RecordAccess(0);

    telepath::FrameId victim = telepath::kInvalidFrameId;
    assert(replacer->Victim(&victim));
    // History victims are preferred over cache victims.
    assert(victim == 1);
  }

  {
    std::unique_ptr<telepath::Replacer> replacer =
        telepath::MakeTwoQueueReplacer(6);

    replacer->RecordAccess(0);
    replacer->RecordAccess(0);
    replacer->RecordAccess(1);
    replacer->RecordAccess(1);
    replacer->SetEvictable(0, true);
    replacer->SetEvictable(1, true);
    replacer->RecordAccess(0);

    telepath::FrameId victim = telepath::kInvalidFrameId;
    assert(replacer->Victim(&victim));
    // Cache queue behaves like LRU among promoted pages.
    assert(victim == 1);
  }

  {
    std::unique_ptr<telepath::Replacer> replacer =
        telepath::MakeTwoQueueReplacer(6);

    replacer->RecordAccess(2);
    replacer->SetEvictable(2, true);
    assert(replacer->Size() == 1);
    replacer->SetEvictable(2, false);
    assert(replacer->Size() == 0);
  }

  return 0;
}
