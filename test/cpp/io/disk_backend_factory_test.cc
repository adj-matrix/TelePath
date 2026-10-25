#include <cassert>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <utility>

#include "io_test_support.h"
#include "telepath/io/disk_backend_factory.h"

namespace {

// Larger than the kernel's io_uring entry limit, so ring setup fails even on
// hosts where io_uring works. This makes the fallback path testable in every
// build configuration.
constexpr std::size_t kUnsupportedQueueDepth = std::size_t{1} << 20;

auto CreateBackend(
  const std::filesystem::path &root,
  telepath::DiskBackendKind kind,
  bool allow_fallback,
  std::size_t queue_depth = 0
) -> telepath::Result<std::unique_ptr<telepath::DiskBackend>> {
  telepath::DiskBackendOptions options;
  options.preferred_kind = kind;
  options.allow_fallback = allow_fallback;
  options.queue_depth = queue_depth;
  return telepath::CreateDiskBackend(root.string(), 4096, options);
}

void AssertBackend(
  telepath::Result<std::unique_ptr<telepath::DiskBackend>> backend_result,
  telepath::DiskBackendKind expected_kind,
  bool expected_fallback
) {
  assert(backend_result.ok());

  const auto capabilities = backend_result.value()->GetCapabilities();
  assert(capabilities.kind == expected_kind);
  assert(capabilities.is_fallback_backend == expected_fallback);
  backend_result.value()->Shutdown();
}

// Strict io_uring selection either yields the native backend or reports
// Unavailable, depending on whether this build and kernel can set up a ring.
auto ProbeStrictIoUringSelection(const std::filesystem::path &root) -> bool {
  auto backend_result = CreateBackend(root, telepath::DiskBackendKind::kIoUring, false);
  if (!backend_result.ok()) {
    assert(backend_result.status().code() == telepath::StatusCode::kUnavailable);
    assert(!telepath::io_test_support::RequireIoUringSuccess());
    return false;
  }

  assert(telepath::IsIoUringBackendSupported());
  AssertBackend(std::move(backend_result), telepath::DiskBackendKind::kIoUring, false);
  return true;
}

void AssertExplicitPosixBackend(const std::filesystem::path &root) {
  AssertBackend(CreateBackend(root, telepath::DiskBackendKind::kPosix, true), telepath::DiskBackendKind::kPosix, false);
}

void AssertAutoBackendSelection(const std::filesystem::path &root, bool native_io_uring) {
  auto backend_result = CreateBackend(root, telepath::DiskBackendKind::kAuto, false);
  if (native_io_uring) {
    AssertBackend(std::move(backend_result), telepath::DiskBackendKind::kIoUring, false);
    return;
  }
  AssertBackend(std::move(backend_result), telepath::DiskBackendKind::kPosix, true);
}

void AssertIoUringWithFallbackSelection(const std::filesystem::path &root, bool native_io_uring) {
  auto backend_result = CreateBackend(root, telepath::DiskBackendKind::kIoUring, true);
  if (native_io_uring) {
    AssertBackend(std::move(backend_result), telepath::DiskBackendKind::kIoUring, false);
    return;
  }
  AssertBackend(std::move(backend_result), telepath::DiskBackendKind::kPosix, true);
}

void AssertRingSetupFailureFallsBackToPosix(const std::filesystem::path &root) {
  AssertBackend(
    CreateBackend(root, telepath::DiskBackendKind::kAuto, false, kUnsupportedQueueDepth),
    telepath::DiskBackendKind::kPosix, true);
  AssertBackend(
    CreateBackend(root, telepath::DiskBackendKind::kIoUring, true, kUnsupportedQueueDepth),
    telepath::DiskBackendKind::kPosix, true);

  auto strict_result = CreateBackend(root, telepath::DiskBackendKind::kIoUring, false, kUnsupportedQueueDepth);
  assert(!strict_result.ok());
  assert(strict_result.status().code() == telepath::StatusCode::kUnavailable);
}

}  // namespace

int main() {
  telepath::io_test_support::TestRootGuard root_guard("telepath_backend_factory_test");
  const bool native_io_uring = ProbeStrictIoUringSelection(root_guard.path());
  AssertExplicitPosixBackend(root_guard.path());
  AssertAutoBackendSelection(root_guard.path(), native_io_uring);
  AssertIoUringWithFallbackSelection(root_guard.path(), native_io_uring);
  AssertRingSetupFailureFallsBackToPosix(root_guard.path());
  return 0;
}
