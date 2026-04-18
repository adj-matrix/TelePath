#ifndef TELEPATH_LIB_BUFFER_PAGE_TABLE_H_
#define TELEPATH_LIB_BUFFER_PAGE_TABLE_H_

#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "telepath/common/types.h"

namespace telepath {

class BufferManagerPageTable {
 public:
  explicit BufferManagerPageTable(std::size_t stripe_count);

  BufferManagerPageTable(const BufferManagerPageTable &) = delete;
  BufferManagerPageTable &operator=(const BufferManagerPageTable &) = delete;

  auto LookupFrameId(const BufferTag &tag) -> std::optional<FrameId>;
  void Install(const BufferTag &tag, FrameId frame_id);
  void Remove(const BufferTag &tag, FrameId frame_id);
  void Replace(
    const BufferTag &old_tag,
    const BufferTag &new_tag,
    FrameId frame_id);

 private:
  auto GetStripe(const BufferTag &tag) const -> std::size_t;
  void ReplaceInStripe(
    std::size_t stripe,
    const BufferTag &old_tag,
    const BufferTag &new_tag,
    FrameId frame_id);
  void ReplaceAcrossStripes(
    std::size_t old_stripe,
    const BufferTag &old_tag,
    std::size_t new_stripe, 
    const BufferTag &new_tag,
    FrameId frame_id);

  std::vector<std::mutex> latches_;
  std::vector<std::unordered_map<BufferTag, FrameId, BufferTagHash>> shards_;
};

}  // namespace telepath

#endif  // TELEPATH_LIB_BUFFER_PAGE_TABLE_H_
