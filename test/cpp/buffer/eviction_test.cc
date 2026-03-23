#include <cassert>
#include <cstddef>
#include <filesystem>
#include <memory>

#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

int main() {
  namespace fs = std::filesystem;

  const fs::path root = fs::temp_directory_path() / "telepath_eviction_test_data";
  fs::remove_all(root);
  fs::create_directories(root);

  auto telemetry = telepath::MakeCounterTelemetrySink();
  auto disk_backend =
      std::make_unique<telepath::PosixDiskBackend>(root.string(), 4096);
  auto replacer = telepath::MakeLruKReplacer(2, 2);
  telepath::BufferManager manager(2, 4096, std::move(disk_backend),
                                  std::move(replacer), telemetry);

  {
    auto result = manager.ReadBuffer(1, 0);
    assert(result.ok());
    telepath::BufferHandle handle = std::move(result.value());
    handle.mutable_data()[0] = std::byte{0x11};
    assert(manager.MarkBufferDirty(handle).ok());
  }

  {
    auto result = manager.ReadBuffer(1, 1);
    assert(result.ok());
    telepath::BufferHandle handle = std::move(result.value());
    handle.mutable_data()[0] = std::byte{0x22};
    assert(manager.MarkBufferDirty(handle).ok());
  }

  {
    auto result = manager.ReadBuffer(1, 2);
    assert(result.ok());
    telepath::BufferHandle handle = std::move(result.value());
    handle.mutable_data()[0] = std::byte{0x33};
    assert(manager.MarkBufferDirty(handle).ok());
  }

  {
    auto result = manager.ReadBuffer(1, 0);
    assert(result.ok());
    telepath::BufferHandle handle = std::move(result.value());
    assert(handle.data()[0] == std::byte{0x11});
  }

  const telepath::TelemetrySnapshot snapshot = telemetry->Snapshot();
  assert(snapshot.evictions >= 1);
  assert(snapshot.dirty_flushes >= 1);

  fs::remove_all(root);
  return 0;
}
