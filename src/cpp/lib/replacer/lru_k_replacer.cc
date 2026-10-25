#include "telepath/replacer/lru_k_replacer.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

#include "access_batcher.h"

namespace telepath {

struct LruKReplacer::State {
  using QueueKey = std::pair<std::uint64_t, FrameId>;

  struct Entry {
    bool tracked{false};
    std::deque<std::uint64_t> history;
  };

  State(std::size_t capacity, std::size_t history_length)
    : capacity(capacity), history_length(history_length == 0 ? 1 : history_length), entries(capacity) {}

  bool HasIncompleteHistory(const Entry &entry) const { return entry.history.size() < history_length; }

  auto QueueFor(const Entry &entry) -> std::set<QueueKey> & {
    return HasIncompleteHistory(entry) ? incomplete_queue : complete_queue;
  }

  auto KeyFor(const Entry &entry, FrameId frame_id) const -> QueueKey {
    return HasIncompleteHistory(entry) ? QueueKey{entry.history.back(), frame_id} : QueueKey{entry.history.front(), frame_id};
  }

  void Enqueue(FrameId frame_id) { QueueFor(entries[frame_id]).insert(KeyFor(entries[frame_id], frame_id)); }
  void Dequeue(FrameId frame_id) { QueueFor(entries[frame_id]).erase(KeyFor(entries[frame_id], frame_id)); }

  void Touch(FrameId frame_id) {
    if (frame_id >= capacity || !entries[frame_id].tracked) return;
    Dequeue(frame_id);
    Entry &entry = entries[frame_id];
    entry.history.push_back(++clock);
    while (entry.history.size() > history_length) entry.history.pop_front();
    Enqueue(frame_id);
  }

  static bool PickFrom(const std::set<QueueKey> &queue, const EvictionProbe &probe, FrameId *frame_id) {
    for (const auto &[timestamp, candidate] : queue) {
      if (!probe.IsEvictable(candidate)) continue;
      *frame_id = candidate;
      return true;
    }
    return false;
  }

  const std::size_t capacity;
  const std::size_t history_length;
  std::mutex latch;
  AccessBatcher batcher;
  std::vector<Entry> entries;
  std::set<QueueKey> incomplete_queue;
  std::set<QueueKey> complete_queue;
  std::uint64_t clock{0};
};

LruKReplacer::LruKReplacer(std::size_t capacity, std::size_t history_length)
  : state_(std::make_unique<State>(capacity, history_length)) {}

LruKReplacer::~LruKReplacer() = default;

void LruKReplacer::RecordAccess(FrameId frame_id) {
  state_->batcher.Record(frame_id, state_->latch, [this](FrameId frame) { state_->Touch(frame); });
}

void LruKReplacer::RecordLoad(FrameId frame_id) {
  if (frame_id >= state_->capacity) return;
  std::lock_guard<std::mutex> guard(state_->latch);
  state_->batcher.DrainCurrentThread([this](FrameId frame) { state_->Touch(frame); });
  State::Entry &entry = state_->entries[frame_id];
  if (entry.tracked) state_->Dequeue(frame_id);
  entry.tracked = true;
  entry.history.assign(1, ++state_->clock);
  state_->Enqueue(frame_id);
}

bool LruKReplacer::Victim(const EvictionProbe &probe, FrameId *frame_id) {
  if (frame_id == nullptr) return false;
  std::lock_guard<std::mutex> guard(state_->latch);
  state_->batcher.DrainAll([this](FrameId frame) { state_->Touch(frame); });
  return State::PickFrom(state_->incomplete_queue, probe, frame_id) || State::PickFrom(state_->complete_queue, probe, frame_id);
}

void LruKReplacer::Remove(FrameId frame_id) {
  if (frame_id >= state_->capacity) return;
  std::lock_guard<std::mutex> guard(state_->latch);
  state_->batcher.DrainCurrentThread([this](FrameId frame) { state_->Touch(frame); });
  State::Entry &entry = state_->entries[frame_id];
  if (!entry.tracked) return;
  state_->Dequeue(frame_id);
  entry.tracked = false;
  entry.history.clear();
}

auto MakeLruKReplacer(std::size_t capacity, std::size_t history_length) -> std::unique_ptr<Replacer> {
  return std::make_unique<LruKReplacer>(capacity, history_length);
}

}  // namespace telepath
