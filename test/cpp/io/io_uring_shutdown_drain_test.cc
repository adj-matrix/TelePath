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

bool HandleUnavailableIoUringBackend(const telepath::Result<std::unique_ptr<telepath::IoUringDiskBackend>> &backend_result) {
  if (backend_result.has_value()) return false;

  assert(!telepath::io_test_support::RequireIoUringSuccess());
  assert(backend_result.error().code() == telepath::ErrorCode::kUnavailable);
  return true;
}

void AssertShutdownDrainsPendingWrite(
    const std::filesystem::path &root,
    const telepath::BufferTag &tag,
    const std::array<std::byte, 4096> &page) {
  auto backend_result = telepath::IoUringDiskBackend::Create(root.string(), telepath::BlockGeometry::Of<4096>(), 8);
  assert(backend_result.has_value());
  telepath::IoUringDiskBackend &backend = *backend_result.value();

  auto request = backend.SubmitWrite(tag, page.data(), page.size());
  assert(request.has_value());

  backend.Shutdown();

  auto completion = backend.PollCompletion();
  assert(completion.has_value());
  assert(completion.value().request_id == request.value());
  assert(completion.value().operation == telepath::DiskOperation::kWrite);
  assert(completion.value().tag == tag);
  assert(completion.value().status.has_value());

  auto drained_completion = backend.PollCompletion();
  assert(!drained_completion.has_value());
  assert(drained_completion.error().code() == telepath::ErrorCode::kUnavailable);
}

void AssertDestructorDrainsOutstandingWrite(
    const std::filesystem::path &root,
    const telepath::BufferTag &tag,
    const std::array<std::byte, 4096> &page) {
  auto backend_result = telepath::IoUringDiskBackend::Create(root.string(), telepath::BlockGeometry::Of<4096>(), 8);
  assert(backend_result.has_value());
  telepath::IoUringDiskBackend &backend = *backend_result.value();

  auto request = backend.SubmitWrite(tag, page.data(), page.size());
  assert(request.has_value());
}

void AssertPersistedWrites(
    const std::filesystem::path &root,
    const telepath::BufferTag &first_tag,
    const std::array<std::byte, 4096> &first_page,
    const telepath::BufferTag &second_tag,
    const std::array<std::byte, 4096> &second_page) {
  telepath::PosixDiskBackend verifier(root.string(), telepath::BlockGeometry::Of<4096>());
  std::array<std::byte, 4096> verify_first{};
  std::array<std::byte, 4096> verify_second{};

  auto verify_first_request = verifier.SubmitRead(first_tag, verify_first.data(), verify_first.size());
  auto verify_second_request = verifier.SubmitRead(second_tag, verify_second.data(), verify_second.size());
  assert(verify_first_request.has_value());
  assert(verify_second_request.has_value());

  auto verify_first_completion = verifier.PollCompletion();
  auto verify_second_completion = verifier.PollCompletion();
  assert(verify_first_completion.has_value());
  assert(verify_second_completion.has_value());
  assert(verify_first_completion.value().status.has_value());
  assert(verify_second_completion.value().status.has_value());
  AssertPageMarkers(verify_first, first_page[0], first_page[4095]);
  AssertPageMarkers(verify_second, second_page[0], second_page[4095]);
}

}  // namespace

int main() {
  telepath::io_test_support::TestRootGuard root_guard("telepath_io_uring_shutdown_drain_test");
  auto probe_backend_result = telepath::IoUringDiskBackend::Create(root_guard.path().string(), telepath::BlockGeometry::Of<4096>(), 8);
  if (HandleUnavailableIoUringBackend(probe_backend_result)) return 0;
  telepath::IoUringDiskBackend &probe_backend = *probe_backend_result.value();
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
