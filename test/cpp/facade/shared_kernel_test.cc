// Facades attached to one BufferManager, each in its own namespace: they see
// one coherent copy of every page, their quotas isolate them, and closing one
// facade leaves the manager and the other facades running.
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "buffer_test_support.h"
#include "disk_io_helpers.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/facade/ml_prefetch_buffer.h"
#include "telepath/facade/page_cache.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

constexpr std::size_t kBlock = 4096;
constexpr std::size_t kPoolSize = 16;
constexpr telepath::FileId kFile = 3;

// Forwards to a PosixDiskBackend and counts block reads.
class CountingBackend : public telepath::DiskBackend {
 public:
  explicit CountingBackend(const std::string &root) : inner_(root, telepath::BlockGeometry::Of<kBlock>()) {}
  auto Submit(std::span<const telepath::DiskRequest> requests, telepath::DiskSubmitMode mode) -> telepath::Result<std::size_t> override {
    for (const auto &request : requests) {
      if (request.operation == telepath::DiskOperation::kRead) reads_.fetch_add(request.block_count);
    }
    return inner_.Submit(requests, mode);
  }
  auto Reap(std::span<telepath::DiskCompletion> out, bool wait) -> telepath::Result<std::size_t> override { return inner_.Reap(out, wait); }
  void Shutdown() override { inner_.Shutdown(); }
  auto GetCapabilities() const -> telepath::DiskBackendCapabilities override { return inner_.GetCapabilities(); }
  auto GetFileSize(telepath::FileId file_id) -> telepath::Result<std::uint64_t> override { return inner_.GetFileSize(file_id); }
  auto SetFileSize(telepath::FileId file_id, std::uint64_t size) -> telepath::Status override { return inner_.SetFileSize(file_id, size); }
  auto reads() const -> std::uint64_t { return reads_.load(); }

 private:
  telepath::PosixDiskBackend inner_;
  std::atomic<std::uint64_t> reads_{0};
};

struct Kernel {
  explicit Kernel(const char *name, std::size_t pool_size = kPoolSize) : root(name) {
    auto owned = std::make_unique<CountingBackend>(root.path().string());
    backend = owned.get();
    telepath::BufferManagerOptions options;
    options.pool_size = pool_size;
    options.page_size = kBlock;
    auto created = telepath::BufferManager::Create(options, std::move(owned), telepath::MakeClockSweepReplacer(pool_size), telepath::MakeNoOpTelemetrySink());
    assert(created.has_value());
    manager = std::move(created.value());
  }

  auto Cache(const char *name, std::size_t min_frames = 0, std::size_t max_frames = 0) -> std::unique_ptr<telepath::PageCache> {
    telepath::NamespaceOptions space;
    space.name = name;
    space.min_frames = min_frames;
    space.max_frames = max_frames;
    telepath::PageCacheOptions options;
    options.max_readahead_blocks = 0;
    auto created = telepath::PageCache::Create(options, *manager, std::move(space));
    assert(created.has_value());
    return std::move(created.value());
  }

  auto Dataset(const char *name, std::size_t min_frames = 0, std::size_t max_frames = 0, std::size_t depth = 2) -> std::unique_ptr<telepath::MLPrefetchBuffer> {
    telepath::NamespaceOptions space;
    space.name = name;
    space.min_frames = min_frames;
    space.max_frames = max_frames;
    telepath::MLPrefetchBufferOptions options;
    options.prefetch_depth = depth;
    auto created = telepath::MLPrefetchBuffer::Create(options, *manager, std::move(space));
    assert(created.has_value());
    return std::move(created.value());
  }

  telepath::buffer_test_support::TestRootGuard root;
  CountingBackend *backend{nullptr};
  std::unique_ptr<telepath::BufferManager> manager;
};

auto Block(std::uint8_t value) -> std::vector<std::byte> { return std::vector<std::byte>(kBlock, static_cast<std::byte>(value)); }

// What one facade wrote, the other reads from the same copy, without the disk
// and before any writeback.
void ExpectFacadesShareOneCopy() {
  Kernel kernel("telepath_shared_kernel_copy");
  auto cache = kernel.Cache("files");
  auto dataset = kernel.Dataset("training");
  for (std::uint8_t block = 0; block < 4; ++block) assert(cache->Write(kFile, block * kBlock, Block(0x40 + block)).has_value());

  const std::uint64_t reads = kernel.backend->reads();
  const std::vector<telepath::BufferTag> plan{{kFile, 2}, {kFile, 0}, {kFile, 3}};
  assert(dataset->SetPlan(plan).has_value());
  for (std::size_t position = 0; position < plan.size(); ++position) {
    auto handle = dataset->Fetch(position);
    assert(handle.has_value() && handle->data()[0] == static_cast<std::byte>(0x40 + plan[position].block_id));
  }
  assert(kernel.backend->reads() == reads);
  assert(kernel.manager->ExportSnapshot().dirty_page_count == 4);
}

// A training pass over a dataset much larger than the pool does not evict the
// pages a file cache's guarantee protects.
void ExpectQuotasIsolateFacades() {
  Kernel kernel("telepath_shared_kernel_quota");
  auto cache = kernel.Cache("files", 6);
  auto dataset = kernel.Dataset("training", 0, 8);
  for (std::uint8_t block = 0; block < 6; ++block) assert(cache->Write(kFile, block * kBlock, Block(block)).has_value());

  std::vector<telepath::BufferTag> plan;
  for (telepath::BlockId block = 100; block < 400; ++block) plan.push_back({kFile + 1, block});
  assert(dataset->SetPlan(plan).has_value());
  while (true) {
    auto next = dataset->Next();
    assert(next.has_value());
    if (!next->has_value()) break;
  }

  const std::uint64_t reads = kernel.backend->reads();
  std::array<std::byte, kBlock> out{};
  for (std::uint8_t block = 0; block < 6; ++block) {
    assert(cache->Read(kFile, block * kBlock, out).value() == kBlock && out[0] == static_cast<std::byte>(block));
  }
  assert(kernel.backend->reads() == reads);
}

// Closing an attached facade makes it durable and stops it, but the manager
// and the other facade keep working; the manager outlives both.
void ExpectClosingOneFacadeLeavesTheRest() {
  Kernel kernel("telepath_shared_kernel_close");
  auto cache = kernel.Cache("files");
  auto dataset = kernel.Dataset("training");
  assert(cache->Write(kFile, 0, Block(0x77)).has_value());
  assert(cache->Write(kFile, kBlock, std::vector<std::byte>(100, std::byte{0x78})).has_value());
  assert(cache->Close().has_value());

  auto after = cache->FileSize(kFile);
  assert(!after.has_value() && after.error().code() == telepath::ErrorCode::kUnavailable);
  assert(kernel.backend->GetFileSize(kFile).value() == kBlock + 100);
  telepath::PosixDiskBackend verifier(kernel.root.path().string(), telepath::BlockGeometry::Of<kBlock>());
  std::array<std::byte, kBlock> page{};
  assert(telepath::test_support::ReadBlock(verifier, {kFile, 0}, page).has_value() && page[0] == std::byte{0x77});

  const std::vector<telepath::BufferTag> plan{{kFile, 0}};
  assert(dataset->SetPlan(plan).has_value());
  auto handle = dataset->Fetch(0);
  assert(handle.has_value() && handle->data()[0] == std::byte{0x77});
  handle->Reset();
  assert(dataset->Close().has_value());
  assert(kernel.manager->ReadBuffer(kFile, 0).has_value());
  assert(kernel.manager->Close().has_value());
}

void ExpectInvalidAttachmentsAreRejected() {
  Kernel kernel("telepath_shared_kernel_invalid");
  telepath::NamespaceOptions own_replacer;
  own_replacer.name = "training";
  own_replacer.replacer = telepath::MakeLruReplacer(kPoolSize);
  auto refused = telepath::MLPrefetchBuffer::Create({}, *kernel.manager, std::move(own_replacer));
  assert(!refused.has_value() && refused.error().code() == telepath::ErrorCode::kInvalidArgument);

  auto first = kernel.Cache("files");
  telepath::NamespaceOptions duplicate;
  duplicate.name = "files";
  auto twice = telepath::PageCache::Create({}, *kernel.manager, std::move(duplicate));
  assert(!twice.has_value() && twice.error().code() == telepath::ErrorCode::kInvalidArgument);
}

// A file cache writer and dataset readers in separate namespaces of a pool far
// smaller than their data run concurrently; every byte read matches.
void ExpectConcurrentFacadesStayConsistent() {
  Kernel kernel("telepath_shared_kernel_concurrent", 12);
  auto cache = kernel.Cache("files", 3);
  auto dataset = kernel.Dataset("training", 0, 6, 3);
  constexpr telepath::BlockId kDatasetBlocks = 40;
  for (telepath::BlockId block = 0; block < kDatasetBlocks; ++block) assert(cache->Write(kFile + 1, block * kBlock, Block(static_cast<std::uint8_t>(block))).has_value());
  assert(cache->Sync(kFile + 1).has_value());

  std::vector<telepath::BufferTag> plan;
  for (int round = 0; round < 5; ++round) {
    for (telepath::BlockId block = 0; block < kDatasetBlocks; ++block) plan.push_back({kFile + 1, (block * 7 + round) % kDatasetBlocks});
  }
  assert(dataset->SetPlan(plan).has_value());

  std::atomic<bool> stop{false};
  std::thread writer([&] {
    std::uint8_t generation = 0;
    while (!stop.load()) {
      ++generation;
      for (telepath::BlockId block = 0; block < 8; ++block) assert(cache->Write(kFile, block * kBlock, Block(generation)).has_value());
      std::array<std::byte, kBlock> out{};
      for (telepath::BlockId block = 0; block < 8; ++block) {
        assert(cache->Read(kFile, block * kBlock, out).value() == kBlock && out[0] == static_cast<std::byte>(generation));
      }
    }
  });
  std::vector<std::thread> readers;
  for (int reader = 0; reader < 3; ++reader) {
    readers.emplace_back([&] {
      while (true) {
        auto next = dataset->Next();
        if (!next.has_value()) {
          assert(next.error().code() == telepath::ErrorCode::kResourceExhausted);
          continue;
        }
        if (!next->has_value()) return;
        const auto &planned = next->value();
        assert(planned.handle.data()[0] == static_cast<std::byte>(plan[planned.position].block_id));
      }
    });
  }
  for (auto &reader : readers) reader.join();
  stop.store(true);
  writer.join();
}

}  // namespace

int main() {
  ExpectFacadesShareOneCopy();
  ExpectQuotasIsolateFacades();
  ExpectClosingOneFacadeLeavesTheRest();
  ExpectInvalidAttachmentsAreRejected();
  ExpectConcurrentFacadesStayConsistent();
  return 0;
}
