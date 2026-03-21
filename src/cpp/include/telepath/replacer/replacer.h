#ifndef TELEPATH_REPLACER_REPLACER_H_
#define TELEPATH_REPLACER_REPLACER_H_

#include <cstddef>
#include <memory>

#include "telepath/common/types.h"

namespace telepath {

class Replacer {
 public:
  virtual ~Replacer() = default;

  virtual void RecordAccess(FrameId frame_id) = 0;
  virtual void SetEvictable(FrameId frame_id, bool evictable) = 0;
  virtual bool Victim(FrameId *frame_id) = 0;
  virtual void Remove(FrameId frame_id) = 0;
  virtual std::size_t Size() const = 0;
};

std::unique_ptr<Replacer> MakeClockReplacer(std::size_t capacity);
std::unique_ptr<Replacer> MakeLruReplacer(std::size_t capacity);

}  // namespace telepath

#endif  // TELEPATH_REPLACER_REPLACER_H_
