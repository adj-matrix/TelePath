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
  explicit PosixDiskBackend(std::string root_path, std::size_t page_size);
  ~PosixDiskBackend() override;

  Result<uint64_t> SubmitRead(const BufferTag &tag, std::byte *out,
                              std::size_t size) override;
  Result<uint64_t> SubmitWrite(const BufferTag &tag, const std::byte *data,
                               std::size_t size) override;
  Result<DiskCompletion> PollCompletion() override;

 private:
  Status ExecuteRead(const BufferTag &tag, std::byte *out, std::size_t size);
  Status ExecuteWrite(const BufferTag &tag, const std::byte *data,
                      std::size_t size);
  Result<int> OpenFile(FileId file_id, int flags) const;
  std::string BuildPath(FileId file_id) const;
  void WorkerLoop();

  std::mutex queue_latch_;
  std::condition_variable request_cv_;
  std::condition_variable completion_cv_;
  std::deque<DiskRequest> pending_requests_;
  std::deque<DiskCompletion> completed_requests_;
  uint64_t next_request_id_{1};
  bool shutdown_{false};
  std::string root_path_;
  std::size_t page_size_{0};
  std::thread worker_;
};

}  // namespace telepath

#endif  // TELEPATH_IO_POSIX_DISK_BACKEND_H_
