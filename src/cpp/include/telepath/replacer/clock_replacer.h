#ifndef TELEPATH_REPLACER_CLOCK_REPLACER_H_
#define TELEPATH_REPLACER_CLOCK_REPLACER_H_

#include <mutex>
#include <vector>

#include "telepath/replacer/replacer.h"

namespace telepath {

class ClockReplacer : public Replacer {
 public:
  explicit ClockReplacer(std::size_t capacity);

  void RecordAccess(FrameId frame_id) override;
  void SetEvictable(FrameId frame_id, bool evictable) override;
  bool Victim(FrameId *frame_id) override;
  void Remove(FrameId frame_id) override;
  std::size_t Size() const override;

 private:
  struct Entry {
    bool referenced{false};
    bool evictable{false};
    bool present{false};
  };

  bool IsValidFrame(FrameId frame_id) const;

  const std::size_t capacity_;
  mutable std::mutex latch_;
  std::vector<Entry> entries_;
  std::size_t hand_{0};
  std::size_t size_{0};
};

}  // namespace telepath

#endif  // TELEPATH_REPLACER_CLOCK_REPLACER_H_
