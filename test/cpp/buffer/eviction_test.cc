#include <cassert>
#include <memory>

#include "buffer_test_support.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

struct EvictionTestContext {
  std::shared_ptr<telepath::TelemetrySink> telemetry;
  std::unique_ptr<telepath::BufferManager> manager;
};

auto BuildContext(const std::filesystem::path &root) -> EvictionTestContext {
  auto telemetry = telepath::MakeCounterTelemetrySink();
  auto disk_backend = std::make_unique<telepath::PosixDiskBackend>(root.string(), telepath::BlockGeometry::Of<4096>());
  auto replacer = telepath::MakeLruKReplacer(2, 2);
  return {
    telemetry,
    telepath::BufferManager::Create(2, 4096, std::move(disk_backend), std::move(replacer), telemetry).value(),
  };
}

void WriteDirtyPage(
  telepath::BufferManager *manager,
  telepath::BlockId block_id,
  std::byte first_byte
) {
  auto result = manager->ReadBufferExclusive(1, block_id);
  assert(result.has_value());

  auto handle = std::move(result.value());
  handle.mutable_data()[0] = first_byte;
}

void FillPoolAndTriggerEviction(telepath::BufferManager *manager) {
  WriteDirtyPage(manager, 0, std::byte{0x11});
  WriteDirtyPage(manager, 1, std::byte{0x22});
  WriteDirtyPage(manager, 2, std::byte{0x33});
}

void ExpectEvictedPageCanBeReadBack(telepath::BufferManager *manager) {
  auto result = manager->ReadBuffer(1, 0);
  assert(result.has_value());

  auto handle = std::move(result.value());
  assert(handle.data()[0] == std::byte{0x11});
}

void ExpectEvictionTelemetry(const telepath::TelemetrySnapshot &snapshot) {
  assert(snapshot.evictions >= 1);
  assert(snapshot.dirty_flushes >= 1);
}

}  // namespace

int main() {
  telepath::buffer_test_support::TestRootGuard root_guard("telepath_eviction_test_data");
  auto context = BuildContext(root_guard.path());

  FillPoolAndTriggerEviction(context.manager.get());
  ExpectEvictedPageCanBeReadBack(context.manager.get());
  ExpectEvictionTelemetry(context.telemetry->Snapshot());
  return 0;
}
