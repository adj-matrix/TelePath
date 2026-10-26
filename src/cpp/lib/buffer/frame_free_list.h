#ifndef TELEPATH_LIB_BUFFER_FRAME_FREE_LIST_H_
#define TELEPATH_LIB_BUFFER_FRAME_FREE_LIST_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "telepath/common/types.h"
#include "../common/state_layout.h"

namespace telepath {

// Lock-free LIFO of free frame ids. Links are frame indices and the head
// carries a version counter against ABA, so the structure holds no pointers.
class FrameFreeList {
 public:
  struct Offsets {
    std::size_t head;
    std::size_t next;
  };

  static auto Reserve(StateLayout *layout, std::size_t capacity) -> Offsets {
    return Offsets{layout->Add<std::atomic<std::uint64_t>>(1), layout->Add<std::atomic<std::uint32_t>>(capacity)};
  }

  // Lives at `base` + `offsets`; `initialize` empties the list, otherwise it
  // uses a list another process initialized there.
  FrameFreeList(std::size_t capacity, std::byte *base, const Offsets &offsets, bool initialize) {
    if (initialize) {
      head_ = ConstructState<std::atomic<std::uint64_t>>(base, offsets.head, 1, Pack(kNone, 0));
      next_ = ConstructState<std::atomic<std::uint32_t>>(base, offsets.next, capacity, kNone);
      return;
    }
    head_ = StateAt<std::atomic<std::uint64_t>>(base, offsets.head);
    next_ = StateAt<std::atomic<std::uint32_t>>(base, offsets.next);
  }

  void Push(FrameId frame_id) {
    std::uint64_t head = head_->load(std::memory_order_relaxed);
    while (true) {
      next_[frame_id].store(Index(head), std::memory_order_relaxed);
      if (head_->compare_exchange_weak(head, Pack(static_cast<std::uint32_t>(frame_id), Version(head) + 1), std::memory_order_release, std::memory_order_relaxed)) return;
    }
  }

  auto Pop() -> std::optional<FrameId> {
    std::uint64_t head = head_->load(std::memory_order_acquire);
    while (Index(head) != kNone) {
      const std::uint32_t next = next_[Index(head)].load(std::memory_order_relaxed);
      if (head_->compare_exchange_weak(head, Pack(next, Version(head) + 1), std::memory_order_acquire, std::memory_order_acquire)) return Index(head);
    }
    return std::nullopt;
  }

 private:
  static constexpr std::uint32_t kNone = UINT32_MAX;

  static constexpr auto Pack(std::uint32_t index, std::uint32_t version) -> std::uint64_t { return (std::uint64_t{version} << 32) | index; }
  static constexpr auto Index(std::uint64_t head) -> std::uint32_t { return static_cast<std::uint32_t>(head); }
  static constexpr auto Version(std::uint64_t head) -> std::uint32_t { return static_cast<std::uint32_t>(head >> 32); }

  std::atomic<std::uint64_t> *head_{nullptr};
  std::atomic<std::uint32_t> *next_{nullptr};
};

}  // namespace telepath

#endif  // TELEPATH_LIB_BUFFER_FRAME_FREE_LIST_H_
