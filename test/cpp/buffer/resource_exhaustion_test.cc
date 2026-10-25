#include <cassert>
#include <memory>

#include "buffer_test_support.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

auto BuildManager(const std::filesystem::path &root) -> std::unique_ptr<telepath::BufferManager> {
  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto disk_backend = std::make_unique<telepath::PosixDiskBackend>(root.string(), telepath::BlockGeometry::Of<4096>());
  auto replacer = telepath::MakeClockReplacer(1);
  return telepath::BufferManager::Create(1, 4096, std::move(disk_backend), std::move(replacer), telemetry).value();
}

void ExpectReadFailsWhileOnlyFrameIsPinned(telepath::BufferManager *manager) {
  auto first = manager->ReadBuffer(5, 0);
  assert(first.has_value());
  auto pinned = std::move(first.value());

  auto second = manager->ReadBuffer(5, 1);
  assert(!second.has_value());
  assert(second.error().code() == telepath::ErrorCode::kResourceExhausted);

  pinned.Reset();
  auto retry = manager->ReadBuffer(5, 1);
  assert(retry.has_value());
}

}  // namespace

int main() {
  telepath::buffer_test_support::TestRootGuard root_guard( "telepath_resource_exhaustion_test_data");
  auto manager_owner = BuildManager(root_guard.path());
  telepath::BufferManager &manager = *manager_owner;
  ExpectReadFailsWhileOnlyFrameIsPinned(&manager);
  return 0;
}
