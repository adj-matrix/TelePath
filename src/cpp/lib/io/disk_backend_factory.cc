#include "telepath/io/disk_backend_factory.h"

#include <memory>
#include <utility>

#include "telepath/io/io_uring_disk_backend.h"
#include "telepath/io/posix_disk_backend.h"

namespace telepath {

namespace {

Result<std::unique_ptr<DiskBackend>> MakePosixBackend(std::string root_path,
                                                      std::size_t page_size,
                                                      bool is_fallback) {
  std::unique_ptr<DiskBackend> backend =
      std::make_unique<PosixDiskBackend>(std::move(root_path), page_size,
                                         is_fallback);
  return backend;
}

Result<std::unique_ptr<DiskBackend>> MakeIoUringBackend(std::string root_path,
                                                        std::size_t page_size,
                                                        std::size_t queue_depth) {
  if (!IsIoUringBackendSupported()) {
    return Status::Unavailable(
        "io_uring backend is not available in this build");
  }
  std::unique_ptr<DiskBackend> backend =
      std::make_unique<IoUringDiskBackend>(std::move(root_path), page_size,
                                           queue_depth);
  auto *io_uring_backend = static_cast<IoUringDiskBackend *>(backend.get());
  if (!io_uring_backend->initialization_status().ok()) {
    return io_uring_backend->initialization_status();
  }
  return backend;
}

}  // namespace

#if TELEPATH_HAS_LIBURING
bool IsIoUringBackendSupported() { return true; }
#else
bool IsIoUringBackendSupported() { return false; }
#endif

Result<std::unique_ptr<DiskBackend>> CreateDiskBackend(
    std::string root_path, std::size_t page_size,
    const DiskBackendOptions &options) {
  switch (options.preferred_kind) {
    case DiskBackendKind::kPosix:
      return MakePosixBackend(std::move(root_path), page_size, false);
    case DiskBackendKind::kIoUring: {
      auto io_uring_backend = MakeIoUringBackend(root_path, page_size,
                                                 options.ResolveQueueDepth());
      if (io_uring_backend.ok()) {
        return io_uring_backend;
      }
      if (!options.allow_fallback) {
        return io_uring_backend.status();
      }
      return MakePosixBackend(std::move(root_path), page_size, true);
    }
    case DiskBackendKind::kAuto: {
      auto io_uring_backend = MakeIoUringBackend(root_path, page_size,
                                                 options.ResolveQueueDepth());
      if (io_uring_backend.ok()) {
        return io_uring_backend;
      }
      return MakePosixBackend(std::move(root_path), page_size, true);
    }
  }

  return Status::InvalidArgument("unknown disk backend kind");
}

}  // namespace telepath
