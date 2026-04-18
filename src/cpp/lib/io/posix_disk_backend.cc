#include "telepath/io/posix_disk_backend.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace telepath {

namespace {

std::string BuildErrnoMessage(const std::string &prefix) {
  return prefix + ": " + std::strerror(errno);
}

}  // namespace

PosixDiskBackend::PosixDiskBackend(std::string root_path, std::size_t page_size, bool is_fallback_backend)
  : is_fallback_backend_(is_fallback_backend),
    root_path_(std::move(root_path)),
    page_size_(page_size),
    worker_(&PosixDiskBackend::WorkerLoop, this) {}

PosixDiskBackend::~PosixDiskBackend() {
  Shutdown();
  if (worker_.joinable()) worker_.join();
}

auto PosixDiskBackend::SubmitRead(const BufferTag &tag, std::byte *out, std::size_t size) -> Result<uint64_t> {
  Status validate_status = ValidateReadRequest(out, size);
  if (!validate_status.ok()) return validate_status;
  return SubmitRequest(DiskOperation::kRead, tag, out, nullptr, size);
}

auto PosixDiskBackend::SubmitWrite(const BufferTag &tag, const std::byte *data, std::size_t size) -> Result<uint64_t> {
  Status validate_status = ValidateWriteRequest(data, size);
  if (!validate_status.ok()) return validate_status;
  return SubmitRequest(DiskOperation::kWrite, tag, nullptr, data, size);
}

auto PosixDiskBackend::PollCompletion() -> Result<DiskCompletion> {
  std::unique_lock<std::mutex> lock(queue_latch_);
  completion_cv_.wait(lock, [this]() {
    return !completed_requests_.empty() || CanReturnUnavailableAfterShutdown();
  });
  if (completed_requests_.empty()) return Status::Unavailable("disk backend is shutting down");

  DiskCompletion completion = completed_requests_.front();
  completed_requests_.pop_front();
  return completion;
}

void PosixDiskBackend::Shutdown() {
  {
    std::lock_guard<std::mutex> guard(queue_latch_);
    shutdown_ = true;
  }
  request_cv_.notify_all();
  completion_cv_.notify_all();
}

auto PosixDiskBackend::GetCapabilities() const -> DiskBackendCapabilities {
  return DiskBackendCapabilities{
    DiskBackendKind::kPosix,
    false,
    false,
    1,
    is_fallback_backend_,
  };
}

auto PosixDiskBackend::SubmitRequest(DiskOperation operation, const BufferTag &tag, std::byte *mutable_buffer, const std::byte *const_buffer, std::size_t size) -> Result<uint64_t> {
  std::lock_guard<std::mutex> guard(queue_latch_);
  if (shutdown_) return Status::Unavailable("disk backend is shutting down");

  const uint64_t request_id = next_request_id_++;
  pending_requests_.push_back(DiskRequest{request_id, operation, tag, mutable_buffer, const_buffer, size});
  request_cv_.notify_one();
  return request_id;
}

auto PosixDiskBackend::ValidateReadRequest(std::byte *out, std::size_t size) const -> Status {
  return ValidateRequest(out, size, DiskOperation::kRead);
}

auto PosixDiskBackend::ValidateWriteRequest(const std::byte *data, std::size_t size) const -> Status {
  return ValidateRequest(data, size, DiskOperation::kWrite);
}

auto PosixDiskBackend::ValidateRequest(const void *buffer, std::size_t size, DiskOperation operation) const -> Status {
  if (buffer == nullptr) {
    if (operation == DiskOperation::kRead) return Status::InvalidArgument("read buffer must not be null");
    return Status::InvalidArgument("write buffer must not be null");
  }
  if (page_size_ == 0) return Status::InvalidArgument("disk backend page size must not be zero");
  if (size != page_size_) {
    if (operation == DiskOperation::kRead) return Status::InvalidArgument("read size does not match page size");
    return Status::InvalidArgument("write size does not match page size");
  }
  if (root_path_.empty()) return Status::InvalidArgument("disk backend root path must not be empty");
  return Status::Ok();
}

bool PosixDiskBackend::CanReturnUnavailableAfterShutdown() const {
  return shutdown_ && pending_requests_.empty() && !worker_active_;
}

auto PosixDiskBackend::ExecuteRequest(const DiskRequest &request) -> Status {
  if (request.operation == DiskOperation::kRead) return ExecuteRead(request.tag, request.mutable_buffer, request.size);
  return ExecuteWrite(request.tag, request.const_buffer, request.size);
}

void PosixDiskBackend::CompleteRequest(const DiskRequest &request, const Status &status) {
  {
    std::lock_guard<std::mutex> guard(queue_latch_);
    worker_active_ = false;
    completed_requests_.push_back(DiskCompletion{request.request_id, request.operation, request.tag, status});
  }
  completion_cv_.notify_one();
}

auto PosixDiskBackend::ExecuteRead(const BufferTag &tag, std::byte *out, std::size_t size) -> Status {
  if (size != page_size_) return Status::InvalidArgument("read size does not match page size");

  Result<int> fd_result = OpenFile(tag.file_id, O_RDWR | O_CREAT);
  if (!fd_result.ok()) return fd_result.status();
  const int fd = fd_result.value();

  const off_t offset = static_cast<off_t>(tag.block_id * page_size_);
  const ssize_t bytes_read = pread(fd, out, size, offset);
  if (bytes_read < 0) {
    close(fd);
    return Status::IoError(BuildErrnoMessage("pread failed"));
  }
  if (bytes_read < static_cast<ssize_t>(size)) std::memset(out + bytes_read, 0, size - bytes_read);

  close(fd);
  return Status::Ok();
}

auto PosixDiskBackend::ExecuteWrite(const BufferTag &tag, const std::byte *data, std::size_t size) -> Status {
  if (size != page_size_) return Status::InvalidArgument("write size does not match page size");

  Result<int> fd_result = OpenFile(tag.file_id, O_RDWR | O_CREAT);
  if (!fd_result.ok()) return fd_result.status();
  const int fd = fd_result.value();

  const off_t offset = static_cast<off_t>(tag.block_id * page_size_);
  const ssize_t bytes_written = pwrite(fd, data, size, offset);
  if (bytes_written != static_cast<ssize_t>(size)) {
    close(fd);
    if (bytes_written < 0) return Status::IoError(BuildErrnoMessage("pwrite failed"));
    return Status::IoError("partial page write");
  }

  if (fsync(fd) != 0) {
    close(fd);
    return Status::IoError(BuildErrnoMessage("fsync failed"));
  }

  close(fd);
  return Status::Ok();
}

auto PosixDiskBackend::OpenFile(FileId file_id, int flags) const -> Result<int> {
  const std::string path = BuildPath(file_id);
  const int fd = open(path.c_str(), flags, 0644);
  if (fd < 0) return Status::IoError(BuildErrnoMessage("open failed"));
  return fd;
}

auto PosixDiskBackend::BuildPath(FileId file_id) const -> std::string {
  return root_path_ + "/file_" + std::to_string(file_id) + ".tp";
}

void PosixDiskBackend::WorkerLoop() {
  while (true) {
    DiskRequest request;
    {
      std::unique_lock<std::mutex> lock(queue_latch_);
      request_cv_.wait(lock, [this]() {
        return shutdown_ || !pending_requests_.empty();
      });
      if (shutdown_ && pending_requests_.empty()) return;
      request = pending_requests_.front();
      pending_requests_.pop_front();
      worker_active_ = true;
    }

    Status status = ExecuteRequest(request);
    CompleteRequest(request, status);
  }
}

}  // namespace telepath
