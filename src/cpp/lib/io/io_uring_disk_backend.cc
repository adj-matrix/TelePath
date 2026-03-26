#include "telepath/io/io_uring_disk_backend.h"

#include <utility>

namespace telepath {

IoUringDiskBackend::IoUringDiskBackend(std::string root_path,
                                       std::size_t page_size,
                                       std::size_t queue_depth)
    : root_path_(std::move(root_path)),
      page_size_(page_size),
      queue_depth_(queue_depth) {}

IoUringDiskBackend::~IoUringDiskBackend() { Shutdown(); }

Result<uint64_t> IoUringDiskBackend::SubmitRead(const BufferTag &,
                                                std::byte *,
                                                std::size_t) {
  return Status::Unavailable("io_uring backend is not enabled in this build");
}

Result<uint64_t> IoUringDiskBackend::SubmitWrite(const BufferTag &,
                                                 const std::byte *,
                                                 std::size_t) {
  return Status::Unavailable("io_uring backend is not enabled in this build");
}

Result<DiskCompletion> IoUringDiskBackend::PollCompletion() {
  std::unique_lock<std::mutex> lock(latch_);
  completion_cv_.wait(lock, [this]() { return shutdown_; });
  return Status::Unavailable("io_uring backend is shutting down");
}

void IoUringDiskBackend::Shutdown() {
  {
    std::lock_guard<std::mutex> guard(latch_);
    shutdown_ = true;
  }
  completion_cv_.notify_all();
}

DiskBackendCapabilities IoUringDiskBackend::GetCapabilities() const {
  return DiskBackendCapabilities{
      DiskBackendKind::kIoUring,
      true,
      true,
      queue_depth_ == 0 ? 64 : queue_depth_,
      false,
  };
}

}  // namespace telepath
