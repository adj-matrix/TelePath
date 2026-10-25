#include <array>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <filesystem>

#include "disk_io_helpers.h"
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

  const uint64_t request = telepath::test_support::NextToken();
  assert(telepath::test_support::SubmitOne(*backend, telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kRead, tag, const_cast<std::byte *>(page.data()), request)).has_value());

  auto completion = telepath::test_support::ReapOne(*backend);
  assert(completion.has_value());
  assert(completion.value().token == request);
  assert(completion.value().status.has_value());

  for (const std::byte value : page) assert(value == std::byte{0});
}

void AssertInvalidArgumentsRejected(
    telepath::IoUringDiskBackend *backend,
    const telepath::BufferTag &tag,
    const std::array<std::byte, 4096> &page) {
  auto null_read = telepath::test_support::SubmitOne(*backend, telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kRead, tag, nullptr));
  assert(!null_read.has_value());
  assert(null_read.error().code() == telepath::ErrorCode::kInvalidArgument);

  auto oversized_write = telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kWrite, tag, const_cast<std::byte *>(page.data()));
  oversized_write.block_count = telepath::kMaxDiskRequestBlocks + 1;
  auto rejected = telepath::test_support::SubmitOne(*backend, oversized_write);
  assert(!rejected.has_value());
  assert(rejected.error().code() == telepath::ErrorCode::kInvalidArgument);
}

void AssertWriteCompletes(
    telepath::IoUringDiskBackend *backend,
    const telepath::BufferTag &tag,
    const std::array<std::byte, 4096> &page) {
  const uint64_t request = telepath::test_support::NextToken();
  assert(telepath::test_support::SubmitOne(*backend, telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kWrite, tag, const_cast<std::byte *>(page.data()), request)).has_value());

  auto completion = telepath::test_support::ReapOne(*backend);
  assert(completion.has_value());
  assert(completion.value().token == request);
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

  const uint64_t verify_first_request = telepath::test_support::NextToken();
  assert(telepath::test_support::SubmitOne(verifier, telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kRead, first_tag, const_cast<std::byte *>(verify_first.data()), verify_first_request)).has_value());
  const uint64_t verify_second_request = telepath::test_support::NextToken();
  assert(telepath::test_support::SubmitOne(verifier, telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kRead, second_tag, const_cast<std::byte *>(verify_second.data()), verify_second_request)).has_value());

  auto verify_first_completion = telepath::test_support::ReapOne(verifier);
  auto verify_second_completion = telepath::test_support::ReapOne(verifier);
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
