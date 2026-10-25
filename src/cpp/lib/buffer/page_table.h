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

}  // namespace telepath

#endif  // TELEPATH_LIB_BUFFER_PAGE_TABLE_H_
