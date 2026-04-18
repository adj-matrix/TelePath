#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "buffer_test_support.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

auto BuildManager(const std::filesystem::path &root) -> telepath::BufferManager {
  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto disk_backend = std::make_unique<telepath::PosixDiskBackend>(root.string(), 4096);
  auto replacer = telepath::MakeClockReplacer(4);
  return telepath::BufferManager(4, 4096, std::move(disk_backend), std::move(replacer), telemetry);
}

auto ComputeFrameDistance(
  const telepath::BufferHandle &first_handle,
  const telepath::BufferHandle &second_handle
) -> std::uintptr_t {
  const auto first_addr = reinterpret_cast<std::uintptr_t>(first_handle.data());
  const auto second_addr = reinterpret_cast<std::uintptr_t>(second_handle.data());
  if (first_addr > second_addr) return first_addr - second_addr;
  return second_addr - first_addr;
}

void ExpectFramesArePageSizedApart(telepath::BufferManager *manager) {
  auto first_result = manager->ReadBuffer(1, 0);
  auto second_result = manager->ReadBuffer(1, 1);
  assert(first_result.ok());
  assert(second_result.ok());

  auto first_handle = std::move(first_result.value());
  auto second_handle = std::move(second_result.value());
  assert(ComputeFrameDistance(first_handle, second_handle) == 4096);
}

}  // namespace

int main() {
  telepath::buffer_test_support::TestRootGuard root_guard("telepath_memory_layout_data");
  auto manager = BuildManager(root_guard.path());
  ExpectFramesArePageSizedApart(&manager);
  return 0;
}
