#include "page_table.h"

#include <algorithm>
#include <bit>
#include <thread>
#include <utility>

namespace telepath {

namespace {

void LockStripeWord(std::atomic<std::uint32_t> &lock) {
  while (lock.exchange(1, std::memory_order_acquire) != 0) {
    while (lock.load(std::memory_order_relaxed) != 0) std::this_thread::yield();
  }
}

auto MixTag(const BufferTag &tag) -> std::uint64_t {
  std::uint64_t value = tag.block_id ^ (std::uint64_t{tag.file_id} << 40) ^ (std::uint64_t{tag.file_id} >> 24);
  value ^= value >> 30;
  value *= 0xbf58476d1ce4e5b9ULL;
  value ^= value >> 27;
  value *= 0x94d049bb133111ebULL;
  value ^= value >> 31;
  return value;
}

}  // namespace

BufferManagerPageTable::StripeWriter::StripeWriter(Stripe *first, Stripe *second) : first_(first), second_(second == first ? nullptr : second) {
  if (second_ != nullptr && second_ < first_) std::swap(first_, second_);
  for (Stripe *stripe : {first_, second_}) {
    if (stripe == nullptr) continue;
    LockStripeWord(stripe->lock);
    stripe->sequence.store(stripe->sequence.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  }
  std::atomic_thread_fence(std::memory_order_release);
}

BufferManagerPageTable::StripeWriter::~StripeWriter() {
  for (Stripe *stripe : {second_, first_}) {
    if (stripe == nullptr) continue;
    stripe->sequence.store(stripe->sequence.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    stripe->lock.store(0, std::memory_order_release);
  }
}

auto BufferManagerPageTable::BucketCount(std::size_t frame_capacity) -> std::size_t {
  return std::bit_ceil(std::max<std::size_t>(frame_capacity, 1));
}

auto BufferManagerPageTable::Reserve(StateLayout *layout, std::size_t frame_capacity, std::size_t stripe_count) -> Offsets {
  Offsets offsets{};
  offsets.heads = layout->Add<std::atomic<std::uint32_t>>(BucketCount(frame_capacity));
  offsets.next = layout->Add<std::atomic<std::uint32_t>>(frame_capacity);
  offsets.bucket_of = layout->Add<std::atomic<std::uint32_t>>(frame_capacity);
  offsets.key_file_id = layout->Add<std::atomic<FileId>>(frame_capacity);
  offsets.key_block_id = layout->Add<std::atomic<BlockId>>(frame_capacity);
  offsets.stripes = layout->Add<Stripe>(std::max<std::size_t>(stripe_count, 1));
  return offsets;
}

BufferManagerPageTable::BufferManagerPageTable(std::size_t frame_capacity, std::size_t stripe_count)
  : frame_capacity_(frame_capacity), bucket_mask_(BucketCount(frame_capacity) - 1), stripe_count_(std::max<std::size_t>(stripe_count, 1)) {
  StateLayout layout;
  const Offsets offsets = Reserve(&layout, frame_capacity, stripe_count);
  owned_ = std::make_unique<std::byte[]>(layout.size());
  Place(owned_.get(), offsets, true);
}

BufferManagerPageTable::BufferManagerPageTable(std::size_t frame_capacity, std::size_t stripe_count, std::byte *base, const Offsets &offsets, bool initialize)
  : frame_capacity_(frame_capacity), bucket_mask_(BucketCount(frame_capacity) - 1), stripe_count_(std::max<std::size_t>(stripe_count, 1)) {
  Place(base, offsets, initialize);
}

void BufferManagerPageTable::Place(std::byte *base, const Offsets &offsets, bool initialize) {
  if (!initialize) {
    heads_ = StateAt<std::atomic<std::uint32_t>>(base, offsets.heads);
    next_ = StateAt<std::atomic<std::uint32_t>>(base, offsets.next);
    bucket_of_ = StateAt<std::atomic<std::uint32_t>>(base, offsets.bucket_of);
    key_file_id_ = StateAt<std::atomic<FileId>>(base, offsets.key_file_id);
    key_block_id_ = StateAt<std::atomic<BlockId>>(base, offsets.key_block_id);
    stripes_ = StateAt<Stripe>(base, offsets.stripes);
    return;
  }
  heads_ = ConstructState<std::atomic<std::uint32_t>>(base, offsets.heads, bucket_mask_ + 1, kNone);
  next_ = ConstructState<std::atomic<std::uint32_t>>(base, offsets.next, frame_capacity_, kNone);
  bucket_of_ = ConstructState<std::atomic<std::uint32_t>>(base, offsets.bucket_of, frame_capacity_, kNone);
  key_file_id_ = ConstructState<std::atomic<FileId>>(base, offsets.key_file_id, frame_capacity_, FileId{0});
  key_block_id_ = ConstructState<std::atomic<BlockId>>(base, offsets.key_block_id, frame_capacity_, BlockId{0});
  stripes_ = ConstructState<Stripe>(base, offsets.stripes, stripe_count_);
}

auto BufferManagerPageTable::LookupFrameId(const BufferTag &tag) const -> std::optional<FrameId> {
  const std::size_t bucket = BucketFor(tag);
  const Stripe *stripe = StripeFor(bucket);
  while (true) {
    const std::uint32_t before = stripe->sequence.load(std::memory_order_acquire);
    if ((before & 1U) != 0) {
      std::this_thread::yield();
      continue;
    }

    std::optional<FrameId> found;
    std::uint32_t frame = heads_[bucket].load(std::memory_order_relaxed);
    for (std::size_t steps = 0; frame != kNone && steps < frame_capacity_; ++steps) {
      if (MapsTo(frame, tag)) {
        found = frame;
        break;
      }
      frame = next_[frame].load(std::memory_order_relaxed);
    }

    std::atomic_thread_fence(std::memory_order_acquire);
    if (stripe->sequence.load(std::memory_order_relaxed) == before) return found;
  }
}

void BufferManagerPageTable::Install(const BufferTag &tag, FrameId frame_id) {
  Replace(tag, tag, frame_id);
}

void BufferManagerPageTable::Remove(const BufferTag &tag, FrameId frame_id) {
  const std::uint32_t bucket = bucket_of_[frame_id].load(std::memory_order_relaxed);
  if (bucket == kNone) return;
  StripeWriter writer(StripeFor(bucket), nullptr);
  if (MapsTo(frame_id, tag)) Unlink(frame_id);
}

void BufferManagerPageTable::Replace(const BufferTag &old_tag, const BufferTag &new_tag, FrameId frame_id) {
  const std::uint32_t old_bucket = bucket_of_[frame_id].load(std::memory_order_relaxed);
  const std::size_t new_bucket = BucketFor(new_tag);
  StripeWriter writer(StripeFor(new_bucket), old_bucket == kNone ? nullptr : StripeFor(old_bucket));
  if (old_bucket != kNone && (MapsTo(frame_id, old_tag) || !MapsTo(frame_id, new_tag))) Unlink(frame_id);
  if (bucket_of_[frame_id].load(std::memory_order_relaxed) == kNone) Link(new_bucket, new_tag, frame_id);
}

auto BufferManagerPageTable::BucketFor(const BufferTag &tag) const -> std::size_t {
  return static_cast<std::size_t>(MixTag(tag)) & bucket_mask_;
}

auto BufferManagerPageTable::StripeFor(std::size_t bucket) -> Stripe * {
  return &stripes_[bucket % stripe_count_];
}

auto BufferManagerPageTable::StripeFor(std::size_t bucket) const -> const Stripe * {
  return &stripes_[bucket % stripe_count_];
}

bool BufferManagerPageTable::MapsTo(FrameId frame_id, const BufferTag &tag) const {
  return key_file_id_[frame_id].load(std::memory_order_relaxed) == tag.file_id &&
         key_block_id_[frame_id].load(std::memory_order_relaxed) == tag.block_id;
}

void BufferManagerPageTable::Link(std::size_t bucket, const BufferTag &tag, FrameId frame_id) {
  key_file_id_[frame_id].store(tag.file_id, std::memory_order_relaxed);
  key_block_id_[frame_id].store(tag.block_id, std::memory_order_relaxed);
  next_[frame_id].store(heads_[bucket].load(std::memory_order_relaxed), std::memory_order_relaxed);
  bucket_of_[frame_id].store(static_cast<std::uint32_t>(bucket), std::memory_order_relaxed);
  heads_[bucket].store(frame_id, std::memory_order_relaxed);
}

void BufferManagerPageTable::Unlink(FrameId frame_id) {
  const std::uint32_t bucket = bucket_of_[frame_id].load(std::memory_order_relaxed);
  const std::uint32_t after = next_[frame_id].load(std::memory_order_relaxed);
  std::uint32_t current = heads_[bucket].load(std::memory_order_relaxed);
  if (current == frame_id) {
    heads_[bucket].store(after, std::memory_order_relaxed);
  } else {
    while (current != kNone) {
      const std::uint32_t next = next_[current].load(std::memory_order_relaxed);
      if (next == frame_id) {
        next_[current].store(after, std::memory_order_relaxed);
        break;
      }
      current = next;
    }
  }
  next_[frame_id].store(kNone, std::memory_order_relaxed);
  bucket_of_[frame_id].store(kNone, std::memory_order_relaxed);
}

}  // namespace telepath
