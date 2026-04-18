#include <cassert>
#include <cstddef>

#include "page_table.h"
#include "telepath/common/types.h"

namespace {

auto StripeForTag(const telepath::BufferTag &tag, std::size_t stripe_count) -> std::size_t {
  return telepath::BufferTagHash{}(tag) % stripe_count;
}

auto FindTagWithStripe(std::size_t stripe, std::size_t stripe_count, telepath::FileId file_id) -> telepath::BufferTag {
  for (telepath::BlockId block_id = 0;; ++block_id) {
    telepath::BufferTag tag{file_id, block_id};
    if (StripeForTag(tag, stripe_count) == stripe) return tag;
  }
}

void AssertInstallLookupAndRemoveRespectFrameId() {
  telepath::BufferManagerPageTable page_table(2);
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

void AssertReplaceWithinSameStripeMovesMapping() {
  telepath::BufferManagerPageTable page_table(2);
  const auto old_tag = FindTagWithStripe(0, 2, 11);
  const auto new_tag = FindTagWithStripe(0, 2, 12);
  assert(old_tag != new_tag);

  page_table.Install(old_tag, 3);
  page_table.Replace(old_tag, new_tag, 3);

  assert(!page_table.LookupFrameId(old_tag).has_value());
  auto frame_id = page_table.LookupFrameId(new_tag);
  assert(frame_id.has_value());
  assert(frame_id.value() == 3);
}

void AssertReplaceAcrossStripesMovesMapping() {
  telepath::BufferManagerPageTable page_table(2);
  const auto old_tag = FindTagWithStripe(0, 2, 21);
  const auto new_tag = FindTagWithStripe(1, 2, 22);

  page_table.Install(old_tag, 9);
  page_table.Replace(old_tag, new_tag, 9);

  assert(!page_table.LookupFrameId(old_tag).has_value());
  auto frame_id = page_table.LookupFrameId(new_tag);
  assert(frame_id.has_value());
  assert(frame_id.value() == 9);
}

}  // namespace

int main() {
  AssertInstallLookupAndRemoveRespectFrameId();
  AssertReplaceWithinSameStripeMovesMapping();
  AssertReplaceAcrossStripesMovesMapping();
  return 0;
}
