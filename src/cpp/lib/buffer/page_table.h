#ifndef TELEPATH_LIB_BUFFER_PAGE_TABLE_H_
#define TELEPATH_LIB_BUFFER_PAGE_TABLE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "telepath/common/types.h"

namespace telepath {

// Maps resident tags to frames. Chains are intrusive: frames are the nodes, so
// the table never holds more entries than frames and never allocates after
// construction. Lookups take no lock and write nothing; they validate against
// per-stripe sequence counters. Writers serialize per stripe.
class BufferManagerPageTable {
 public:
  BufferManagerPageTable(std::size_t frame_capacity, std::size_t stripe_count);

  BufferManagerPageTable(const BufferManagerPageTable &) = delete;
  BufferManagerPageTable &operator=(const BufferManagerPageTable &) = delete;

  auto LookupFrameId(const BufferTag &tag) const -> std::optional<FrameId>;
  void Install(const BufferTag &tag, FrameId frame_id);
  // Unmaps `tag` only if it currently maps to `frame_id`.
  void Remove(const BufferTag &tag, FrameId frame_id);
  // Maps `new_tag` to `frame_id`; unmaps `old_tag` only if it currently maps
  // to `frame_id`.
  void Replace(const BufferTag &old_tag, const BufferTag &new_tag, FrameId frame_id);
  // Like Replace(), but if another frame mapped under `new_tag` satisfies
  // `live(frame)`, leaves `new_tag` alone and returns that frame. `frame_id`'s
  // old mapping is dropped either way. `old_tag` is null for a frame that has
  // no mapping. `live` runs under the stripe lock.
  template <typename Live>
  auto ReplaceUnlessLive(const BufferTag *old_tag, const BufferTag &new_tag, FrameId frame_id, Live live) -> std::optional<FrameId>;

 private:
  static constexpr std::uint32_t kNone = UINT32_MAX;

  struct alignas(kCacheLineSize) Stripe {
    std::atomic<std::uint32_t> lock{0};
    std::atomic<std::uint32_t> sequence{0};
  };

  class StripeWriter {
   public:
    StripeWriter(Stripe *first, Stripe *second);
    ~StripeWriter();
    StripeWriter(const StripeWriter &) = delete;
    StripeWriter &operator=(const StripeWriter &) = delete;

   private:
    Stripe *first_;
    Stripe *second_;
  };

  auto BucketFor(const BufferTag &tag) const -> std::size_t;
  auto StripeFor(std::size_t bucket) -> Stripe *;
  auto StripeFor(std::size_t bucket) const -> const Stripe *;
  bool MapsTo(FrameId frame_id, const BufferTag &tag) const;
  void Link(std::size_t bucket, const BufferTag &tag, FrameId frame_id);
  void Unlink(FrameId frame_id);

  std::size_t frame_capacity_;
  std::size_t bucket_mask_;
  std::size_t stripe_count_;
  std::unique_ptr<std::atomic<std::uint32_t>[]> heads_;
  std::unique_ptr<std::atomic<std::uint32_t>[]> next_;
  std::unique_ptr<std::atomic<std::uint32_t>[]> bucket_of_;
  std::unique_ptr<std::atomic<FileId>[]> key_file_id_;
  std::unique_ptr<std::atomic<BlockId>[]> key_block_id_;
  std::unique_ptr<Stripe[]> stripes_;
};

template <typename Live>
auto BufferManagerPageTable::ReplaceUnlessLive(const BufferTag *old_tag, const BufferTag &new_tag, FrameId frame_id, Live live) -> std::optional<FrameId> {
  const std::uint32_t old_bucket = bucket_of_[frame_id].load(std::memory_order_relaxed);
  const std::size_t new_bucket = BucketFor(new_tag);
  StripeWriter writer(StripeFor(new_bucket), old_bucket == kNone ? nullptr : StripeFor(old_bucket));
  std::optional<FrameId> existing;
  std::uint32_t frame = heads_[new_bucket].load(std::memory_order_relaxed);
  for (std::size_t steps = 0; frame != kNone && steps < frame_capacity_; ++steps) {
    if (frame != frame_id && MapsTo(frame, new_tag) && live(static_cast<FrameId>(frame))) {
      existing = frame;
      break;
    }
    frame = next_[frame].load(std::memory_order_relaxed);
  }
  const bool linked_under_new = old_bucket != kNone && MapsTo(frame_id, new_tag);
  if (old_bucket != kNone && ((old_tag != nullptr && MapsTo(frame_id, *old_tag)) || !linked_under_new || existing.has_value())) Unlink(frame_id);
  if (existing.has_value()) return existing;
  if (bucket_of_[frame_id].load(std::memory_order_relaxed) == kNone) Link(new_bucket, new_tag, frame_id);
  return std::nullopt;
}

}  // namespace telepath

#endif  // TELEPATH_LIB_BUFFER_PAGE_TABLE_H_
