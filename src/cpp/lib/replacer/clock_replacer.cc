#include "telepath/replacer/clock_replacer.h"

#include <memory>

namespace telepath {

ClockReplacer::ClockReplacer(std::size_t capacity)
    : capacity_(capacity), entries_(capacity) {}

void ClockReplacer::RecordAccess(FrameId frame_id) {
  std::lock_guard<std::mutex> guard(latch_);
  if (!IsValidFrame(frame_id)) {
    return;
  }
  Entry &entry = entries_[frame_id];
  if (!entry.present) {
    entry.present = true;
  }
  entry.referenced = true;
}

void ClockReplacer::SetEvictable(FrameId frame_id, bool evictable) {
  std::lock_guard<std::mutex> guard(latch_);
  if (!IsValidFrame(frame_id)) {
    return;
  }
  Entry &entry = entries_[frame_id];
  if (!entry.present) {
    entry.present = true;
    entry.referenced = true;
  }
  if (entry.evictable == evictable) {
    return;
  }
  entry.evictable = evictable;
  if (evictable) {
    ++size_;
  } else if (size_ > 0) {
    --size_;
  }
}

bool ClockReplacer::Victim(FrameId *frame_id) {
  std::lock_guard<std::mutex> guard(latch_);
  if (frame_id == nullptr || size_ == 0 || capacity_ == 0) {
    return false;
  }

  std::size_t scanned = 0;
  while (scanned < capacity_ * 2) {
    Entry &entry = entries_[hand_];
    if (entry.present && entry.evictable) {
      if (entry.referenced) {
        entry.referenced = false;
      } else {
        entry.present = false;
        entry.evictable = false;
        *frame_id = static_cast<FrameId>(hand_);
        hand_ = (hand_ + 1) % capacity_;
        --size_;
        return true;
      }
    }
    hand_ = (hand_ + 1) % capacity_;
    ++scanned;
  }
  return false;
}

void ClockReplacer::Remove(FrameId frame_id) {
  std::lock_guard<std::mutex> guard(latch_);
  if (!IsValidFrame(frame_id)) {
    return;
  }
  Entry &entry = entries_[frame_id];
  if (entry.present && entry.evictable && size_ > 0) {
    --size_;
  }
  entry = Entry{};
}

std::size_t ClockReplacer::Size() const {
  std::lock_guard<std::mutex> guard(latch_);
  return size_;
}

bool ClockReplacer::IsValidFrame(FrameId frame_id) const {
  return frame_id < capacity_;
}

std::unique_ptr<Replacer> MakeClockReplacer(std::size_t capacity) {
  return std::make_unique<ClockReplacer>(capacity);
}

}  // namespace telepath
