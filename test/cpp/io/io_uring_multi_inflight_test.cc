#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

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

void AssertConcurrentWrites(
    telepath::IoUringDiskBackend *backend,
    const telepath::BufferTag &tag_a,
    const std::array<std::byte, 4096> &page_a,
    const telepath::BufferTag &tag_b,
    const std::array<std::byte, 4096> &page_b) {
  const uint64_t write_a = telepath::test_support::NextToken();
  assert(telepath::test_support::SubmitOne(*backend, telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kWrite, tag_a, const_cast<std::byte *>(page_a.data()), write_a)).has_value());
  const uint64_t write_b = telepath::test_support::NextToken();
  assert(telepath::test_support::SubmitOne(*backend, telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kWrite, tag_b, const_cast<std::byte *>(page_b.data()), write_b)).has_value());
  assert(write_a != write_b);

  std::unordered_set<uint64_t> pending_writes{write_a, write_b};
  for (int i = 0; i < 2; ++i) {
    auto completion = telepath::test_support::ReapOne(*backend);
    assert(completion.has_value());
    assert(completion.value().status.has_value());
    assert(pending_writes.erase(completion.value().token) == 1);
  }

  assert(pending_writes.empty());
}

void AssertConcurrentReads(
    telepath::IoUringDiskBackend *backend,
    const telepath::BufferTag &tag_a,
    std::array<std::byte, 4096> *read_a,
    const telepath::BufferTag &tag_b,
    std::array<std::byte, 4096> *read_b) {
  // Both reads go to the kernel in one Submit() call.
  const uint64_t request_a = telepath::test_support::NextToken();
  const uint64_t request_b = telepath::test_support::NextToken();
  const std::array<telepath::DiskRequest, 2> requests{
    telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kRead, tag_a, read_a->data(), request_a),
    telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kRead, tag_b, read_b->data(), request_b),
  };
  auto accepted = backend->Submit(requests, telepath::DiskSubmitMode::kAsync);
  assert(accepted.has_value() && accepted.value() == 2);

  std::unordered_map<uint64_t, telepath::BufferTag> pending_reads{
    {request_a, tag_a},
    {request_b, tag_b},
  };
  for (int i = 0; i < 2; ++i) {
    auto completion = telepath::test_support::ReapOne(*backend);
    assert(completion.has_value());
    assert(completion.value().status.has_value());

    auto it = pending_reads.find(completion.value().token);
    assert(it != pending_reads.end());
    pending_reads.erase(it);
  }

  assert(pending_reads.empty());
}

void AssertMultiBlockRead(
    telepath::IoUringDiskBackend *backend,
    const telepath::BufferTag &first_tag) {
  std::array<std::byte, 4096> first{};
  std::array<std::byte, 4096> second{};
  std::array<std::byte, 4096> past_end{};
  past_end.fill(std::byte{0xFF});
  auto request = telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kRead, first_tag, first.data(), 99);
  request.block_count = 3;
  request.buffers[1] = second.data();
  request.buffers[2] = past_end.data();
  assert(telepath::test_support::SubmitOne(*backend, request).has_value());
  auto completion = telepath::test_support::ReapOne(*backend);
  assert(completion.has_value());
  assert(completion.value().token == 99);
  assert(completion.value().status.has_value());
  AssertPageMarkers(first, std::byte{0xA1}, std::byte{0xA2});
  AssertPageMarkers(second, std::byte{0xB1}, std::byte{0xB2});
  for (const std::byte value : past_end) assert(value == std::byte{0});
}

void AssertReadPagesMatch(const std::array<std::byte, 4096> &read_a, const std::array<std::byte, 4096> &read_b) {
  AssertPageMarkers(read_a, std::byte{0xA1}, std::byte{0xA2});
  AssertPageMarkers(read_b, std::byte{0xB1}, std::byte{0xB2});
}

void AssertPersistedPagesMatch(
    const std::filesystem::path &root,
    const telepath::BufferTag &tag_a,
    const telepath::BufferTag &tag_b) {
  telepath::PosixDiskBackend verifier(root.string(), telepath::BlockGeometry::Of<4096>());
  std::array<std::byte, 4096> verify_a{};
  std::array<std::byte, 4096> verify_b{};

  const uint64_t verify_request_a = telepath::test_support::NextToken();
  assert(telepath::test_support::SubmitOne(verifier, telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kRead, tag_a, const_cast<std::byte *>(verify_a.data()), verify_request_a)).has_value());
  const uint64_t verify_request_b = telepath::test_support::NextToken();
  assert(telepath::test_support::SubmitOne(verifier, telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kRead, tag_b, const_cast<std::byte *>(verify_b.data()), verify_request_b)).has_value());

  auto verify_completion_a = telepath::test_support::ReapOne(verifier);
  auto verify_completion_b = telepath::test_support::ReapOne(verifier);
  assert(verify_completion_a.has_value());
  assert(verify_completion_b.has_value());
  assert(verify_completion_a.value().status.has_value());
  assert(verify_completion_b.value().status.has_value());
  AssertReadPagesMatch(verify_a, verify_b);
}

}  // namespace

int main() {
  telepath::io_test_support::TestRootGuard root_guard("telepath_io_uring_multi_inflight_test");
  auto backend_result = telepath::IoUringDiskBackend::Create(root_guard.path().string(), telepath::BlockGeometry::Of<4096>(), 8);
  if (HandleUnavailableIoUringBackend(backend_result)) return 0;
  telepath::IoUringDiskBackend &backend = *backend_result.value();

  const telepath::BufferTag tag_a{19, 0};
  const telepath::BufferTag tag_b{19, 1};
  auto page_a = BuildMarkedPage(std::byte{0xA1}, std::byte{0xA2});
  auto page_b = BuildMarkedPage(std::byte{0xB1}, std::byte{0xB2});
  std::array<std::byte, 4096> read_a{};
  std::array<std::byte, 4096> read_b{};
  AssertConcurrentWrites(&backend, tag_a, page_a, tag_b, page_b);
  AssertConcurrentReads(&backend, tag_a, &read_a, tag_b, &read_b);
  AssertReadPagesMatch(read_a, read_b);
  AssertMultiBlockRead(&backend, tag_a);
  AssertPersistedPagesMatch(root_guard.path(), tag_a, tag_b);
  backend.Shutdown();
  return 0;
}
