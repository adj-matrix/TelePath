#ifndef TELEPATH_COMMON_BLOCK_GEOMETRY_H_
#define TELEPATH_COMMON_BLOCK_GEOMETRY_H_

#include <bit>
#include <cstddef>
#include <cstdint>

#include "telepath/common/status.h"
#include "telepath/common/types.h"

namespace telepath {

// Validated block size of one cache instance and its backend. A BlockGeometry
// can only be obtained through Create() or Of<N>(), so holding one guarantees
// a power-of-two size within [kMinBlockSize, kMaxBlockSize]. All offset math
// uses shifts and masks.
class BlockGeometry {
 public:
  static constexpr std::size_t kMinBlockSize = std::size_t{1} << 12;
  static constexpr std::size_t kMaxBlockSize = std::size_t{1} << 22;

  static constexpr bool IsValidBlockSize(std::size_t block_size) noexcept {
    return std::has_single_bit(block_size) && block_size >= kMinBlockSize && block_size <= kMaxBlockSize;
  }

  // Returns kInvalidArgument unless block_size is a power of two within
  // [kMinBlockSize, kMaxBlockSize].
  static constexpr auto Create(std::size_t block_size) noexcept -> Result<BlockGeometry> {
    if (!IsValidBlockSize(block_size)) {
      return InvalidArgument("block size must be a power of two between 4 KiB and 4 MiB");
    }
    return BlockGeometry(static_cast<std::uint32_t>(std::countr_zero(block_size)));
  }

  // Compile-time checked geometry for a constant block size.
  template <std::size_t kBlockSize>
  static constexpr auto Of() noexcept -> BlockGeometry {
    static_assert(IsValidBlockSize(kBlockSize), "block size must be a power of two between 4 KiB and 4 MiB");
    return BlockGeometry(static_cast<std::uint32_t>(std::countr_zero(kBlockSize)));
  }

  constexpr auto block_size() const noexcept -> std::size_t { return std::size_t{1} << block_shift_; }
  constexpr auto block_shift() const noexcept -> std::uint32_t { return block_shift_; }
  constexpr auto block_mask() const noexcept -> std::uint64_t { return (std::uint64_t{1} << block_shift_) - 1; }

  // Byte offset of a block inside its backing file.
  constexpr auto BlockOffset(BlockId block_id) const noexcept -> std::uint64_t {
    return std::uint64_t{block_id} << block_shift_;
  }

  // Byte offset of a frame inside a contiguous frame arena.
  constexpr auto FrameOffset(FrameId frame_id) const noexcept -> std::size_t {
    return std::size_t{frame_id} << block_shift_;
  }

  // Largest frame count whose arena size fits in std::size_t.
  constexpr auto MaxFrameCount() const noexcept -> std::size_t {
    return SIZE_MAX >> block_shift_;
  }

  friend constexpr bool operator==(BlockGeometry, BlockGeometry) noexcept = default;

 private:
  explicit constexpr BlockGeometry(std::uint32_t block_shift) noexcept : block_shift_(block_shift) {}

  std::uint32_t block_shift_;
};

}  // namespace telepath

#endif  // TELEPATH_COMMON_BLOCK_GEOMETRY_H_
