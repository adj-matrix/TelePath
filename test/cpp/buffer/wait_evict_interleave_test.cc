#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

#include "disk_io_helpers.h"
#include "buffer_test_support.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

void SeedDiskPages(const std::filesystem::path &root) {
  auto disk_backend = std::make_unique<telepath::PosixDiskBackend>(root.string(), telepath::BlockGeometry::Of<4096>());
  std::array<std::byte, 4096> seed_a{};
  std::array<std::byte, 4096> seed_b{};
  seed_a[0] = std::byte{0xA1};
  seed_b[0] = std::byte{0xB2};

  assert(telepath::test_support::WriteBlock(*disk_backend, telepath::BufferTag{3, 0}, seed_a).has_value());

  assert(telepath::test_support::WriteBlock(*disk_backend, telepath::BufferTag{3, 1}, seed_b).has_value());
}

auto BuildManager(const std::filesystem::path &root) -> std::unique_ptr<telepath::BufferManager> {
  auto telemetry = telepath::MakeCounterTelemetrySink();
  auto disk_backend = std::make_unique<telepath::PosixDiskBackend>(root.string(), telepath::BlockGeometry::Of<4096>());
  auto replacer = telepath::MakeLruKReplacer(1, 2);
  return telepath::BufferManager::Create(1, 4096, std::move(disk_backend), std::move(replacer), telemetry).value();
}

void RunHotPageWorker(
  telepath::BufferManager *manager,
  std::atomic<bool> *failed
) {
  auto result = manager->ReadBuffer(3, 0);
  if (!result.has_value()) {
    failed->store(true);
    return;
  }

  auto handle = std::move(result.value());
  if (handle.data()[0] == std::byte{0xA1}) return;
  failed->store(true);
}

void ExpectHotPageWaitersCompleteFirst(telepath::BufferManager *manager) {
  std::atomic<bool> failed{false};
  std::vector<std::thread> workers;
  workers.reserve(4);

  for (int i = 0; i < 4; ++i) {
    workers.emplace_back([manager, &failed]() {
      RunHotPageWorker(manager, &failed);
    });
  }

  for (auto &worker : workers) worker.join();
  assert(!failed.load());
}

void ExpectColdPageCanEvictAndBeRead(telepath::BufferManager *manager) {
  auto result = manager->ReadBuffer(3, 1);
  assert(result.has_value());
  auto handle = std::move(result.value());
  assert(handle.data()[0] == std::byte{0xB2});
}

void ExpectOriginalPageCanBeReadAgain(telepath::BufferManager *manager) {
  auto result = manager->ReadBuffer(3, 0);
  assert(result.has_value());
  auto handle = std::move(result.value());
  assert(handle.data()[0] == std::byte{0xA1});
}

}  // namespace

int main() {
  telepath::buffer_test_support::TestRootGuard root_guard("telepath_wait_evict_interleave_data");
  SeedDiskPages(root_guard.path());
  auto manager_owner = BuildManager(root_guard.path());
  telepath::BufferManager &manager = *manager_owner;
  ExpectHotPageWaitersCompleteFirst(&manager);
  ExpectColdPageCanEvictAndBeRead(&manager);
  ExpectOriginalPageCanBeReadAgain(&manager);
  return 0;
}
