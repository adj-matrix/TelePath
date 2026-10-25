#ifndef TELEPATH_IO_DISK_BACKEND_H_
#define TELEPATH_IO_DISK_BACKEND_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "telepath/common/status.h"
#include "telepath/common/types.h"
#include "telepath/io/disk_backend_options.h"

namespace telepath {

enum class DiskOperation {
  kRead = 0,
  kWrite,
};

// How the submitter consumes the completions of one Submit() call.
enum class DiskSubmitMode {
  // The submitting thread reaps these completions itself right after Submit()
  // returns. A backend may perform the requests on the calling thread, and
  // need not wake threads blocked in Reap() for them.
  kWait = 0,
  // Nobody waits on these completions right away (prefetch, background
  // writeback). The backend should return without performing the I/O on the
  // calling thread, and must wake a blocked Reap() caller when they complete.
  kAsync,
};

// Upper bound on DiskRequest::block_count for every backend.
inline constexpr std::size_t kMaxDiskRequestBlocks = 32;

// One read or write of `block_count` consecutive blocks starting at `tag`.
// Block i is transferred to or from `buffers[i]`, each exactly one block long;
// a write only reads its buffers. The buffers must stay valid until the
// request's completion has been reaped. A read past the end of the backing
// file completes successfully with the missing bytes zero-filled.
struct DiskRequest {
  // Opaque value returned unchanged in the request's DiskCompletion.
  uint64_t token{0};
  DiskOperation operation{DiskOperation::kRead};
  BufferTag tag{};
  std::uint32_t block_count{1};
  std::array<std::byte *, kMaxDiskRequestBlocks> buffers{};
};

struct DiskCompletion {
  uint64_t token{0};
  Status status{};
};

struct DiskBackendCapabilities {
  DiskBackendKind kind{DiskBackendKind::kPosix};
  bool is_fallback_backend{false};
  // Number of requests the backend can usefully keep in flight at once.
  std::size_t queue_depth{1};
  // Largest DiskRequest::block_count the backend accepts, at most
  // kMaxDiskRequestBlocks.
  std::size_t max_request_blocks{1};
  // Block size the backend was configured with, or 0 when the backend accepts
  // any request size. BufferManager::Create rejects a backend whose block size
  // differs from the manager's.
  std::size_t block_size{0};
};

// Asynchronous block storage. Every accepted request produces exactly one
// DiskCompletion, carrying the request's token, from Reap(). All methods are
// thread-safe.
class DiskBackend {
 public:
  virtual ~DiskBackend() = default;

  // Starts `requests` in order and returns how many were accepted. Requests
  // after the accepted prefix were not started and may be submitted again.
  // Returns an error only when no request was accepted; kInvalidArgument
  // reports a malformed first request and kUnavailable a backend that was shut
  // down.
  virtual auto Submit(std::span<const DiskRequest> requests, DiskSubmitMode mode) -> Result<std::size_t> = 0;
  // Moves up to `out.size()` completions into `out` and returns their number.
  // Without `wait` it never blocks and may return 0, for instance when the
  // ready completions belong to requests submitted by other threads. With
  // `wait` it blocks until a completion is available, but may return 0 after a
  // spurious wakeup; it returns kUnavailable once Shutdown() was called and no
  // accepted request is outstanding. Concurrent callers receive disjoint
  // completions.
  virtual auto Reap(std::span<DiskCompletion> out, bool wait) -> Result<std::size_t> = 0;
  // Rejects further submissions. Outstanding requests still complete, after
  // which blocked Reap() callers return kUnavailable.
  virtual void Shutdown() = 0;
  // Describes the backend implementation and its limits.
  virtual auto GetCapabilities() const -> DiskBackendCapabilities = 0;
};

}  // namespace telepath

#endif  // TELEPATH_IO_DISK_BACKEND_H_
