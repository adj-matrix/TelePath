#include <cerrno>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "telepath/io/io_uring_disk_backend.h"
#include "telepath/io/io_uring_disk_backend_test_peer.h"

namespace {

bool RequireIoUringSuccess() {
  const char *value = std::getenv("TELEPATH_REQUIRE_IO_URING_SUCCESS");
  return value != nullptr && std::string(value) == "1";
}

}  // namespace

int main() {
  namespace fs = std::filesystem;

  const fs::path root =
      fs::temp_directory_path() / "telepath_io_uring_failure_paths_test";
  fs::remove_all(root);
  fs::create_directories(root);

  telepath::IoUringDiskBackend backend(root.string(), 4096, 8);
  if (!backend.initialization_status().ok()) {
    assert(!RequireIoUringSuccess());
    assert(backend.initialization_status().code() ==
           telepath::StatusCode::kUnavailable);
    fs::remove_all(root);
    return 0;
  }

  const telepath::BufferTag submit_fail_tag{37, 0};
  const telepath::BufferTag completion_fail_tag{37, 1};
  const telepath::BufferTag recovery_tag{37, 2};

  std::array<std::byte, 4096> page{};
  page[0] = std::byte{0x91};
  page[4095] = std::byte{0xA1};

  telepath::IoUringDiskBackendTestPeer::ForceNextSubmitResult(backend, 0);
  auto submit_failure =
      backend.SubmitWrite(submit_fail_tag, page.data(), page.size());
  assert(!submit_failure.ok());
  assert(submit_failure.status().code() == telepath::StatusCode::kIoError);
  assert(telepath::IoUringDiskBackendTestPeer::InFlightRequestCount(backend) == 0);

  std::array<std::byte, 4096> completion_buffer{};
  completion_buffer.fill(std::byte{0xFF});
  auto completion_request = backend.SubmitRead(completion_fail_tag,
                                               completion_buffer.data(),
                                               completion_buffer.size());
  assert(completion_request.ok());
  assert(telepath::IoUringDiskBackendTestPeer::InFlightRequestCount(backend) == 1);

  telepath::IoUringDiskBackendTestPeer::ForceNextCompletionResult(backend, -EIO);
  auto completion_failure = backend.PollCompletion();
  assert(completion_failure.ok());
  assert(completion_failure.value().request_id == completion_request.value());
  assert(completion_failure.value().operation == telepath::DiskOperation::kRead);
  assert(completion_failure.value().tag == completion_fail_tag);
  assert(!completion_failure.value().status.ok());
  assert(completion_failure.value().status.code() == telepath::StatusCode::kIoError);
  assert(telepath::IoUringDiskBackendTestPeer::InFlightRequestCount(backend) == 0);

  auto recovery_write =
      backend.SubmitWrite(recovery_tag, page.data(), page.size());
  assert(recovery_write.ok());
  auto recovery_completion = backend.PollCompletion();
  assert(recovery_completion.ok());
  assert(recovery_completion.value().request_id == recovery_write.value());
  assert(recovery_completion.value().status.ok());
  assert(telepath::IoUringDiskBackendTestPeer::InFlightRequestCount(backend) == 0);

  backend.Shutdown();
  fs::remove_all(root);
  return 0;
}
