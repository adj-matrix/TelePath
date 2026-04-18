#include "telepath/io/disk_backend_factory.h"

#include <memory>
#include <utility>

#include "telepath/io/io_uring_disk_backend.h"
#include "telepath/io/posix_disk_backend.h"

namespace telepath {

namespace {

auto MakePosixBackend(std::string root_path, std::size_t page_size, bool is_fallback) -> Result<std::unique_ptr<DiskBackend>> {
  std::unique_ptr<DiskBackend> backend = std::make_unique<PosixDiskBackend>(std::move(root_path), page_size, is_fallback);
  return backend;
}

auto MakeIoUringBackend(std::string root_path, std::size_t page_size, std::size_t queue_depth) -> Result<std::unique_ptr<DiskBackend>> {
  if (!IsIoUringBackendSupported()) return Status::Unavailable("io_uring backend is not available in this build");
  std::unique_ptr<DiskBackend> backend = std::make_unique<IoUringDiskBackend>(std::move(root_path), page_size, queue_depth);
  auto *io_uring_backend = static_cast<IoUringDiskBackend *>(backend.get());
  if (!io_uring_backend->initialization_status().ok()) return io_uring_backend->initialization_status();
  return backend;
}

auto CreateIoUringOrFallback(std::string root_path, std::size_t page_size, std::size_t queue_depth, bool allow_fallback) -> Result<std::unique_ptr<DiskBackend>> {
  auto io_uring_backend = MakeIoUringBackend(root_path, page_size, queue_depth);
  if (io_uring_backend.ok()) return io_uring_backend;
  if (!allow_fallback) return io_uring_backend.status();
  return MakePosixBackend(std::move(root_path), page_size, true);
}

auto CreateAutoBackend(std::string root_path, std::size_t page_size, std::size_t queue_depth) -> Result<std::unique_ptr<DiskBackend>> {
  auto io_uring_backend = MakeIoUringBackend(root_path, page_size, queue_depth);
  if (io_uring_backend.ok()) return io_uring_backend;
  return MakePosixBackend(std::move(root_path), page_size, true);
}

}  // namespace

#if TELEPATH_HAS_LIBURING
bool IsIoUringBackendSupported() { return true; }
#else
bool IsIoUringBackendSupported() { return false; }
#endif

auto CreateDiskBackend(std::string root_path, std::size_t page_size, const DiskBackendOptions &options) -> Result<std::unique_ptr<DiskBackend>> {
  const std::size_t queue_depth = options.ResolveQueueDepth();
  switch (options.preferred_kind) {
  case DiskBackendKind::kPosix:
    return MakePosixBackend(std::move(root_path), page_size, false);
  case DiskBackendKind::kIoUring:
    return CreateIoUringOrFallback(std::move(root_path), page_size, queue_depth, options.allow_fallback);
  case DiskBackendKind::kAuto:
    return CreateAutoBackend(std::move(root_path), page_size, queue_depth);
  }

  return Status::InvalidArgument("unknown disk backend kind");
}

}  // namespace telepath
