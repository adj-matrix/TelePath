#include <cerrno>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <filesystem>

#include "io_test_support.h"
#include "telepath/io/io_uring_disk_backend.h"
#include "telepath/io/io_uring_disk_backend_test_peer.h"

namespace {

auto BuildMarkedPage() -> std::array<std::byte, 4096> {
  std::array<std::byte, 4096> page{};
  page[0] = std::byte{0x91};
  page[4095] = std::byte{0xA1};
  return page;
}

bool HandleUnavailableIoUringBackend(const telepath::IoUringDiskBackend &backend) {
  if (backend.initialization_status().ok()) return false;

  assert(!telepath::io_test_support::RequireIoUringSuccess());
  assert(backend.initialization_status().code() == telepath::StatusCode::kUnavailable);
  return true;
}

void AssertSubmitFailureDoesNotLeaveInFlightRequest(
    telepath::IoUringDiskBackend *backend,
    const telepath::BufferTag &tag,
    const std::array<std::byte, 4096> &page) {
  telepath::IoUringDiskBackendTestPeer::ForceNextSubmitResult(*backend, 0);
  auto submit_failure = backend->SubmitWrite(tag, page.data(), page.size());
  assert(!submit_failure.ok());
  assert(submit_failure.status().code() == telepath::StatusCode::kIoError);
  assert(telepath::IoUringDiskBackendTestPeer::InFlightRequestCount(*backend) == 0);
}

void AssertCompletionFailureReportsIoError(telepath::IoUringDiskBackend *backend, const telepath::BufferTag &tag) {
  std::array<std::byte, 4096> buffer{};
  buffer.fill(std::byte{0xFF});

  auto completion_request = backend->SubmitRead(tag, buffer.data(), buffer.size());
  assert(completion_request.ok());
  assert(telepath::IoUringDiskBackendTestPeer::InFlightRequestCount(*backend) == 1);

  telepath::IoUringDiskBackendTestPeer::ForceNextCompletionResult(*backend, -EIO);
  auto completion_failure = backend->PollCompletion();
  assert(completion_failure.ok());
  assert(completion_failure.value().request_id == completion_request.value());
  assert(completion_failure.value().operation == telepath::DiskOperation::kRead);
  assert(completion_failure.value().tag == tag);
  assert(!completion_failure.value().status.ok());
  assert(completion_failure.value().status.code() == telepath::StatusCode::kIoError);
  assert(telepath::IoUringDiskBackendTestPeer::InFlightRequestCount(*backend) == 0);
}

void AssertBackendRecoversAfterFailure(
    telepath::IoUringDiskBackend *backend,
    const telepath::BufferTag &tag,
    const std::array<std::byte, 4096> &page) {
  auto recovery_write = backend->SubmitWrite(tag, page.data(), page.size());
  assert(recovery_write.ok());

  auto recovery_completion = backend->PollCompletion();
  assert(recovery_completion.ok());
  assert(recovery_completion.value().request_id == recovery_write.value());
  assert(recovery_completion.value().status.ok());
  assert(telepath::IoUringDiskBackendTestPeer::InFlightRequestCount(*backend) == 0);
}

}  // namespace

int main() {
  telepath::io_test_support::TestRootGuard root_guard("telepath_io_uring_failure_paths_test");
  telepath::IoUringDiskBackend backend(root_guard.path().string(), 4096, 8);
  if (HandleUnavailableIoUringBackend(backend)) return 0;

  const telepath::BufferTag submit_fail_tag{37, 0};
  const telepath::BufferTag completion_fail_tag{37, 1};
  const telepath::BufferTag recovery_tag{37, 2};
  auto page = BuildMarkedPage();
  AssertSubmitFailureDoesNotLeaveInFlightRequest(&backend, submit_fail_tag, page);
  AssertCompletionFailureReportsIoError(&backend, completion_fail_tag);
  AssertBackendRecoversAfterFailure(&backend, recovery_tag, page);
  backend.Shutdown();
  return 0;
}
