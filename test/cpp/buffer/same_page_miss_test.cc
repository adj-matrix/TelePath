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

void SeedDiskPage(const std::filesystem::path &root) {
  auto disk_backend = std::make_unique<telepath::PosixDiskBackend>(root.string(), 4096);
  std::array<std::byte, 4096> seed{};
  seed[0] = std::byte{0x55};
  auto request = disk_backend->SubmitWrite(telepath::BufferTag{11, 3}, seed.data(), seed.size());
  assert(request.ok());
  auto completion = disk_backend->PollCompletion();
  assert(completion.ok());
  assert(completion.value().status.ok());
}

struct SamePageMissContext {
  std::shared_ptr<telepath::TelemetrySink> telemetry;
  telepath::BufferManager manager;
};

auto BuildContext(const std::filesystem::path &root) -> SamePageMissContext {
  auto telemetry = telepath::MakeCounterTelemetrySink();
  auto disk_backend = std::make_unique<telepath::PosixDiskBackend>(root.string(), 4096);
  auto replacer = telepath::MakeClockReplacer(4);
  return {
    telemetry,
    telepath::BufferManager(4, 4096, std::move(disk_backend), std::move(replacer), telemetry),
  };
}

void RunWorker(
  telepath::BufferManager *manager,
  std::atomic<bool> *failed
) {
  auto result = manager->ReadBuffer(11, 3);
  if (!result.ok()) {
    failed->store(true);
    return;
  }

  auto handle = std::move(result.value());
  if (handle.data()[0] == std::byte{0x55}) return;
  failed->store(true);
}

void ExpectConcurrentReadersShareSingleMiss(telepath::BufferManager *manager) {
  std::atomic<bool> failed{false};
  std::vector<std::thread> workers;
  workers.reserve(8);

  for (int i = 0; i < 8; ++i) {
    workers.emplace_back([manager, &failed]() {
      RunWorker(manager, &failed);
    });
  }

  for (auto &worker : workers) worker.join();
  assert(!failed.load());
}

void ExpectMissAndHitTelemetry(const telepath::TelemetrySnapshot &snapshot) {
  assert(snapshot.buffer_misses >= 1);
  assert(snapshot.buffer_hits >= 1);
}

}  // namespace

int main() {
  telepath::buffer_test_support::TestRootGuard root_guard("telepath_same_page_miss_data");
  SeedDiskPage(root_guard.path());
  auto context = BuildContext(root_guard.path());
  ExpectConcurrentReadersShareSingleMiss(&context.manager);
  ExpectMissAndHitTelemetry(context.telemetry->Snapshot());
  return 0;
}
