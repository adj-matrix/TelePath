#ifndef TELEPATH_IO_IO_URING_DISK_BACKEND_H_
#define TELEPATH_IO_IO_URING_DISK_BACKEND_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "telepath/io/disk_backend.h"

namespace telepath {

class IoUringDiskBackendTestPeer;

class IoUringDiskBackend : public DiskBackend {
 public:
  IoUringDiskBackend(std::string root_path, std::size_t page_size,
                     std::size_t queue_depth);
  ~IoUringDiskBackend() override;

  Result<uint64_t> SubmitRead(const BufferTag &tag, std::byte *out,
                              std::size_t size) override;
  Result<uint64_t> SubmitWrite(const BufferTag &tag, const std::byte *data,
                               std::size_t size) override;
  Result<DiskCompletion> PollCompletion() override;
  void Shutdown() override;
  DiskBackendCapabilities GetCapabilities() const override;
  Status initialization_status() const { return init_status_; }

 private:
  friend class IoUringDiskBackendTestPeer;

  struct RequestContext;
  struct Impl;

  Result<uint64_t> SubmitRequest(const BufferTag &tag, DiskOperation operation,
                                 std::byte *mutable_buffer,
                                 const std::byte *const_buffer,
                                 std::size_t size);
  Status ValidateReadRequest(std::byte *buffer, std::size_t size) const;
  Status ValidateWriteRequest(const std::byte *buffer, std::size_t size) const;
  std::string BuildPath(FileId file_id) const;
  bool ShouldReturnShutdownUnavailable() const;
  void DrainInFlightRequestsForShutdown();
  void SetNextSubmitResultForTest(int result);
  void SetNextCompletionResultForTest(int result);
  std::size_t InFlightRequestCountForTest() const;

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
