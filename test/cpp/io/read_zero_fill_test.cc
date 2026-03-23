#include <array>
#include <cassert>
#include <cstddef>
#include <filesystem>

#include "telepath/io/posix_disk_backend.h"

int main() {
  namespace fs = std::filesystem;

  const fs::path root =
      fs::temp_directory_path() / "telepath_read_zero_fill_test_data";
  fs::remove_all(root);
  fs::create_directories(root);

  telepath::PosixDiskBackend backend(root.string(), 4096);
  std::array<std::byte, 4096> page{};
  page.fill(std::byte{0xFF});

  auto request =
      backend.SubmitRead(telepath::BufferTag{99, 7}, page.data(), page.size());
  assert(request.ok());
  auto completion = backend.PollCompletion();
  assert(completion.ok());
  assert(completion.value().status.ok());

  for (const std::byte value : page) {
    assert(value == std::byte{0});
  }

  fs::remove_all(root);
  return 0;
}
