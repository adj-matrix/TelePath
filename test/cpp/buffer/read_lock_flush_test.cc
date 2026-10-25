#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>

#include "disk_io_helpers.h"
#include "buffer_test_support.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

auto BuildManager(const std::filesystem::path &root) -> std::unique_ptr<telepath::BufferManager> {
  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto disk_backend = std::make_unique<telepath::PosixDiskBackend>(root.string(), telepath::BlockGeometry::Of<4096>());
  auto replacer = telepath::MakeClockReplacer(2);
  return telepath::BufferManager::Create(2, 4096, std::move(disk_backend), std::move(replacer), telemetry).value();
}

void SeedDirtyPage(telepath::BufferManager *manager) {
  auto seed_result = manager->ReadBufferExclusive(31, 0);
  assert(seed_result.has_value());

  auto seed = std::move(seed_result.value());
  seed.mutable_data()[0] = std::byte{0x6C};
  seed.mutable_data()[1] = std::byte{0x7D};
}

auto AcquireReader(telepath::BufferManager *manager) -> telepath::SharedBufferHandle {
  auto reader_result = manager->ReadBuffer(31, 0);
  assert(reader_result.has_value());
  return std::move(reader_result.value());
}

void ExpectFlushCompletesWhileReadHandleStaysPinned(
  telepath::BufferManager *manager,
  const telepath::SharedBufferHandle &reader
) {
  assert(reader.data()[0] == std::byte{0x6C});

  std::atomic<bool> flush_finished{false};
  telepath::Status flush_status = telepath::Ok();
  std::thread flush_thread([&]() {
    auto flusher = manager->ReadBuffer(31, 0);
    assert(flusher.has_value());
    flush_status = manager->FlushBuffer(*flusher);
    flush_finished.store(true);
  });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!flush_finished.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(flush_finished.load());
  flush_thread.join();
  assert(flush_status.has_value());
}

void ExpectPersistedPageMatches(const std::filesystem::path &root) {
  auto verifier = std::make_unique<telepath::PosixDiskBackend>(root.string(), telepath::BlockGeometry::Of<4096>());
  std::array<std::byte, 4096> page{};
  assert(telepath::test_support::ReadBlock(*verifier, telepath::BufferTag{31, 0}, page).has_value());
  assert(page[0] == std::byte{0x6C});
  assert(page[1] == std::byte{0x7D});
}

}  // namespace

int main() {
  telepath::buffer_test_support::TestRootGuard root_guard("telepath_read_lock_flush_test_data");
  auto manager_owner = BuildManager(root_guard.path());
  telepath::BufferManager &manager = *manager_owner;
  SeedDirtyPage(&manager);
  const telepath::SharedBufferHandle reader = AcquireReader(&manager);
  ExpectFlushCompletesWhileReadHandleStaysPinned(&manager, reader);
  ExpectPersistedPageMatches(root_guard.path());
  return 0;
}
