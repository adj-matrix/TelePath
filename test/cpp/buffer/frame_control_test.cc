#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>

#include "frame_control.h"

namespace fc = telepath::frame_control;

namespace {

auto ResidentWord(std::uint32_t pins, std::uint32_t flags = 0) -> std::uint32_t {
  return static_cast<std::uint32_t>(telepath::BufferFrameState::kResident) | flags | (pins << fc::kPinShift);
}

void AssertPinRequiresResidentState() {
  std::atomic<std::uint32_t> control{0};
  assert(!fc::TryPinResident(control));

  control.store(static_cast<std::uint32_t>(telepath::BufferFrameState::kLoading) | fc::kPinOne);
  assert(!fc::TryPinResident(control));

  control.store(ResidentWord(0));
  assert(fc::TryPinResident(control));
  assert(fc::TryPinResident(control));
  assert(fc::Pins(control.load()) == 2);

  auto previous = fc::Unpin(control);
  assert(previous.has_value() && fc::Pins(*previous) == 2);
  previous = fc::Unpin(control);
  assert(previous.has_value() && fc::Pins(*previous) == 1);
  assert(!fc::Unpin(control).has_value());
}

void AssertReserveClaimsOnlyIdleFrames() {
  std::uint32_t observed = 0;

  // Only the thread that popped a free frame from the free list may claim it;
  // a stale victim nomination of a frame being released must not.
  std::atomic<std::uint32_t> free_frame{0};
  assert(fc::TryReserve(free_frame, &observed, false) == fc::ReserveOutcome::kRejected);
  assert(free_frame.load() == 0);
  assert(fc::TryReserve(free_frame, &observed, true) == fc::ReserveOutcome::kReserved);
  assert(fc::State(free_frame.load()) == telepath::BufferFrameState::kLoading);
  assert(fc::Pins(free_frame.load()) == 1);

  std::atomic<std::uint32_t> clean{ResidentWord(0, fc::kRedirtied)};
  assert(fc::TryReserve(clean, &observed, false) == fc::ReserveOutcome::kReserved);
  assert(fc::IsResident(observed));
  assert((clean.load() & (fc::kDirty | fc::kRedirtied)) == 0);

  std::atomic<std::uint32_t> pinned{ResidentWord(1)};
  assert(fc::TryReserve(pinned, &observed, false) == fc::ReserveOutcome::kRejected);

  std::atomic<std::uint32_t> latched{ResidentWord(0, fc::kSharedOne)};
  assert(fc::TryReserve(latched, &observed, false) == fc::ReserveOutcome::kRejected);

  std::atomic<std::uint32_t> flushing{ResidentWord(0, fc::kDirty | fc::kWritebackActive)};
  assert(fc::TryReserve(flushing, &observed, false) == fc::ReserveOutcome::kRejected);

  std::atomic<std::uint32_t> dirty{ResidentWord(0, fc::kDirty)};
  assert(fc::TryReserve(dirty, &observed, false) == fc::ReserveOutcome::kNeedsFlush);
  assert(dirty.load() == ResidentWord(0, fc::kDirty));
}

void AssertLatchModesExcludeEachOther() {
  std::atomic<std::uint32_t> control{ResidentWord(1)};
  assert(fc::TryAcquireShared(control));
  assert(fc::TryAcquireShared(control));
  assert(!fc::TryAcquireExclusive(control));
  fc::ReleaseShared(control);
  fc::ReleaseShared(control);

  std::atomic<std::uint64_t> version{7};
  assert(fc::TryAcquireExclusive(control));
  assert(!fc::TryAcquireShared(control));
  assert(!fc::TryAcquireExclusive(control));
  fc::ReleaseExclusive(control, version);
  assert(version.load() == 8);
  assert((control.load() & fc::kLatchMask) == 0);
}

void AssertCompletionClearsDirtyOnlyWithoutRedirty() {
  std::atomic<std::uint32_t> control{ResidentWord(0, fc::kDirty)};
  assert(fc::TryQueueWriteback(control, false) == fc::QueueOutcome::kQueued);
  assert(fc::TryQueueWriteback(control, false) == fc::QueueOutcome::kPending);
  fc::BeginWriteback(control);
  auto completion = fc::CompleteWriteback(control, true);
  assert(completion.cleared_dirty);
  assert(!fc::IsDirty(control.load()));
  assert(!fc::HasWriteback(control.load()));
}

void AssertExclusiveReleaseDuringWritebackKeepsDirty() {
  std::atomic<std::uint32_t> control{ResidentWord(1, fc::kDirty)};
  std::atomic<std::uint64_t> version{0};
  assert(fc::TryQueueWriteback(control, false) == fc::QueueOutcome::kQueued);
  assert(fc::TryAcquireExclusive(control));
  fc::ReleaseExclusive(control, version);
  assert((control.load() & fc::kRedirtied) != 0);

  fc::BeginWriteback(control);
  auto completion = fc::CompleteWriteback(control, true);
  assert(!completion.cleared_dirty);
  assert(fc::IsDirty(control.load()));
  assert((control.load() & fc::kRedirtied) == 0);
}

void AssertMarkDirtyDuringWritebackKeepsDirty() {
  std::atomic<std::uint32_t> control{ResidentWord(1, fc::kDirty)};
  assert(fc::TryQueueWriteback(control, false) == fc::QueueOutcome::kQueued);
  auto became_dirty = fc::MarkDirty(control);
  assert(became_dirty.has_value() && !*became_dirty);
  auto completion = fc::CompleteWriteback(control, true);
  assert(!completion.cleared_dirty);
  assert(fc::IsDirty(control.load()));
}

void AssertExclusiveHolderAtCompletionKeepsDirty() {
  std::atomic<std::uint32_t> control{ResidentWord(1, fc::kDirty | fc::kExclusive)};
  assert(fc::TryQueueWriteback(control, false) == fc::QueueOutcome::kQueued);
  fc::BeginWriteback(control);
  auto completion = fc::CompleteWriteback(control, true);
  assert(!completion.cleared_dirty);
  assert(fc::IsDirty(control.load()));
}

void AssertFailedWritebackKeepsDirty() {
  std::atomic<std::uint32_t> control{ResidentWord(0, fc::kDirty)};
  assert(fc::TryQueueWriteback(control, false) == fc::QueueOutcome::kQueued);
  auto completion = fc::CompleteWriteback(control, false);
  assert(!completion.cleared_dirty);
  assert(fc::IsDirty(control.load()));
}

void AssertCleanerSkipsPinnedFrames() {
  std::atomic<std::uint32_t> control{ResidentWord(1, fc::kDirty)};
  assert(fc::TryQueueWriteback(control, true) == fc::QueueOutcome::kPinned);
  assert(fc::TryQueueWriteback(control, false) == fc::QueueOutcome::kQueued);
}

void AssertExclusiveWaiterWakesAfterSharedRelease() {
  std::atomic<std::uint32_t> control{ResidentWord(2)};
  std::atomic<std::uint64_t> version{0};
  fc::AcquireShared(control);
  std::atomic<bool> acquired{false};
  std::thread writer([&]() {
    fc::AcquireExclusive(control);
    acquired.store(true);
    fc::ReleaseExclusive(control, version);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  assert(!acquired.load());
  fc::ReleaseShared(control);
  writer.join();
  assert(acquired.load());
  assert((control.load() & (fc::kLatchMask | fc::kWaiters)) == 0);
}

void AssertPendingFlushWaiterWakesOnCompletion() {
  std::atomic<std::uint32_t> control{ResidentWord(0, fc::kDirty)};
  assert(fc::TryQueueWriteback(control, false) == fc::QueueOutcome::kQueued);
  std::atomic<bool> released{false};
  std::thread waiter([&]() {
    fc::WaitWhile(control, [](std::uint32_t word) { return fc::HasWriteback(word); });
    released.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  assert(!released.load());
  fc::BeginWriteback(control);
  fc::CompleteWriteback(control, true);
  waiter.join();
  assert(released.load());
}

void AssertWritebackErrorRoundTrip() {
  assert(fc::PackWritebackError(telepath::Ok()) == 0);
  auto status = fc::UnpackWritebackError(fc::PackWritebackError(telepath::IoError("disk full", 28)));
  assert(!status.has_value());
  assert(status.error().code() == telepath::ErrorCode::kIoError);
  assert(status.error().system_error() == 28);
}

}  // namespace

// A flusher may hold the shared latch on a frame whose load is abandoned.
void AssertReleaseToFreeKeepsLatchHolders() {
  std::atomic<std::uint32_t> control{0};
  std::uint32_t observed = 0;
  assert(fc::TryReserve(control, &observed, true) == fc::ReserveOutcome::kReserved);
  assert(fc::TryAcquireShared(control));
  fc::ReleaseToFree(control);
  assert(fc::State(control.load()) == telepath::BufferFrameState::kFree);
  assert(fc::Pins(control.load()) == 0);
  assert(fc::SharedHolders(control.load()) == 1);
  assert(fc::TryReserve(control, &observed, true) == fc::ReserveOutcome::kRejected);
  fc::ReleaseShared(control);
  assert(control.load() == 0);
}

int main() {
  AssertReleaseToFreeKeepsLatchHolders();
  AssertPinRequiresResidentState();
  AssertReserveClaimsOnlyIdleFrames();
  AssertLatchModesExcludeEachOther();
  AssertCompletionClearsDirtyOnlyWithoutRedirty();
  AssertExclusiveReleaseDuringWritebackKeepsDirty();
  AssertMarkDirtyDuringWritebackKeepsDirty();
  AssertExclusiveHolderAtCompletionKeepsDirty();
  AssertFailedWritebackKeepsDirty();
  AssertCleanerSkipsPinnedFrames();
  AssertExclusiveWaiterWakesAfterSharedRelease();
  AssertPendingFlushWaiterWakesOnCompletion();
  AssertWritebackErrorRoundTrip();
  return 0;
}
