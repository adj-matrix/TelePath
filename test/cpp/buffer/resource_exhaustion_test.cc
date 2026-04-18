#include <cassert>
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
  auto replacer = telepath::MakeClockReplacer(1);
  return telepath::BufferManager(1, 4096, std::move(disk_backend), std::move(replacer), telemetry);
}

void ExpectReadFailsWhileOnlyFrameIsPinned(telepath::BufferManager *manager) {
  auto first = manager->ReadBuffer(5, 0);
  assert(first.ok());
  auto pinned = std::move(first.value());

  auto second = manager->ReadBuffer(5, 1);
  assert(!second.ok());
  assert(second.status().code() == telepath::StatusCode::kResourceExhausted);

  pinned.Reset();
  auto retry = manager->ReadBuffer(5, 1);
  assert(retry.ok());
}

}  // namespace

int main() {
  telepath::buffer_test_support::TestRootGuard root_guard( "telepath_resource_exhaustion_test_data");
  auto manager = BuildManager(root_guard.path());
  ExpectReadFailsWhileOnlyFrameIsPinned(&manager);
  return 0;
}
