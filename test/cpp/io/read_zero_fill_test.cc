#include <array>
#include <cassert>
#include <cstddef>
#include <filesystem>

#include "io_test_support.h"
#include "telepath/io/posix_disk_backend.h"

namespace {

void AssertMissingReadReturnsZeroFilledPage(const std::filesystem::path &root) {
  telepath::PosixDiskBackend backend(root.string(), 4096);
  std::array<std::byte, 4096> page{};
  page.fill(std::byte{0xFF});

  auto request = backend.SubmitRead(telepath::BufferTag{99, 7}, page.data(), page.size());
  assert(request.ok());
  auto completion = backend.PollCompletion();
  assert(completion.ok());
  assert(completion.value().status.ok());

  for (const std::byte value : page) {
    assert(value == std::byte{0});
  }
}

}  // namespace

int main() {
  telepath::io_test_support::TestRootGuard root_guard("telepath_read_zero_fill_test_data");
  AssertMissingReadReturnsZeroFilledPage(root_guard.path());
  return 0;
}
