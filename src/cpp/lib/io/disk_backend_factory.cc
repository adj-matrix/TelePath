#include "telepath/io/disk_backend_factory.h"

#include <memory>
#include <utility>

#include "telepath/io/io_uring_disk_backend.h"
#include "telepath/io/posix_disk_backend.h"

namespace telepath {

namespace {

auto MakePosixBackend(
  std::string root_path,
  BlockGeometry geometry,
  bool is_fallback,
  const DiskBackendOptions &options
) -> Result<std::unique_ptr<DiskBackend>> {
  std::unique_ptr<DiskBackend> backend = std::make_unique<PosixDiskBackend>(
    std::move(root_path),
    geometry,
    is_fallback,
    options.ResolveMaxOpenFiles(),
    options.sync_on_write,
    options.ResolveIoWorkerCount());
  return backend;
}

auto MakeIoUringBackend(
  std::string root_path,
  BlockGeometry geometry,
  const DiskBackendOptions &options
) -> Result<std::unique_ptr<DiskBackend>> {
  if (!IsIoUringBackendSupported()) return Unavailable("io_uring backend is not available in this build");
  auto backend = IoUringDiskBackend::Create(std::move(root_path), geometry, options.ResolveQueueDepth(), options.ResolveMaxOpenFiles(), options.sync_on_write);
  if (!backend.has_value()) return PropagateError(backend);
  return std::unique_ptr<DiskBackend>(std::move(backend.value()));
}

auto CreateIoUringOrFallback(
  std::string root_path,
  BlockGeometry geometry,
  const DiskBackendOptions &options
) -> Result<std::unique_ptr<DiskBackend>> {
  auto io_uring_backend = MakeIoUringBackend(root_path, geometry, options);
  if (io_uring_backend.has_value()) return io_uring_backend;
  if (options.preferred_kind == DiskBackendKind::kIoUring && !options.allow_fallback) return PropagateError(io_uring_backend);
  return MakePosixBackend(std::move(root_path), geometry, true, options);
}

}  // namespace

#if TELEPATH_HAS_LIBURING
bool IsIoUringBackendSupported() { return true; }
#else
bool IsIoUringBackendSupported() { return false; }
#endif

auto CreateDiskBackend(std::string root_path, std::size_t page_size, const DiskBackendOptions &options) -> Result<std::unique_ptr<DiskBackend>> {
  auto geometry_result = BlockGeometry::Create(page_size);
  if (!geometry_result.has_value()) return PropagateError(geometry_result);
  const BlockGeometry geometry = geometry_result.value();
  switch (options.preferred_kind) {
  case DiskBackendKind::kPosix:
    return MakePosixBackend(std::move(root_path), geometry, false, options);
  case DiskBackendKind::kIoUring:
  case DiskBackendKind::kAuto:
    return CreateIoUringOrFallback(std::move(root_path), geometry, options);
  }

  return InvalidArgument("unknown disk backend kind");
}

}  // namespace telepath
