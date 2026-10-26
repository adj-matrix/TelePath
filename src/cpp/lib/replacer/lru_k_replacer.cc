#include "telepath/replacer/lru_k_replacer.h"

#include <cstdint>
#include <mutex>
#include <utility>

#include "access_batcher.h"
#include "indexed_ordered_set.h"
#include "../common/state_layout.h"
#include "../sync/futex.h"

namespace telepath {

namespace {

struct Header {
  sync::FutexMutex latch;
  std::uint64_t clock{0};
  FrameId incomplete_root{IndexedOrderedSet::kNone};
  FrameId complete_root{IndexedOrderedSet::kNone};
};

// Access history of one frame: a ring of its last K timestamps.
struct History {
  bool tracked{false};
  std::uint32_t count{0};
  std::uint32_t oldest{0};
};

struct Offsets {
  std::size_t header;
  std::size_t histories;
  std::size_t timestamps;
  std::size_t nodes;
  std::size_t bytes;
};

auto LayoutFor(std::size_t capacity, std::size_t history_length) -> Offsets {
  StateLayout layout;
  Offsets offsets{};
  offsets.header = layout.Add<Header>(1);
  offsets.histories = layout.Add<History>(capacity);
  offsets.timestamps = layout.Add<std::uint64_t>(capacity * history_length);
  offsets.nodes = layout.Add<IndexedOrderedSet::Node>(capacity);
  offsets.bytes = layout.size();
  return offsets;
}

}  // namespace

// A frame with fewer than K recorded accesses waits in the incomplete queue,
// ordered by its latest access; the others wait in the complete queue, ordered
// by their K-th most recent access. Both queues share one node array because
// a frame is in at most one of them.
struct LruKReplacer::State {
  State(std::size_t capacity, std::size_t history_length)
    : capacity(capacity),
      history_length(history_length == 0 ? 1 : history_length),
      offsets(LayoutFor(capacity, this->history_length)),
      owned(std::make_unique<std::byte[]>(offsets.bytes)) {
    Place(owned.get(), true);
  }

  void Place(std::byte *base, bool initialize) {
    if (initialize) {
      header = ConstructState<Header>(base, offsets.header, 1);
      histories = ConstructState<History>(base, offsets.histories, capacity);
      timestamps = ConstructState<std::uint64_t>(base, offsets.timestamps, capacity * history_length, std::uint64_t{0});
      nodes = ConstructState<IndexedOrderedSet::Node>(base, offsets.nodes, capacity);
    } else {
      header = StateAt<Header>(base, offsets.header);
      histories = StateAt<History>(base, offsets.histories);
      timestamps = StateAt<std::uint64_t>(base, offsets.timestamps);
      nodes = StateAt<IndexedOrderedSet::Node>(base, offsets.nodes);
    }
    incomplete = IndexedOrderedSet(nodes, &header->incomplete_root);
    complete = IndexedOrderedSet(nodes, &header->complete_root);
  }

  bool HasIncompleteHistory(FrameId frame_id) const { return histories[frame_id].count < history_length; }

  auto Timestamp(FrameId frame_id, std::size_t age) const -> std::uint64_t {
    const History &history = histories[frame_id];
    return timestamps[frame_id * history_length + (history.oldest + age) % history_length];
  }

  void Enqueue(FrameId frame_id) {
    const std::uint32_t count = histories[frame_id].count;
    if (HasIncompleteHistory(frame_id)) incomplete.Insert(frame_id, Timestamp(frame_id, count - 1));
    else complete.Insert(frame_id, Timestamp(frame_id, 0));
  }

  void Dequeue(FrameId frame_id) {
    if (HasIncompleteHistory(frame_id)) incomplete.Erase(frame_id);
    else complete.Erase(frame_id);
  }

  void Append(FrameId frame_id) {
    History &history = histories[frame_id];
    const std::uint64_t now = ++header->clock;
    if (history.count < history_length) {
      timestamps[frame_id * history_length + (history.oldest + history.count) % history_length] = now;
      ++history.count;
      return;
    }
    timestamps[frame_id * history_length + history.oldest] = now;
    history.oldest = static_cast<std::uint32_t>((history.oldest + 1) % history_length);
  }

  void Touch(FrameId frame_id) {
    if (frame_id >= capacity || !histories[frame_id].tracked) return;
    Dequeue(frame_id);
    Append(frame_id);
    Enqueue(frame_id);
  }

  static bool PickFrom(const IndexedOrderedSet &queue, const EvictionProbe &probe, FrameId *frame_id) {
    for (FrameId candidate = queue.First(); candidate != IndexedOrderedSet::kNone; candidate = queue.Next(candidate)) {
      if (!probe.IsEvictable(candidate)) continue;
      *frame_id = candidate;
      return true;
    }
    return false;
  }

  const std::size_t capacity;
  const std::size_t history_length;
  const Offsets offsets;
  std::unique_ptr<std::byte[]> owned;
  Header *header{nullptr};
  History *histories{nullptr};
  std::uint64_t *timestamps{nullptr};
  IndexedOrderedSet::Node *nodes{nullptr};
  IndexedOrderedSet incomplete;
  IndexedOrderedSet complete;
  AccessBatcher batcher;
};

LruKReplacer::LruKReplacer(std::size_t capacity, std::size_t history_length)
  : state_(std::make_unique<State>(capacity, history_length)) {}

LruKReplacer::~LruKReplacer() = default;

auto LruKReplacer::SharedStateBytes() const -> std::size_t { return state_->offsets.bytes; }

auto LruKReplacer::BindSharedState(std::span<std::byte> memory, bool initialize) -> Status {
  if (Status valid = CheckStateMemory(memory, state_->offsets.bytes); !valid.has_value()) return valid;
  state_->Place(memory.data(), initialize);
  state_->owned.reset();
  return Ok();
}

void LruKReplacer::RecordAccess(FrameId frame_id) {
  state_->batcher.Record(frame_id, state_->header->latch, [this](FrameId frame) { state_->Touch(frame); });
}

void LruKReplacer::RecordLoad(FrameId frame_id) {
  if (frame_id >= state_->capacity) return;
  std::lock_guard<sync::FutexMutex> guard(state_->header->latch);
  state_->batcher.DrainCurrentThread([this](FrameId frame) { state_->Touch(frame); });
  History &history = state_->histories[frame_id];
  if (history.tracked) state_->Dequeue(frame_id);
  history = History{true, 0, 0};
  state_->Append(frame_id);
  state_->Enqueue(frame_id);
}

bool LruKReplacer::Victim(const EvictionProbe &probe, FrameId *frame_id) {
  if (frame_id == nullptr) return false;
  std::lock_guard<sync::FutexMutex> guard(state_->header->latch);
  state_->batcher.DrainAll([this](FrameId frame) { state_->Touch(frame); });
  return State::PickFrom(state_->incomplete, probe, frame_id) || State::PickFrom(state_->complete, probe, frame_id);
}

void LruKReplacer::Remove(FrameId frame_id) {
  if (frame_id >= state_->capacity) return;
  std::lock_guard<sync::FutexMutex> guard(state_->header->latch);
  state_->batcher.DrainCurrentThread([this](FrameId frame) { state_->Touch(frame); });
  History &history = state_->histories[frame_id];
  if (!history.tracked) return;
  state_->Dequeue(frame_id);
  history = History{};
}

auto MakeLruKReplacer(std::size_t capacity, std::size_t history_length) -> std::unique_ptr<Replacer> {
  return std::make_unique<LruKReplacer>(capacity, history_length);
}

}  // namespace telepath
