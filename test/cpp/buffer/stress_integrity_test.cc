#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "buffer_test_support.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/options/buffer_manager_options.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

constexpr std::size_t kPageSize = 4096;
constexpr telepath::FileId kFileId = 3;

struct StressConfig {
  const char *name;
  std::size_t pool_size;
  std::size_t block_count;
  std::size_t threads;
  std::size_t ops_per_thread;
  bool background_cleaner;
  const char *replacer;
};

auto MakeReplacer(const StressConfig &config) -> std::unique_ptr<telepath::Replacer> {
  const std::string name = config.replacer;
  if (name == "clock") return telepath::MakeClockReplacer(config.pool_size);
  if (name == "clock_sweep") return telepath::MakeClockSweepReplacer(config.pool_size);
  if (name == "sieve") return telepath::MakeSieveReplacer(config.pool_size);
  if (name == "lru") return telepath::MakeLruReplacer(config.pool_size);
  if (name == "two_queue") return telepath::MakeTwoQueueReplacer(config.pool_size);
  return telepath::MakeLruKReplacer(config.pool_size, 2);
}

// Page layout: an 8-byte write counter followed by its low byte repeated, so
// a reader can detect a torn page.
void WritePattern(std::span<std::byte> page, std::uint64_t counter) {
  std::memcpy(page.data(), &counter, sizeof(counter));
  std::memset(page.data() + sizeof(counter), static_cast<int>(counter & 0xFF), kPageSize - sizeof(counter));
}

auto ReadAndCheckPattern(std::span<const std::byte> page) -> std::uint64_t {
  std::uint64_t counter = 0;
  std::memcpy(&counter, page.data(), sizeof(counter));
  const auto fill = static_cast<std::byte>(counter & 0xFF);
  for (std::size_t i = sizeof(counter); i < kPageSize; ++i) {
    if (page[i] != fill) {
      std::fprintf(stderr, "torn page: counter %llu, byte %zu\n", static_cast<unsigned long long>(counter), i);
      assert(false);
    }
  }
  return counter;
}

auto CreateManager(const std::filesystem::path &root, const StressConfig &config) -> std::unique_ptr<telepath::BufferManager> {
  telepath::BufferManagerOptions options;
  options.pool_size = config.pool_size;
  options.page_size = kPageSize;
  options.enable_background_cleaner = config.background_cleaner;
  options.disk_backend.sync_on_write = false;
  auto manager = telepath::BufferManager::Create(
    options,
    std::make_unique<telepath::PosixDiskBackend>(root.string(), telepath::BlockGeometry::Of<kPageSize>(), false, 64, false),
    MakeReplacer(config),
    telepath::MakeNoOpTelemetrySink());
  assert(manager.has_value());
  return std::move(manager.value());
}

void RunWorker(telepath::BufferManager *manager, const StressConfig &config, std::size_t worker, std::vector<std::atomic<std::uint64_t>> *expected) {
  std::mt19937_64 rng(0x5eed + worker);
  std::uniform_int_distribution<telepath::BlockId> block_dist(0, config.block_count - 1);
  std::uniform_int_distribution<int> op_dist(0, 99);

  for (std::size_t op = 0; op < config.ops_per_thread; ++op) {
    const telepath::BlockId block = block_dist(rng);
    const int kind = op_dist(rng);
    if (kind >= 95) {
      if (kind < 98) assert(manager->FlushAll().has_value());
      continue;
    }
    if (kind < 65) {
      auto handle = manager->ReadBuffer(kFileId, block);
      if (!handle.has_value()) {
        assert(handle.error().code() == telepath::ErrorCode::kResourceExhausted);
        continue;
      }
      ReadAndCheckPattern(handle->data());
      continue;
    }

    auto handle = manager->ReadBufferExclusive(kFileId, block);
    if (!handle.has_value()) {
      assert(handle.error().code() == telepath::ErrorCode::kResourceExhausted);
      continue;
    }
    auto page = handle->mutable_data();
    const std::uint64_t next = ReadAndCheckPattern(page) + 1;
    WritePattern(page, next);
    (*expected)[block].fetch_add(1, std::memory_order_relaxed);
    if (kind >= 92) assert(manager->FlushBuffer(*handle).has_value());
  }
}

void RunStress(const StressConfig &config) {
  telepath::buffer_test_support::TestRootGuard root(std::string("telepath_stress_") + config.name + "_" + config.replacer);
  std::vector<std::atomic<std::uint64_t>> expected(config.block_count);
  {
    auto manager = CreateManager(root.path(), config);
    for (telepath::BlockId block = 0; block < config.block_count; ++block) {
      auto handle = manager->ReadBufferExclusive(kFileId, block);
      assert(handle.has_value());
      WritePattern(handle->mutable_data(), 0);
    }

    std::vector<std::thread> workers;
    for (std::size_t worker = 0; worker < config.threads; ++worker) {
      workers.emplace_back(RunWorker, manager.get(), std::cref(config), worker, &expected);
    }
    for (auto &worker : workers) worker.join();

    for (telepath::BlockId block = 0; block < config.block_count; ++block) {
      auto handle = manager->ReadBuffer(kFileId, block);
      assert(handle.has_value());
      assert(ReadAndCheckPattern(handle->data()) == expected[block].load());
    }
    assert(manager->FlushAll().has_value());
    const auto snapshot = manager->ExportSnapshot();
    assert(snapshot.dirty_page_count == 0);
    assert(snapshot.flush_queued_count == 0 && snapshot.flush_in_flight_count == 0);
  }

  auto reopened = CreateManager(root.path(), config);
  for (telepath::BlockId block = 0; block < config.block_count; ++block) {
    auto handle = reopened->ReadBuffer(kFileId, block);
    assert(handle.has_value());
    const std::uint64_t persisted = ReadAndCheckPattern(handle->data());
    if (persisted != expected[block].load()) {
      std::fprintf(stderr, "%s: block %llu persisted %llu, expected %llu\n", config.name,
                   static_cast<unsigned long long>(block), static_cast<unsigned long long>(persisted),
                   static_cast<unsigned long long>(expected[block].load()));
      assert(false);
    }
  }
}

}  // namespace

int main() {
  for (const char *replacer : {"lru_k", "clock", "clock_sweep", "sieve", "lru", "two_queue"}) {
    RunStress({"eviction_heavy", 8, 64, 8, 4000, false, replacer});
  }
  RunStress({"cleaner", 16, 48, 8, 4000, true, "clock_sweep"});
  RunStress({"hot_pages", 32, 8, 12, 4000, true, "sieve"});
  return 0;
}
