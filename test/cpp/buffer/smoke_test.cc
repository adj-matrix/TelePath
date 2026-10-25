#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "buffer_test_support.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/clock_replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

struct SmokeTestContext {
  std::shared_ptr<telepath::TelemetrySink> telemetry;
  std::unique_ptr<telepath::BufferManager> manager;
};

auto BuildContext(const std::filesystem::path &root) -> SmokeTestContext {
  auto telemetry = telepath::MakeCounterTelemetrySink();
  auto disk_backend = std::make_unique<telepath::PosixDiskBackend>(root.string(), telepath::BlockGeometry::Of<4096>());
  auto replacer = std::make_unique<telepath::ClockReplacer>(2);
  return {
    telemetry,
    telepath::BufferManager::Create(2, 4096, std::move(disk_backend), std::move(replacer), telemetry).value(),
  };
}

void WriteAndFlushFirstPage(telepath::BufferManager *manager) {
  auto first_result = manager->ReadBufferExclusive(1, 0);
  assert(first_result.has_value());

  auto first_handle = std::move(first_result.value());
  auto data = first_handle.mutable_data();
  data[0] = std::byte{0x2A};
  data[1] = std::byte{0x0B};
  assert(manager->FlushBuffer(first_handle).has_value());
}

void ExpectSecondReadHitsCachedBytes(telepath::BufferManager *manager) {
  auto second_result = manager->ReadBuffer(1, 0);
  assert(second_result.has_value());

  auto second_handle = std::move(second_result.value());
  auto read_back = second_handle.data();
  assert(std::to_integer<int>(read_back[0]) == 0x2A);
  assert(std::to_integer<int>(read_back[1]) == 0x0B);
}

void ExpectSmokeTelemetry(const telepath::TelemetrySnapshot &snapshot) {
  assert(snapshot.buffer_misses >= 1);
  assert(snapshot.disk_reads >= 1);
  assert(snapshot.disk_writes >= 1);
  assert(snapshot.dirty_flushes >= 1);
}

}  // namespace

int main() {
  telepath::buffer_test_support::TestRootGuard root_guard("telepath_smoke_data");
  auto context = BuildContext(root_guard.path());
  WriteAndFlushFirstPage(context.manager.get());
  ExpectSecondReadHitsCachedBytes(context.manager.get());
  ExpectSmokeTelemetry(context.telemetry->Snapshot());
  return 0;
}
