#ifndef TELEPATH_BUFFER_BUFFER_HANDLE_H_
#define TELEPATH_BUFFER_BUFFER_HANDLE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <shared_mutex>

#include "telepath/common/types.h"

namespace telepath {

class BufferManager;

class BufferHandle {
 public:
  BufferHandle() = default;
  BufferHandle(BufferManager *manager, FrameId frame_id, const BufferTag &tag,
               std::byte *data, std::size_t size);

  BufferHandle(const BufferHandle &) = delete;
  BufferHandle &operator=(const BufferHandle &) = delete;

  BufferHandle(BufferHandle &&other) noexcept;
  BufferHandle &operator=(BufferHandle &&other) noexcept;

  ~BufferHandle();

  const BufferTag &tag() const { return tag_; }
  FrameId frame_id() const { return frame_id_; }
  std::size_t size() const { return size_; }

  const std::byte *data() const;
  std::byte *mutable_data();
  bool writable() const { return data_ != nullptr; }

  bool valid() const { return manager_ != nullptr; }

  void Reset();

 private:
  friend class BufferManager;

  BufferManager *manager_{nullptr};
  FrameId frame_id_{kInvalidFrameId};
  BufferTag tag_{};
  std::byte *data_{nullptr};
  std::size_t size_{0};
  mutable std::shared_lock<std::shared_mutex> read_lock_;
  std::unique_lock<std::shared_mutex> write_lock_;
};

}  // namespace telepath

#endif  // TELEPATH_BUFFER_BUFFER_HANDLE_H_
