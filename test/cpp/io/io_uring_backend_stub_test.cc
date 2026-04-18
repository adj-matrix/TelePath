#include <array>
#include <cassert>
#include <cstddef>

#include "io_test_support.h"
#include "telepath/io/io_uring_disk_backend.h"

namespace {

void AssertNullReadRejected(telepath::IoUringDiskBackend *backend, std::size_t page_size) {
  auto result = backend->SubmitRead(telepath::BufferTag{1, 0}, nullptr, page_size);
  assert(!result.ok());
  assert(result.status().code() == telepath::StatusCode::kInvalidArgument);
}

void AssertNullWriteRejected(telepath::IoUringDiskBackend *backend, std::size_t page_size) {
  auto result = backend->SubmitWrite(telepath::BufferTag{1, 0}, nullptr, page_size);
  assert(!result.ok());
  assert(result.status().code() == telepath::StatusCode::kInvalidArgument);
}

void AssertWrongSizedReadRejected(telepath::IoUringDiskBackend *backend, std::byte *page) {
  auto result = backend->SubmitRead(telepath::BufferTag{1, 0}, page, 1024);
  assert(!result.ok());
  assert(result.status().code() == telepath::StatusCode::kInvalidArgument);
}

void AssertWrongSizedWriteRejected(telepath::IoUringDiskBackend *backend, const std::byte *page) {
  auto result = backend->SubmitWrite(telepath::BufferTag{1, 0}, page, 1024);
  assert(!result.ok());
  assert(result.status().code() == telepath::StatusCode::kInvalidArgument);
}

void AssertSubmitReadUnavailable(telepath::IoUringDiskBackend *backend, std::byte *page, std::size_t page_size) {
  auto result = backend->SubmitRead(telepath::BufferTag{1, 0}, page, page_size);
  assert(!result.ok());
  assert(result.status().code() == telepath::StatusCode::kUnavailable);
}

void AssertCapabilities(const telepath::IoUringDiskBackend &backend) {
  const auto capabilities = backend.GetCapabilities();
  assert(capabilities.kind == telepath::DiskBackendKind::kIoUring);
  assert(capabilities.supports_submit_batching);
  assert(capabilities.supports_completion_batching);
  assert(capabilities.recommended_queue_depth == 64);
}

void AssertShutdownLeavesNoCompletions(telepath::IoUringDiskBackend *backend) {
  backend->Shutdown();
  auto completion = backend->PollCompletion();
  assert(!completion.ok());
  assert(completion.status().code() == telepath::StatusCode::kUnavailable);
}

}  // namespace

int main() {
  telepath::io_test_support::TestRootGuard root_guard("telepath_io_uring_stub_test");
  telepath::IoUringDiskBackend backend(root_guard.path().string(), 4096, 64);
  std::array<std::byte, 4096> page{};

  AssertNullReadRejected(&backend, page.size());
  AssertNullWriteRejected(&backend, page.size());
  AssertWrongSizedReadRejected(&backend, page.data());
  AssertWrongSizedWriteRejected(&backend, page.data());
  AssertSubmitReadUnavailable(&backend, page.data(), page.size());
  AssertCapabilities(backend);
  AssertShutdownLeavesNoCompletions(&backend);
  return 0;
}
