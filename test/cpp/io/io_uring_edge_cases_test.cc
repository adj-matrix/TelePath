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

void AssertMissingReadZeroFillsPage(telepath::IoUringDiskBackend *backend, const telepath::BufferTag &tag) {
  std::array<std::byte, 4096> page{};
  page.fill(std::byte{0xFF});

  auto request = backend->SubmitRead(tag, page.data(), page.size());
  assert(request.has_value());

  auto completion = backend->PollCompletion();
  assert(completion.has_value());
  assert(completion.value().request_id == request.value());
  assert(completion.value().operation == telepath::DiskOperation::kRead);
  assert(completion.value().tag == tag);
  assert(completion.value().status.has_value());

  for (const std::byte value : page) assert(value == std::byte{0});
}

void AssertInvalidArgumentsRejected(
    telepath::IoUringDiskBackend *backend,
    const telepath::BufferTag &tag,
    const std::array<std::byte, 4096> &page) {
  auto null_read = backend->SubmitRead(tag, nullptr, page.size());
  assert(!null_read.has_value());
  assert(null_read.error().code() == telepath::ErrorCode::kInvalidArgument);

  auto wrong_size_write = backend->SubmitWrite(tag, page.data(), 1024);
  assert(!wrong_size_write.has_value());
  assert(wrong_size_write.error().code() == telepath::ErrorCode::kInvalidArgument);
}

void AssertWriteCompletes(
    telepath::IoUringDiskBackend *backend,
    const telepath::BufferTag &tag,
    const std::array<std::byte, 4096> &page) {
  auto request = backend->SubmitWrite(tag, page.data(), page.size());
  assert(request.has_value());

  auto completion = backend->PollCompletion();
  assert(completion.has_value());
  assert(completion.value().request_id == request.value());
  assert(completion.value().operation == telepath::DiskOperation::kWrite);
  assert(completion.value().tag == tag);
  assert(completion.value().status.has_value());
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
  telepath::io_test_support::TestRootGuard root_guard("telepath_io_uring_edge_cases_test");
  auto backend_result = telepath::IoUringDiskBackend::Create(root_guard.path().string(), telepath::BlockGeometry::Of<4096>(), 1);
  if (HandleUnavailableIoUringBackend(backend_result)) return 0;
  telepath::IoUringDiskBackend &backend = *backend_result.value();

  const telepath::BufferTag zero_fill_tag{23, 7};
  const telepath::BufferTag first_write_tag{23, 0};
  const telepath::BufferTag second_write_tag{23, 1};
  auto first_page = BuildMarkedPage(std::byte{0x11}, std::byte{0x22});
  auto second_page = BuildMarkedPage(std::byte{0x33}, std::byte{0x44});
  AssertMissingReadZeroFillsPage(&backend, zero_fill_tag);
  AssertWriteCompletes(&backend, first_write_tag, first_page);
  AssertInvalidArgumentsRejected(&backend, second_write_tag, second_page);
  AssertWriteCompletes(&backend, second_write_tag, second_page);
  AssertPersistedWrites(root_guard.path(), first_write_tag, first_page, second_write_tag, second_page);
  backend.Shutdown();
  return 0;
}
