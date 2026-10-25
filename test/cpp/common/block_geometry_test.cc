#include <cassert>
#include <cstddef>

#include "telepath/common/block_geometry.h"

namespace {

void AssertValidSizesProduceShiftAndMask() {
  for (std::size_t size = telepath::BlockGeometry::kMinBlockSize; size <= telepath::BlockGeometry::kMaxBlockSize; size <<= 1) {
    auto geometry = telepath::BlockGeometry::Create(size);
    assert(geometry.has_value());
    assert(geometry->block_size() == size);
    assert((std::size_t{1} << geometry->block_shift()) == size);
    assert(geometry->block_mask() == size - 1);
  }
}

void AssertInvalidSizesRejected() {
  for (std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{512}, std::size_t{2048}, std::size_t{4095},
                           std::size_t{6144}, std::size_t{10000}, std::size_t{8} << 20}) {
    auto geometry = telepath::BlockGeometry::Create(size);
    assert(!geometry.has_value());
    assert(geometry.error().code() == telepath::ErrorCode::kInvalidArgument);
  }
}

void AssertOffsetsUseShift() {
  constexpr auto geometry = telepath::BlockGeometry::Of<8192>();
  static_assert(geometry.block_shift() == 13);
  static_assert(geometry.BlockOffset(3) == 3 * 8192);
  static_assert(geometry.FrameOffset(5) == 5 * 8192);
  assert(geometry.BlockOffset(telepath::BlockId{1} << 40) == (std::uint64_t{1} << 53));
  assert(geometry.MaxFrameCount() == (SIZE_MAX >> 13));
}

void AssertEqualityComparesBlockSize() {
  assert(telepath::BlockGeometry::Of<4096>() == telepath::BlockGeometry::Create(4096).value());
  assert(!(telepath::BlockGeometry::Of<4096>() == telepath::BlockGeometry::Of<8192>()));
}

}  // namespace

int main() {
  AssertValidSizesProduceShiftAndMask();
  AssertInvalidSizesRejected();
  AssertOffsetsUseShift();
  AssertEqualityComparesBlockSize();
  return 0;
}
