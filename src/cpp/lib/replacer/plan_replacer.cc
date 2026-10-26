#include "telepath/replacer/plan_replacer.h"

#include <mutex>

#include "indexed_ordered_set.h"
#include "../common/state_layout.h"
#include "../sync/futex.h"

namespace telepath {

namespace {

struct Header {
  sync::FutexMutex latch;
  FrameId root{IndexedOrderedSet::kNone};
};

struct Offsets {
  std::size_t header;
  std::size_t nodes;
  std::size_t bytes;
};

auto LayoutFor(std::size_t capacity) -> Offsets {
  StateLayout layout;
  Offsets offsets{};
  offsets.header = layout.Add<Header>(1);
  offsets.nodes = layout.Add<IndexedOrderedSet::Node>(capacity);
  offsets.bytes = layout.size();
  return offsets;
}

}  // namespace

// Tracked frames are the members of `order`, keyed by their next use.
struct PlanReplacer::State {
  explicit State(std::size_t capacity) : capacity(capacity), offsets(LayoutFor(capacity)), owned(std::make_unique<std::byte[]>(offsets.bytes)) { Place(owned.get(), true); }

  void Place(std::byte *base, bool initialize) {
    if (initialize) {
      header = ConstructState<Header>(base, offsets.header, 1);
      nodes = ConstructState<IndexedOrderedSet::Node>(base, offsets.nodes, capacity);
    } else {
      header = StateAt<Header>(base, offsets.header);
      nodes = StateAt<IndexedOrderedSet::Node>(base, offsets.nodes);
    }
    order = IndexedOrderedSet(nodes, &header->root);
  }

  void Update(FrameId frame_id, std::uint64_t next_use) {
    order.Erase(frame_id);
    order.Insert(frame_id, next_use);
  }

  const std::size_t capacity;
  const Offsets offsets;
  std::unique_ptr<std::byte[]> owned;
  Header *header{nullptr};
  IndexedOrderedSet::Node *nodes{nullptr};
  IndexedOrderedSet order;
};

PlanReplacer::PlanReplacer(std::size_t capacity) : state_(std::make_unique<State>(capacity)) {}

PlanReplacer::~PlanReplacer() = default;

auto PlanReplacer::SharedStateBytes() const -> std::size_t { return state_->offsets.bytes; }

auto PlanReplacer::BindSharedState(std::span<std::byte> memory, bool initialize) -> Status {
  if (Status valid = CheckStateMemory(memory, state_->offsets.bytes); !valid.has_value()) return valid;
  state_->Place(memory.data(), initialize);
  state_->owned.reset();
  return Ok();
}

void PlanReplacer::RecordAccess(FrameId) {}

void PlanReplacer::RecordLoad(FrameId frame_id) {
  if (frame_id >= state_->capacity) return;
  std::lock_guard<sync::FutexMutex> guard(state_->header->latch);
  state_->Update(frame_id, 0);
}

bool PlanReplacer::Victim(const EvictionProbe &probe, FrameId *frame_id) {
  if (frame_id == nullptr) return false;
  std::lock_guard<sync::FutexMutex> guard(state_->header->latch);
  for (FrameId candidate = state_->order.Last(); candidate != IndexedOrderedSet::kNone; candidate = state_->order.Previous(candidate)) {
    if (!probe.IsEvictable(candidate)) continue;
    *frame_id = candidate;
    return true;
  }
  return false;
}

void PlanReplacer::Remove(FrameId frame_id) {
  if (frame_id >= state_->capacity) return;
  std::lock_guard<sync::FutexMutex> guard(state_->header->latch);
  state_->order.Erase(frame_id);
}

void PlanReplacer::SetNextUse(FrameId frame_id, std::uint64_t next_use) {
  if (frame_id >= state_->capacity) return;
  std::lock_guard<sync::FutexMutex> guard(state_->header->latch);
  if (state_->order.Contains(frame_id)) state_->Update(frame_id, next_use);
}

void PlanReplacer::ForgetPlan() {
  std::lock_guard<sync::FutexMutex> guard(state_->header->latch);
  for (FrameId frame_id = 0; frame_id < state_->capacity; ++frame_id) {
    if (state_->order.Contains(frame_id)) state_->Update(frame_id, kNeverUsed);
  }
}

}  // namespace telepath
