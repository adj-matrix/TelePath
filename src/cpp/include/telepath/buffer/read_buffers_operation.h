#ifndef TELEPATH_BUFFER_READ_BUFFERS_OPERATION_H_
#define TELEPATH_BUFFER_READ_BUFFERS_OPERATION_H_

#include <memory>
#include <span>
#include <vector>

#include "telepath/buffer/buffer_handle.h"

namespace telepath {

class BufferManager;
struct ReadBuffersState;

// A read of consecutive blocks started by BufferManager::StartReadBuffers()
// and finished by WaitReadBuffers(), modeled after PostgreSQL's
// ReadBuffersOperation. Between the two calls the blocks are pinned and the
// missing ones are being read, so the caller can overlap other work with the
// I/O. Destroying an operation that was started but not waited for waits for
// its reads and drops the pins. Not thread-safe; an operation may be reused
// once it is no longer in progress.
class ReadBuffersOperation {
 public:
  ReadBuffersOperation();
  ReadBuffersOperation(const ReadBuffersOperation &) = delete;
  ReadBuffersOperation &operator=(const ReadBuffersOperation &) = delete;
  ~ReadBuffersOperation();

  // Shared handles for the blocks in block order, filled by a successful
  // WaitReadBuffers(). They stay valid until moved out, the operation is
  // reused, or the operation is destroyed.
  auto handles() noexcept -> std::span<SharedBufferHandle> { return handles_; }
  // True between a successful StartReadBuffers() and the matching
  // WaitReadBuffers().
  bool in_progress() const noexcept { return state_ != nullptr; }

 private:
  friend class BufferManager;

  BufferManager *manager_{nullptr};
  std::unique_ptr<ReadBuffersState> state_;
  std::vector<SharedBufferHandle> handles_;
};

}  // namespace telepath

#endif  // TELEPATH_BUFFER_READ_BUFFERS_OPERATION_H_
