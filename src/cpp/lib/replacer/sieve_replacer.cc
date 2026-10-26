#include "telepath/replacer/sieve_replacer.h"

#include <mutex>

#include "../common/state_layout.h"
#include "../sync/futex.h"

namespace telepath {

namespace {

constexpr FrameId kNone = kInvalidFrameId;

// Queue ends and hand, guarded by the latch.
struct Queue {
  sync::FutexMutex latch;
  FrameId newest{kNone};
  FrameId oldest{kNone};
  FrameId hand{kNone};
  std::size_t linked_count{0};
};

struct Offsets {
  std::size_t queue;
  std::size_t visited;
  std::size_t newer;
  std::size_t older;
  std::size_t linked;
  std::size_t bytes;
};

auto LayoutFor(std::size_t capacity) -> Offsets {
  StateLayout layout;
  Offsets offsets{};
  offsets.queue = layout.Add<Queue>(1);
  offsets.visited = layout.Add<std::atomic<std::uint8_t>>(capacity);
  offsets.newer = layout.Add<FrameId>(capacity);
  offsets.older = layout.Add<FrameId>(capacity);
  offsets.linked = layout.Add<bool>(capacity);
  offsets.bytes = layout.size();
  return offsets;
}

}  // namespace

struct SieveReplacer::State {
  explicit State(std::size_t capacity) : capacity(capacity), offsets(LayoutFor(capacity)), owned(std::make_unique<std::byte[]>(offsets.bytes)) { Place(owned.get(), true); }

  void Place(std::byte *base, bool initialize) {
    if (initialize) {
      queue = ConstructState<Queue>(base, offsets.queue, 1);
      visited = ConstructState<std::atomic<std::uint8_t>>(base, offsets.visited, capacity, std::uint8_t{0});
      newer = ConstructState<FrameId>(base, offsets.newer, capacity, kNone);
      older = ConstructState<FrameId>(base, offsets.older, capacity, kNone);
      linked = ConstructState<bool>(base, offsets.linked, capacity, false);
      return;
    }
    queue = StateAt<Queue>(base, offsets.queue);
    visited = StateAt<std::atomic<std::uint8_t>>(base, offsets.visited);
    newer = StateAt<FrameId>(base, offsets.newer);
    older = StateAt<FrameId>(base, offsets.older);
    linked = StateAt<bool>(base, offsets.linked);
  }

  void Unlink(FrameId frame_id) {
    const FrameId newer_frame = newer[frame_id];
    const FrameId older_frame = older[frame_id];
    if (queue->hand == frame_id) queue->hand = newer_frame;
    if (newer_frame != kNone) older[newer_frame] = older_frame;
    else queue->newest = older_frame;
    if (older_frame != kNone) newer[older_frame] = newer_frame;
    else queue->oldest = newer_frame;
    linked[frame_id] = false;
    --queue->linked_count;
  }

  void PushNewest(FrameId frame_id) {
    newer[frame_id] = kNone;
    older[frame_id] = queue->newest;
    if (queue->newest != kNone) newer[queue->newest] = frame_id;
    queue->newest = frame_id;
    if (queue->oldest == kNone) queue->oldest = frame_id;
    linked[frame_id] = true;
    ++queue->linked_count;
  }

  const std::size_t capacity;
  const Offsets offsets;
  std::unique_ptr<std::byte[]> owned;
  Queue *queue{nullptr};
  std::atomic<std::uint8_t> *visited{nullptr};
  FrameId *newer{nullptr};
  FrameId *older{nullptr};
  bool *linked{nullptr};
};

SieveReplacer::SieveReplacer(std::size_t capacity) : state_(std::make_unique<State>(capacity)) {}

SieveReplacer::~SieveReplacer() = default;

auto SieveReplacer::SharedStateBytes() const -> std::size_t { return state_->offsets.bytes; }

auto SieveReplacer::BindSharedState(std::span<std::byte> memory, bool initialize) -> Status {
  if (Status valid = CheckStateMemory(memory, state_->offsets.bytes); !valid.has_value()) return valid;
  state_->Place(memory.data(), initialize);
  state_->owned.reset();
  return Ok();
}

void SieveReplacer::RecordAccess(FrameId frame_id) {
  if (frame_id >= state_->capacity) return;
  std::atomic<std::uint8_t> &visited = state_->visited[frame_id];
  if (visited.load(std::memory_order_relaxed) == 0) visited.store(1, std::memory_order_relaxed);
}

void SieveReplacer::RecordLoad(FrameId frame_id) {
  if (frame_id >= state_->capacity) return;
  std::lock_guard<sync::FutexMutex> guard(state_->queue->latch);
  if (state_->linked[frame_id]) state_->Unlink(frame_id);
  state_->visited[frame_id].store(0, std::memory_order_relaxed);
  state_->PushNewest(frame_id);
}

bool SieveReplacer::Victim(const EvictionProbe &probe, FrameId *frame_id) {
  if (frame_id == nullptr) return false;
  Queue &queue = *state_->queue;
  std::lock_guard<sync::FutexMutex> guard(queue.latch);
  FrameId candidate = queue.hand != kNone ? queue.hand : queue.oldest;
  for (std::size_t scanned = 0; candidate != kNone && scanned < 2 * queue.linked_count; ++scanned) {
    const FrameId next = state_->newer[candidate] != kNone ? state_->newer[candidate] : queue.oldest;
    if (!probe.IsEvictable(candidate)) {
      candidate = next;
      continue;
    }
    std::atomic<std::uint8_t> &visited = state_->visited[candidate];
    if (visited.load(std::memory_order_relaxed) != 0) {
      visited.store(0, std::memory_order_relaxed);
      candidate = next;
      continue;
    }
    queue.hand = next;
    *frame_id = candidate;
    return true;
  }
  queue.hand = candidate;
  return false;
}

void SieveReplacer::Remove(FrameId frame_id) {
  if (frame_id >= state_->capacity) return;
  std::lock_guard<sync::FutexMutex> guard(state_->queue->latch);
  if (state_->linked[frame_id]) state_->Unlink(frame_id);
  state_->visited[frame_id].store(0, std::memory_order_relaxed);
}

auto MakeSieveReplacer(std::size_t capacity) -> std::unique_ptr<Replacer> {
  return std::make_unique<SieveReplacer>(capacity);
}

}  // namespace telepath
