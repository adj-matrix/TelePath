#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

int main() {
  namespace fs = std::filesystem;

  const fs::path root = fs::temp_directory_path() / "telepath_memory_layout_data";
  fs::remove_all(root);
  fs::create_directories(root);

  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto disk_backend =
      std::make_unique<telepath::PosixDiskBackend>(root.string(), 4096);
  auto replacer = telepath::MakeClockReplacer(4);
  telepath::BufferManager manager(4, 4096, std::move(disk_backend),
                                  std::move(replacer), telemetry);

  auto first_result = manager.ReadBuffer(1, 0);
  auto second_result = manager.ReadBuffer(1, 1);
  assert(first_result.ok());
  assert(second_result.ok());

  telepath::BufferHandle first_handle = std::move(first_result.value());
  telepath::BufferHandle second_handle = std::move(second_result.value());

  const auto first_addr =
      reinterpret_cast<std::uintptr_t>(first_handle.mutable_data());
  const auto second_addr =
      reinterpret_cast<std::uintptr_t>(second_handle.mutable_data());
  const std::uintptr_t diff =
      first_addr > second_addr ? first_addr - second_addr : second_addr - first_addr;

  assert(diff == 4096);

  first_handle.Reset();
  second_handle.Reset();
  fs::remove_all(root);
  return 0;
}
