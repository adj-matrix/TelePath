#include <cassert>
#include <memory>

#include "buffer_test_support.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

auto BuildManager(const std::filesystem::path &root) -> std::unique_ptr<telepath::BufferManager> {
  auto telemetry = telepath::MakeCounterTelemetrySink();
  auto disk_backend = std::make_unique<telepath::PosixDiskBackend>(root.string(), telepath::BlockGeometry::Of<4096>());
  auto replacer = telepath::MakeClockReplacer(2);
  return telepath::BufferManager::Create(2, 4096, std::move(disk_backend), std::move(replacer), telemetry).value();
}

auto AcquireWritableHandle(telepath::BufferManager *manager) -> telepath::ExclusiveBufferHandle {
  auto result = manager->ReadBufferExclusive(7, 1);
  assert(result.has_value());
  return std::move(result.value());
}

void ExpectHandleBecomesInvalidAfterRelease(telepath::BufferManager *manager) {
  telepath::ExclusiveBufferHandle handle = AcquireWritableHandle(manager);

  assert(handle.valid());
  assert(handle.size() == 4096);
  handle.mutable_data()[0] = std::byte{0x4D};

  assert(manager->ReleaseBuffer(std::move(handle)).has_value());
  assert(!handle.valid());
  assert(handle.data().empty());
  assert(handle.mutable_data().empty());
  assert(manager->FlushBuffer(handle).error().code() == telepath::ErrorCode::kInvalidArgument);
  assert(manager->ReleaseBuffer(std::move(handle)).error().code() == telepath::ErrorCode::kInvalidArgument);
}

void ExpectMovedFromHandleIsEmpty(telepath::BufferManager *manager) {
  telepath::ExclusiveBufferHandle source = AcquireWritableHandle(manager);
  telepath::ExclusiveBufferHandle target = std::move(source);
  assert(!source.valid());
  assert(target.valid());
  assert(target.data()[0] == std::byte{0x4D});
  target.Reset();

  auto reread = manager->ReadBuffer(7, 1);
  assert(reread.has_value());
  assert(reread->data()[0] == std::byte{0x4D});
}

}  // namespace

int main() {
  telepath::buffer_test_support::TestRootGuard root_guard("telepath_handle_test_data");
  auto manager_owner = BuildManager(root_guard.path());
  telepath::BufferManager &manager = *manager_owner;
  ExpectHandleBecomesInvalidAfterRelease(&manager);
  ExpectMovedFromHandleIsEmpty(&manager);
  return 0;
}
