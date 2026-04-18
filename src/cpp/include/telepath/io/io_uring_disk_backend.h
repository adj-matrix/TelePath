#ifndef TELEPATH_IO_IO_URING_DISK_BACKEND_H_
#define TELEPATH_IO_IO_URING_DISK_BACKEND_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "telepath/io/disk_backend.h"

#if TELEPATH_HAS_LIBURING
struct io_uring_cqe;
#endif

namespace telepath {

class IoUringDiskBackendTestPeer;

class IoUringDiskBackend : public DiskBackend {
 public:
  IoUringDiskBackend(
    std::string root_path,
    std::size_t page_size,
    std::size_t queue_depth
  );
  ~IoUringDiskBackend() override;

  auto SubmitRead(
    const BufferTag &tag,
    std::byte *out,
    std::size_t size
  ) -> Result<uint64_t> override;
  auto SubmitWrite(
    const BufferTag &tag,
    const std::byte *data,
    std::size_t size
  ) -> Result<uint64_t> override;
  auto PollCompletion() -> Result<DiskCompletion> override;
  void Shutdown() override;
  auto GetCapabilities() const -> DiskBackendCapabilities override;
  auto initialization_status() const -> Status { return init_status_; }

 private:
  friend class IoUringDiskBackendTestPeer;

  struct RequestContext;
  struct Impl;

  auto InitializeRootDirectory() -> Status;
  auto InitializeRing() -> Status;
  auto SubmitRequest(
    const BufferTag &tag,
    DiskOperation operation,
    std::byte *mutable_buffer,
    const std::byte *const_buffer,
    std::size_t size
  ) -> Result<uint64_t>;
  auto ValidateReadRequest(
    std::byte *buffer,
    std::size_t size
  ) const -> Status;
  auto ValidateWriteRequest(
    const std::byte *buffer,
    std::size_t size
  ) const -> Status;
  auto ValidateRequest(
    const void *buffer,
    std::size_t size,
    DiskOperation operation
  ) const -> Status;
  auto OpenFile(FileId file_id) const -> Result<int>;
  auto BuildRequestContext(
    const BufferTag &tag,
    DiskOperation operation,
    std::byte *mutable_buffer,
    const std::byte *const_buffer,
    std::size_t size,
    int fd
  ) -> std::unique_ptr<RequestContext>;
  void CloseRequestContext(RequestContext *context) const;
  auto RollBackSubmittedRequest(uint64_t request_id, int submit_result) -> Status;
#if TELEPATH_HAS_LIBURING
  auto WaitForCompletionEntry(io_uring_cqe **cqe) -> Result<bool>;
  auto TakeCompletedContext(
    io_uring_cqe *cqe,
    int *completion_result
  ) -> Result<std::unique_ptr<RequestContext>>;
#endif
  auto FinalizeCompletion(RequestContext *context, int completion_result)
    -> Status;
  auto BuildPath(FileId file_id) const -> std::string;
  bool ShouldReturnShutdownUnavailable() const;
  void DrainInFlightRequestsForShutdown();
  void SetNextSubmitResultForTest(int result);
  void SetNextCompletionResultForTest(int result);
  auto InFlightRequestCountForTest() const -> std::size_t;

  std::string root_path_;
  std::size_t page_size_{0};
  std::size_t queue_depth_{0};
  uint64_t next_request_id_{1};
  Status init_status_{Status::Ok()};
  mutable std::mutex latch_;
  bool shutdown_{false};
  std::optional<int> next_submit_result_for_test_;
  std::optional<int> next_completion_result_for_test_;
  std::unique_ptr<Impl> impl_;
};

}  // namespace telepath

#endif  // TELEPATH_IO_IO_URING_DISK_BACKEND_H_
