#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "buffer_test_support.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

constexpr int kWorkerCount = 8;
constexpr int kRoundCount = 200;
constexpr telepath::BlockId kSeededBlockCount = 4;

auto BuildManager(const std::filesystem::path &root) -> telepath::BufferManager {
  auto telemetry = telepath::MakeCounterTelemetrySink();
  auto disk_backend = std::make_unique<telepath::PosixDiskBackend>(root.string(), 4096);
  auto replacer = telepath::MakeClockReplacer(8);
  return telepath::BufferManager(8, 4096, std::move(disk_backend), std::move(replacer), telemetry);
}

void SeedPersistedPages(telepath::BufferManager *manager) {
  for (telepath::BlockId block_id = 0; block_id < kSeededBlockCount; ++block_id) {
    auto result = manager->ReadBuffer(1, block_id);
    assert(result.ok());

    auto handle = std::move(result.value());
    handle.mutable_data()[0] = static_cast<std::byte>(block_id + 1);
    assert(manager->MarkBufferDirty(handle).ok());
    handle.Reset();
  }

  const auto flush_status = manager->FlushAll();
  if (flush_status.ok()) return;

  std::cerr << "FlushAll failed: " << flush_status.message() << "\n";
  std::abort();
}

void RunWorker(
  telepath::BufferManager *manager,
  std::atomic<bool> *failed,
  int worker
) {
  for (int round = 0; round < kRoundCount; ++round) {
    const auto block_id = static_cast<telepath::BlockId>((worker + round) % kSeededBlockCount);
    auto result = manager->ReadBuffer(1, block_id);
    if (!result.ok()) {
      failed->store(true);
      return;
    }

    auto handle = std::move(result.value());
    const int observed = std::to_integer<int>(handle.data()[0]);
    if (observed == static_cast<int>(block_id + 1)) continue;
    failed->store(true);
    return;
  }
}

void ExpectConcurrentReadsStayConsistent(telepath::BufferManager *manager) {
  std::atomic<bool> failed{false};
  std::vector<std::thread> workers;
  workers.reserve(kWorkerCount);

  for (int worker = 0; worker < kWorkerCount; ++worker) {
    workers.emplace_back([manager, &failed, worker]() {
      RunWorker(manager, &failed, worker);
    });
  }

  for (auto &worker : workers) worker.join();
  assert(!failed.load());
}

}  // namespace

int main() {
  telepath::buffer_test_support::TestRootGuard root_guard("telepath_concurrency_test_data");
  auto manager = BuildManager(root_guard.path());
  SeedPersistedPages(&manager);
  ExpectConcurrentReadsStayConsistent(&manager);
  return 0;
}
