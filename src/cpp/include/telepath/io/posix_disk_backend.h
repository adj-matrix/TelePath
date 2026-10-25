#ifndef TELEPATH_IO_POSIX_DISK_BACKEND_H_
#define TELEPATH_IO_POSIX_DISK_BACKEND_H_

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "telepath/common/block_geometry.h"
#include "telepath/io/disk_backend.h"

namespace telepath {

class FileDescriptorCache;

// Backend built on preadv/pwritev. Requests submitted with
// DiskSubmitMode::kWait run on the submitting thread; kAsync requests run on a
// pool of `io_worker_count` threads. Each block of a file lives at offset
// block_id * block_size of "<root_path>/file_<file_id>.tp".
class PosixDiskBackend : public DiskBackend {
 public:
  explicit PosixDiskBackend(
    std::string root_path,
    BlockGeometry geometry,
    bool is_fallback_backend = false,
    std::size_t max_open_files = 64,
    bool sync_on_write = true,
    std::size_t io_worker_count = 4
  );
  ~PosixDiskBackend() override;

  auto Submit(std::span<const DiskRequest> requests, DiskSubmitMode mode) -> Result<std::size_t> override;
  auto Reap(std::span<DiskCompletion> out, bool wait) -> Result<std::size_t> override;
  void Shutdown() override;
  auto GetCapabilities() const -> DiskBackendCapabilities override;

 private:
  auto Execute(const DiskRequest &request) -> Status;
  auto ExecuteRead(int fd, const DiskRequest &request) -> Status;
  auto ExecuteWrite(int fd, const DiskRequest &request) -> Status;
  void PushCompletion(DiskCompletion completion, bool wake_reapers);
  bool IsDrainedLocked() const;
  void WorkerLoop();

  std::string root_path_;
  BlockGeometry geometry_;
  bool is_fallback_backend_{false};
  bool sync_on_write_{true};
  std::unique_ptr<FileDescriptorCache> files_;
  std::mutex latch_;
  std::condition_variable request_cv_;
  std::condition_variable completion_cv_;
  std::deque<DiskRequest> pending_;
  std::deque<DiskCompletion> completed_;
  std::size_t executing_{0};
  bool shutdown_{false};
  std::vector<std::thread> workers_;
};

}  // namespace telepath

#endif  // TELEPATH_IO_POSIX_DISK_BACKEND_H_
