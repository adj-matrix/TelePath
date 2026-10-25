#include "telepath/buffer/buffer_manager.h"

#include <vector>

#include "buffer_manager_observer.h"
#include "frame_control.h"
#include "page_table.h"
#include "telepath/replacer/replacer.h"

namespace telepath {

namespace {

class FrameEvictionProbe final : public EvictionProbe {
 public:
  explicit FrameEvictionProbe(const std::vector<BufferDescriptor> &descriptors) : descriptors_(descriptors) {}

  bool IsEvictable(FrameId frame_id) const override {
    if (frame_id >= descriptors_.size()) return false;
    const std::uint32_t word = descriptors_[frame_id].control.load(std::memory_order_acquire);
    if (!frame_control::IsResident(word) || frame_control::Pins(word) != 0) return false;
    return (word & (frame_control::kLatchMask | frame_control::kWritebackMask)) == 0;
  }

 private:
  const std::vector<BufferDescriptor> &descriptors_;
};

}  // namespace

auto BufferManager::AbortLoadReservation(FrameId frame_id, const BufferTag &tag, const Status &status) -> Status {
  page_table_->Remove(tag, frame_id);
  replacer_->Remove(frame_id);
  StoreTag(&descriptors_[frame_id], BufferTag{});
  frame_control::ReleaseToFree(descriptors_[frame_id].control);
  ReleaseFreeFrame(frame_id);
  return status;
}

void BufferManager::ReleaseFreeFrame(FrameId frame_id) {
  ResetCleanerCandidate(frame_id);
  std::lock_guard<std::mutex> free_list_guard(free_list_latch_);
  free_list_.push_back(frame_id);
}

auto BufferManager::AcquireReservationFrame() -> Result<FrameId> {
  {
    std::lock_guard<std::mutex> free_list_guard(free_list_latch_);
    if (!free_list_.empty()) {
      const FrameId frame_id = free_list_.back();
      free_list_.pop_back();
      return frame_id;
    }
  }

  FrameId frame_id = kInvalidFrameId;
  if (replacer_->Victim(FrameEvictionProbe(descriptors_), &frame_id)) return frame_id;
  return ResourceExhausted("no evictable frame available");
}

bool BufferManager::HasUnpinnedFrame() const {
  for (const BufferDescriptor &descriptor : descriptors_) {
    if (frame_control::Pins(descriptor.control.load(std::memory_order_acquire)) == 0) return true;
  }
  return false;
}

void BufferManager::InstallReservationEntry(const BufferTag &tag, const FrameReservation &reservation) {
  if (!reservation.had_evicted_page) {
    page_table_->Install(tag, reservation.frame_id);
    return;
  }
  page_table_->Replace(reservation.evicted_tag, tag, reservation.frame_id);
}

auto BufferManager::TryReserveFrameForTag(FrameId frame_id, const BufferTag &tag, bool allow_victim_flush, FrameReservation *reservation) -> FrameReservationOutcome {
  BufferDescriptor &descriptor = descriptors_[frame_id];
  std::uint32_t observed = 0;
  const frame_control::ReserveOutcome outcome = frame_control::TryReserve(descriptor.control, &observed);
  if (outcome == frame_control::ReserveOutcome::kRejected) return FrameReservationOutcome::kRejected;
  if (outcome == frame_control::ReserveOutcome::kNeedsFlush) {
    return allow_victim_flush ? FrameReservationOutcome::kNeedsFlush : FrameReservationOutcome::kRejected;
  }

  const bool had_evicted_page = frame_control::IsResident(observed);
  *reservation = FrameReservation{frame_id, had_evicted_page, had_evicted_page ? LoadTag(descriptor) : BufferTag{}};
  StoreTag(&descriptor, tag);
  descriptor.version.fetch_add(1, std::memory_order_relaxed);
  InstallReservationEntry(tag, *reservation);
  ResetCleanerCandidate(frame_id);
  return FrameReservationOutcome::kReserved;
}

auto BufferManager::FlushVictimBeforeReuse(FrameId frame_id) -> Status {
  Status flush_status = FlushFrame(frame_id);
  if (flush_status.has_value()) return Ok();

  observer_->RecordEvictionFailure(LoadTag(descriptors_[frame_id]));
  return flush_status;
}

}  // namespace telepath
