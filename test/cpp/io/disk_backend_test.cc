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

}  // namespace

int main() {
  telepath::io_test_support::TestRootGuard root_guard("telepath_disk_backend_data");
  AssertRoundTripReadWrite(root_guard.path());
  AssertInvalidReadSizeRejected(root_guard.path());
  AssertShutdownDrainsPendingWrites(root_guard.path());
  return 0;
}
