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

  const fs::path root =
      fs::temp_directory_path() / "telepath_wait_evict_interleave_data";
  fs::remove_all(root);
  fs::create_directories(root);

  {
    auto disk_backend =
        std::make_unique<telepath::PosixDiskBackend>(root.string(), 4096);
    std::array<std::byte, 4096> seed_a{};
    std::array<std::byte, 4096> seed_b{};
    seed_a[0] = std::byte{0xA1};
    seed_b[0] = std::byte{0xB2};

    auto write_a = disk_backend->SubmitWrite(telepath::BufferTag{3, 0},
                                             seed_a.data(), seed_a.size());
    assert(write_a.ok());
    auto completion_a = disk_backend->PollCompletion();
    assert(completion_a.ok());
    assert(completion_a.value().status.ok());

    auto write_b = disk_backend->SubmitWrite(telepath::BufferTag{3, 1},
                                             seed_b.data(), seed_b.size());
    assert(write_b.ok());
    auto completion_b = disk_backend->PollCompletion();
    assert(completion_b.ok());
    assert(completion_b.value().status.ok());
  }

  auto telemetry = telepath::MakeCounterTelemetrySink();
  auto disk_backend =
      std::make_unique<telepath::PosixDiskBackend>(root.string(), 4096);
  auto replacer = telepath::MakeLruKReplacer(1, 2);
  telepath::BufferManager manager(1, 4096, std::move(disk_backend),
                                  std::move(replacer), telemetry);

  std::atomic<bool> failed{false};
  std::vector<std::thread> workers;
  workers.reserve(4);

  for (int i = 0; i < 4; ++i) {
    workers.emplace_back([&manager, &failed]() {
      auto result = manager.ReadBuffer(3, 0);
      if (!result.ok()) {
        failed.store(true);
        return;
      }
      telepath::BufferHandle handle = std::move(result.value());
      if (handle.data()[0] != std::byte{0xA1}) {
        failed.store(true);
      }
    });
  }

  for (auto &worker : workers) {
    worker.join();
  }

  {
    auto result = manager.ReadBuffer(3, 1);
    assert(result.ok());
    telepath::BufferHandle handle = std::move(result.value());
    assert(handle.data()[0] == std::byte{0xB2});
  }

  {
    auto result = manager.ReadBuffer(3, 0);
    assert(result.ok());
    telepath::BufferHandle handle = std::move(result.value());
    assert(handle.data()[0] == std::byte{0xA1});
  }

  assert(!failed.load());
  fs::remove_all(root);
  return 0;
}
