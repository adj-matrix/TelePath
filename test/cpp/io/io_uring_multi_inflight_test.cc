#include <array>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "telepath/io/io_uring_disk_backend.h"
#include "telepath/io/posix_disk_backend.h"

namespace {

bool RequireIoUringSuccess() {
  const char *value = std::getenv("TELEPATH_REQUIRE_IO_URING_SUCCESS");
  return value != nullptr && std::string(value) == "1";
}

}  // namespace

int main() {
  namespace fs = std::filesystem;

  const fs::path root =
      fs::temp_directory_path() / "telepath_io_uring_multi_inflight_test";
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

  const telepath::BufferTag tag_a{19, 0};
  const telepath::BufferTag tag_b{19, 1};

  std::array<std::byte, 4096> page_a{};
  std::array<std::byte, 4096> page_b{};
  page_a[0] = std::byte{0xA1};
  page_a[4095] = std::byte{0xA2};
  page_b[0] = std::byte{0xB1};
  page_b[4095] = std::byte{0xB2};

  auto write_a = backend.SubmitWrite(tag_a, page_a.data(), page_a.size());
  auto write_b = backend.SubmitWrite(tag_b, page_b.data(), page_b.size());
  assert(write_a.ok());
  assert(write_b.ok());
  assert(write_a.value() != write_b.value());

  std::unordered_set<uint64_t> pending_writes{write_a.value(), write_b.value()};
  for (int i = 0; i < 2; ++i) {
    auto completion = backend.PollCompletion();
    assert(completion.ok());
    assert(completion.value().status.ok());
    assert(completion.value().operation == telepath::DiskOperation::kWrite);
    assert(pending_writes.erase(completion.value().request_id) == 1);
  }
  assert(pending_writes.empty());

  std::array<std::byte, 4096> read_a{};
  std::array<std::byte, 4096> read_b{};
  auto request_read_a = backend.SubmitRead(tag_a, read_a.data(), read_a.size());
  auto request_read_b = backend.SubmitRead(tag_b, read_b.data(), read_b.size());
  assert(request_read_a.ok());
  assert(request_read_b.ok());

  std::unordered_map<uint64_t, telepath::BufferTag> pending_reads{
      {request_read_a.value(), tag_a},
      {request_read_b.value(), tag_b},
  };
  for (int i = 0; i < 2; ++i) {
    auto completion = backend.PollCompletion();
    assert(completion.ok());
    assert(completion.value().status.ok());
    assert(completion.value().operation == telepath::DiskOperation::kRead);
    auto it = pending_reads.find(completion.value().request_id);
    assert(it != pending_reads.end());
    assert(completion.value().tag == it->second);
    pending_reads.erase(it);
  }
  assert(pending_reads.empty());

  assert(read_a[0] == std::byte{0xA1});
  assert(read_a[4095] == std::byte{0xA2});
  assert(read_b[0] == std::byte{0xB1});
  assert(read_b[4095] == std::byte{0xB2});

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
  assert(verify_a[0] == std::byte{0xA1});
  assert(verify_a[4095] == std::byte{0xA2});
  assert(verify_b[0] == std::byte{0xB1});
  assert(verify_b[4095] == std::byte{0xB2});

  backend.Shutdown();
  fs::remove_all(root);
  return 0;
}
