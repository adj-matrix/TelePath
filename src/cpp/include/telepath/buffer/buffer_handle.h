#ifndef TELEPATH_BUFFER_BUFFER_HANDLE_H_
#define TELEPATH_BUFFER_BUFFER_HANDLE_H_

#include <cstddef>
#include <span>

#include "telepath/common/types.h"

namespace telepath {

class BufferManager;

// Shared access to one resident page, returned by BufferManager::ReadBuffer().
// The handle holds a pin and the page's shared content latch from creation
// until Reset() or destruction, so the page cannot be evicted or modified
// while it is alive. Handles are movable and may be moved to another thread,
// but must not be used by two threads at once. A thread must not hold two
// handles on the same page.
class SharedBufferHandle {
 public:
  SharedBufferHandle() = default;
  SharedBufferHandle(const SharedBufferHandle &) = delete;
  SharedBufferHandle &operator=(const SharedBufferHandle &) = delete;
  SharedBufferHandle(SharedBufferHandle &&other) noexcept;
  SharedBufferHandle &operator=(SharedBufferHandle &&other) noexcept;
  ~SharedBufferHandle();

  // Read-only view of the page; empty for a released handle.
  auto data() const noexcept -> std::span<const std::byte> { return {data_, size_}; }
  auto tag() const noexcept -> const BufferTag & { return tag_; }
  auto frame_id() const noexcept -> FrameId { return frame_id_; }
  auto size() const noexcept -> std::size_t { return size_; }
  bool valid() const noexcept { return manager_ != nullptr; }

  // Releases the latch and the pin. Safe to call more than once.
  void Reset() noexcept;

 private:
  friend class BufferManager;

  SharedBufferHandle(BufferManager *manager, FrameId frame_id, const BufferTag &tag, std::byte *data, std::size_t size) noexcept;

  BufferManager *manager_{nullptr};
  FrameId frame_id_{kInvalidFrameId};
  BufferTag tag_{};
  std::byte *data_{nullptr};
  std::size_t size_{0};
};

// Exclusive access to one resident page, returned by
// BufferManager::ReadBufferExclusive(). The handle holds a pin and the page's
// exclusive content latch from creation until Reset() or destruction.
// Obtaining the writable view marks the page dirty. A caller with a
// write-ahead log also reports each logged change through MarkDirty(lsn).
// Same threading rules as SharedBufferHandle.
class ExclusiveBufferHandle {
 public:
  ExclusiveBufferHandle() = default;
  ExclusiveBufferHandle(const ExclusiveBufferHandle &) = delete;
  ExclusiveBufferHandle &operator=(const ExclusiveBufferHandle &) = delete;
  ExclusiveBufferHandle(ExclusiveBufferHandle &&other) noexcept;
  ExclusiveBufferHandle &operator=(ExclusiveBufferHandle &&other) noexcept;
  ~ExclusiveBufferHandle();

  // Read-only view of the page. Does not mark the page dirty.
  auto data() const noexcept -> std::span<const std::byte> { return {data_, size_}; }
  // Writable view of the page; marks the page dirty. Empty for a released
  // handle.
  auto mutable_data() noexcept -> std::span<std::byte>;
  // Marks the page dirty and records that the log record at `lsn` changed it,
  // which BufferManager::MinRecLsn() and CheckpointBuffers() use. Call it for
  // every logged change, like PostgreSQL's MarkBufferDirty() with
  // PageSetLSN(); the earliest LSN reported since the page's last writeback
  // becomes its recLSN. Pages changed without an LSN are not tracked.
  void MarkDirty(Lsn lsn) noexcept;
  auto tag() const noexcept -> const BufferTag & { return tag_; }
  auto frame_id() const noexcept -> FrameId { return frame_id_; }
  auto size() const noexcept -> std::size_t { return size_; }
  bool valid() const noexcept { return manager_ != nullptr; }

  // Releases the latch and the pin. Safe to call more than once.
  void Reset() noexcept;

 private:
  friend class BufferManager;

  ExclusiveBufferHandle(BufferManager *manager, FrameId frame_id, const BufferTag &tag, std::byte *data, std::size_t size) noexcept;

  BufferManager *manager_{nullptr};
  FrameId frame_id_{kInvalidFrameId};
  BufferTag tag_{};
  std::byte *data_{nullptr};
  std::size_t size_{0};
  bool marked_dirty_{false};
};

}  // namespace telepath

#endif  // TELEPATH_BUFFER_BUFFER_HANDLE_H_
