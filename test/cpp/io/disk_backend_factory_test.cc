#include <cassert>
#include <filesystem>

#include "telepath/io/disk_backend_factory.h"

int main() {
  namespace fs = std::filesystem;

  const fs::path root = fs::temp_directory_path() / "telepath_backend_factory_test";
  fs::remove_all(root);
  fs::create_directories(root);

  {
    telepath::DiskBackendOptions options;
    options.preferred_kind = telepath::DiskBackendKind::kAuto;
    auto backend_result =
        telepath::CreateDiskBackend(root.string(), 4096, options);
    assert(backend_result.ok());
    const auto capabilities = backend_result.value()->GetCapabilities();
    assert(capabilities.kind == telepath::DiskBackendKind::kPosix);
    assert(capabilities.is_fallback_backend);
    backend_result.value()->Shutdown();
  }

  {
    telepath::DiskBackendOptions options;
    options.preferred_kind = telepath::DiskBackendKind::kPosix;
    auto backend_result =
        telepath::CreateDiskBackend(root.string(), 4096, options);
    assert(backend_result.ok());
    const auto capabilities = backend_result.value()->GetCapabilities();
    assert(capabilities.kind == telepath::DiskBackendKind::kPosix);
    assert(!capabilities.is_fallback_backend);
    backend_result.value()->Shutdown();
  }

  {
    telepath::DiskBackendOptions options;
    options.preferred_kind = telepath::DiskBackendKind::kIoUring;
    options.allow_fallback = false;
    auto backend_result =
        telepath::CreateDiskBackend(root.string(), 4096, options);
    assert(!backend_result.ok());
    assert(backend_result.status().code() == telepath::StatusCode::kUnavailable);
  }

  {
    telepath::DiskBackendOptions options;
    options.preferred_kind = telepath::DiskBackendKind::kIoUring;
    options.allow_fallback = true;
    auto backend_result =
        telepath::CreateDiskBackend(root.string(), 4096, options);
    assert(backend_result.ok());
    const auto capabilities = backend_result.value()->GetCapabilities();
    assert(capabilities.kind == telepath::DiskBackendKind::kPosix);
    assert(capabilities.is_fallback_backend);
    backend_result.value()->Shutdown();
  }

  fs::remove_all(root);
  return 0;
}
