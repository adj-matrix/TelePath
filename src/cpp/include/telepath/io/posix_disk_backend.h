#ifndef TELEPATH_IO_POSIX_DISK_BACKEND_H_
#define TELEPATH_IO_POSIX_DISK_BACKEND_H_

#include <cstdint>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "telepath/io/disk_backend.h"

namespace telepath {

class PosixDiskBackend : public DiskBackend {
 public:
  explicit PosixDiskBackend(
    std::string root_path,
    std::size_t page_size,
    bool is_fallback_backend = false
  );
  ~PosixDiskBackend() override;

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

 private:
  auto SubmitRequest(
    DiskOperation operation,
    const BufferTag &tag,
    std::byte *mutable_buffer,
    const std::byte *const_buffer,
    std::size_t size
  ) -> Result<uint64_t>;
  auto ValidateReadRequest(
    std::byte *out,
    std::size_t size
  ) const -> Status;
  auto ValidateWriteRequest(
    const std::byte *data,
    std::size_t size
  ) const -> Status;
  auto ValidateRequest(
    const void *buffer,
    std::size_t size,
    DiskOperation operation
  ) const -> Status;
  bool CanReturnUnavailableAfterShutdown() const;
  auto ExecuteRequest(const DiskRequest &request) -> Status;
  void CompleteRequest(const DiskRequest &request, const Status &status);
  auto ExecuteRead(
    const BufferTag &tag,
    std::byte *out,
    std::size_t size
  ) -> Status;
  auto ExecuteWrite(
    const BufferTag &tag,
    const std::byte *data,
    std::size_t size
  ) -> Status;
  auto OpenFile(FileId file_id, int flags) const -> Result<int>;
  auto BuildPath(FileId file_id) const -> std::string;
  void WorkerLoop();

  std::mutex queue_latch_;
  std::condition_variable request_cv_;
  std::condition_variable completion_cv_;
  std::deque<DiskRequest> pending_requests_;
  std::deque<DiskCompletion> completed_requests_;
  uint64_t next_request_id_{1};
  bool shutdown_{false};
  bool worker_active_{false};
  bool is_fallback_backend_{false};
  std::string root_path_;
  std::size_t page_size_{0};
  std::thread worker_;
};

}  // namespace telepath

#endif  // TELEPATH_IO_POSIX_DISK_BACKEND_H_
