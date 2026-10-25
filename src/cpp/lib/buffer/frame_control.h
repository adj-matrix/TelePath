#ifndef TELEPATH_LIB_BUFFER_FRAME_CONTROL_H_
#define TELEPATH_LIB_BUFFER_FRAME_CONTROL_H_

#include <atomic>
#include <cstdint>
#include <optional>
#include <thread>

#include "telepath/buffer/buffer_descriptor.h"
#include "telepath/common/status.h"

namespace telepath::frame_control {

inline constexpr std::uint32_t kStateMask = 0x3u;
inline constexpr std::uint32_t kDirty = 1u << 2;
inline constexpr std::uint32_t kWritebackQueued = 1u << 3;
inline constexpr std::uint32_t kWritebackActive = 1u << 4;
inline constexpr std::uint32_t kPrefetched = 1u << 5;
inline constexpr std::uint32_t kRedirtied = 1u << 6;
inline constexpr std::uint32_t kExclusive = 1u << 7;
inline constexpr std::uint32_t kSharedShift = 8;
inline constexpr std::uint32_t kSharedOne = 1u << kSharedShift;
inline constexpr std::uint32_t kSharedMask = 0xFFu << kSharedShift;
inline constexpr std::uint32_t kMaxShared = 0xFFu;
inline constexpr std::uint32_t kWaiters = 1u << 16;
inline constexpr std::uint32_t kPinShift = 17;
inline constexpr std::uint32_t kPinOne = 1u << kPinShift;
inline constexpr std::uint32_t kPinMask = 0x7FFFu << kPinShift;
inline constexpr std::uint32_t kMaxPins = 0x7FFFu;
inline constexpr std::uint32_t kWritebackMask = kWritebackQueued | kWritebackActive;
inline constexpr std::uint32_t kLatchMask = kExclusive | kSharedMask;

constexpr auto State(std::uint32_t word) -> BufferFrameState { return static_cast<BufferFrameState>(word & kStateMask); }
constexpr auto WithState(std::uint32_t word, BufferFrameState state) -> std::uint32_t {
  return (word & ~kStateMask) | static_cast<std::uint32_t>(state);
}
constexpr auto Pins(std::uint32_t word) -> std::uint32_t { return (word & kPinMask) >> kPinShift; }
constexpr auto SharedHolders(std::uint32_t word) -> std::uint32_t { return (word & kSharedMask) >> kSharedShift; }
constexpr bool IsResident(std::uint32_t word) { return State(word) == BufferFrameState::kResident; }
constexpr bool IsDirty(std::uint32_t word) { return (word & kDirty) != 0; }
constexpr bool HasWriteback(std::uint32_t word) { return (word & kWritebackMask) != 0; }

void FutexWait(std::atomic<std::uint32_t> &control, std::uint32_t expected);
void FutexWakeAll(std::atomic<std::uint32_t> &control);

// Sleeps until `blocked(word)` is false and returns the observed word.
template <typename Blocked>
auto WaitWhile(std::atomic<std::uint32_t> &control, Blocked blocked) -> std::uint32_t {
  std::uint32_t word = control.load(std::memory_order_acquire);
  while (blocked(word)) {
    if ((word & kWaiters) == 0) {
      if (!control.compare_exchange_weak(word, word | kWaiters, std::memory_order_acquire, std::memory_order_acquire)) continue;
      word |= kWaiters;
    }
    FutexWait(control, word);
    word = control.load(std::memory_order_acquire);
  }
  return word;
}

// Applies `next(word)` with a CAS loop; clears the waiter bit and wakes
// sleepers when `wakes(word, new_word)` holds. Returns the replaced word.
template <typename Next, typename Wakes>
auto UpdateAndWake(std::atomic<std::uint32_t> &control, Next next, Wakes wakes) -> std::uint32_t {
  std::uint32_t word = control.load(std::memory_order_relaxed);
  while (true) {
    std::uint32_t new_word = next(word);
    const bool wake = (word & kWaiters) != 0 && wakes(word, new_word);
    if (wake) new_word &= ~kWaiters;
    if (control.compare_exchange_weak(word, new_word, std::memory_order_acq_rel, std::memory_order_relaxed)) {
      if (wake) FutexWakeAll(control);
      return word;
    }
  }
}

inline bool TryPinResident(std::atomic<std::uint32_t> &control) {
  std::uint32_t word = control.load(std::memory_order_relaxed);
  while (true) {
    if (!IsResident(word)) return false;
    if (Pins(word) == kMaxPins) {
      std::this_thread::yield();
      word = control.load(std::memory_order_relaxed);
      continue;
    }
    if (control.compare_exchange_weak(word, word + kPinOne, std::memory_order_acquire, std::memory_order_relaxed)) return true;
  }
}

// Drops one pin. Returns the word before the release, or nullopt on underflow.
inline auto Unpin(std::atomic<std::uint32_t> &control) -> std::optional<std::uint32_t> {
  std::uint32_t word = control.load(std::memory_order_relaxed);
  while (true) {
    if (Pins(word) == 0) return std::nullopt;
    if (control.compare_exchange_weak(word, word - kPinOne, std::memory_order_release, std::memory_order_relaxed)) return word;
  }
}

enum class ReserveOutcome {
  kReserved,
  kNeedsFlush,
  kRejected,
};

// Claims a free frame, or a clean, unpinned, unlatched resident frame with no
// writeback, for a new load: the frame becomes Loading with one pin held by
// the caller. A dirty but otherwise reservable frame yields kNeedsFlush.
inline auto TryReserve(std::atomic<std::uint32_t> &control, std::uint32_t *observed) -> ReserveOutcome {
  std::uint32_t word = control.load(std::memory_order_acquire);
  while (true) {
    *observed = word;
    if (State(word) == BufferFrameState::kLoading) return ReserveOutcome::kRejected;
    if (Pins(word) != 0 || (word & (kLatchMask | kWritebackMask)) != 0) return ReserveOutcome::kRejected;
    if (IsResident(word) && IsDirty(word)) return ReserveOutcome::kNeedsFlush;
    const std::uint32_t next = (word & kWaiters) | kPinOne | static_cast<std::uint32_t>(BufferFrameState::kLoading);
    if (control.compare_exchange_weak(word, next, std::memory_order_acq_rel, std::memory_order_acquire)) return ReserveOutcome::kReserved;
  }
}

// Loading -> Resident; the loader keeps its pin.
inline void PublishResident(std::atomic<std::uint32_t> &control) {
  UpdateAndWake(
    control,
    [](std::uint32_t word) { return WithState(word, BufferFrameState::kResident); },
    [](std::uint32_t, std::uint32_t) { return true; });
}

// Loading -> Free after a failed load; drops the loader's pin.
inline void ReleaseToFree(std::atomic<std::uint32_t> &control) {
  UpdateAndWake(
    control,
    [](std::uint32_t word) { return word & kWaiters; },
    [](std::uint32_t, std::uint32_t) { return true; });
}

// Sets the dirty bit on a resident frame. Returns whether the frame became
// dirty, or nullopt if it is not resident. Marking dirty while a writeback is
// pending also sets `redirtied`.
inline auto MarkDirty(std::atomic<std::uint32_t> &control) -> std::optional<bool> {
  std::uint32_t word = control.load(std::memory_order_relaxed);
  while (true) {
    if (!IsResident(word)) return std::nullopt;
    std::uint32_t next = word | kDirty;
    if (HasWriteback(word)) next |= kRedirtied;
    if (next == word) return false;
    if (control.compare_exchange_weak(word, next, std::memory_order_acq_rel, std::memory_order_relaxed)) return !IsDirty(word);
  }
}

inline bool TryAcquireShared(std::atomic<std::uint32_t> &control) {
  std::uint32_t word = control.load(std::memory_order_relaxed);
  while ((word & kExclusive) == 0 && SharedHolders(word) < kMaxShared) {
    if (control.compare_exchange_weak(word, word + kSharedOne, std::memory_order_acquire, std::memory_order_relaxed)) return true;
  }
  return false;
}

inline void AcquireShared(std::atomic<std::uint32_t> &control) {
  while (!TryAcquireShared(control)) {
    WaitWhile(control, [](std::uint32_t word) { return (word & kExclusive) != 0 || SharedHolders(word) >= kMaxShared; });
  }
}

inline void ReleaseShared(std::atomic<std::uint32_t> &control) {
  UpdateAndWake(
    control,
    [](std::uint32_t word) { return word - kSharedOne; },
    [](std::uint32_t old_word, std::uint32_t new_word) {
      return SharedHolders(new_word) == 0 || SharedHolders(old_word) == kMaxShared;
    });
}

inline bool TryAcquireExclusive(std::atomic<std::uint32_t> &control) {
  std::uint32_t word = control.load(std::memory_order_relaxed);
  while ((word & kLatchMask) == 0) {
    if (control.compare_exchange_weak(word, word | kExclusive, std::memory_order_acquire, std::memory_order_relaxed)) return true;
  }
  return false;
}

inline void AcquireExclusive(std::atomic<std::uint32_t> &control) {
  while (!TryAcquireExclusive(control)) {
    WaitWhile(control, [](std::uint32_t word) { return (word & kLatchMask) != 0; });
  }
}

// Releases the exclusive latch after bumping the version. Releasing while a
// writeback is pending sets `redirtied` so that writeback cannot clear dirty.
inline void ReleaseExclusive(std::atomic<std::uint32_t> &control, std::atomic<std::uint64_t> &version) {
  version.fetch_add(1, std::memory_order_release);
  UpdateAndWake(
    control,
    [](std::uint32_t word) {
      std::uint32_t next = word & ~kExclusive;
      if (HasWriteback(word)) next |= kRedirtied;
      return next;
    },
    [](std::uint32_t, std::uint32_t) { return true; });
}

enum class QueueOutcome {
  kQueued,
  kNotDirty,
  kPending,
  kPinned,
};

// Publishes a queued writeback for a dirty resident frame in one CAS. With
// `skip_if_pinned`, a pinned frame yields kPinned instead.
inline auto TryQueueWriteback(std::atomic<std::uint32_t> &control, bool skip_if_pinned) -> QueueOutcome {
  std::uint32_t word = control.load(std::memory_order_acquire);
  while (true) {
    if (HasWriteback(word)) return QueueOutcome::kPending;
    if (!IsResident(word) || !IsDirty(word)) return QueueOutcome::kNotDirty;
    if (skip_if_pinned && Pins(word) != 0) return QueueOutcome::kPinned;
    const std::uint32_t next = (word | kWritebackQueued) & ~kRedirtied;
    if (control.compare_exchange_weak(word, next, std::memory_order_acq_rel, std::memory_order_acquire)) return QueueOutcome::kQueued;
  }
}

inline void BeginWriteback(std::atomic<std::uint32_t> &control) {
  UpdateAndWake(
    control,
    [](std::uint32_t word) { return (word & ~kWritebackQueued) | kWritebackActive; },
    [](std::uint32_t, std::uint32_t) { return false; });
}

// Withdraws a queued writeback that never reached the scheduler.
inline void CancelQueuedWriteback(std::atomic<std::uint32_t> &control) {
  UpdateAndWake(
    control,
    [](std::uint32_t word) { return word & ~(kWritebackQueued | kRedirtied); },
    [](std::uint32_t, std::uint32_t) { return true; });
}

struct WritebackCompletion {
  bool cleared_dirty{false};
  std::uint32_t word{0};
};

// Ends a writeback. Dirty is cleared only when the write succeeded, nothing
// changed the page since its snapshot (`redirtied` clear), and no exclusive
// holder could still change it, decided in the same CAS that clears the
// writeback flags.
inline auto CompleteWriteback(std::atomic<std::uint32_t> &control, bool succeeded) -> WritebackCompletion {
  WritebackCompletion completion;
  const std::uint32_t old_word = UpdateAndWake(
    control,
    [succeeded](std::uint32_t word) {
      std::uint32_t next = word & ~(kWritebackMask | kRedirtied);
      if (succeeded && IsResident(word) && IsDirty(word) && (word & (kRedirtied | kExclusive)) == 0) next &= ~kDirty;
      return next;
    },
    [](std::uint32_t, std::uint32_t) { return true; });
  completion.cleared_dirty = IsDirty(old_word) && succeeded && IsResident(old_word) && (old_word & (kRedirtied | kExclusive)) == 0;
  completion.word = old_word & ~(kWritebackMask | kRedirtied | kWaiters);
  if (completion.cleared_dirty) completion.word &= ~kDirty;
  return completion;
}

auto PackWritebackError(const Status &status) -> std::uint64_t;
auto UnpackWritebackError(std::uint64_t packed) -> Status;

}  // namespace telepath::frame_control

#endif  // TELEPATH_LIB_BUFFER_FRAME_CONTROL_H_
