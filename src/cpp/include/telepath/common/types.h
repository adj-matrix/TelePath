#ifndef TELEPATH_COMMON_TYPES_H_
#define TELEPATH_COMMON_TYPES_H_

#include <cstddef>
#include <cstdint>
#include <functional>

namespace telepath {

using FileId = uint32_t;
using BlockId = uint64_t;
using FrameId = uint32_t;

constexpr std::size_t kDefaultPageSize = 4096;
constexpr FrameId kInvalidFrameId = static_cast<FrameId>(-1);

struct BufferTag {
  FileId file_id{0};
  BlockId block_id{0};

  bool operator==(const BufferTag &other) const {
    return file_id == other.file_id && block_id == other.block_id;
  }

  bool operator!=(const BufferTag &other) const { return !(*this == other); }
};

struct BufferTagHash {
  std::size_t operator()(const BufferTag &tag) const {
    const std::size_t left = std::hash<FileId>{}(tag.file_id);
    const std::size_t right = std::hash<BlockId>{}(tag.block_id);
    return left ^ (right + 0x9e3779b9 + (left << 6U) + (left >> 2U));
  }
};

}  // namespace telepath

#endif  // TELEPATH_COMMON_TYPES_H_
