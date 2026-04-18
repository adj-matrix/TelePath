#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

#include "buffer_test_support.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

void SeedDiskPages(const std::filesystem::path &root) {
  auto disk_backend = std::make_unique<telepath::PosixDiskBackend>(root.string(), 4096);
  std::array<std::byte, 4096> seed_a{};
  std::array<std::byte, 4096> seed_b{};
  seed_a[0] = std::byte{0xA1};
  seed_b[0] = std::byte{0xB2};

  auto write_a = disk_backend->SubmitWrite(telepath::BufferTag{3, 0}, seed_a.data(), seed_a.size());
  assert(write_a.ok());
  auto completion_a = disk_backend->PollCompletion();
  assert(completion_a.ok());
  assert(completion_a.value().status.ok());

  auto write_b = disk_backend->SubmitWrite(telepath::BufferTag{3, 1}, seed_b.data(), seed_b.size());
  assert(write_b.ok());
  auto completion_b = disk_backend->PollCompletion();
  assert(completion_b.ok());
  assert(completion_b.value().status.ok());
}

auto BuildManager(const std::filesystem::path &root) -> telepath::BufferManager {
  auto telemetry = telepath::MakeCounterTelemetrySink();
  auto disk_backend = std::make_unique<telepath::PosixDiskBackend>(root.string(), 4096);
  auto replacer = telepath::MakeLruKReplacer(1, 2);
  return telepath::BufferManager(1, 4096, std::move(disk_backend), std::move(replacer), telemetry);
}

void RunHotPageWorker(
  telepath::BufferManager *manager,
  std::atomic<bool> *failed
) {
  auto result = manager->ReadBuffer(3, 0);
  if (!result.ok()) {
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
  assert(result.ok());
  auto handle = std::move(result.value());
  assert(handle.data()[0] == std::byte{0xB2});
}

void ExpectOriginalPageCanBeReadAgain(telepath::BufferManager *manager) {
  auto result = manager->ReadBuffer(3, 0);
  assert(result.ok());
  auto handle = std::move(result.value());
  assert(handle.data()[0] == std::byte{0xA1});
}

}  // namespace

int main() {
  telepath::buffer_test_support::TestRootGuard root_guard("telepath_wait_evict_interleave_data");
  SeedDiskPages(root_guard.path());
  auto manager = BuildManager(root_guard.path());
  ExpectHotPageWaitersCompleteFirst(&manager);
  ExpectColdPageCanEvictAndBeRead(&manager);
  ExpectOriginalPageCanBeReadAgain(&manager);
  return 0;
}
