#include "telepath/io/posix_disk_backend.h"

#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>

#include "disk_request_util.h"
#include "file_descriptor_cache.h"

namespace telepath {

PosixDiskBackend::PosixDiskBackend(
  std::string root_path,
  BlockGeometry geometry,
  bool is_fallback_backend,
  std::size_t max_open_files,
  bool sync_on_write,
  std::size_t io_worker_count)
  : root_path_(std::move(root_path)),
    geometry_(geometry),
    is_fallback_backend_(is_fallback_backend),
    sync_on_write_(sync_on_write),
    files_(std::make_unique<FileDescriptorCache>(root_path_, max_open_files)) {
  const std::size_t worker_count = std::max<std::size_t>(1, io_worker_count);
  workers_.reserve(worker_count);
  for (std::size_t i = 0; i < worker_count; ++i) workers_.emplace_back(&PosixDiskBackend::WorkerLoop, this);
}

PosixDiskBackend::~PosixDiskBackend() {
  PosixDiskBackend::Shutdown();
  for (auto &worker : workers_) {
    if (worker.joinable()) worker.join();
  }
}

auto PosixDiskBackend::Submit(std::span<const DiskRequest> requests, DiskSubmitMode mode) -> Result<std::size_t> {
  std::size_t accepted = 0;
  for (const DiskRequest &request : requests) {
    Status valid = root_path_.empty() ? InvalidArgument("disk backend root path must not be empty") : ValidateDiskRequest(request, kMaxDiskRequestBlocks);
    if (!valid.has_value()) {
      if (accepted == 0) return PropagateError(valid);
      return accepted;
    }

    {
      std::lock_guard<std::mutex> guard(latch_);
      if (shutdown_) {
        if (accepted == 0) return Unavailable("disk backend is shutting down");
        return accepted;
      }
      if (mode == DiskSubmitMode::kAsync) {
        pending_.push_back(request);
        request_cv_.notify_one();
        ++accepted;
        continue;
      }
      ++executing_;
    }

    Status status = Execute(request);
    PushCompletion(DiskCompletion{request.token, status}, false);
    ++accepted;
  }
  return accepted;
}

auto PosixDiskBackend::Reap(std::span<DiskCompletion> out, bool wait) -> Result<std::size_t> {
  std::unique_lock<std::mutex> lock(latch_);
  if (wait) completion_cv_.wait(lock, [this]() { return !completed_.empty() || IsDrainedLocked(); });
  if (completed_.empty()) {
    if (wait) return Unavailable("disk backend is shutting down");
    return std::size_t{0};
  }

  const std::size_t count = std::min(out.size(), completed_.size());
  for (std::size_t i = 0; i < count; ++i) {
    out[i] = std::move(completed_.front());
    completed_.pop_front();
  }
  return count;
}

void PosixDiskBackend::Shutdown() {
  {
    std::lock_guard<std::mutex> guard(latch_);
    shutdown_ = true;
  }
  request_cv_.notify_all();
  completion_cv_.notify_all();
}

auto PosixDiskBackend::GetCapabilities() const -> DiskBackendCapabilities {
  return DiskBackendCapabilities{DiskBackendKind::kPosix, is_fallback_backend_, workers_.size(), kMaxDiskRequestBlocks, geometry_.block_size()};
}

auto PosixDiskBackend::Execute(const DiskRequest &request) -> Status {
  Result<int> fd = files_->Acquire(request.tag.file_id);
  if (!fd.has_value()) return PropagateError(fd);
  Status status = request.operation == DiskOperation::kRead ? ExecuteRead(fd.value(), request) : ExecuteWrite(fd.value(), request);
  files_->Release(request.tag.file_id);
  return status;
}

auto PosixDiskBackend::ExecuteRead(int fd, const DiskRequest &request) -> Status {
  const std::size_t block_size = geometry_.block_size();
  const std::size_t total = request.block_count * block_size;
  const off_t base = static_cast<off_t>(geometry_.BlockOffset(request.tag.block_id));
  std::size_t done = 0;
  while (done < total) {
    std::array<iovec, kMaxDiskRequestBlocks> iov{};
    int iov_count = 0;
    for (std::uint32_t i = static_cast<std::uint32_t>(done / block_size); i < request.block_count; ++i) {
      const std::size_t skip = i == done / block_size ? done % block_size : 0;
      iov[iov_count++] = iovec{request.buffers[i] + skip, block_size - skip};
    }
    const ssize_t n = preadv(fd, iov.data(), iov_count, base + static_cast<off_t>(done));
    if (n < 0) {
      if (errno == EINTR) continue;
      return IoError("preadv failed", errno);
    }
    if (n == 0) break;
    done += static_cast<std::size_t>(n);
  }
  if (done < total) ZeroFillReadTail(request, block_size, done);
  return Ok();
}

auto PosixDiskBackend::ExecuteWrite(int fd, const DiskRequest &request) -> Status {
  const std::size_t block_size = geometry_.block_size();
  const std::size_t total = request.block_count * block_size;
  const off_t base = static_cast<off_t>(geometry_.BlockOffset(request.tag.block_id));
  std::size_t done = 0;
  while (done < total) {
    std::array<iovec, kMaxDiskRequestBlocks> iov{};
    int iov_count = 0;
    for (std::uint32_t i = static_cast<std::uint32_t>(done / block_size); i < request.block_count; ++i) {
      const std::size_t skip = i == done / block_size ? done % block_size : 0;
      iov[iov_count++] = iovec{request.buffers[i] + skip, block_size - skip};
    }
    const ssize_t n = pwritev(fd, iov.data(), iov_count, base + static_cast<off_t>(done));
    if (n < 0) {
      if (errno == EINTR) continue;
      return IoError("pwritev failed", errno);
    }
    if (n == 0) return IoError("partial page write");
    done += static_cast<std::size_t>(n);
  }
  if (sync_on_write_ && fdatasync(fd) != 0) return IoError("fdatasync failed", errno);
  return Ok();
}

void PosixDiskBackend::PushCompletion(DiskCompletion completion, bool wake_reapers) {
  bool drained = false;
  {
    std::lock_guard<std::mutex> guard(latch_);
    completed_.push_back(std::move(completion));
    --executing_;
    drained = shutdown_ && pending_.empty() && executing_ == 0;
  }
  if (wake_reapers || drained) completion_cv_.notify_all();
}

bool PosixDiskBackend::IsDrainedLocked() const {
  return shutdown_ && pending_.empty() && executing_ == 0;
}

void PosixDiskBackend::WorkerLoop() {
  while (true) {
    DiskRequest request;
    {
      std::unique_lock<std::mutex> lock(latch_);
      request_cv_.wait(lock, [this]() { return shutdown_ || !pending_.empty(); });
      if (pending_.empty()) {
        lock.unlock();
        files_->CloseIdle();
        return;
      }
      request = pending_.front();
      pending_.pop_front();
      ++executing_;
    }
    Status status = Execute(request);
    PushCompletion(DiskCompletion{request.token, status}, true);
  }
}

}  // namespace telepath
