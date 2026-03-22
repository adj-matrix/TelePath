#include <atomic>
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

int main() {
  namespace fs = std::filesystem;

  const fs::path root = fs::temp_directory_path() / "telepath_same_page_miss_data";
  fs::remove_all(root);
  fs::create_directories(root);

  {
    auto disk_backend =
        std::make_unique<telepath::PosixDiskBackend>(root.string(), 4096);
    std::array<std::byte, 4096> seed{};
    seed[0] = std::byte{0x55};
    auto request = disk_backend->SubmitWrite(telepath::BufferTag{11, 3}, seed.data(),
                                             seed.size());
    assert(request.ok());
    auto completion = disk_backend->PollCompletion();
    assert(completion.ok());
    assert(completion.value().status.ok());
  }

  auto telemetry = telepath::MakeCounterTelemetrySink();
  auto disk_backend =
      std::make_unique<telepath::PosixDiskBackend>(root.string(), 4096);
  auto replacer = telepath::MakeClockReplacer(4);
  telepath::BufferManager manager(4, 4096, std::move(disk_backend),
                                  std::move(replacer), telemetry);

  std::atomic<bool> failed{false};
  std::vector<std::thread> workers;
  workers.reserve(8);

  for (int i = 0; i < 8; ++i) {
    workers.emplace_back([&manager, &failed]() {
      auto result = manager.ReadBuffer(11, 3);
      if (!result.ok()) {
        failed.store(true);
        return;
      }
      telepath::BufferHandle handle = std::move(result.value());
      if (handle.data()[0] != std::byte{0x55}) {
        failed.store(true);
      }
    });
  }

  for (auto &worker : workers) {
    worker.join();
  }

  const telepath::TelemetrySnapshot snapshot = telemetry->Snapshot();
  assert(!failed.load());
  assert(snapshot.buffer_misses >= 1);
  assert(snapshot.buffer_hits >= 1);

  fs::remove_all(root);
  return 0;
}
