#include "telepath/buffer/buffer_manager.h"

#include <algorithm>
#include <array>
#include <tuple>
#include <utility>
#include <vector>

#include "buffer_manager_observer.h"
#include "frame_control.h"
#include "frame_free_list.h"
#include "page_table.h"
#include "telepath/replacer/replacer.h"

namespace telepath {

namespace {

class FrameEvictionProbe final : public EvictionProbe {
 public:
  explicit FrameEvictionProbe(std::span<const BufferDescriptor> descriptors) : descriptors_(descriptors) {}

  bool IsEvictable(FrameId frame_id) const override {
    if (frame_id >= descriptors_.size()) return false;
    const std::uint32_t word = descriptors_[frame_id].control.load(std::memory_order_acquire);
    if (!frame_control::IsResident(word) || frame_control::Pins(word) != 0) return false;
    return (word & (frame_control::kLatchMask | frame_control::kWritebackMask)) == 0;
  }

 private:
  std::span<const BufferDescriptor> descriptors_;
};

}  // namespace

auto BufferManager::AbortLoadReservation(FrameId frame_id, const BufferTag &tag, const Status &status) -> Status {
  page_table_->Remove(tag, frame_id);
  ForgetFrame(frame_id);
  StoreTag(&descriptors_[frame_id], BufferTag{});
  frame_control::ReleaseToFree(descriptors_[frame_id].control);
  ReleaseFreeFrame(frame_id);
  return status;
}

void BufferManager::ReleaseFreeFrame(FrameId frame_id) {
  ResetCleanerCandidate(frame_id);
  free_list_->Push(frame_id);
}

auto BufferManager::AcquireReservationFrame(NamespaceId space, std::uint32_t *eligible) -> Result<ReservationCandidate> {
  if (namespace_count_.load(std::memory_order_acquire) == 1) {
    *eligible = ~std::uint32_t{0};
    if (std::optional<FrameId> free_frame = free_list_->Pop(); free_frame.has_value()) return ReservationCandidate{free_frame.value(), true};
    FrameId frame_id = kInvalidFrameId;
    if (replacer_->Victim(FrameEvictionProbe(descriptors_), &frame_id)) return ReservationCandidate{frame_id, false};
    return ResourceExhausted("no evictable frame available");
  }

  const NamespaceState &self = namespaces_[space];
  const bool at_max = self.max_frames != 0 && self.owned->load(std::memory_order_relaxed) >= self.max_frames;
  if (!at_max) {
    if (std::optional<FrameId> free_frame = free_list_->Pop(); free_frame.has_value()) return ReservationCandidate{free_frame.value(), true};
  }
  std::optional<FrameId> victim = NominateVictim(space, eligible);
  if (!at_max) *eligible |= kFreeFramesEligible;
  if (victim.has_value()) return ReservationCandidate{victim.value(), false};
  return ResourceExhausted("no evictable frame available within the namespace quotas");
}

auto BufferManager::NominateVictim(NamespaceId space, std::uint32_t *eligible) -> std::optional<FrameId> {
  const std::size_t count = namespace_count_.load(std::memory_order_acquire);
  const NamespaceState &self = namespaces_[space];
  const std::size_t self_owned = self.owned->load(std::memory_order_relaxed);
  std::array<NamespaceId, kMaxNamespaces> order{};
  std::size_t candidates = 0;
  // A namespace at its maximum makes room among its own pages. Otherwise it
  // evicts its own pages while above its guarantee, then takes from the
  // namespaces furthest above theirs, and evicts below its own guarantee only
  // last; nobody else's guarantee is touched.
  if (self.max_frames != 0 && self_owned >= self.max_frames) {
    order[candidates++] = space;
  } else {
    const bool self_first = self_owned > self.min_frames;
    if (self_first) order[candidates++] = space;
    // Counts keep changing under other threads, so sort a snapshot of them.
    std::array<std::pair<std::size_t, NamespaceId>, kMaxNamespaces> others{};
    std::size_t other_count = 0;
    for (NamespaceId other = 0; other < count; ++other) {
      if (other == space) continue;
      const NamespaceState &state = namespaces_[other];
      const std::size_t owned = state.owned->load(std::memory_order_relaxed);
      if (owned > state.min_frames) others[other_count++] = {owned - state.min_frames, other};
    }
    std::sort(others.begin(), others.begin() + static_cast<std::ptrdiff_t>(other_count), [](const auto &a, const auto &b) { return a.first > b.first; });
    for (std::size_t index = 0; index < other_count; ++index) order[candidates++] = others[index].second;
    if (!self_first) order[candidates++] = space;
  }

  *eligible = 0;
  for (std::size_t index = 0; index < candidates; ++index) *eligible |= std::uint32_t{1} << order[index];
  for (std::size_t index = 0; index < candidates; ++index) {
    FrameId frame_id = kInvalidFrameId;
    if (replacers_[order[index]]->Victim(FrameEvictionProbe(descriptors_), &frame_id)) return frame_id;
  }
  return std::nullopt;
}

auto BufferManager::DropFileBuffers(FileId file_id, BlockId first_block) -> Status {
  if (Status usable = CheckUsable(); !usable.has_value()) return usable;
  Status result = Ok();
  for (FrameId frame_id = 0; frame_id < pool_size_; ++frame_id) {
    BufferDescriptor &descriptor = descriptors_[frame_id];
    while (true) {
      const BufferTag tag = LoadTag(descriptor);
      const std::uint32_t word = descriptor.control.load(std::memory_order_acquire);
      if (frame_control::State(word) == BufferFrameState::kFree || tag.file_id != file_id || tag.block_id < first_block) break;
      if (frame_control::State(word) == BufferFrameState::kLoading) {
        if (!WaitForLoad(frame_id, tag)) return CrashedSegmentError();
        continue;
      }
      if (!TryPinResidentFrame(frame_id, tag, false)) continue;

      std::uint32_t previous = 0;
      const frame_control::DiscardOutcome outcome = frame_control::TryDiscardPinned(descriptor.control, &previous);
      if (outcome == frame_control::DiscardOutcome::kWriteback) {
        ReleaseFrame(frame_id);
        if (Status waited = WaitForPendingFlush(&descriptor); !waited.has_value() && waited.error().code() == ErrorCode::kFailed) return waited;
        continue;
      }
      if (outcome == frame_control::DiscardOutcome::kLatched) {
        const bool released = frame_control::WaitWhile(descriptor.control, [](std::uint32_t word) { return (word & frame_control::kLatchMask) != 0; }, fence_).has_value();
        ReleaseFrame(frame_id);
        if (!released) return CrashedSegmentError();
        continue;
      }
      if (outcome == frame_control::DiscardOutcome::kBusy) {
        ReleaseFrame(frame_id);
        result = InvalidArgument("a page in the dropped range is in use");
        break;
      }
      if (frame_control::IsDirty(previous)) dirty_page_count_->fetch_sub(1, std::memory_order_acq_rel);
      descriptor.rec_lsn.store(kInvalidLsn, std::memory_order_release);
      descriptor.writeback_rec_lsn.store(kInvalidLsn, std::memory_order_release);
      page_table_->Remove(tag, frame_id);
      ForgetFrame(frame_id);
      StoreTag(&descriptor, BufferTag{});
      ReleaseFreeFrame(frame_id);
      break;
    }
  }
  return result;
}

bool BufferManager::HasPinnedFrame() const {
  for (const BufferDescriptor &descriptor : descriptors_) {
    if (frame_control::Pins(descriptor.control.load(std::memory_order_acquire)) != 0) return true;
  }
  return false;
}

bool BufferManager::HasTransientFrame(std::uint32_t eligible) const {
  const bool free_eligible = (eligible & kFreeFramesEligible) != 0;
  for (const BufferDescriptor &descriptor : descriptors_) {
    const std::uint32_t word = descriptor.control.load(std::memory_order_acquire);
    const NamespaceId owner = descriptor.namespace_id.load(std::memory_order_relaxed);
    const bool owner_eligible = owner < kMaxNamespaces && ((eligible >> owner) & 1) != 0;
    if (frame_control::State(word) == BufferFrameState::kLoading) {
      if (free_eligible || owner_eligible) return true;
      continue;
    }
    if (frame_control::Pins(word) != 0) continue;
    if (!frame_control::IsResident(word) ? free_eligible : owner_eligible) return true;
  }
  return false;
}

auto BufferManager::InstallReservationEntry(const BufferTag &tag, const FrameReservation &reservation) -> std::optional<FrameId> {
  const BufferTag *evicted_tag = reservation.had_evicted_page ? &reservation.evicted_tag : nullptr;
  return page_table_->ReplaceUnlessLive(evicted_tag, tag, reservation.frame_id, [&](FrameId other) { return IsLiveFor(other, tag); });
}

auto BufferManager::TryReserveFrameForTag(FrameId frame_id, const BufferTag &tag, bool allow_victim_flush, bool from_free_list, FrameReservation *reservation, NamespaceId space) -> FrameReservationOutcome {
  BufferDescriptor &descriptor = descriptors_[frame_id];
  std::uint32_t observed = 0;
  const frame_control::ReserveOutcome outcome = frame_control::TryReserve(descriptor.control, &observed, from_free_list);
  if (outcome == frame_control::ReserveOutcome::kRejected) return FrameReservationOutcome::kRejected;
  if (outcome == frame_control::ReserveOutcome::kNeedsFlush) {
    return allow_victim_flush ? FrameReservationOutcome::kNeedsFlush : FrameReservationOutcome::kRejected;
  }

  const bool had_evicted_page = frame_control::IsResident(observed);
  *reservation = FrameReservation{frame_id, had_evicted_page, had_evicted_page ? LoadTag(descriptor) : BufferTag{}};
  StoreTag(&descriptor, tag);
  descriptor.version.fetch_add(1, std::memory_order_relaxed);
  if (InstallReservationEntry(tag, *reservation).has_value()) {
    // Another frame won the tag; this frame lost its old page and goes back
    // to the free list.
    ForgetFrame(frame_id);
    StoreTag(&descriptor, BufferTag{});
    frame_control::ReleaseToFree(descriptor.control);
    ReleaseFreeFrame(frame_id);
    return FrameReservationOutcome::kTagExists;
  }
  AssignFrame(frame_id, space);
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
