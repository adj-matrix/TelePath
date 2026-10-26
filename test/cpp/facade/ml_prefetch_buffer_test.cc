#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "buffer_test_support.h"
#include "disk_io_helpers.h"
#include "telepath/facade/ml_prefetch_buffer.h"
#include "telepath/io/posix_disk_backend.h"

namespace {

constexpr std::size_t kBlock = 4096;
constexpr telepath::FileId kFile = 2;
constexpr telepath::BlockId kBlocks = 24;

// Forwards to a PosixDiskBackend and counts the blocks it reads.
class CountingBackend : public telepath::DiskBackend {
 public:
  explicit CountingBackend(const std::string &root) : inner_(root, telepath::BlockGeometry::Of<kBlock>()) {}

  auto Submit(std::span<const telepath::DiskRequest> requests, telepath::DiskSubmitMode mode) -> telepath::Result<std::size_t> override {
    for (const auto &request : requests) {
      if (request.operation == telepath::DiskOperation::kRead) blocks_read_.fetch_add(request.block_count);
    }
    return inner_.Submit(requests, mode);
  }

  auto Reap(std::span<telepath::DiskCompletion> out, bool wait) -> telepath::Result<std::size_t> override { return inner_.Reap(out, wait); }
  void Shutdown() override { inner_.Shutdown(); }
  auto GetCapabilities() const -> telepath::DiskBackendCapabilities override { return inner_.GetCapabilities(); }

  auto blocks_read() const -> std::size_t { return blocks_read_.load(); }

 private:
  telepath::PosixDiskBackend inner_;
  std::atomic<std::size_t> blocks_read_{0};
};

void SeedDataset(const std::filesystem::path &root) {
  telepath::PosixDiskBackend backend(root.string(), telepath::BlockGeometry::Of<kBlock>());
  std::array<std::byte, kBlock> page{};
  for (telepath::BlockId block = 0; block < kBlocks; ++block) {
    page[0] = static_cast<std::byte>(block);
    page[kBlock - 1] = static_cast<std::byte>(0xF0 ^ block);
    assert(telepath::test_support::WriteBlock(backend, {kFile, block}, page).has_value());
  }
}

struct Fixture {
  Fixture(const char *name, std::size_t pool_size, std::size_t prefetch_depth) : root(name) {
    SeedDataset(root.path());
    auto owned = std::make_unique<CountingBackend>(root.path().string());
    backend = owned.get();
    telepath::MLPrefetchBufferOptions options;
    options.buffer.pool_size = pool_size;
    options.buffer.page_size = kBlock;
    options.prefetch_depth = prefetch_depth;
    auto created = telepath::MLPrefetchBuffer::Create(options, std::move(owned));
    assert(created.has_value());
    cache = std::move(created.value());
  }

  telepath::buffer_test_support::TestRootGuard root;
  CountingBackend *backend{nullptr};
  std::unique_ptr<telepath::MLPrefetchBuffer> cache;
};

void ExpectBlock(const telepath::SharedBufferHandle &handle, telepath::BlockId block) {
  assert(handle.data()[0] == static_cast<std::byte>(block));
  assert(handle.data()[kBlock - 1] == static_cast<std::byte>(0xF0 ^ block));
}

auto ShuffledPlan(std::size_t length, unsigned seed) -> std::vector<telepath::BufferTag> {
  std::mt19937 rng(seed);
  std::vector<telepath::BufferTag> plan;
  for (std::size_t i = 0; i < length; ++i) plan.push_back({kFile, rng() % kBlocks});
  return plan;
}

// Misses of Belady's optimal policy on `plan` with `capacity` frames.
auto BeladyMisses(const std::vector<telepath::BufferTag> &plan, std::size_t capacity) -> std::size_t {
  std::set<telepath::BlockId> resident;
  std::size_t misses = 0;
  for (std::size_t i = 0; i < plan.size(); ++i) {
    const telepath::BlockId block = plan[i].block_id;
    if (resident.count(block) != 0) continue;
    ++misses;
    if (resident.size() == capacity) {
      telepath::BlockId victim = 0;
      std::size_t furthest = 0;
      for (telepath::BlockId candidate : resident) {
        std::size_t next = plan.size();
        for (std::size_t j = i + 1; j < plan.size(); ++j) {
          if (plan[j].block_id == candidate) {
            next = j;
            break;
          }
        }
        if (next >= furthest) {
          furthest = next;
          victim = candidate;
        }
      }
      resident.erase(victim);
    }
    resident.insert(block);
  }
  return misses;
}

void ExpectBeladyMissCount() {
  Fixture fixture("telepath_ml_prefetch_belady", 6, 0);
  const auto plan = ShuffledPlan(200, 7);
  assert(fixture.cache->SetPlan(plan).has_value());
  for (std::size_t position = 0; position < plan.size(); ++position) {
    auto handle = fixture.cache->Fetch(position);
    assert(handle.has_value());
    ExpectBlock(*handle, plan[position].block_id);
  }
  assert(fixture.backend->blocks_read() == BeladyMisses(plan, 6));
}

void ExpectNextHandsOutEveryPositionOnce() {
  Fixture fixture("telepath_ml_prefetch_next", 8, 4);
  const auto plan = ShuffledPlan(300, 11);
  assert(fixture.cache->SetPlan(plan).has_value());

  std::mutex latch;
  std::vector<int> seen(plan.size(), 0);
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&]() {
      while (true) {
        auto next = fixture.cache->Next();
        assert(next.has_value());
        if (!next->has_value()) return;
        ExpectBlock((*next)->handle, plan[(*next)->position].block_id);
        std::lock_guard<std::mutex> guard(latch);
        ++seen[(*next)->position];
      }
    });
  }
  for (auto &thread : threads) thread.join();
  assert(std::all_of(seen.begin(), seen.end(), [](int count) { return count == 1; }));
  auto exhausted = fixture.cache->Next();
  assert(exhausted.has_value() && !exhausted->has_value());
}

void ExpectDeterministicPartitionWithFetch() {
  Fixture fixture("telepath_ml_prefetch_partition", 8, 4);
  const auto plan = ShuffledPlan(240, 13);
  assert(fixture.cache->SetPlan(plan).has_value());
  constexpr std::size_t kWorkers = 3;
  std::vector<std::thread> threads;
  for (std::size_t worker = 0; worker < kWorkers; ++worker) {
    threads.emplace_back([&, worker]() {
      for (std::size_t position = worker; position < plan.size(); position += kWorkers) {
        auto handle = fixture.cache->Fetch(position);
        assert(handle.has_value());
        ExpectBlock(*handle, plan[position].block_id);
      }
    });
  }
  for (auto &thread : threads) thread.join();
}

void ExpectPrefetchReadsAheadAndNewPlanRestarts() {
  Fixture fixture("telepath_ml_prefetch_ahead", 16, 6);
  std::vector<telepath::BufferTag> plan;
  for (telepath::BlockId block = 0; block < 12; ++block) plan.push_back({kFile, block});
  assert(fixture.cache->SetPlan(plan).has_value());
  assert(fixture.cache->Fetch(0).has_value());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (fixture.backend->blocks_read() < 7) {
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::yield();
  }

  auto outside = fixture.cache->Fetch(plan.size());
  assert(!outside.has_value() && outside.error().code() == telepath::ErrorCode::kInvalidArgument);

  std::reverse(plan.begin(), plan.end());
  assert(fixture.cache->SetPlan(plan).has_value());
  auto first = fixture.cache->Next();
  assert(first.has_value() && first->has_value() && (*first)->position == 0);
  ExpectBlock((*first)->handle, 11);
}

void ExpectCloseRejectsLaterCalls() {
  Fixture fixture("telepath_ml_prefetch_close", 4, 2);
  const std::vector<telepath::BufferTag> plan{{kFile, 1}, {kFile, 2}};
  assert(fixture.cache->SetPlan(plan).has_value());
  {
    auto held = fixture.cache->Fetch(0);
    assert(held.has_value());
    auto refused = fixture.cache->Close();
    assert(!refused.has_value() && refused.error().code() == telepath::ErrorCode::kInvalidArgument);
  }
  assert(fixture.cache->Close().has_value());
  auto after = fixture.cache->Fetch(1);
  assert(!after.has_value() && after.error().code() == telepath::ErrorCode::kUnavailable);
}

}  // namespace

int main() {
  ExpectBeladyMissCount();
  ExpectNextHandsOutEveryPositionOnce();
  ExpectDeterministicPartitionWithFetch();
  ExpectPrefetchReadsAheadAndNewPlanRestarts();
  ExpectCloseRejectsLaterCalls();
  return 0;
}
