#ifndef TELEPATH_LIB_REPLACER_FRAME_LIST_H_
#define TELEPATH_LIB_REPLACER_FRAME_LIST_H_

#include <cstddef>
#include <memory>

#include "../common/state_layout.h"
#include "telepath/common/types.h"

namespace telepath {

// Intrusive doubly linked list over frame ids in [0, capacity). The front is
// the most recently inserted end.
class FrameList {
 public:
  static constexpr FrameId kNone = kInvalidFrameId;

  struct Ends {
    FrameId front{kNone};
    FrameId back{kNone};
  };

  struct Offsets {
    std::size_t ends;
    std::size_t front_to_back;
    std::size_t back_to_front;
    std::size_t linked;
  };

  static auto Reserve(StateLayout *layout, std::size_t capacity) -> Offsets {
    return Offsets{layout->Add<Ends>(1), layout->Add<FrameId>(capacity), layout->Add<FrameId>(capacity), layout->Add<bool>(capacity)};
  }

  FrameList() = default;

  void Place(std::byte *base, const Offsets &offsets, std::size_t capacity, bool initialize) {
    if (initialize) {
      ends_ = ConstructState<Ends>(base, offsets.ends, 1);
      front_to_back_ = ConstructState<FrameId>(base, offsets.front_to_back, capacity, kNone);
      back_to_front_ = ConstructState<FrameId>(base, offsets.back_to_front, capacity, kNone);
      linked_ = ConstructState<bool>(base, offsets.linked, capacity, false);
      return;
    }
    ends_ = StateAt<Ends>(base, offsets.ends);
    front_to_back_ = StateAt<FrameId>(base, offsets.front_to_back);
    back_to_front_ = StateAt<FrameId>(base, offsets.back_to_front);
    linked_ = StateAt<bool>(base, offsets.linked);
  }

  bool Contains(FrameId frame_id) const { return linked_[frame_id]; }
  auto back() const -> FrameId { return ends_->back; }
  auto TowardFront(FrameId frame_id) const -> FrameId { return back_to_front_[frame_id]; }

  void PushFront(FrameId frame_id) {
    front_to_back_[frame_id] = ends_->front;
    back_to_front_[frame_id] = kNone;
    if (ends_->front != kNone) back_to_front_[ends_->front] = frame_id;
    ends_->front = frame_id;
    if (ends_->back == kNone) ends_->back = frame_id;
    linked_[frame_id] = true;
  }

  void Unlink(FrameId frame_id) {
    const FrameId toward_back = front_to_back_[frame_id];
    const FrameId toward_front = back_to_front_[frame_id];
    if (toward_front != kNone) front_to_back_[toward_front] = toward_back;
    else ends_->front = toward_back;
    if (toward_back != kNone) back_to_front_[toward_back] = toward_front;
    else ends_->back = toward_front;
    linked_[frame_id] = false;
  }

  void MoveToFront(FrameId frame_id) {
    Unlink(frame_id);
    PushFront(frame_id);
  }

 private:
  Ends *ends_{nullptr};
  FrameId *front_to_back_{nullptr};
  FrameId *back_to_front_{nullptr};
  bool *linked_{nullptr};
};

}  // namespace telepath

#endif  // TELEPATH_LIB_REPLACER_FRAME_LIST_H_
