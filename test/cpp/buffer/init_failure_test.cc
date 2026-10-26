#include <cassert>
#include <memory>

#include "polling_disk_backend.h"
#include "buffer_test_support.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

class NoOpDiskBackend : public telepath::test_support::PollingDiskBackend {
 public:
  auto SubmitRead(
    const telepath::BufferTag &,
    std::byte *,
    std::size_t
  ) -> telepath::Result<uint64_t> override {
    return telepath::Unavailable("not expected");
  }

  auto SubmitWrite(
    const telepath::BufferTag &,
    const std::byte *,
    std::size_t
  ) -> telepath::Result<uint64_t> override {
    return telepath::Unavailable("not expected");
  }

  auto PollCompletion() -> telepath::Result<telepath::test_support::LegacyDiskCompletion> override {
    return telepath::Unavailable("not expected");
  }

  void OnShutdown() override {}

};

auto CreateManager(std::size_t pool_size) -> telepath::Result<std::unique_ptr<telepath::BufferManager>> {
  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto disk_backend = std::make_unique<NoOpDiskBackend>();
  auto replacer = telepath::MakeClockReplacer(pool_size == 0 ? 1 : pool_size);
  return telepath::BufferManager::Create(pool_size, 4096, std::move(disk_backend), std::move(replacer), telemetry);
}

void ExpectInitializationFailureIsReportedByCreate() {
  auto manager_result = CreateManager(0);
  assert(!manager_result.has_value());
  assert(manager_result.error().code() == telepath::ErrorCode::kInvalidArgument);
}

// Frame ids are 32 bits wide; a larger pool is refused before any memory is
// allocated for it.
void ExpectPoolBeyondFrameIdRangeIsRejected() {
  for (std::size_t pool_size : {std::size_t{1} << 32, std::size_t{telepath::kInvalidFrameId}}) {
    auto manager_result = telepath::BufferManager::Create(pool_size, 4096, std::make_unique<NoOpDiskBackend>(), telepath::MakeClockReplacer(1), nullptr);
    assert(!manager_result.has_value());
    assert(manager_result.error().code() == telepath::ErrorCode::kInvalidArgument);
  }
}

void ExpectMissingDependencyIsReportedByCreate() {
  auto manager_result = telepath::BufferManager::Create(1, 4096, nullptr, telepath::MakeClockReplacer(1), nullptr);
  assert(!manager_result.has_value());
  assert(manager_result.error().code() == telepath::ErrorCode::kInvalidArgument);
}

void ExpectInvalidPageSizeIsReportedByCreate() {
  auto manager_result = telepath::BufferManager::Create(1, 1000, std::make_unique<NoOpDiskBackend>(), telepath::MakeClockReplacer(1), nullptr);
  assert(!manager_result.has_value());
  assert(manager_result.error().code() == telepath::ErrorCode::kInvalidArgument);
}

void ExpectBackendBlockSizeMismatchIsReportedByCreate() {
  telepath::buffer_test_support::TestRootGuard root("telepath_init_failure_block_size");
  auto backend = std::make_unique<telepath::PosixDiskBackend>(root.path().string(), telepath::BlockGeometry::Of<4096>());
  auto manager_result = telepath::BufferManager::Create(1, 8192, std::move(backend), telepath::MakeClockReplacer(1), nullptr);
  assert(!manager_result.has_value());
  assert(manager_result.error().code() == telepath::ErrorCode::kInvalidArgument);
}

void ExpectForeignGuardIsRejected() {
  auto manager_result = CreateManager(1);
  assert(manager_result.has_value());
  telepath::SharedBufferHandle empty_guard;
  const auto flush_status = manager_result.value()->FlushBuffer(empty_guard);
  assert(!flush_status.has_value());
  assert(flush_status.error().code() == telepath::ErrorCode::kInvalidArgument);
}

}  // namespace

int main() {
  ExpectInitializationFailureIsReportedByCreate();
  ExpectMissingDependencyIsReportedByCreate();
  ExpectPoolBeyondFrameIdRangeIsRejected();
  ExpectInvalidPageSizeIsReportedByCreate();
  ExpectBackendBlockSizeMismatchIsReportedByCreate();
  ExpectForeignGuardIsRejected();
  return 0;
}
