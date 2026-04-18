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
  telepath::BufferManager manager;
};

auto BuildContext(const std::filesystem::path &root) -> SmokeTestContext {
  auto telemetry = telepath::MakeCounterTelemetrySink();
  auto disk_backend = std::make_unique<telepath::PosixDiskBackend>(root.string(), 4096);
  auto replacer = std::make_unique<telepath::ClockReplacer>(2);
  return {
    telemetry,
    telepath::BufferManager(2, 4096, std::move(disk_backend), std::move(replacer), telemetry),
  };
}

void WriteAndFlushFirstPage(telepath::BufferManager *manager) {
  auto first_result = manager->ReadBuffer(1, 0);
  assert(first_result.ok());

  auto first_handle = std::move(first_result.value());
  std::byte *data = first_handle.mutable_data();
  data[0] = std::byte{0x2A};
  data[1] = std::byte{0x0B};
  assert(manager->MarkBufferDirty(first_handle).ok());
  assert(manager->FlushBuffer(first_handle).ok());
}

void ExpectSecondReadHitsCachedBytes(telepath::BufferManager *manager) {
  auto second_result = manager->ReadBuffer(1, 0);
  assert(second_result.ok());

  auto second_handle = std::move(second_result.value());
  const std::byte *read_back = second_handle.data();
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
  WriteAndFlushFirstPage(&context.manager);
  ExpectSecondReadHitsCachedBytes(&context.manager);
  ExpectSmokeTelemetry(context.telemetry->Snapshot());
  return 0;
}
