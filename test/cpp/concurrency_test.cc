#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

int main() {
  namespace fs = std::filesystem;

  const auto unique_suffix =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path root =
      fs::temp_directory_path() /
      ("telepath_concurrency_test_data_" + std::to_string(unique_suffix));
  fs::remove_all(root);
  fs::create_directories(root);

  auto telemetry = telepath::MakeCounterTelemetrySink();
  auto disk_backend =
      std::make_unique<telepath::PosixDiskBackend>(root.string(), 4096);
  auto replacer = telepath::MakeClockReplacer(8);
  telepath::BufferManager manager(8, 4096, std::move(disk_backend),
                                  std::move(replacer), telemetry);

  for (telepath::BlockId block_id = 0; block_id < 4; ++block_id) {
    auto result = manager.ReadBuffer(1, block_id);
    assert(result.ok());
    telepath::BufferHandle handle = std::move(result.value());
    handle.mutable_data()[0] = static_cast<std::byte>(block_id + 1);
    assert(manager.MarkBufferDirty(handle).ok());
    handle.Reset();
  }
  const telepath::Status flush_status = manager.FlushAll();
  if (!flush_status.ok()) {
    std::cerr << "FlushAll failed: " << flush_status.message() << "\n";
    return 1;
  }

  std::atomic<bool> failed{false};
  std::vector<std::thread> workers;
  workers.reserve(8);

  for (int worker = 0; worker < 8; ++worker) {
    workers.emplace_back([&manager, &failed, worker]() {
      for (int round = 0; round < 200; ++round) {
        const telepath::BlockId block_id =
            static_cast<telepath::BlockId>((worker + round) % 4);
        auto result = manager.ReadBuffer(1, block_id);
        if (!result.ok()) {
          failed.store(true);
          return;
        }

        telepath::BufferHandle handle = std::move(result.value());
        const int observed = std::to_integer<int>(handle.data()[0]);
        if (observed != static_cast<int>(block_id + 1)) {
          failed.store(true);
          return;
        }
      }
    });
  }

  for (auto &worker : workers) {
    worker.join();
  }

  assert(!failed.load());
  fs::remove_all(root);
  return 0;
}
