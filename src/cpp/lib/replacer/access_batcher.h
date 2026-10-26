#ifndef TELEPATH_LIB_REPLACER_ACCESS_BATCHER_H_
#define TELEPATH_LIB_REPLACER_ACCESS_BATCHER_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>

#include "telepath/common/types.h"

namespace telepath {

class AccessBatcher {
 public:
  static constexpr std::size_t kBatchSize = 64;

  AccessBatcher()
    : slot_count_(std::bit_ceil(std::max<std::size_t>(4, 2 * std::max(1U, std::thread::hardware_concurrency())))),
      slots_(std::make_unique<Slot[]>(slot_count_)) {}

  template <typename PolicyLatch, typename Apply>
  void Record(FrameId frame_id, PolicyLatch &policy_latch, Apply &&apply) {
    std::array<FrameId, kBatchSize> batch;
    {
      Slot &slot = slots_[SlotIndex()];
      std::lock_guard<std::mutex> slot_guard(slot.latch);
      slot.frames[slot.count++] = frame_id;
      if (slot.count < kBatchSize) return;
      batch = slot.frames;
      slot.count = 0;
    }
    std::lock_guard<PolicyLatch> policy_guard(policy_latch);
    for (FrameId frame : batch) apply(frame);
  }

  template <typename Apply>
  void DrainCurrentThread(Apply &&apply) {
    Slot &slot = slots_[SlotIndex()];
    std::lock_guard<std::mutex> slot_guard(slot.latch);
    for (std::size_t entry = 0; entry < slot.count; ++entry) apply(slot.frames[entry]);
    slot.count = 0;
  }

  template <typename Apply>
  void DrainAll(Apply &&apply) {
    for (std::size_t index = 0; index < slot_count_; ++index) {
      Slot &slot = slots_[index];
      std::lock_guard<std::mutex> slot_guard(slot.latch);
      for (std::size_t entry = 0; entry < slot.count; ++entry) apply(slot.frames[entry]);
      slot.count = 0;
    }
  }

 private:
  struct alignas(kCacheLineSize) Slot {
    std::mutex latch;
    std::size_t count{0};
    std::array<FrameId, kBatchSize> frames{};
  };

  auto SlotIndex() const -> std::size_t {
    static std::atomic<std::size_t> next_index{0};
    thread_local const std::size_t index = next_index.fetch_add(1, std::memory_order_relaxed);
    return index & (slot_count_ - 1);
  }

  std::size_t slot_count_;
  std::unique_ptr<Slot[]> slots_;
};

}  // namespace telepath

#endif  // TELEPATH_LIB_REPLACER_ACCESS_BATCHER_H_
