#include <cassert>
#include <cstddef>

#include "page_table.h"
#include "telepath/common/types.h"

namespace {

void AssertInstallLookupAndRemoveRespectFrameId() {
  telepath::BufferManagerPageTable page_table(16, 2);
  const telepath::BufferTag tag{1, 1};
  page_table.Install(tag, 7);

  auto frame_id = page_table.LookupFrameId(tag);
  assert(frame_id.has_value());
  assert(frame_id.value() == 7);

  page_table.Remove(tag, 8);
  frame_id = page_table.LookupFrameId(tag);
  assert(frame_id.has_value());
  assert(frame_id.value() == 7);

  page_table.Remove(tag, 7);
  assert(!page_table.LookupFrameId(tag).has_value());
}

void AssertReplaceWithSingleStripeMovesMapping() {
  telepath::BufferManagerPageTable page_table(16, 1);
  const telepath::BufferTag old_tag{11, 0};
  const telepath::BufferTag new_tag{12, 0};

  page_table.Install(old_tag, 3);
  page_table.Replace(old_tag, new_tag, 3);

  assert(!page_table.LookupFrameId(old_tag).has_value());
  auto frame_id = page_table.LookupFrameId(new_tag);
  assert(frame_id.has_value());
  assert(frame_id.value() == 3);
}

void AssertReplaceAcrossStripesMovesMapping() {
  telepath::BufferManagerPageTable page_table(16, 16);
  const telepath::BufferTag old_tag{21, 0};
  const telepath::BufferTag new_tag{22, 0};

  page_table.Install(old_tag, 9);
  page_table.Replace(old_tag, new_tag, 9);

  assert(!page_table.LookupFrameId(old_tag).has_value());
  auto frame_id = page_table.LookupFrameId(new_tag);
  assert(frame_id.has_value());
  assert(frame_id.value() == 9);
}

void AssertReplaceKeepsOldMappingOwnedByAnotherFrame() {
  telepath::BufferManagerPageTable page_table(16, 4);
  const telepath::BufferTag old_tag{31, 5};
  const telepath::BufferTag new_tag{31, 6};
  page_table.Install(old_tag, 2);
  page_table.Install(old_tag, 4);
  assert(page_table.LookupFrameId(old_tag).value() == 4);

  page_table.Replace(old_tag, new_tag, 2);
  assert(page_table.LookupFrameId(old_tag).value() == 4);
  assert(page_table.LookupFrameId(new_tag).value() == 2);
}

void AssertCollidingTagsShareBucketsCorrectly() {
  telepath::BufferManagerPageTable page_table(8, 2);
  for (telepath::FrameId frame = 0; frame < 8; ++frame) page_table.Install({40, frame}, frame);
  for (telepath::FrameId frame = 0; frame < 8; ++frame) assert(page_table.LookupFrameId({40, frame}).value() == frame);
  for (telepath::FrameId frame = 0; frame < 8; frame += 2) page_table.Remove({40, frame}, frame);
  for (telepath::FrameId frame = 0; frame < 8; ++frame) {
    assert(page_table.LookupFrameId({40, frame}).has_value() == (frame % 2 == 1));
  }
  assert(!page_table.LookupFrameId({41, 0}).has_value());
}

}  // namespace

int main() {
  AssertInstallLookupAndRemoveRespectFrameId();
  AssertReplaceWithSingleStripeMovesMapping();
  AssertReplaceAcrossStripesMovesMapping();
  AssertReplaceKeepsOldMappingOwnedByAnotherFrame();
  AssertCollidingTagsShareBucketsCorrectly();
  return 0;
}
