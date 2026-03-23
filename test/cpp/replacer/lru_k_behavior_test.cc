#include <cassert>
#include <memory>
#include <vector>

#include "telepath/replacer/replacer.h"

int main() {
  {
    std::unique_ptr<telepath::Replacer> replacer =
        telepath::MakeLruKReplacer(6, 2);

    replacer->RecordAccess(0);  // t1
    replacer->RecordAccess(1);  // t2
    replacer->RecordAccess(2);  // t3
    replacer->RecordAccess(0);  // t4
    replacer->RecordAccess(1);  // t5
    replacer->RecordAccess(3);  // t6

    replacer->SetEvictable(0, true);
    replacer->SetEvictable(1, true);
    replacer->SetEvictable(2, true);
    replacer->SetEvictable(3, true);

    telepath::FrameId victim = telepath::kInvalidFrameId;
    assert(replacer->Victim(&victim));
    // Frame 2 and 3 have fewer than K accesses. Frame 2 is older.
    assert(victim == 2);
  }

  {
    std::unique_ptr<telepath::Replacer> replacer =
        telepath::MakeLruKReplacer(6, 2);

    replacer->RecordAccess(0);  // t1
    replacer->RecordAccess(1);  // t2
    replacer->RecordAccess(2);  // t3
    replacer->RecordAccess(0);  // t4
    replacer->RecordAccess(1);  // t5
    replacer->RecordAccess(2);  // t6

    replacer->SetEvictable(0, true);
    replacer->SetEvictable(1, true);
    replacer->SetEvictable(2, true);

    telepath::FrameId victim = telepath::kInvalidFrameId;
    assert(replacer->Victim(&victim));
    // Compare the 2nd most recent backward distance:
    // frame 0 uses t1/t4, frame 1 uses t2/t5, frame 2 uses t3/t6.
    // frame 0 should be evicted first.
    assert(victim == 0);
  }

  {
    std::unique_ptr<telepath::Replacer> replacer =
        telepath::MakeLruKReplacer(6, 2);

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

  return 0;
}
