#include "page_table.h"

namespace telepath {

BufferManagerPageTable::BufferManagerPageTable(std::size_t stripe_count)
  : latches_(stripe_count == 0 ? 1 : stripe_count),
    shards_(latches_.size()) {}

auto BufferManagerPageTable::LookupFrameId(const BufferTag &tag) -> std::optional<FrameId> {
  const std::size_t stripe = GetStripe(tag);
  std::lock_guard<std::mutex> page_table_guard(latches_[stripe]);
  const auto &shard = shards_[stripe];
  auto it = shard.find(tag);
  if (it == shard.end()) return std::nullopt;
  return it->second;
}

void BufferManagerPageTable::Install(const BufferTag &tag, FrameId frame_id) {
  const std::size_t stripe = GetStripe(tag);
  std::lock_guard<std::mutex> page_table_guard(latches_[stripe]);
  shards_[stripe][tag] = frame_id;
}

void BufferManagerPageTable::Remove(const BufferTag &tag, FrameId frame_id) {
  const std::size_t stripe = GetStripe(tag);
  std::lock_guard<std::mutex> page_table_guard(latches_[stripe]);
  auto &shard = shards_[stripe];
  auto it = shard.find(tag);
  if (it == shard.end() || it->second != frame_id) return;
  shard.erase(it);
}

void BufferManagerPageTable::Replace(const BufferTag &old_tag, const BufferTag &new_tag, FrameId frame_id) {
  const std::size_t old_stripe = GetStripe(old_tag);
  const std::size_t new_stripe = GetStripe(new_tag);
  if (old_stripe == new_stripe) {
    ReplaceInStripe(old_stripe, old_tag, new_tag, frame_id);
    return;
  }
  ReplaceAcrossStripes(old_stripe, old_tag, new_stripe, new_tag, frame_id);
}

auto BufferManagerPageTable::GetStripe(const BufferTag &tag) const -> std::size_t {
  return BufferTagHash{}(tag) % latches_.size();
}

void BufferManagerPageTable::ReplaceInStripe(std::size_t stripe, const BufferTag &old_tag, const BufferTag &new_tag, FrameId frame_id) {
  std::lock_guard<std::mutex> page_table_guard(latches_[stripe]);
  auto &shard = shards_[stripe];
  shard.erase(old_tag);
  shard[new_tag] = frame_id;
}

void BufferManagerPageTable::ReplaceAcrossStripes(std::size_t old_stripe, const BufferTag &old_tag, std::size_t new_stripe, const BufferTag &new_tag, FrameId frame_id) {
  if (old_stripe < new_stripe) {
    std::lock_guard<std::mutex> old_guard(latches_[old_stripe]);
    std::lock_guard<std::mutex> new_guard(latches_[new_stripe]);
    shards_[old_stripe].erase(old_tag);
    shards_[new_stripe][new_tag] = frame_id;
    return;
  }

  std::lock_guard<std::mutex> new_guard(latches_[new_stripe]);
  std::lock_guard<std::mutex> old_guard(latches_[old_stripe]);
  shards_[old_stripe].erase(old_tag);
  shards_[new_stripe][new_tag] = frame_id;
}

}  // namespace telepath
