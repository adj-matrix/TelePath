#ifndef TELEPATH_REPLACER_LRU_K_REPLACER_H_
#define TELEPATH_REPLACER_LRU_K_REPLACER_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>

#include "telepath/replacer/replacer.h"

namespace telepath {

class LruKReplacer : public Replacer {
 public:
  LruKReplacer(std::size_t capacity, std::size_t history_length);

  void RecordAccess(FrameId frame_id) override;
  void SetEvictable(FrameId frame_id, bool evictable) override;
  bool Victim(FrameId *frame_id) override;
  void Remove(FrameId frame_id) override;
  std::size_t Size() const override;

 private:
  struct Entry {
    bool evictable{false};
    std::deque<uint64_t> access_history;
  };

  bool IsValidFrame(FrameId frame_id) const;
  bool PreferCandidate(const Entry &candidate, FrameId candidate_id,
                       const Entry *current_best,
                       FrameId current_best_id) const;

  const std::size_t capacity_;
  const std::size_t history_length_;
  mutable std::mutex latch_;
  std::unordered_map<FrameId, Entry> entries_;
  uint64_t current_timestamp_{0};
};

}  // namespace telepath

#endif  // TELEPATH_REPLACER_LRU_K_REPLACER_H_
