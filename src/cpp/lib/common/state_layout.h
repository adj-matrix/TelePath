#ifndef TELEPATH_LIB_COMMON_STATE_LAYOUT_H_
#define TELEPATH_LIB_COMMON_STATE_LAYOUT_H_

#include <cstddef>
#include <memory>
#include <new>
#include <span>

#include "telepath/common/status.h"
#include "telepath/common/types.h"

namespace telepath {

// Places arrays one after another in one block, each aligned to a cache line
// or more, so the block can live on the heap or in shared memory. Offsets are
// relative to a base aligned to at least the largest alignment requested.
class StateLayout {
 public:
  template <typename T>
  auto Add(std::size_t count, std::size_t alignment = kCacheLineSize) -> std::size_t {
    const std::size_t offset = Align(size_, alignment);
    size_ = offset + count * sizeof(T);
    return offset;
  }

  auto size() const -> std::size_t { return Align(size_ == 0 ? 1 : size_, kCacheLineSize); }

 private:
  static auto Align(std::size_t value, std::size_t alignment) -> std::size_t { return (value + alignment - 1) & ~(alignment - 1); }

  std::size_t size_{0};
};

template <typename T>
auto StateAt(std::byte *base, std::size_t offset) -> T * {
  return std::launder(reinterpret_cast<T *>(base + offset));
}

// Starts the lifetime of `count` objects at `offset`, each built from `args`.
template <typename T, typename... Args>
auto ConstructState(std::byte *base, std::size_t offset, std::size_t count, const Args &...args) -> T * {
  T *first = reinterpret_cast<T *>(base + offset);
  for (std::size_t i = 0; i < count; ++i) std::construct_at(first + i, args...);
  return first;
}

inline auto CheckStateMemory(std::span<std::byte> memory, std::size_t bytes) -> Status {
  if (memory.size() < bytes) return InvalidArgument("replacer state memory is too small");
  if (reinterpret_cast<std::uintptr_t>(memory.data()) % kCacheLineSize != 0) return InvalidArgument("replacer state memory must be cache-line aligned");
  return Ok();
}

// Owns the heap block a replacer starts with, until its state moves to
// caller-provided memory.
class StateStorage {
 public:
  explicit StateStorage(std::size_t bytes) : owned_(std::make_unique<std::byte[]>(bytes)), data_(owned_.get()), size_(bytes) {}

  auto data() const -> std::byte * { return data_; }
  auto size() const -> std::size_t { return size_; }

  auto Bind(std::span<std::byte> memory) -> Status {
    if (memory.size() < size_) return InvalidArgument("replacer state memory is too small");
    if (reinterpret_cast<std::uintptr_t>(memory.data()) % kCacheLineSize != 0) return InvalidArgument("replacer state memory must be cache-line aligned");
    data_ = memory.data();
    owned_.reset();
    return Ok();
  }

 private:
  std::unique_ptr<std::byte[]> owned_;
  std::byte *data_;
  std::size_t size_;
};

}  // namespace telepath

#endif  // TELEPATH_LIB_COMMON_STATE_LAYOUT_H_
