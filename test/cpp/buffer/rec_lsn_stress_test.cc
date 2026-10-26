// Concurrent writers report LSNs while writeback, eviction, and injected write
// faults run. A checker pauses the writers now and then and verifies the two
// promises of the WAL support: MinRecLsn() never exceeds the LSN of a change
// that is not on disk yet, and after CheckpointBuffers(lsn) every change below
// lsn is on disk.
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <span>
#include <thread>
#include <vector>

#include "buffer_test_support.h"
#include "disk_io_helpers.h"
#include "fault_injection_backend.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/options/buffer_manager_options.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

constexpr std::size_t kPageSize = 4096;
constexpr telepath::FileId kFile = 1;
constexpr telepath::BlockId kBlocks = 32;
constexpr std::size_t kWriters = 6;
constexpr int kChecks = 60;

// The LSNs applied to one page, in order.
struct PageLog {
  std::mutex latch;
  std::vector<telepath::Lsn> lsns;
};

auto DiskLsn(telepath::PosixDiskBackend &verifier, telepath::BlockId block) -> telepath::Lsn {
  std::array<std::byte, kPageSize> page{};
  assert(telepath::test_support::ReadBlock(verifier, {kFile, block}, page).has_value());
  telepath::Lsn lsn = 0;
  std::memcpy(&lsn, page.data(), sizeof(lsn));
  return lsn;
}

void Fail(const char *what, telepath::BlockId block, telepath::Lsn a, telepath::Lsn b) {
  std::fprintf(stderr, "%s: block %llu (%llu vs %llu)\n", what, static_cast<unsigned long long>(block), static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
  std::abort();
}

void Run(const char *name, std::size_t pool_size, telepath::ForegroundWriteback foreground, unsigned write_faults) {
  telepath::buffer_test_support::TestRootGuard root(name);
  telepath::BufferManagerOptions options;
  options.pool_size = pool_size;
  options.page_size = kPageSize;
  options.foreground_writeback = foreground;
  options.enable_background_cleaner = true;
  auto inner = std::make_unique<telepath::PosixDiskBackend>(root.path().string(), telepath::BlockGeometry::Of<kPageSize>());
  auto backend = std::make_unique<telepath::test_support::FaultInjectionBackend>(std::move(inner), 99);
  auto *faults = backend.get();
  auto created = telepath::BufferManager::Create(options, std::move(backend), telepath::MakeLruKReplacer(pool_size, 2), telepath::MakeNoOpTelemetrySink());
  assert(created.has_value());
  auto &manager = created.value();
  telepath::PosixDiskBackend verifier(root.path().string(), telepath::BlockGeometry::Of<kPageSize>());
  faults->SetFaultRate(telepath::DiskOperation::kWrite, write_faults);

  std::vector<PageLog> logs(kBlocks);
  // Writers hold `phase` shared for each change; the checker takes it
  // exclusively to see a quiescent state, raising `pausing` first so that
  // writers cannot starve it.
  std::shared_mutex phase;
  std::atomic<bool> pausing{false};
  std::atomic<telepath::Lsn> next_lsn{1};
  std::atomic<bool> stop{false};

  std::vector<std::thread> writers;
  for (std::size_t writer = 0; writer < kWriters; ++writer) {
    writers.emplace_back([&, writer] {
      std::mt19937_64 rng(writer + 1);
      while (!stop.load()) {
        const telepath::BlockId block = rng() % kBlocks;
        while (pausing.load()) std::this_thread::yield();
        std::shared_lock<std::shared_mutex> paused(phase);
        auto handle = manager->ReadBufferExclusive(kFile, block);
        if (!handle.has_value()) {
          assert(handle.error().code() == telepath::ErrorCode::kIoError || handle.error().code() == telepath::ErrorCode::kResourceExhausted);
          continue;
        }
        const telepath::Lsn lsn = next_lsn.fetch_add(1);
        std::memcpy(handle->mutable_data().data(), &lsn, sizeof(lsn));
        handle->MarkDirty(lsn);
        std::lock_guard<std::mutex> guard(logs[block].latch);
        logs[block].lsns.push_back(lsn);
      }
    });
  }
  std::thread flusher([&] {
    std::mt19937_64 rng(77);
    while (!stop.load()) std::ignore = manager->FlushBuffer(kFile, rng() % kBlocks);
  });

  int checkpoints = 0;
  for (int check = 0; check < kChecks; ++check) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    telepath::Lsn target = 0;
    {
      pausing.store(true);
      std::unique_lock<std::shared_mutex> paused(phase);
      pausing.store(false);
      target = next_lsn.load();
      const std::optional<telepath::Lsn> min_rec_lsn = manager->MinRecLsn();
      for (telepath::BlockId block = 0; block < kBlocks; ++block) {
        const telepath::Lsn on_disk = DiskLsn(verifier, block);
        std::lock_guard<std::mutex> guard(logs[block].latch);
        const auto &lsns = logs[block].lsns;
        const auto unwritten = std::upper_bound(lsns.begin(), lsns.end(), on_disk);
        if (unwritten == lsns.end()) continue;
        if (!min_rec_lsn.has_value()) Fail("no recLSN although a change is not on disk", block, *unwritten, 0);
        if (*min_rec_lsn > *unwritten) Fail("MinRecLsn above an unwritten change", block, *min_rec_lsn, *unwritten);
      }
    }

    // Writers keep going while the checkpoint runs.
    telepath::Status checkpointed = manager->CheckpointBuffers(target);
    if (!checkpointed.has_value()) {
      assert(checkpointed.error().code() == telepath::ErrorCode::kIoError);
      continue;
    }
    ++checkpoints;
    for (telepath::BlockId block = 0; block < kBlocks; ++block) {
      telepath::Lsn latest_before = 0;
      {
        std::lock_guard<std::mutex> guard(logs[block].latch);
        const auto &lsns = logs[block].lsns;
        auto below = std::lower_bound(lsns.begin(), lsns.end(), target);
        if (below != lsns.begin()) latest_before = *std::prev(below);
      }
      const telepath::Lsn on_disk = DiskLsn(verifier, block);
      if (on_disk < latest_before) Fail("checkpoint left a change below its LSN off disk", block, on_disk, latest_before);
    }
  }
  stop.store(true);
  for (auto &writer : writers) writer.join();
  flusher.join();
  assert(checkpoints > 0);

  faults->ClearFaults();
  assert(manager->CheckpointBuffers(next_lsn.load()).has_value());
  assert(!manager->MinRecLsn().has_value());
  for (telepath::BlockId block = 0; block < kBlocks; ++block) {
    std::lock_guard<std::mutex> guard(logs[block].latch);
    const telepath::Lsn last = logs[block].lsns.empty() ? 0 : logs[block].lsns.back();
    if (DiskLsn(verifier, block) != last) Fail("final page not on disk", block, DiskLsn(verifier, block), last);
  }
}

}  // namespace

int main() {
  Run("telepath_rec_lsn_stress_evicting", 8, telepath::ForegroundWriteback::kDirect, 0);
  Run("telepath_rec_lsn_stress_faults", 8, telepath::ForegroundWriteback::kQueued, 20);
  Run("telepath_rec_lsn_stress_roomy", 64, telepath::ForegroundWriteback::kDirect, 20);
  return 0;
}
