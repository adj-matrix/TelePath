#include <array>
#include <cassert>
#include <cstddef>
#include <filesystem>

#include "io_test_support.h"
#include "telepath/io/disk_backend.h"
#include "telepath/io/posix_disk_backend.h"

namespace {

auto BuildMarkedPage(std::byte first, std::byte last) -> std::array<std::byte, 4096> {
  std::array<std::byte, 4096> page{};
  page[0] = first;
  page[4095] = last;
  return page;
}

void AssertRoundTripReadWrite(const std::filesystem::path &root) {
  telepath::PosixDiskBackend backend(root.string(), 4096);
  auto write_buffer = BuildMarkedPage(std::byte{0x33}, std::byte{0x7A});
  std::array<std::byte, 4096> read_buffer{};
  const telepath::BufferTag tag{5, 9};

  auto write_submit = backend.SubmitWrite(tag, write_buffer.data(), write_buffer.size());
  assert(write_submit.ok());
  auto write_completion = backend.PollCompletion();
  assert(write_completion.ok());
  assert(write_completion.value().request_id == write_submit.value());
  assert(write_completion.value().status.ok());
  assert(write_completion.value().operation == telepath::DiskOperation::kWrite);

  auto read_submit = backend.SubmitRead(tag, read_buffer.data(), read_buffer.size());
  assert(read_submit.ok());
  auto read_completion = backend.PollCompletion();
  assert(read_completion.ok());
  assert(read_completion.value().request_id == read_submit.value());
  assert(read_completion.value().status.ok());
  assert(read_completion.value().operation == telepath::DiskOperation::kRead);

  assert(read_buffer[0] == std::byte{0x33});
  assert(read_buffer[4095] == std::byte{0x7A});
}

void AssertInvalidReadSizeRejected(const std::filesystem::path &root) {
  telepath::PosixDiskBackend backend(root.string(), 4096);
  std::array<std::byte, 4096> read_buffer{};
  const telepath::BufferTag tag{5, 9};

  auto invalid_read = backend.SubmitRead(tag, read_buffer.data(), read_buffer.size() - 1);
  assert(!invalid_read.ok());
  assert(invalid_read.status().code() == telepath::StatusCode::kInvalidArgument);
}

void AssertShutdownDrainsPendingWrites(const std::filesystem::path &root) {
  telepath::PosixDiskBackend shutdown_backend(root.string(), 4096);
  auto write_buffer = BuildMarkedPage(std::byte{0x33}, std::byte{0x7A});

  auto first_shutdown_submit = shutdown_backend.SubmitWrite(telepath::BufferTag{7, 1}, write_buffer.data(), write_buffer.size());
  auto second_shutdown_submit = shutdown_backend.SubmitWrite(telepath::BufferTag{7, 2}, write_buffer.data(), write_buffer.size());
  assert(first_shutdown_submit.ok());
  assert(second_shutdown_submit.ok());

  shutdown_backend.Shutdown();

  auto first_shutdown_completion = shutdown_backend.PollCompletion();
  assert(first_shutdown_completion.ok());
  assert(first_shutdown_completion.value().request_id == first_shutdown_submit.value());
  assert(first_shutdown_completion.value().status.ok());

  auto second_shutdown_completion = shutdown_backend.PollCompletion();
  assert(second_shutdown_completion.ok());
  assert(second_shutdown_completion.value().request_id == second_shutdown_submit.value());
  assert(second_shutdown_completion.value().status.ok());

  auto drained_completion = shutdown_backend.PollCompletion();
  assert(!drained_completion.ok());
  assert(drained_completion.status().code() == telepath::StatusCode::kUnavailable);
}

void AssertRepeatedAccessReusesAndClosesFileDescriptor(const std::filesystem::path &root) {
#if defined(__linux__)
  const telepath::FileId file_id = 9;
  const std::filesystem::path file_path = root / "file_9.tp";
  auto write_buffer = BuildMarkedPage(std::byte{0x44}, std::byte{0x55});
  std::array<std::byte, 4096> read_buffer{};

  assert(telepath::io_test_support::CountOpenDescriptorsForPath(file_path) == 0);
  {
    telepath::PosixDiskBackend backend(root.string(), 4096);
    for (telepath::BlockId block_id = 0; block_id < 4; ++block_id) {
      const telepath::BufferTag tag{file_id, block_id};
      auto write_submit = backend.SubmitWrite(tag, write_buffer.data(), write_buffer.size());
      assert(write_submit.ok());
      auto write_completion = backend.PollCompletion();
      assert(write_completion.ok());
      assert(write_completion.value().status.ok());

      auto read_submit = backend.SubmitRead(tag, read_buffer.data(), read_buffer.size());
      assert(read_submit.ok());
      auto read_completion = backend.PollCompletion();
      assert(read_completion.ok());
      assert(read_completion.value().status.ok());
    }

    assert(telepath::io_test_support::CountOpenDescriptorsForPath(file_path) == 1);
  }
  assert(telepath::io_test_support::CountOpenDescriptorsForPath(file_path) == 0);
#else
  (void)root;
#endif
}

void AssertFileDescriptorCacheEvictsLeastRecentlyUsedFile(const std::filesystem::path &root) {
#if defined(__linux__)
  const telepath::FileId first_file_id = 10;
  const telepath::FileId second_file_id = 11;
  const std::filesystem::path first_file_path = root / "file_10.tp";
  const std::filesystem::path second_file_path = root / "file_11.tp";
  auto write_buffer = BuildMarkedPage(std::byte{0x66}, std::byte{0x77});

  {
    telepath::PosixDiskBackend backend(root.string(), 4096, false, 1);
    auto first_submit = backend.SubmitWrite({first_file_id, 0}, write_buffer.data(), write_buffer.size());
    assert(first_submit.ok());
    auto first_completion = backend.PollCompletion();
    assert(first_completion.ok());
    assert(first_completion.value().status.ok());
    assert(telepath::io_test_support::CountOpenDescriptorsForPath(first_file_path) == 1);

    auto second_submit = backend.SubmitWrite({second_file_id, 0}, write_buffer.data(), write_buffer.size());
    assert(second_submit.ok());
    auto second_completion = backend.PollCompletion();
    assert(second_completion.ok());
    assert(second_completion.value().status.ok());
    assert(telepath::io_test_support::CountOpenDescriptorsForPath(first_file_path) == 0);
    assert(telepath::io_test_support::CountOpenDescriptorsForPath(second_file_path) == 1);

    auto first_reopen = backend.SubmitWrite({first_file_id, 1}, write_buffer.data(), write_buffer.size());
    assert(first_reopen.ok());
    auto first_reopen_completion = backend.PollCompletion();
    assert(first_reopen_completion.ok());
    assert(first_reopen_completion.value().status.ok());
    assert(telepath::io_test_support::CountOpenDescriptorsForPath(first_file_path) == 1);
    assert(telepath::io_test_support::CountOpenDescriptorsForPath(second_file_path) == 0);
  }
  assert(telepath::io_test_support::CountOpenDescriptorsForPath(first_file_path) == 0);
  assert(telepath::io_test_support::CountOpenDescriptorsForPath(second_file_path) == 0);
#else
  (void)root;
#endif
}

}  // namespace

int main() {
  telepath::io_test_support::TestRootGuard root_guard("telepath_disk_backend_data");
  AssertRoundTripReadWrite(root_guard.path());
  AssertInvalidReadSizeRejected(root_guard.path());
  AssertShutdownDrainsPendingWrites(root_guard.path());
  AssertRepeatedAccessReusesAndClosesFileDescriptor(root_guard.path());
  AssertFileDescriptorCacheEvictsLeastRecentlyUsedFile(root_guard.path());
  return 0;
}
