#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "buffer_test_support.h"
#include "telepath/facade/page_cache.h"
#include "telepath/io/posix_disk_backend.h"

namespace {

constexpr std::size_t kBlock = 4096;

// Forwards to a PosixDiskBackend and records reads (first block and count)
// and sync requests.
class RecordingBackend : public telepath::DiskBackend {
 public:
  explicit RecordingBackend(const std::string &root) : inner_(root, telepath::BlockGeometry::Of<kBlock>()) {}

  auto Submit(std::span<const telepath::DiskRequest> requests, telepath::DiskSubmitMode mode) -> telepath::Result<std::size_t> override {
    {
      std::lock_guard<std::mutex> guard(latch_);
      for (const auto &request : requests) {
        if (request.operation == telepath::DiskOperation::kRead) reads_.push_back({request.tag.block_id, request.block_count});
        if (request.operation == telepath::DiskOperation::kSync) ++syncs_;
      }
    }
    return inner_.Submit(requests, mode);
  }

  auto Reap(std::span<telepath::DiskCompletion> out, bool wait) -> telepath::Result<std::size_t> override { return inner_.Reap(out, wait); }
  void Shutdown() override { inner_.Shutdown(); }
  auto GetCapabilities() const -> telepath::DiskBackendCapabilities override { return inner_.GetCapabilities(); }
  auto GetFileSize(telepath::FileId file_id) -> telepath::Result<std::uint64_t> override { return inner_.GetFileSize(file_id); }
  auto SetFileSize(telepath::FileId file_id, std::uint64_t size) -> telepath::Status override { return inner_.SetFileSize(file_id, size); }

  struct Read {
    telepath::BlockId first;
    std::uint32_t count;
  };

  auto reads() -> std::vector<Read> {
    std::lock_guard<std::mutex> guard(latch_);
    return reads_;
  }

  void ClearReads() {
    std::lock_guard<std::mutex> guard(latch_);
    reads_.clear();
  }

  int syncs() {
    std::lock_guard<std::mutex> guard(latch_);
    return syncs_;
  }

 private:
  telepath::PosixDiskBackend inner_;
  std::mutex latch_;
  std::vector<Read> reads_;
  int syncs_{0};
};

struct Fixture {
  explicit Fixture(const std::filesystem::path &path, std::size_t pool_size = 16, std::size_t max_readahead = 32) {
    auto owned = std::make_unique<RecordingBackend>(path.string());
    backend = owned.get();
    telepath::PageCacheOptions options;
    options.buffer.pool_size = pool_size;
    options.buffer.page_size = kBlock;
    options.max_readahead_blocks = max_readahead;
    auto created = telepath::PageCache::Create(options, std::move(owned));
    assert(created.has_value());
    cache = std::move(created.value());
  }

  RecordingBackend *backend{nullptr};
  std::unique_ptr<telepath::PageCache> cache;
};

auto Pattern(std::size_t length, std::uint8_t seed) -> std::vector<std::byte> {
  std::vector<std::byte> bytes(length);
  for (std::size_t i = 0; i < length; ++i) bytes[i] = static_cast<std::byte>((i * 31 + seed) & 0xFF);
  return bytes;
}

auto ReadAll(telepath::PageCache &cache, telepath::FileId file, std::uint64_t offset, std::size_t length) -> std::vector<std::byte> {
  std::vector<std::byte> out(length);
  auto read = cache.Read(file, offset, out);
  assert(read.has_value());
  out.resize(read.value());
  return out;
}

auto BackingSize(const std::filesystem::path &root, telepath::FileId file) -> std::uint64_t {
  return std::filesystem::file_size(root / ("file_" + std::to_string(file) + ".tp"));
}

void ExpectUnalignedWritesReadBackAndExtendSize() {
  telepath::buffer_test_support::TestRootGuard root("telepath_page_cache_unaligned");
  Fixture fixture(root.path());
  auto &cache = *fixture.cache;
  assert(cache.FileSize(1).value() == 0);
  assert(ReadAll(cache, 1, 0, 10).empty());

  const auto data = Pattern(3 * kBlock + 123, 7);
  assert(cache.Write(1, 1000, data).has_value());
  assert(cache.FileSize(1).value() == 1000 + data.size());
  assert(ReadAll(cache, 1, 1000, data.size()) == data);

  const auto head = ReadAll(cache, 1, 0, 1000);
  assert(head.size() == 1000);
  for (std::byte value : head) assert(value == std::byte{0});

  const auto tail = ReadAll(cache, 1, 1000 + data.size() - 5, 100);
  assert(tail.size() == 5);
  assert(ReadAll(cache, 1, 1000 + data.size(), 10).empty());
}

void ExpectFullBlockWritesSkipDiskReads() {
  telepath::buffer_test_support::TestRootGuard root("telepath_page_cache_zero_and_lock");
  Fixture fixture(root.path());
  auto &cache = *fixture.cache;
  assert(cache.Write(2, 0, Pattern(4 * kBlock, 1)).has_value());
  assert(fixture.backend->reads().empty());
  assert(cache.Write(2, 4 * kBlock + 10, Pattern(20, 2)).has_value());
  assert(fixture.backend->reads().size() == 1);
}

void ExpectSyncPersistsDataAndExactSize() {
  telepath::buffer_test_support::TestRootGuard root("telepath_page_cache_sync");
  const auto data = Pattern(2 * kBlock + 77, 9);
  {
    Fixture fixture(root.path());
    assert(fixture.cache->Write(3, 0, data).has_value());
    const int syncs_before = fixture.backend->syncs();
    assert(fixture.cache->Sync(3).has_value());
    assert(fixture.backend->syncs() == syncs_before + 1);
    assert(BackingSize(root.path(), 3) == data.size());
  }
  Fixture reopened(root.path());
  assert(reopened.cache->FileSize(3).value() == data.size());
  assert(ReadAll(*reopened.cache, 3, 0, data.size() + 100) == data);
}

void ExpectTruncateDiscardsAndExtensionReadsZeros() {
  telepath::buffer_test_support::TestRootGuard root("telepath_page_cache_truncate");
  Fixture fixture(root.path());
  auto &cache = *fixture.cache;
  const auto data = Pattern(3 * kBlock, 3);
  assert(cache.Write(4, 0, data).has_value());
  assert(cache.Sync(4).has_value());

  assert(cache.Truncate(4, kBlock + 100).has_value());
  assert(cache.FileSize(4).value() == kBlock + 100);
  auto kept = ReadAll(cache, 4, 0, 3 * kBlock);
  assert(kept.size() == kBlock + 100);
  assert(std::equal(kept.begin(), kept.end(), data.begin()));

  assert(cache.Truncate(4, 3 * kBlock).has_value());
  const auto extended = ReadAll(cache, 4, kBlock + 100, 3 * kBlock);
  assert(extended.size() == 2 * kBlock - 100);
  for (std::byte value : extended) assert(value == std::byte{0});

  assert(cache.Sync(4).has_value());
  assert(BackingSize(root.path(), 4) == 3 * kBlock);
}

void ExpectSequentialReadsTriggerReadahead() {
  telepath::buffer_test_support::TestRootGuard root("telepath_page_cache_readahead");
  {
    Fixture writer(root.path(), 64);
    assert(writer.cache->Write(5, 0, Pattern(48 * kBlock, 5)).has_value());
    assert(writer.cache->Close().has_value());
  }
  Fixture fixture(root.path(), 64, 16);
  auto &cache = *fixture.cache;
  for (std::size_t block = 0; block < 32; ++block) assert(ReadAll(cache, 5, block * kBlock, kBlock).size() == kBlock);

  bool multi_block = false;
  std::size_t blocks_read = 0;
  for (const auto &read : fixture.backend->reads()) {
    multi_block = multi_block || read.count > 1;
    blocks_read += read.count;
  }
  assert(multi_block);
  assert(blocks_read >= 32 && blocks_read <= 48);

  fixture.backend->ClearReads();
  for (std::size_t block : {40, 33, 45}) assert(ReadAll(cache, 5, block * kBlock, 10).size() == 10);
  for (const auto &read : fixture.backend->reads()) assert(read.count == 1);
}

void ExpectConcurrentWritersAndReaders() {
  telepath::buffer_test_support::TestRootGuard root("telepath_page_cache_concurrent");
  Fixture fixture(root.path(), 8);
  auto &cache = *fixture.cache;
  constexpr int kThreads = 4;
  constexpr std::size_t kChunk = kBlock + 512;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t]() {
      const auto data = Pattern(kChunk, static_cast<std::uint8_t>(t));
      for (int round = 0; round < 50; ++round) {
        const std::uint64_t offset = (static_cast<std::uint64_t>(round) * kThreads + t) * kChunk;
        assert(cache.Write(6, offset, data).has_value());
        assert(ReadAll(cache, 6, offset, kChunk) == data);
      }
    });
  }
  for (auto &thread : threads) thread.join();
  assert(cache.FileSize(6).value() == 50 * kThreads * kChunk);
  assert(cache.Close().has_value());

  Fixture reopened(root.path(), 8);
  for (int t = 0; t < kThreads; ++t) {
    const auto data = Pattern(kChunk, static_cast<std::uint8_t>(t));
    for (int round = 0; round < 50; ++round) {
      assert(ReadAll(*reopened.cache, 6, (static_cast<std::uint64_t>(round) * kThreads + t) * kChunk, kChunk) == data);
    }
  }
}

void ExpectCloseRejectsLaterCalls() {
  telepath::buffer_test_support::TestRootGuard root("telepath_page_cache_close");
  Fixture fixture(root.path());
  assert(fixture.cache->Write(7, 0, Pattern(100, 1)).has_value());
  assert(fixture.cache->Close().has_value());
  assert(BackingSize(root.path(), 7) == 100);
  auto refused = fixture.cache->FileSize(7);
  assert(!refused.has_value() && refused.error().code() == telepath::ErrorCode::kUnavailable);
  assert(fixture.cache->Close().has_value());
}

}  // namespace

int main() {
  ExpectUnalignedWritesReadBackAndExtendSize();
  ExpectFullBlockWritesSkipDiskReads();
  ExpectSyncPersistsDataAndExactSize();
  ExpectTruncateDiscardsAndExtensionReadsZeros();
  ExpectSequentialReadsTriggerReadahead();
  ExpectConcurrentWritersAndReaders();
  ExpectCloseRejectsLaterCalls();
  return 0;
}
