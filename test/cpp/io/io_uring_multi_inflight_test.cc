#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

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

void AssertConcurrentWrites(
    telepath::IoUringDiskBackend *backend,
    const telepath::BufferTag &tag_a,
    const std::array<std::byte, 4096> &page_a,
    const telepath::BufferTag &tag_b,
    const std::array<std::byte, 4096> &page_b) {
  auto write_a = backend->SubmitWrite(tag_a, page_a.data(), page_a.size());
  auto write_b = backend->SubmitWrite(tag_b, page_b.data(), page_b.size());
  assert(write_a.ok());
  assert(write_b.ok());
  assert(write_a.value() != write_b.value());

  std::unordered_set<uint64_t> pending_writes{write_a.value(), write_b.value()};
  for (int i = 0; i < 2; ++i) {
    auto completion = backend->PollCompletion();
    assert(completion.ok());
    assert(completion.value().status.ok());
    assert(completion.value().operation == telepath::DiskOperation::kWrite);
    assert(pending_writes.erase(completion.value().request_id) == 1);
  }

  assert(pending_writes.empty());
}

void AssertConcurrentReads(
    telepath::IoUringDiskBackend *backend,
    const telepath::BufferTag &tag_a,
    std::array<std::byte, 4096> *read_a,
    const telepath::BufferTag &tag_b,
    std::array<std::byte, 4096> *read_b) {
  auto request_a = backend->SubmitRead(tag_a, read_a->data(), read_a->size());
  auto request_b = backend->SubmitRead(tag_b, read_b->data(), read_b->size());
  assert(request_a.ok());
  assert(request_b.ok());

  std::unordered_map<uint64_t, telepath::BufferTag> pending_reads{
    {request_a.value(), tag_a},
    {request_b.value(), tag_b},
  };
  for (int i = 0; i < 2; ++i) {
    auto completion = backend->PollCompletion();
    assert(completion.ok());
    assert(completion.value().status.ok());
    assert(completion.value().operation == telepath::DiskOperation::kRead);

    auto it = pending_reads.find(completion.value().request_id);
    assert(it != pending_reads.end());
    assert(completion.value().tag == it->second);
    pending_reads.erase(it);
  }

  assert(pending_reads.empty());
}

void AssertReadPagesMatch(const std::array<std::byte, 4096> &read_a, const std::array<std::byte, 4096> &read_b) {
  AssertPageMarkers(read_a, std::byte{0xA1}, std::byte{0xA2});
  AssertPageMarkers(read_b, std::byte{0xB1}, std::byte{0xB2});
}

void AssertPersistedPagesMatch(
    const std::filesystem::path &root,
    const telepath::BufferTag &tag_a,
    const telepath::BufferTag &tag_b) {
  telepath::PosixDiskBackend verifier(root.string(), 4096);
  std::array<std::byte, 4096> verify_a{};
  std::array<std::byte, 4096> verify_b{};

  auto verify_request_a = verifier.SubmitRead(tag_a, verify_a.data(), verify_a.size());
  auto verify_request_b = verifier.SubmitRead(tag_b, verify_b.data(), verify_b.size());
  assert(verify_request_a.ok());
  assert(verify_request_b.ok());

  auto verify_completion_a = verifier.PollCompletion();
  auto verify_completion_b = verifier.PollCompletion();
  assert(verify_completion_a.ok());
  assert(verify_completion_b.ok());
  assert(verify_completion_a.value().status.ok());
  assert(verify_completion_b.value().status.ok());
  AssertReadPagesMatch(verify_a, verify_b);
}

}  // namespace

int main() {
  telepath::io_test_support::TestRootGuard root_guard("telepath_io_uring_multi_inflight_test");
  telepath::IoUringDiskBackend backend(root_guard.path().string(), 4096, 8);
  if (HandleUnavailableIoUringBackend(backend)) return 0;

  const telepath::BufferTag tag_a{19, 0};
  const telepath::BufferTag tag_b{19, 1};
  auto page_a = BuildMarkedPage(std::byte{0xA1}, std::byte{0xA2});
  auto page_b = BuildMarkedPage(std::byte{0xB1}, std::byte{0xB2});
  std::array<std::byte, 4096> read_a{};
  std::array<std::byte, 4096> read_b{};
  AssertConcurrentWrites(&backend, tag_a, page_a, tag_b, page_b);
  AssertConcurrentReads(&backend, tag_a, &read_a, tag_b, &read_b);
  AssertReadPagesMatch(read_a, read_b);
  AssertPersistedPagesMatch(root_guard.path(), tag_a, tag_b);
  backend.Shutdown();
  return 0;
}
