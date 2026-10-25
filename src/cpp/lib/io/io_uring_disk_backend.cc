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

struct IoUringDiskBackend::RequestContext {
  uint64_t request_id{0};
  DiskOperation operation{DiskOperation::kRead};
  BufferTag tag{};
  std::byte *mutable_buffer{nullptr};
  const std::byte *const_buffer{nullptr};
  std::size_t size{0};
  FileId file_id{0};
  int fd{-1};
};

struct IoUringDiskBackend::Impl {
#if TELEPATH_HAS_LIBURING
  io_uring ring{};
#endif
  std::unordered_map<uint64_t, std::unique_ptr<RequestContext>> in_flight_requests;
};

auto IoUringDiskBackend::Create(
  std::string root_path,
  BlockGeometry geometry,
  std::size_t queue_depth,
  std::size_t max_open_files,
  bool sync_on_write) -> Result<std::unique_ptr<IoUringDiskBackend>> {
  std::unique_ptr<IoUringDiskBackend> backend(new IoUringDiskBackend(std::move(root_path), geometry, queue_depth, max_open_files, sync_on_write));
  Status init_status = backend->Initialize();
  if (!init_status.has_value()) return PropagateError(init_status);
  return backend;
}

IoUringDiskBackend::IoUringDiskBackend(
  std::string root_path,
  BlockGeometry geometry,
  std::size_t queue_depth,
  std::size_t max_open_files,
  bool sync_on_write)
  : root_path_(std::move(root_path)),
    geometry_(geometry),
    queue_depth_(queue_depth == 0 ? 64 : queue_depth),
    max_open_files_(max_open_files == 0 ? 64 : max_open_files),
    sync_on_write_(sync_on_write),
    impl_(std::make_unique<Impl>()) {}

auto IoUringDiskBackend::Initialize() -> Status {
  if (root_path_.empty()) return InvalidArgument("io_uring backend root path must not be empty");
  if (Status root_status = InitializeRootDirectory(); !root_status.has_value()) return root_status;
  if (Status ring_status = InitializeRing(); !ring_status.has_value()) return ring_status;
  ring_initialized_ = true;
  return Ok();
}

IoUringDiskBackend::~IoUringDiskBackend() {
  IoUringDiskBackend::Shutdown();
  DrainInFlightRequestsForShutdown();

  std::lock_guard<std::mutex> guard(latch_);
  if (impl_ == nullptr) return;
  CloseCachedFilesLocked();
#if TELEPATH_HAS_LIBURING
  if (ring_initialized_) io_uring_queue_exit(&impl_->ring);
#endif
}

auto IoUringDiskBackend::SubmitRead(const BufferTag &tag, std::byte *buffer, std::size_t size) -> Result<uint64_t> {
  Status status = ValidateReadRequest(buffer, size);
  if (!status.has_value()) return PropagateError(status);
  return SubmitRequest(tag, DiskOperation::kRead, buffer, nullptr, size);
}

auto IoUringDiskBackend::SubmitWrite(const BufferTag &tag, const std::byte *buffer, std::size_t size) -> Result<uint64_t> {
  Status status = ValidateWriteRequest(buffer, size);
  if (!status.has_value()) return PropagateError(status);
  return SubmitRequest(tag, DiskOperation::kWrite, nullptr, buffer, size);
}

auto IoUringDiskBackend::PollCompletion() -> Result<DiskCompletion> {

#if !TELEPATH_HAS_LIBURING
  return Unavailable("io_uring backend is not enabled in this build");
#else
  while (true) {
    if (ShouldReturnShutdownUnavailable()) return Unavailable("io_uring backend is shutting down");
    io_uring_cqe *cqe = nullptr;
    Result<bool> wait_result = WaitForCompletionEntry(&cqe);
    if (!wait_result.has_value()) return PropagateError(wait_result);
    if (!wait_result.value()) continue;

    int completion_result = 0;
    Result<std::unique_ptr<RequestContext>> context_result = TakeCompletedContext(cqe, &completion_result);
    io_uring_cqe_seen(&impl_->ring, cqe);
    if (!context_result.has_value()) return PropagateError(context_result);

    std::unique_ptr<RequestContext> owned_context = std::move(context_result.value());
    Status completion_status = FinalizeCompletion(owned_context.get(), completion_result);
    ReleaseRequestFile(*owned_context);
    return DiskCompletion{owned_context->request_id, owned_context->operation, owned_context->tag, completion_status};
  }
#endif
}

void IoUringDiskBackend::Shutdown() {
  std::lock_guard<std::mutex> guard(latch_);
  shutdown_ = true;
  if (impl_ == nullptr || impl_->in_flight_requests.empty()) CloseCachedFilesLocked();
}

auto IoUringDiskBackend::GetCapabilities() const -> DiskBackendCapabilities {
  return DiskBackendCapabilities{DiskBackendKind::kIoUring, true, true, queue_depth_, false, geometry_.block_size()};
}

auto IoUringDiskBackend::InitializeRootDirectory() -> Status {
  std::error_code create_error;
  std::filesystem::create_directories(root_path_, create_error);
  if (create_error) return IoError("failed to create io_uring root path", create_error.value());
  return Ok();
}

auto IoUringDiskBackend::InitializeRing() -> Status {
#if !TELEPATH_HAS_LIBURING
  return Unavailable("io_uring backend is not enabled in this build");
#else
  const int queue_init_result = io_uring_queue_init(static_cast<unsigned int>(queue_depth_), &impl_->ring, 0);
  if (queue_init_result < 0) return Unavailable("io_uring queue init failed", -queue_init_result);
  return Ok();
#endif
}

auto IoUringDiskBackend::SubmitRequest(const BufferTag &tag, DiskOperation operation, std::byte *mutable_buffer, const std::byte *const_buffer, std::size_t size) -> Result<uint64_t> {

#if !TELEPATH_HAS_LIBURING
  (void)tag;
  (void)operation;
  (void)mutable_buffer;
  (void)const_buffer;
  (void)size;
  return Unavailable("io_uring backend is not enabled in this build");
#else
  std::lock_guard<std::mutex> guard(latch_);
  if (shutdown_) return Unavailable("io_uring backend is shutting down");

  std::optional<int> forced_submit_result;
  if (next_submit_result_for_test_.has_value()) {
    forced_submit_result = *next_submit_result_for_test_;
    next_submit_result_for_test_.reset();
    if (*forced_submit_result == 1) return Internal("test submit override must not report success");
  }

  Result<int> fd_result = OpenFileLocked(tag.file_id);
  if (!fd_result.has_value()) return PropagateError(fd_result);

  auto context = BuildRequestContext(tag, operation, mutable_buffer, const_buffer, size, fd_result.value());
  const uint64_t request_id = context->request_id;
  impl_->in_flight_requests.emplace(request_id, std::move(context));

  if (forced_submit_result.has_value()) return PropagateError(RollBackSubmittedRequestLocked(request_id, *forced_submit_result));

  io_uring_sqe *sqe = io_uring_get_sqe(&impl_->ring);
  if (sqe == nullptr) return PropagateError(RollBackSubmittedRequestLocked(request_id, 0));

  const off_t offset = static_cast<off_t>(geometry_.BlockOffset(tag.block_id));
  auto *request_context = impl_->in_flight_requests.find(request_id)->second.get();
  if (operation == DiskOperation::kRead) io_uring_prep_read(sqe, request_context->fd, mutable_buffer, size, offset);
  else io_uring_prep_write(sqe, request_context->fd, const_buffer, size, offset);
  io_uring_sqe_set_data(sqe, request_context);
  const int submit_result = io_uring_submit(&impl_->ring);
  if (submit_result != 1) return PropagateError(RollBackSubmittedRequestLocked(request_id, submit_result));

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
    if (operation == DiskOperation::kRead) return InvalidArgument("io_uring read buffer must not be null");
    return InvalidArgument("io_uring write buffer must not be null");
  }
  if (size != geometry_.block_size()) {
    if (operation == DiskOperation::kRead) return InvalidArgument("io_uring read size does not match page size");
    return InvalidArgument("io_uring write size does not match page size");
  }
  if (root_path_.empty()) return InvalidArgument("io_uring backend root path must not be empty");
  return Ok();
}

auto IoUringDiskBackend::OpenFileLocked(FileId file_id) -> Result<int> {
  auto cached = open_files_.find(file_id);
  if (cached != open_files_.end()) {
    ++cached->second.in_flight;
    cached->second.last_used = ++open_file_clock_;
    return cached->second.fd;
  }

  EvictLeastRecentlyUsedIdleFilesLocked(max_open_files_ - 1);
  const int fd = open(BuildPath(file_id).c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) return IoError("open failed", errno);
  open_files_.emplace(file_id, CachedFile{fd, ++open_file_clock_, 1});
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
  context->file_id = tag.file_id;
  context->fd = fd;
  return context;
}

void IoUringDiskBackend::ReleaseRequestFile(const RequestContext &context) {
  std::lock_guard<std::mutex> guard(latch_);
  ReleaseRequestFileLocked(context);
}

void IoUringDiskBackend::ReleaseRequestFileLocked(const RequestContext &context) {
  auto cached = open_files_.find(context.file_id);
  if (cached == open_files_.end()) return;
  if (cached->second.in_flight > 0) --cached->second.in_flight;
  cached->second.last_used = ++open_file_clock_;
  EvictLeastRecentlyUsedIdleFilesLocked(max_open_files_);
  if (shutdown_ && (impl_ == nullptr || impl_->in_flight_requests.empty())) CloseCachedFilesLocked();
}

void IoUringDiskBackend::EvictLeastRecentlyUsedIdleFilesLocked(std::size_t target_size) {
  while (open_files_.size() > target_size) {
    auto victim = open_files_.end();
    for (auto it = open_files_.begin(); it != open_files_.end(); ++it) {
      if (it->second.in_flight != 0) continue;
      if (victim == open_files_.end() || it->second.last_used < victim->second.last_used) victim = it;
    }
    if (victim == open_files_.end()) return;
    if (victim->second.fd >= 0) close(victim->second.fd);
    open_files_.erase(victim);
  }
}

void IoUringDiskBackend::CloseCachedFilesLocked() {
  for (const auto &[file_id, cached_file] : open_files_) {
    (void)file_id;
    if (cached_file.fd >= 0) close(cached_file.fd);
  }
  open_files_.clear();
}

auto IoUringDiskBackend::RollBackSubmittedRequestLocked(uint64_t request_id, int submit_result) -> Status {
  auto it = impl_->in_flight_requests.find(request_id);
  if (it != impl_->in_flight_requests.end()) {
    if (it->second != nullptr) {
      RequestContext context = *it->second;
      impl_->in_flight_requests.erase(it);
      ReleaseRequestFileLocked(context);
    } else {
      impl_->in_flight_requests.erase(it);
    }
  }
  if (submit_result == 0) return IoError("io_uring submit accepted no requests");
  return IoError("io_uring submit failed", -submit_result);
}

#if TELEPATH_HAS_LIBURING
auto IoUringDiskBackend::WaitForCompletionEntry(io_uring_cqe **cqe) -> Result<bool> {
  __kernel_timespec completion_wait_timeout{};
  completion_wait_timeout.tv_sec = 0;
  completion_wait_timeout.tv_nsec = 50 * 1000 * 1000;
  const int wait_result = io_uring_wait_cqe_timeout(&impl_->ring, cqe, &completion_wait_timeout);
  if (wait_result == -ETIME) {
    if (ShouldReturnShutdownUnavailable()) return Unavailable("io_uring backend is shutting down");
    return false;
  }
  if (wait_result < 0) return IoError("io_uring wait for completion failed", -wait_result);
  if (*cqe == nullptr) return Internal("io_uring returned a null completion entry");
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
  if (context == nullptr) return Internal("io_uring completion is missing request context");

  auto it = impl_->in_flight_requests.find(context->request_id);
  if (it == impl_->in_flight_requests.end()) return Internal("io_uring completion refers to unknown request");

  std::unique_ptr<RequestContext> owned_context = std::move(it->second);
  impl_->in_flight_requests.erase(it);
  return owned_context;
}
#endif

auto IoUringDiskBackend::FinalizeCompletion(RequestContext *context, int completion_result) -> Status {
  if (completion_result < 0) return IoError("io_uring operation failed", -completion_result);
  if (context->operation == DiskOperation::kWrite && completion_result != static_cast<int>(context->size)) return IoError("partial page write");
  if (context->operation == DiskOperation::kRead && completion_result < static_cast<int>(context->size)) {
    std::memset(context->mutable_buffer + completion_result, 0, context->size - static_cast<std::size_t>(completion_result));
    return Ok();
  }
  if (context->operation != DiskOperation::kWrite) return Ok();
  if (!sync_on_write_) return Ok();
  if (fdatasync(context->fd) != 0) return IoError("fdatasync failed", errno);
  return Ok();
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
  if (!ring_initialized_) return;

  while (true) {
    {
      std::lock_guard<std::mutex> guard(latch_);
      if (impl_ == nullptr || impl_->in_flight_requests.empty()) return;
    }

    auto completion = PollCompletion();
    if (completion.has_value()) continue;
    if (completion.error().code() == ErrorCode::kUnavailable) return;
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
