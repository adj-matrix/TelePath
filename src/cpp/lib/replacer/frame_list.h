#ifndef TELEPATH_LIB_REPLACER_FRAME_LIST_H_
#define TELEPATH_LIB_REPLACER_FRAME_LIST_H_

#include <cstddef>
#include <memory>

#include "telepath/common/types.h"

namespace telepath {

// Intrusive doubly linked list over frame ids in [0, capacity). The front is
// the most recently inserted end.
class FrameList {
 public:
  static constexpr FrameId kNone = kInvalidFrameId;

  explicit FrameList(std::size_t capacity)
    : front_to_back_(std::make_unique<FrameId[]>(capacity)),
      back_to_front_(std::make_unique<FrameId[]>(capacity)),
      linked_(std::make_unique<bool[]>(capacity)) {}

  bool Contains(FrameId frame_id) const { return linked_[frame_id]; }
  auto back() const -> FrameId { return back_; }
  auto TowardFront(FrameId frame_id) const -> FrameId { return back_to_front_[frame_id]; }

  void PushFront(FrameId frame_id) {
    front_to_back_[frame_id] = front_;
    back_to_front_[frame_id] = kNone;
    if (front_ != kNone) back_to_front_[front_] = frame_id;
    front_ = frame_id;
    if (back_ == kNone) back_ = frame_id;
    linked_[frame_id] = true;
  }

  void Unlink(FrameId frame_id) {
    const FrameId toward_back = front_to_back_[frame_id];
    const FrameId toward_front = back_to_front_[frame_id];
    if (toward_front != kNone) front_to_back_[toward_front] = toward_back;
    else front_ = toward_back;
    if (toward_back != kNone) back_to_front_[toward_back] = toward_front;
    else back_ = toward_front;
    linked_[frame_id] = false;
  }

  void MoveToFront(FrameId frame_id) {
    Unlink(frame_id);
    PushFront(frame_id);
  }

 private:
  std::unique_ptr<FrameId[]> front_to_back_;
  std::unique_ptr<FrameId[]> back_to_front_;
  std::unique_ptr<bool[]> linked_;
  FrameId front_{kNone};
  FrameId back_{kNone};
};

}  // namespace telepath

#endif  // TELEPATH_LIB_REPLACER_FRAME_LIST_H_
