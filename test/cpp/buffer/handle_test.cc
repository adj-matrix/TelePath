#include <cassert>
#include <memory>

#include "buffer_test_support.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

auto BuildManager(const std::filesystem::path &root) -> telepath::BufferManager {
  auto telemetry = telepath::MakeCounterTelemetrySink();
  auto disk_backend = std::make_unique<telepath::PosixDiskBackend>(root.string(), 4096);
  auto replacer = telepath::MakeClockReplacer(2);
  return telepath::BufferManager(2, 4096, std::move(disk_backend), std::move(replacer), telemetry);
}

auto AcquireWritableHandle(telepath::BufferManager *manager) -> telepath::BufferHandle {
  auto result = manager->ReadBuffer(7, 1);
  assert(result.ok());
  return std::move(result.value());
}

void ExpectHandleBecomesInvalidAfterRelease(telepath::BufferManager *manager) {
  telepath::BufferHandle handle = AcquireWritableHandle(manager);

  assert(handle.valid());
  assert(handle.writable());
  handle.mutable_data()[0] = std::byte{0x4D};
  assert(manager->MarkBufferDirty(handle).ok());

  assert(manager->ReleaseBuffer(std::move(handle)).ok());
  assert(!handle.valid());
  assert(manager->MarkBufferDirty(handle).code() == telepath::StatusCode::kInvalidArgument);
  assert(manager->ReleaseBuffer(std::move(handle)).code() == telepath::StatusCode::kInvalidArgument);
}

}  // namespace

int main() {
  telepath::buffer_test_support::TestRootGuard root_guard("telepath_handle_test_data");
  auto manager = BuildManager(root_guard.path());
  ExpectHandleBecomesInvalidAfterRelease(&manager);
  return 0;
}
