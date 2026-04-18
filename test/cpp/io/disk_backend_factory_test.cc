#include <cassert>
#include <filesystem>

#include "io_test_support.h"
#include "telepath/io/disk_backend_factory.h"

namespace {

void AssertAutoBackendFallsBackToPosix(const std::filesystem::path &root) {
  telepath::DiskBackendOptions options;
  options.preferred_kind = telepath::DiskBackendKind::kAuto;

  auto backend_result = telepath::CreateDiskBackend(root.string(), 4096, options);
  assert(backend_result.ok());

  const auto capabilities = backend_result.value()->GetCapabilities();
  assert(capabilities.kind == telepath::DiskBackendKind::kPosix);
  assert(capabilities.is_fallback_backend);
  backend_result.value()->Shutdown();
}

void AssertExplicitPosixBackend(const std::filesystem::path &root) {
  telepath::DiskBackendOptions options;
  options.preferred_kind = telepath::DiskBackendKind::kPosix;

  auto backend_result = telepath::CreateDiskBackend(root.string(), 4096, options);
  assert(backend_result.ok());

  const auto capabilities = backend_result.value()->GetCapabilities();
  assert(capabilities.kind == telepath::DiskBackendKind::kPosix);
  assert(!capabilities.is_fallback_backend);
  backend_result.value()->Shutdown();
}

void AssertStrictIoUringSelectionFails(const std::filesystem::path &root) {
  telepath::DiskBackendOptions options;
  options.preferred_kind = telepath::DiskBackendKind::kIoUring;
  options.allow_fallback = false;

  auto backend_result = telepath::CreateDiskBackend(root.string(), 4096, options);
  assert(!backend_result.ok());
  assert(backend_result.status().code() == telepath::StatusCode::kUnavailable);
}

void AssertIoUringFallbackReturnsPosix(const std::filesystem::path &root) {
  telepath::DiskBackendOptions options;
  options.preferred_kind = telepath::DiskBackendKind::kIoUring;
  options.allow_fallback = true;

  auto backend_result = telepath::CreateDiskBackend(root.string(), 4096, options);
  assert(backend_result.ok());

  const auto capabilities = backend_result.value()->GetCapabilities();
  assert(capabilities.kind == telepath::DiskBackendKind::kPosix);
  assert(capabilities.is_fallback_backend);
  backend_result.value()->Shutdown();
}

}  // namespace

int main() {
  telepath::io_test_support::TestRootGuard root_guard("telepath_backend_factory_test");
  AssertAutoBackendFallsBackToPosix(root_guard.path());
  AssertExplicitPosixBackend(root_guard.path());
  AssertStrictIoUringSelectionFails(root_guard.path());
  AssertIoUringFallbackReturnsPosix(root_guard.path());
  return 0;
}
