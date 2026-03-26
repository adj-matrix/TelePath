#ifndef TELEPATH_IO_IO_URING_DISK_BACKEND_H_
#define TELEPATH_IO_IO_URING_DISK_BACKEND_H_

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

#include "telepath/io/disk_backend.h"

namespace telepath {

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

 private:
  std::string root_path_;
  std::size_t page_size_{0};
  std::size_t queue_depth_{0};
  mutable std::mutex latch_;
  std::condition_variable completion_cv_;
  bool shutdown_{false};
};

}  // namespace telepath

#endif  // TELEPATH_IO_IO_URING_DISK_BACKEND_H_
