#ifndef TELEPATH_IO_DISK_BACKEND_H_
#define TELEPATH_IO_DISK_BACKEND_H_

#include <cstddef>
#include <cstdint>

#include "telepath/common/status.h"
#include "telepath/common/types.h"
#include "telepath/io/disk_backend_options.h"

namespace telepath {

enum class DiskOperation {
  kRead = 0,
  kWrite,
};

struct DiskRequest {
  uint64_t request_id{0};
  DiskOperation operation{DiskOperation::kRead};
  BufferTag tag{};
  std::byte *mutable_buffer{nullptr};
  const std::byte *const_buffer{nullptr};
  std::size_t size{0};
};

struct DiskCompletion {
  uint64_t request_id{0};
  DiskOperation operation{DiskOperation::kRead};
  BufferTag tag{};
  Status status{};
};

struct DiskBackendCapabilities {
  DiskBackendKind kind{DiskBackendKind::kPosix};
  bool supports_submit_batching{false};
  bool supports_completion_batching{false};
  std::size_t recommended_queue_depth{1};
  bool is_fallback_backend{false};
};

class DiskBackend {
 public:
  virtual ~DiskBackend() = default;

  // Enqueues an asynchronous page read into `out`. The returned request id must
  // later be matched with PollCompletion().
  virtual auto SubmitRead(
    const BufferTag &tag,
    std::byte *out,
    std::size_t size
  ) -> Result<uint64_t> = 0;
  // Enqueues an asynchronous page write from `data`. The caller must keep the
  // source buffer stable until the corresponding completion is observed.
  virtual auto SubmitWrite(
    const BufferTag &tag,
    const std::byte *data,
    std::size_t size
  ) -> Result<uint64_t> = 0;
  // Waits for and returns the next completed disk request.
  virtual auto PollCompletion() -> Result<DiskCompletion> = 0;
  // Stops the backend and unblocks any thread waiting in PollCompletion().
  virtual void Shutdown() = 0;
  // Describes the backend implementation and key async capabilities.
  virtual auto GetCapabilities() const -> DiskBackendCapabilities = 0;
};

}  // namespace telepath

#endif  // TELEPATH_IO_DISK_BACKEND_H_
