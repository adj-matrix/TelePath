#include <array>
#include <cassert>
#include <cstddef>
#include <filesystem>

#include "telepath/io/disk_backend.h"
#include "telepath/io/posix_disk_backend.h"

int main() {
  namespace fs = std::filesystem;

  const fs::path root = fs::temp_directory_path() / "telepath_disk_backend_data";
  fs::remove_all(root);
  fs::create_directories(root);

  telepath::PosixDiskBackend backend(root.string(), 4096);
  std::array<std::byte, 4096> write_buffer{};
  std::array<std::byte, 4096> read_buffer{};
  write_buffer[0] = std::byte{0x33};
  write_buffer[4095] = std::byte{0x7A};

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

  auto invalid_read = backend.SubmitRead(tag, read_buffer.data(), read_buffer.size() - 1);
  assert(!invalid_read.ok());
  assert(invalid_read.status().code() == telepath::StatusCode::kInvalidArgument);

  telepath::PosixDiskBackend shutdown_backend(root.string(), 4096);
  auto first_shutdown_submit =
      shutdown_backend.SubmitWrite(telepath::BufferTag{7, 1}, write_buffer.data(),
                                   write_buffer.size());
  auto second_shutdown_submit =
      shutdown_backend.SubmitWrite(telepath::BufferTag{7, 2}, write_buffer.data(),
                                   write_buffer.size());
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

  fs::remove_all(root);
  return 0;
}
