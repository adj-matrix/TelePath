#include <array>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <filesystem>

#include "io_test_support.h"
#include "telepath/io/io_uring_disk_backend.h"
#include "telepath/io/posix_disk_backend.h"

namespace {

auto BuildMarkedPage(std::byte first, std::byte last) -> std::array<std::byte, 4096> {
  std::array<std::byte, 4096> page{};
  page[0] = first;
  page[4095] = last;
  return page;
}

void AssertPageMarkers(const std::array<std::byte, 4096> &page, std::byte first, std::byte last) {
  assert(page[0] == first);
  assert(page[4095] == last);
}

bool HandleUnavailableIoUringBackend(const telepath::IoUringDiskBackend &backend) {
  if (backend.initialization_status().ok()) return false;

  assert(!telepath::io_test_support::RequireIoUringSuccess());
  assert(backend.initialization_status().code() == telepath::StatusCode::kUnavailable);
  return true;
}

void AssertShutdownDrainsPendingWrite(
    const std::filesystem::path &root,
    const telepath::BufferTag &tag,
    const std::array<std::byte, 4096> &page) {
  telepath::IoUringDiskBackend backend(root.string(), 4096, 8);
  assert(backend.initialization_status().ok());

  auto request = backend.SubmitWrite(tag, page.data(), page.size());
  assert(request.ok());

  backend.Shutdown();

  auto completion = backend.PollCompletion();
  assert(completion.ok());
  assert(completion.value().request_id == request.value());
  assert(completion.value().operation == telepath::DiskOperation::kWrite);
  assert(completion.value().tag == tag);
  assert(completion.value().status.ok());

  auto drained_completion = backend.PollCompletion();
  assert(!drained_completion.ok());
  assert(drained_completion.status().code() == telepath::StatusCode::kUnavailable);
}

void AssertDestructorDrainsOutstandingWrite(
    const std::filesystem::path &root,
    const telepath::BufferTag &tag,
    const std::array<std::byte, 4096> &page) {
  telepath::IoUringDiskBackend backend(root.string(), 4096, 8);
  assert(backend.initialization_status().ok());

  auto request = backend.SubmitWrite(tag, page.data(), page.size());
  assert(request.ok());
}

void AssertPersistedWrites(
    const std::filesystem::path &root,
    const telepath::BufferTag &first_tag,
    const std::array<std::byte, 4096> &first_page,
    const telepath::BufferTag &second_tag,
    const std::array<std::byte, 4096> &second_page) {
  telepath::PosixDiskBackend verifier(root.string(), 4096);
  std::array<std::byte, 4096> verify_first{};
  std::array<std::byte, 4096> verify_second{};

  auto verify_first_request = verifier.SubmitRead(first_tag, verify_first.data(), verify_first.size());
  auto verify_second_request = verifier.SubmitRead(second_tag, verify_second.data(), verify_second.size());
  assert(verify_first_request.ok());
  assert(verify_second_request.ok());

  auto verify_first_completion = verifier.PollCompletion();
  auto verify_second_completion = verifier.PollCompletion();
  assert(verify_first_completion.ok());
  assert(verify_second_completion.ok());
  assert(verify_first_completion.value().status.ok());
  assert(verify_second_completion.value().status.ok());
  AssertPageMarkers(verify_first, first_page[0], first_page[4095]);
  AssertPageMarkers(verify_second, second_page[0], second_page[4095]);
}

}  // namespace

int main() {
  telepath::io_test_support::TestRootGuard root_guard("telepath_io_uring_shutdown_drain_test");
  telepath::IoUringDiskBackend probe_backend(root_guard.path().string(), 4096, 8);
  if (HandleUnavailableIoUringBackend(probe_backend)) return 0;
  probe_backend.Shutdown();

  auto first_page = BuildMarkedPage(std::byte{0x51}, std::byte{0x61});
  auto second_page = BuildMarkedPage(std::byte{0x71}, std::byte{0x81});
  const telepath::BufferTag first_tag{29, 0};
  const telepath::BufferTag second_tag{29, 1};
  AssertShutdownDrainsPendingWrite(root_guard.path(), first_tag, first_page);
  AssertDestructorDrainsOutstandingWrite(root_guard.path(), second_tag, second_page);
  AssertPersistedWrites(root_guard.path(), first_tag, first_page, second_tag, second_page);
  return 0;
}
