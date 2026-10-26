#ifndef TELEPATH_IO_IO_URING_DISK_BACKEND_H_
#define TELEPATH_IO_IO_URING_DISK_BACKEND_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "telepath/common/block_geometry.h"
#include "telepath/io/disk_backend.h"

namespace telepath {

class FileDescriptorCache;
class IoUringDiskBackendTestPeer;

// Backend built on a set of io_uring instances, one per group of submitting
// threads, because a single ring stops scaling once several threads submit to
// it. A Submit() call is one io_uring_enter on the caller's ring; multi-block
// requests use readv/writev, and a write that must be durable carries a linked
// fdatasync. All rings signal one eventfd, which a blocking Reap() waits on.
class IoUringDiskBackend : public DiskBackend {
 public:
  // Creates a backend with initialized io_uring rings. `ring_count` 0 selects
  // one ring per hardware thread, up to 16; each ring has `queue_depth`
  // entries. Returns ErrorCode::kUnavailable when io_uring is not compiled in
  // or the kernel refuses ring setup, and kInvalidArgument for an empty root
  // path.
  static auto Create(
    std::string root_path,
    BlockGeometry geometry,
    std::size_t queue_depth,
    std::size_t max_open_files = 64,
    bool sync_on_write = false,
    std::size_t ring_count = 0
  ) -> Result<std::unique_ptr<IoUringDiskBackend>>;

  IoUringDiskBackend(const IoUringDiskBackend &) = delete;
  IoUringDiskBackend &operator=(const IoUringDiskBackend &) = delete;
  ~IoUringDiskBackend() override;

  auto Submit(std::span<const DiskRequest> requests, DiskSubmitMode mode) -> Result<std::size_t> override;
  auto Reap(std::span<DiskCompletion> out, bool wait) -> Result<std::size_t> override;
  void Shutdown() override;
  auto GetCapabilities() const -> DiskBackendCapabilities override;
  auto GetFileSize(FileId file_id) -> Result<std::uint64_t> override;
  auto SetFileSize(FileId file_id, std::uint64_t size) -> Status override;

 private:
  friend class IoUringDiskBackendTestPeer;

  struct RequestContext;
  struct Ring;

  IoUringDiskBackend(
    std::string root_path,
    BlockGeometry geometry,
    std::size_t queue_depth,
    std::size_t max_open_files,
    bool sync_on_write,
    std::size_t ring_count);

  auto Initialize() -> Status;
  auto CallerRing() -> std::size_t;
  auto PrepareLocked(Ring &ring, std::size_t ring_index, const DiskRequest &request) -> Result<RequestContext *>;
  auto ReapRing(Ring &ring, std::span<DiskCompletion> out, bool block_on_latch) -> std::size_t;
  void ContinueShortTransfer(RequestContext *context);
  auto FinishContext(RequestContext *context) -> DiskCompletion;
  void WaitForCompletionSignal();
  void SignalShutdown();
  void SetNextSubmitResultForTest(int result);
  void SetNextCompletionResultForTest(int result);
  auto InFlightRequestCountForTest() const -> std::size_t;

  std::string root_path_;
  BlockGeometry geometry_;
  std::size_t queue_depth_{0};
  bool sync_on_write_{true};
  std::size_t ring_count_{1};
  std::unique_ptr<FileDescriptorCache> files_;
  std::unique_ptr<Ring[]> rings_;
  int event_fd_{-1};
  std::atomic<std::size_t> in_flight_{0};
  std::atomic<bool> shutdown_{false};
  std::mutex shutdown_latch_;
  std::mutex test_latch_;
  std::optional<int> next_submit_result_for_test_;
  std::optional<int> next_completion_result_for_test_;
};

}  // namespace telepath

#endif  // TELEPATH_IO_IO_URING_DISK_BACKEND_H_
