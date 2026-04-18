#include "telepath/io/io_uring_disk_backend.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>

#if TELEPATH_HAS_LIBURING
#include <liburing.h>
#endif

namespace telepath {

namespace {

std::string BuildErrnoMessage(const std::string &prefix, int error_code) {
  return prefix + ": " + std::strerror(error_code);
}

std::string BuildNegativeErrnoMessage(const std::string &prefix, int result_code) {
  return prefix + ": " + std::strerror(-result_code);
}

}  // namespace

struct IoUringDiskBackend::RequestContext {
  uint64_t request_id{0};
  DiskOperation operation{DiskOperation::kRead};
  BufferTag tag{};
  std::byte *mutable_buffer{nullptr};
  const std::byte *const_buffer{nullptr};
  std::size_t size{0};
  int fd{-1};
};

struct IoUringDiskBackend::Impl {
#if TELEPATH_HAS_LIBURING
  io_uring ring{};
#endif
  std::unordered_map<uint64_t, std::unique_ptr<RequestContext>> in_flight_requests;
};

IoUringDiskBackend::IoUringDiskBackend(
  std::string root_path,
  std::size_t page_size,
  std::size_t queue_depth)
  : root_path_(std::move(root_path)),
    page_size_(page_size),
    queue_depth_(queue_depth == 0 ? 64 : queue_depth),
    impl_(std::make_unique<Impl>()) {
  if (root_path_.empty()) {
    init_status_ = Status::InvalidArgument("io_uring backend root path must not be empty");
    return;
  }
  if (page_size_ == 0) {
    init_status_ = Status::InvalidArgument("io_uring backend page size must not be zero");
    return;
  }

  init_status_ = InitializeRootDirectory();
  if (!init_status_.ok()) return;
  init_status_ = InitializeRing();
}

IoUringDiskBackend::~IoUringDiskBackend() {
  Shutdown();
  DrainInFlightRequestsForShutdown();

  std::lock_guard<std::mutex> guard(latch_);
  if (impl_ == nullptr) return;
  for (auto &entry : impl_->in_flight_requests) {
    CloseRequestContext(entry.second.get());
  }
#if TELEPATH_HAS_LIBURING
  if (init_status_.ok()) io_uring_queue_exit(&impl_->ring);
#endif
}

auto IoUringDiskBackend::SubmitRead(const BufferTag &tag, std::byte *buffer, std::size_t size) -> Result<uint64_t> {
  Status status = ValidateReadRequest(buffer, size);
  if (!status.ok()) return status;
  return SubmitRequest(tag, DiskOperation::kRead, buffer, nullptr, size);
}

auto IoUringDiskBackend::SubmitWrite(const BufferTag &tag, const std::byte *buffer, std::size_t size) -> Result<uint64_t> {
  Status status = ValidateWriteRequest(buffer, size);
  if (!status.ok()) return status;
  return SubmitRequest(tag, DiskOperation::kWrite, nullptr, buffer, size);
}

auto IoUringDiskBackend::PollCompletion() -> Result<DiskCompletion> {
  if (!init_status_.ok()) return init_status_;

#if !TELEPATH_HAS_LIBURING
  return Status::Unavailable("io_uring backend is not enabled in this build");
#else
  while (true) {
    if (ShouldReturnShutdownUnavailable()) return Status::Unavailable("io_uring backend is shutting down");
    io_uring_cqe *cqe = nullptr;
    Result<bool> wait_result = WaitForCompletionEntry(&cqe);
    if (!wait_result.ok()) return wait_result.status();
    if (!wait_result.value()) continue;

    int completion_result = 0;
    Result<std::unique_ptr<RequestContext>> context_result = TakeCompletedContext(cqe, &completion_result);
    io_uring_cqe_seen(&impl_->ring, cqe);
    if (!context_result.ok()) return context_result.status();

    std::unique_ptr<RequestContext> owned_context = std::move(context_result.value());
    Status completion_status = FinalizeCompletion(owned_context.get(), completion_result);
    CloseRequestContext(owned_context.get());
    return DiskCompletion{owned_context->request_id, owned_context->operation, owned_context->tag, completion_status};
  }
#endif
}

void IoUringDiskBackend::Shutdown() {
  std::lock_guard<std::mutex> guard(latch_);
  shutdown_ = true;
}

auto IoUringDiskBackend::GetCapabilities() const -> DiskBackendCapabilities {
  return DiskBackendCapabilities{DiskBackendKind::kIoUring, true, true, queue_depth_, false, };
}

auto IoUringDiskBackend::InitializeRootDirectory() -> Status {
  std::error_code create_error;
  std::filesystem::create_directories(root_path_, create_error);
  if (create_error) return Status::IoError("failed to create io_uring root path: " + create_error.message());
  return Status::Ok();
}

auto IoUringDiskBackend::InitializeRing() -> Status {
#if !TELEPATH_HAS_LIBURING
  return Status::Unavailable("io_uring backend is not enabled in this build");
#else
  const int queue_init_result = io_uring_queue_init(static_cast<unsigned int>(queue_depth_), &impl_->ring, 0);
  if (queue_init_result < 0) return Status::Unavailable(BuildNegativeErrnoMessage("io_uring queue init failed", queue_init_result));
  return Status::Ok();
#endif
}

auto IoUringDiskBackend::SubmitRequest(const BufferTag &tag, DiskOperation operation, std::byte *mutable_buffer, const std::byte *const_buffer, std::size_t size) -> Result<uint64_t> {
  if (!init_status_.ok()) return init_status_;

#if !TELEPATH_HAS_LIBURING
  (void)tag;
  (void)operation;
  (void)mutable_buffer;
  (void)const_buffer;
  (void)size;
  return Status::Unavailable("io_uring backend is not enabled in this build");
#else
  std::lock_guard<std::mutex> guard(latch_);
  if (shutdown_) return Status::Unavailable("io_uring backend is shutting down");

  if (next_submit_result_for_test_.has_value()) {
    const int submit_result = *next_submit_result_for_test_;
    next_submit_result_for_test_.reset();
    if (submit_result == 1) return Status::Internal("test submit override must not report success");
    if (submit_result == 0) return Status::IoError("io_uring submit accepted no requests");
    return Status::IoError(BuildNegativeErrnoMessage("io_uring submit failed", submit_result));
  }

  io_uring_sqe *sqe = io_uring_get_sqe(&impl_->ring);
  if (sqe == nullptr) return Status::ResourceExhausted("io_uring submission queue is full");

  Result<int> fd_result = OpenFile(tag.file_id);
  if (!fd_result.ok()) return fd_result.status();

  auto context = BuildRequestContext(tag, operation, mutable_buffer, const_buffer, size, fd_result.value());

  const off_t offset = static_cast<off_t>(tag.block_id * page_size_);
  if (operation == DiskOperation::kRead) io_uring_prep_read(sqe, context->fd, mutable_buffer, size, offset);
  else io_uring_prep_write(sqe, context->fd, const_buffer, size, offset);
  io_uring_sqe_set_data(sqe, context.get());

  const uint64_t request_id = context->request_id;
  impl_->in_flight_requests.emplace(request_id, std::move(context));
  const int submit_result = io_uring_submit(&impl_->ring);
  if (submit_result != 1) return RollBackSubmittedRequest(request_id, submit_result);

  return request_id;
#endif
}

auto IoUringDiskBackend::ValidateReadRequest(std::byte *buffer, std::size_t size) const -> Status {
  return ValidateRequest(buffer, size, DiskOperation::kRead);
}

auto IoUringDiskBackend::ValidateWriteRequest(const std::byte *buffer, std::size_t size) const -> Status {
  return ValidateRequest(buffer, size, DiskOperation::kWrite);
}

auto IoUringDiskBackend::ValidateRequest(const void *buffer, std::size_t size, DiskOperation operation) const -> Status {
  if (buffer == nullptr) {
    if (operation == DiskOperation::kRead) return Status::InvalidArgument("io_uring read buffer must not be null");
    return Status::InvalidArgument("io_uring write buffer must not be null");
  }
  if (size != page_size_) {
    if (operation == DiskOperation::kRead) return Status::InvalidArgument("io_uring read size does not match page size");
    return Status::InvalidArgument("io_uring write size does not match page size");
  }
  if (root_path_.empty()) return Status::InvalidArgument("io_uring backend root path must not be empty");
  return Status::Ok();
}

auto IoUringDiskBackend::OpenFile(FileId file_id) const -> Result<int> {
  const int fd = open(BuildPath(file_id).c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) return Status::IoError(BuildErrnoMessage("open failed", errno));
  return fd;
}

auto IoUringDiskBackend::BuildRequestContext(const BufferTag &tag, DiskOperation operation, std::byte *mutable_buffer, const std::byte *const_buffer, std::size_t size, int fd) -> std::unique_ptr<RequestContext> {
  auto context = std::make_unique<RequestContext>();
  context->request_id = next_request_id_++;
  context->operation = operation;
  context->tag = tag;
  context->mutable_buffer = mutable_buffer;
  context->const_buffer = const_buffer;
  context->size = size;
  context->fd = fd;
  return context;
}

void IoUringDiskBackend::CloseRequestContext(RequestContext *context) const {
  if (context == nullptr || context->fd < 0) return;
  close(context->fd);
  context->fd = -1;
}

auto IoUringDiskBackend::RollBackSubmittedRequest(uint64_t request_id, int submit_result) -> Status {
  auto it = impl_->in_flight_requests.find(request_id);
  if (it != impl_->in_flight_requests.end()) {
    CloseRequestContext(it->second.get());
    impl_->in_flight_requests.erase(it);
  }
  if (submit_result == 0) return Status::IoError("io_uring submit accepted no requests");
  return Status::IoError(BuildNegativeErrnoMessage("io_uring submit failed", submit_result));
}

#if TELEPATH_HAS_LIBURING
auto IoUringDiskBackend::WaitForCompletionEntry(io_uring_cqe **cqe) -> Result<bool> {
  __kernel_timespec completion_wait_timeout{};
  completion_wait_timeout.tv_sec = 0;
  completion_wait_timeout.tv_nsec = 50 * 1000 * 1000;
  const int wait_result = io_uring_wait_cqe_timeout(&impl_->ring, cqe, &completion_wait_timeout);
  if (wait_result == -ETIME) {
    if (ShouldReturnShutdownUnavailable()) return Status::Unavailable("io_uring backend is shutting down");
    return false;
  }
  if (wait_result < 0) return Status::IoError(BuildNegativeErrnoMessage("io_uring wait for completion failed", wait_result));
  if (*cqe == nullptr) return Status::Internal("io_uring returned a null completion entry");
  return true;
}

auto IoUringDiskBackend::TakeCompletedContext(io_uring_cqe *cqe, int *completion_result) -> Result<std::unique_ptr<RequestContext>> {
  std::lock_guard<std::mutex> guard(latch_);
  *completion_result = cqe->res;
  if (next_completion_result_for_test_.has_value()) {
    *completion_result = *next_completion_result_for_test_;
    next_completion_result_for_test_.reset();
  }

  auto *context = reinterpret_cast<RequestContext *>(io_uring_cqe_get_data(cqe));
  if (context == nullptr) return Status::Internal("io_uring completion is missing request context");

  auto it = impl_->in_flight_requests.find(context->request_id);
  if (it == impl_->in_flight_requests.end()) return Status::Internal("io_uring completion refers to unknown request");

  std::unique_ptr<RequestContext> owned_context = std::move(it->second);
  impl_->in_flight_requests.erase(it);
  return owned_context;
}
#endif

auto IoUringDiskBackend::FinalizeCompletion(RequestContext *context, int completion_result) -> Status {
  if (completion_result < 0) return Status::IoError(BuildNegativeErrnoMessage("io_uring operation failed", completion_result));
  if (context->operation == DiskOperation::kWrite && completion_result != static_cast<int>(context->size)) return Status::IoError("partial page write");
  if (context->operation == DiskOperation::kRead && completion_result < static_cast<int>(context->size)) {
    std::memset(context->mutable_buffer + completion_result, 0, context->size - static_cast<std::size_t>(completion_result));
    return Status::Ok();
  }
  if (context->operation != DiskOperation::kWrite) return Status::Ok();
  if (fdatasync(context->fd) != 0) return Status::IoError(BuildErrnoMessage("fdatasync failed", errno));
  return Status::Ok();
}

auto IoUringDiskBackend::BuildPath(FileId file_id) const -> std::string {
  return root_path_ + "/file_" + std::to_string(file_id) + ".tp";
}

bool IoUringDiskBackend::ShouldReturnShutdownUnavailable() const {
  std::lock_guard<std::mutex> guard(latch_);
  return shutdown_ && (impl_ == nullptr || impl_->in_flight_requests.empty());
}

void IoUringDiskBackend::DrainInFlightRequestsForShutdown() {
#if !TELEPATH_HAS_LIBURING
  return;
#else
  if (!init_status_.ok()) return;

  while (true) {
    {
      std::lock_guard<std::mutex> guard(latch_);
      if (impl_ == nullptr || impl_->in_flight_requests.empty()) return;
    }

    auto completion = PollCompletion();
    if (completion.ok()) continue;
    if (completion.status().code() == StatusCode::kUnavailable) return;
    return;
  }
#endif
}

void IoUringDiskBackend::SetNextSubmitResultForTest(int result) {
  std::lock_guard<std::mutex> guard(latch_);
  next_submit_result_for_test_ = result;
}

void IoUringDiskBackend::SetNextCompletionResultForTest(int result) {
  std::lock_guard<std::mutex> guard(latch_);
  next_completion_result_for_test_ = result;
}

auto IoUringDiskBackend::InFlightRequestCountForTest() const -> std::size_t {
  std::lock_guard<std::mutex> guard(latch_);
  if (impl_ == nullptr) return 0;
  return impl_->in_flight_requests.size();
}

}  // namespace telepath
