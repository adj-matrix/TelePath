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

bool HandleUnavailableIoUringBackend(const telepath::Result<std::unique_ptr<telepath::IoUringDiskBackend>> &backend_result) {
  if (backend_result.has_value()) return false;

  assert(!telepath::io_test_support::RequireIoUringSuccess());
  assert(backend_result.error().code() == telepath::ErrorCode::kUnavailable);
  return true;
}

void AssertSubmitFailureDoesNotLeaveInFlightRequest(
    telepath::IoUringDiskBackend *backend,
    const telepath::BufferTag &tag,
    const std::array<std::byte, 4096> &page) {
  telepath::IoUringDiskBackendTestPeer::ForceNextSubmitResult(*backend, 0);
  auto submit_failure = backend->SubmitWrite(tag, page.data(), page.size());
  assert(!submit_failure.has_value());
  assert(submit_failure.error().code() == telepath::ErrorCode::kIoError);
  assert(telepath::IoUringDiskBackendTestPeer::InFlightRequestCount(*backend) == 0);
}

void AssertCompletionFailureReportsIoError(telepath::IoUringDiskBackend *backend, const telepath::BufferTag &tag) {
  std::array<std::byte, 4096> buffer{};
  buffer.fill(std::byte{0xFF});

  auto completion_request = backend->SubmitRead(tag, buffer.data(), buffer.size());
  assert(completion_request.has_value());
  assert(telepath::IoUringDiskBackendTestPeer::InFlightRequestCount(*backend) == 1);

  telepath::IoUringDiskBackendTestPeer::ForceNextCompletionResult(*backend, -EIO);
  auto completion_failure = backend->PollCompletion();
  assert(completion_failure.has_value());
  assert(completion_failure.value().request_id == completion_request.value());
  assert(completion_failure.value().operation == telepath::DiskOperation::kRead);
  assert(completion_failure.value().tag == tag);
  assert(!completion_failure.value().status.has_value());
  assert(completion_failure.value().status.error().code() == telepath::ErrorCode::kIoError);
  assert(telepath::IoUringDiskBackendTestPeer::InFlightRequestCount(*backend) == 0);
}

void AssertBackendRecoversAfterFailure(
    telepath::IoUringDiskBackend *backend,
    const telepath::BufferTag &tag,
    const std::array<std::byte, 4096> &page) {
  auto recovery_write = backend->SubmitWrite(tag, page.data(), page.size());
  assert(recovery_write.has_value());

  auto recovery_completion = backend->PollCompletion();
  assert(recovery_completion.has_value());
  assert(recovery_completion.value().request_id == recovery_write.value());
  assert(recovery_completion.value().status.has_value());
  assert(telepath::IoUringDiskBackendTestPeer::InFlightRequestCount(*backend) == 0);
}

}  // namespace

int main() {
  telepath::io_test_support::TestRootGuard root_guard("telepath_io_uring_failure_paths_test");
  auto backend_result = telepath::IoUringDiskBackend::Create(root_guard.path().string(), telepath::BlockGeometry::Of<4096>(), 8);
  if (HandleUnavailableIoUringBackend(backend_result)) return 0;
  telepath::IoUringDiskBackend &backend = *backend_result.value();

  const telepath::BufferTag submit_fail_tag{37, 0};
  const telepath::BufferTag completion_fail_tag{37, 1};
  const telepath::BufferTag recovery_tag{37, 2};
  const std::filesystem::path submit_fail_path = root_guard.path() / "file_37.tp";
  auto page = BuildMarkedPage();
  AssertSubmitFailureDoesNotLeaveInFlightRequest(&backend, submit_fail_tag, page);
#if defined(__linux__)
  assert(telepath::io_test_support::CountOpenDescriptorsForPath(submit_fail_path) == 1);
#endif
  AssertCompletionFailureReportsIoError(&backend, completion_fail_tag);
  AssertBackendRecoversAfterFailure(&backend, recovery_tag, page);
  backend.Shutdown();
#if defined(__linux__)
  assert(telepath::io_test_support::CountOpenDescriptorsForPath(submit_fail_path) == 0);
#endif
  return 0;
}
