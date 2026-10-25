#include "telepath/io/io_uring_disk_backend.h"

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <filesystem>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "disk_request_util.h"
#include "file_descriptor_cache.h"

#if TELEPATH_HAS_LIBURING
#include <liburing.h>
#endif

namespace telepath {

namespace {

#if TELEPATH_HAS_LIBURING
constexpr uint64_t kSyncTag = 1;
#endif
constexpr std::size_t kMaxRings = 16;
constexpr int kSignalPollTimeoutMs = 100;

}  // namespace

struct IoUringDiskBackend::Ring {
#if TELEPATH_HAS_LIBURING
  io_uring ring{};
#endif
  bool initialized{false};
  bool broken{false};
  std::mutex submit_latch;
  std::mutex reap_latch;
};

struct IoUringDiskBackend::RequestContext {
  DiskRequest request;
  std::size_t ring{0};
  int fd{-1};
  std::size_t total_bytes{0};
  std::size_t done_bytes{0};
  bool sync{false};
  int pending_cqes{0};
  int transfer_result{0};
  int sync_result{0};
  std::array<iovec, kMaxDiskRequestBlocks> iov{};
};

auto IoUringDiskBackend::Create(
  std::string root_path,
  BlockGeometry geometry,
  std::size_t queue_depth,
  std::size_t max_open_files,
  bool sync_on_write,
  std::size_t ring_count) -> Result<std::unique_ptr<IoUringDiskBackend>> {
  std::unique_ptr<IoUringDiskBackend> backend(new IoUringDiskBackend(std::move(root_path), geometry, queue_depth, max_open_files, sync_on_write, ring_count));
  Status init_status = backend->Initialize();
  if (!init_status.has_value()) return PropagateError(init_status);
  return backend;
}

IoUringDiskBackend::IoUringDiskBackend(
  std::string root_path,
  BlockGeometry geometry,
  std::size_t queue_depth,
  std::size_t max_open_files,
  bool sync_on_write,
  std::size_t ring_count)
  : root_path_(std::move(root_path)),
    geometry_(geometry),
    queue_depth_(queue_depth == 0 ? 64 : queue_depth),
    sync_on_write_(sync_on_write),
    ring_count_(ring_count != 0 ? std::min(ring_count, kMaxRings) : std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, kMaxRings)),
    files_(std::make_unique<FileDescriptorCache>(root_path_, max_open_files)),
    rings_(std::make_unique<Ring[]>(ring_count_)) {}

auto IoUringDiskBackend::Initialize() -> Status {
  if (root_path_.empty()) return InvalidArgument("io_uring backend root path must not be empty");
  std::error_code create_error;
  std::filesystem::create_directories(root_path_, create_error);
  if (create_error) return IoError("failed to create io_uring root path", create_error.value());
#if !TELEPATH_HAS_LIBURING
  return Unavailable("io_uring backend is not enabled in this build");
#else
  event_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (event_fd_ < 0) return Unavailable("io_uring completion eventfd creation failed", errno);
  // Two entries at least, so a write and its linked fdatasync always fit.
  const unsigned entries = static_cast<unsigned>(std::max<std::size_t>(2, queue_depth_));
  for (std::size_t i = 0; i < ring_count_; ++i) {
    const int init_result = io_uring_queue_init(entries, &rings_[i].ring, 0);
    if (init_result < 0) return Unavailable("io_uring queue init failed", -init_result);
    rings_[i].initialized = true;
    const int register_result = io_uring_register_eventfd(&rings_[i].ring, event_fd_);
    if (register_result < 0) return Unavailable("io_uring eventfd registration failed", -register_result);
  }
  return Ok();
#endif
}

IoUringDiskBackend::~IoUringDiskBackend() {
  IoUringDiskBackend::Shutdown();
#if TELEPATH_HAS_LIBURING
  // A retired ring may hold requests that never complete; draining stops there.
  bool any_broken = false;
  for (std::size_t i = 0; i < ring_count_; ++i) any_broken = any_broken || rings_[i].broken;
  std::array<DiskCompletion, 16> drained;
  while (!any_broken && in_flight_.load() != 0) {
    if (!Reap(drained, true).has_value()) break;
  }
  for (std::size_t i = 0; i < ring_count_; ++i) {
    if (rings_[i].initialized) io_uring_queue_exit(&rings_[i].ring);
  }
#endif
  if (event_fd_ >= 0) close(event_fd_);
}

auto IoUringDiskBackend::CallerRing() -> std::size_t {
  static std::atomic<std::size_t> next_thread{0};
  thread_local const std::size_t thread_index = next_thread.fetch_add(1, std::memory_order_relaxed);
  return thread_index % ring_count_;
}

auto IoUringDiskBackend::Submit(std::span<const DiskRequest> requests, DiskSubmitMode) -> Result<std::size_t> {
#if !TELEPATH_HAS_LIBURING
  (void)requests;
  return Unavailable("io_uring backend is not enabled in this build");
#else
  // Counted as in flight while submitting, so Shutdown() cannot report the
  // backend drained between the shutdown check and the submission.
  in_flight_.fetch_add(1);
  auto release_guard = [this]() {
    if (in_flight_.fetch_sub(1) == 1 && shutdown_.load()) SignalShutdown();
  };
  if (shutdown_.load()) {
    release_guard();
    return Unavailable("io_uring backend is shutting down");
  }

  const std::size_t ring_index = CallerRing();
  Ring &ring = rings_[ring_index];
  std::vector<RequestContext *> prepared;
  std::vector<std::size_t> sqe_counts;
  Status first_error = Ok();
  {
    std::lock_guard<std::mutex> guard(ring.submit_latch);
    if (ring.broken) first_error = IoError("io_uring ring is unusable after a failed submit");
    for (std::size_t i = 0; first_error.has_value() && i < requests.size(); ++i) {
      Result<RequestContext *> context = PrepareLocked(ring, ring_index, requests[i]);
      if (!context.has_value()) {
        first_error = PropagateError(context);
        break;
      }
      prepared.push_back(context.value());
      sqe_counts.push_back(context.value()->sync ? 2 : 1);
    }

    std::size_t consumed_sqes = 0;
    while (!prepared.empty() && io_uring_sq_ready(&ring.ring) > 0) {
      const int submitted = io_uring_submit(&ring.ring);
      if (submitted >= 0) {
        consumed_sqes += static_cast<std::size_t>(submitted);
        continue;
      }
      if (submitted == -EINTR || submitted == -EAGAIN || submitted == -EBUSY) {
        std::this_thread::yield();
        continue;
      }
      // The kernel refused the ring itself. Requests it never started will
      // not complete, so they are taken back and the ring stops accepting
      // requests. Started ones may already have been reaped and freed, so
      // only the recorded SQE counts are consulted for them.
      ring.broken = true;
      first_error = IoError("io_uring submit failed", -submitted);
      std::size_t accepted = 0;
      while (accepted < prepared.size() && consumed_sqes > 0) {
        consumed_sqes -= std::min(consumed_sqes, sqe_counts[accepted]);
        ++accepted;
      }
      for (std::size_t i = accepted; i < prepared.size(); ++i) {
        files_->Release(prepared[i]->request.tag.file_id);
        delete prepared[i];
        in_flight_.fetch_sub(1);
      }
      prepared.resize(accepted);
      break;
    }
  }
  release_guard();
  if (prepared.empty()) return PropagateError(first_error);
  return prepared.size();
#endif
}

auto IoUringDiskBackend::PrepareLocked(Ring &ring, std::size_t ring_index, const DiskRequest &request) -> Result<RequestContext *> {
#if !TELEPATH_HAS_LIBURING
  (void)ring;
  (void)ring_index;
  (void)request;
  return Unavailable("io_uring backend is not enabled in this build");
#else
  if (Status valid = ValidateDiskRequest(request, kMaxDiskRequestBlocks); !valid.has_value()) return PropagateError(valid);
  const bool sync = request.operation == DiskOperation::kWrite && sync_on_write_;
  if (io_uring_sq_space_left(&ring.ring) < (sync ? 2u : 1u)) return ResourceExhausted("io_uring submission queue is full");

  Result<int> fd = files_->Acquire(request.tag.file_id);
  if (!fd.has_value()) return PropagateError(fd);
  {
    std::lock_guard<std::mutex> guard(test_latch_);
    if (next_submit_result_for_test_.has_value()) {
      const int forced = *next_submit_result_for_test_;
      next_submit_result_for_test_.reset();
      files_->Release(request.tag.file_id);
      if (forced == 0) return IoError("io_uring submit accepted no requests");
      return IoError("io_uring submit failed", -forced);
    }
  }

  auto *context = new RequestContext{};
  context->request = request;
  context->ring = ring_index;
  context->fd = fd.value();
  context->sync = sync;
  context->pending_cqes = sync ? 2 : 1;
  if (request.operation == DiskOperation::kSync) {
    io_uring_sqe *sync_sqe = io_uring_get_sqe(&ring.ring);
    io_uring_prep_fsync(sync_sqe, context->fd, IORING_FSYNC_DATASYNC);
    io_uring_sqe_set_data(sync_sqe, context);
    in_flight_.fetch_add(1);
    return context;
  }
  context->total_bytes = request.block_count * geometry_.block_size();
  for (std::uint32_t i = 0; i < request.block_count; ++i) context->iov[i] = iovec{request.buffers[i], geometry_.block_size()};

  const off_t offset = static_cast<off_t>(geometry_.BlockOffset(request.tag.block_id));
  io_uring_sqe *sqe = io_uring_get_sqe(&ring.ring);
  if (request.operation == DiskOperation::kRead) io_uring_prep_readv(sqe, context->fd, context->iov.data(), request.block_count, offset);
  else io_uring_prep_writev(sqe, context->fd, context->iov.data(), request.block_count, offset);
  io_uring_sqe_set_data(sqe, context);
  if (sync) {
    sqe->flags |= IOSQE_IO_LINK;
    io_uring_sqe *sync_sqe = io_uring_get_sqe(&ring.ring);
    io_uring_prep_fsync(sync_sqe, context->fd, IORING_FSYNC_DATASYNC);
    sync_sqe->user_data = reinterpret_cast<uint64_t>(context) | kSyncTag;
  }
  in_flight_.fetch_add(1);
  return context;
#endif
}

auto IoUringDiskBackend::Reap(std::span<DiskCompletion> out, bool wait) -> Result<std::size_t> {
#if !TELEPATH_HAS_LIBURING
  (void)out;
  (void)wait;
  return Unavailable("io_uring backend is not enabled in this build");
#else
  if (out.empty()) return std::size_t{0};
  const std::size_t own = CallerRing();
  std::size_t count = ReapRing(rings_[own], out, true);
  if (count > 0 || !wait) return count;
  if (shutdown_.load() && in_flight_.load() == 0) return Unavailable("io_uring backend is shutting down");
  for (int pass = 0; pass < 2 && count == 0; ++pass) {
    if (pass == 1) WaitForCompletionSignal();
    for (std::size_t i = 0; i < ring_count_ && count < out.size(); ++i) count += ReapRing(rings_[(own + i) % ring_count_], out.subspan(count), true);
  }
  return count;
#endif
}

auto IoUringDiskBackend::ReapRing(Ring &ring, std::span<DiskCompletion> out, bool block_on_latch) -> std::size_t {
#if !TELEPATH_HAS_LIBURING
  (void)ring;
  (void)out;
  (void)block_on_latch;
  return 0;
#else
  std::vector<RequestContext *> finished;
  {
    std::unique_lock<std::mutex> lock(ring.reap_latch, std::defer_lock);
    if (block_on_latch) lock.lock();
    else if (!lock.try_lock()) return 0;
    io_uring_cqe *cqe = nullptr;
    while (finished.size() < out.size() && io_uring_peek_cqe(&ring.ring, &cqe) == 0 && cqe != nullptr) {
      const uint64_t data = cqe->user_data;
      auto *context = reinterpret_cast<RequestContext *>(data & ~kSyncTag);
      if ((data & kSyncTag) != 0) {
        context->sync_result = cqe->res;
      } else {
        context->transfer_result = cqe->res;
        std::lock_guard<std::mutex> guard(test_latch_);
        if (next_completion_result_for_test_.has_value()) {
          context->transfer_result = *next_completion_result_for_test_;
          next_completion_result_for_test_.reset();
        }
      }
      io_uring_cqe_seen(&ring.ring, cqe);
      if (--context->pending_cqes == 0) finished.push_back(context);
    }
  }

  std::size_t count = 0;
  for (RequestContext *context : finished) {
    const int result = context->transfer_result;
    if (result > 0 && static_cast<std::size_t>(result) < context->total_bytes - context->done_bytes) {
      // A short transfer that did not reach end-of-file: continue it.
      context->done_bytes += static_cast<std::size_t>(result);
      ContinueShortTransfer(context);
      if (context->pending_cqes > 0) continue;
      context->transfer_result = -EIO;
    }
    out[count++] = FinishContext(context);
  }
  return count;
#endif
}

void IoUringDiskBackend::ContinueShortTransfer(RequestContext *context) {
#if TELEPATH_HAS_LIBURING
  Ring &ring = rings_[context->ring];
  std::lock_guard<std::mutex> guard(ring.submit_latch);
  context->pending_cqes = 0;
  if (ring.broken || io_uring_sq_space_left(&ring.ring) < (context->sync ? 2u : 1u)) return;

  const std::size_t block_size = geometry_.block_size();
  int iov_count = 0;
  for (std::uint32_t i = static_cast<std::uint32_t>(context->done_bytes / block_size); i < context->request.block_count; ++i) {
    const std::size_t skip = i == context->done_bytes / block_size ? context->done_bytes % block_size : 0;
    context->iov[iov_count++] = iovec{context->request.buffers[i] + skip, block_size - skip};
  }
  const off_t offset = static_cast<off_t>(geometry_.BlockOffset(context->request.tag.block_id) + context->done_bytes);
  io_uring_sqe *sqe = io_uring_get_sqe(&ring.ring);
  if (context->request.operation == DiskOperation::kRead) io_uring_prep_readv(sqe, context->fd, context->iov.data(), iov_count, offset);
  else io_uring_prep_writev(sqe, context->fd, context->iov.data(), iov_count, offset);
  io_uring_sqe_set_data(sqe, context);
  context->pending_cqes = 1;
  if (context->sync) {
    sqe->flags |= IOSQE_IO_LINK;
    io_uring_sqe *sync_sqe = io_uring_get_sqe(&ring.ring);
    io_uring_prep_fsync(sync_sqe, context->fd, IORING_FSYNC_DATASYNC);
    sync_sqe->user_data = reinterpret_cast<uint64_t>(context) | kSyncTag;
    context->pending_cqes = 2;
  }
  if (io_uring_submit(&ring.ring) < 0) {
    ring.broken = true;
    context->pending_cqes = 0;
  }
#else
  (void)context;
#endif
}

auto IoUringDiskBackend::FinishContext(RequestContext *context) -> DiskCompletion {
  const DiskRequest &request = context->request;
  const int result = context->transfer_result;
  Status status = Ok();
  if (request.operation == DiskOperation::kSync) {
    if (result < 0) status = IoError("fdatasync failed", -result);
  } else if (result < 0) {
    status = IoError(request.operation == DiskOperation::kRead ? "io_uring read failed" : "io_uring write failed", -result);
  } else if (request.operation == DiskOperation::kRead) {
    ZeroFillReadTail(request, geometry_.block_size(), context->done_bytes + static_cast<std::size_t>(result));
  } else if (context->done_bytes + static_cast<std::size_t>(result) != context->total_bytes) {
    status = IoError("partial page write");
  } else if (context->sync && context->sync_result < 0) {
    status = IoError("fdatasync failed", -context->sync_result);
  }

  DiskCompletion completion{request.token, status};
  files_->Release(request.tag.file_id);
  delete context;
  if (in_flight_.fetch_sub(1) == 1 && shutdown_.load()) SignalShutdown();
  return completion;
}

void IoUringDiskBackend::WaitForCompletionSignal() {
  pollfd descriptor{event_fd_, POLLIN, 0};
  poll(&descriptor, 1, kSignalPollTimeoutMs);
  // After shutdown the signal is left set so every blocked reaper sees it.
  if (shutdown_.load()) {
    std::this_thread::yield();
    return;
  }
  uint64_t ignored = 0;
  std::ignore = read(event_fd_, &ignored, sizeof(ignored));
}

void IoUringDiskBackend::SignalShutdown() {
  files_->CloseIdle();
  if (event_fd_ < 0) return;
  const uint64_t one = 1;
  std::ignore = write(event_fd_, &one, sizeof(one));
}

void IoUringDiskBackend::Shutdown() {
  {
    std::lock_guard<std::mutex> guard(shutdown_latch_);
    if (shutdown_.exchange(true)) return;
  }
  if (in_flight_.load() == 0) SignalShutdown();
}

auto IoUringDiskBackend::GetCapabilities() const -> DiskBackendCapabilities {
  return DiskBackendCapabilities{DiskBackendKind::kIoUring, false, queue_depth_, kMaxDiskRequestBlocks, geometry_.block_size()};
}

void IoUringDiskBackend::SetNextSubmitResultForTest(int result) {
  std::lock_guard<std::mutex> guard(test_latch_);
  next_submit_result_for_test_ = result;
}

void IoUringDiskBackend::SetNextCompletionResultForTest(int result) {
  std::lock_guard<std::mutex> guard(test_latch_);
  next_completion_result_for_test_ = result;
}

auto IoUringDiskBackend::InFlightRequestCountForTest() const -> std::size_t { return in_flight_.load(); }

}  // namespace telepath
