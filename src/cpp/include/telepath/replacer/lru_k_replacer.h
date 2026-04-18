#ifndef TELEPATH_REPLACER_LRU_K_REPLACER_H_
#define TELEPATH_REPLACER_LRU_K_REPLACER_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <set>
#include <unordered_map>

#include "telepath/replacer/replacer.h"

namespace telepath {

// LRU-K replacer that chooses victims using bounded access history per frame.
class LruKReplacer : public Replacer {
 public:
  // Creates a replacer that can track up to `capacity` frame ids and compares
  // frames using access history of length `history_length`.
  LruKReplacer(std::size_t capacity, std::size_t history_length);

  // Appends a timestamped access for the given frame, inserting it on first
  // use if capacity permits.
  void RecordAccess(FrameId frame_id) override;
  // Toggles whether the frame may currently be chosen as a victim.
  void SetEvictable(FrameId frame_id, bool evictable) override;
  // Removes and returns the best current LRU-K victim among evictable frames.
  bool Victim(FrameId *frame_id) override;
  // Stops tracking the given frame if it is present.
  void Remove(FrameId frame_id) override;
  // Returns the current number of evictable tracked frames.
  auto Size() const -> std::size_t override;

 private:
  using QueueKey = std::pair<uint64_t, FrameId>;

  struct Entry {
    bool evictable{false};
    std::deque<uint64_t> access_history;
  };

  bool IsValidFrame(FrameId frame_id) const;
  bool HasIncompleteHistory(const Entry &entry) const;
  auto HistoryQueueKey(const Entry &entry, FrameId frame_id) const -> QueueKey;
  auto CacheQueueKey(const Entry &entry, FrameId frame_id) const -> QueueKey;
  void InsertEvictableEntry(FrameId frame_id, const Entry &entry);
  void EraseEvictableEntry(FrameId frame_id, const Entry &entry);
  bool TakeVictimFromQueue(std::set<QueueKey> *queue, FrameId *frame_id);

  const std::size_t capacity_;
  const std::size_t history_length_;
  mutable std::mutex latch_;
  std::unordered_map<FrameId, Entry> entries_;
  std::set<QueueKey> history_queue_;
  std::set<QueueKey> cache_queue_;
  uint64_t current_timestamp_{0};
  std::size_t evictable_size_{0};
};

}  // namespace telepath

#endif  // TELEPATH_REPLACER_LRU_K_REPLACER_H_
