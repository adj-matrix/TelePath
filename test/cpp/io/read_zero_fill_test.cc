#include <array>
#include <cassert>
#include <cstddef>
#include <filesystem>

#include "disk_io_helpers.h"
#include "io_test_support.h"
#include "telepath/io/posix_disk_backend.h"

namespace {

void AssertMissingReadReturnsZeroFilledPage(const std::filesystem::path &root) {
  telepath::PosixDiskBackend backend(root.string(), telepath::BlockGeometry::Of<4096>());
  std::array<std::byte, 4096> page{};
  page.fill(std::byte{0xFF});

  const uint64_t request = telepath::test_support::NextToken();
  assert(telepath::test_support::SubmitOne(backend, telepath::test_support::MakeBlockRequest(telepath::DiskOperation::kRead, telepath::BufferTag{99, 7}, const_cast<std::byte *>(page.data()), request)).has_value());
  auto completion = telepath::test_support::ReapOne(backend);
  assert(completion.has_value());
  assert(completion.value().status.has_value());

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
