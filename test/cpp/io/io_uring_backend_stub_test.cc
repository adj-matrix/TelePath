#include <cassert>

#include "io_test_support.h"
#include "telepath/io/disk_backend_factory.h"
#include "telepath/io/io_uring_disk_backend.h"

namespace {

void AssertCreateUnavailable(const std::filesystem::path &root) {
  auto backend_result = telepath::IoUringDiskBackend::Create(root.string(), telepath::BlockGeometry::Of<4096>(), 64);
  assert(!backend_result.has_value());
  assert(backend_result.error().code() == telepath::ErrorCode::kUnavailable);
}

void AssertEmptyRootRejected() {
  auto backend_result = telepath::IoUringDiskBackend::Create("", telepath::BlockGeometry::Of<4096>(), 64);
  assert(!backend_result.has_value());
  assert(backend_result.error().code() == telepath::ErrorCode::kInvalidArgument);
}

void AssertInvalidPageSizeRejectedByFactory(const std::filesystem::path &root) {
  telepath::DiskBackendOptions options;
  options.preferred_kind = telepath::DiskBackendKind::kIoUring;
  auto backend_result = telepath::CreateDiskBackend(root.string(), 0, options);
  assert(!backend_result.has_value());
  assert(backend_result.error().code() == telepath::ErrorCode::kInvalidArgument);
}

}  // namespace

int main() {
  telepath::io_test_support::TestRootGuard root_guard("telepath_io_uring_stub_test");
  AssertCreateUnavailable(root_guard.path());
  AssertEmptyRootRejected();
  AssertInvalidPageSizeRejectedByFactory(root_guard.path());
  return 0;
}
