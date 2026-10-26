// Randomized PageCache operations checked against an in-memory model of each
// file's bytes and size: writes across block boundaries and far past the end,
// zero-length and short reads, truncation in both directions, and syncs, with
// a pool small enough to evict constantly. Threads own disjoint files. The
// cache is closed and reopened at the end and compared again. Set
// TELEPATH_PAGE_CACHE_SEED to vary the run.
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "buffer_test_support.h"
#include "telepath/facade/page_cache.h"
#include "telepath/io/posix_disk_backend.h"

namespace {

constexpr std::size_t kBlock = 4096;
constexpr std::size_t kThreads = 4;
constexpr telepath::FileId kFilesPerThread = 3;

auto Open(const std::filesystem::path &root, std::size_t pool_size, std::size_t readahead) -> std::unique_ptr<telepath::PageCache> {
  telepath::PageCacheOptions options;
  options.buffer.pool_size = pool_size;
  options.buffer.page_size = kBlock;
  options.max_readahead_blocks = readahead;
  auto cache = telepath::PageCache::Create(options, std::make_unique<telepath::PosixDiskBackend>(root.string(), telepath::BlockGeometry::Of<kBlock>()));
  assert(cache.has_value());
  return std::move(cache.value());
}

void Fail(telepath::FileId file, std::uint64_t seed, std::size_t step, const char *what) {
  std::fprintf(stderr, "file %u seed %llu step %zu: %s\n", file, static_cast<unsigned long long>(seed), step, what);
  std::abort();
}

void CheckFile(telepath::PageCache &cache, telepath::FileId file, const std::vector<std::byte> &model, std::uint64_t seed, std::size_t step) {
  auto size = cache.FileSize(file);
  if (!size.has_value() || size.value() != model.size()) Fail(file, seed, step, "file size differs from the model");
  std::vector<std::byte> contents(model.size() + kBlock);
  auto read = cache.Read(file, 0, contents);
  if (!read.has_value() || read.value() != model.size()) Fail(file, seed, step, "full read returned the wrong length");
  if (!std::equal(model.begin(), model.end(), contents.begin())) Fail(file, seed, step, "file contents differ from the model");
}

auto Offset(std::mt19937_64 &rng, std::uint64_t size) -> std::uint64_t {
  switch (rng() % 6) {
    case 0: return 0;
    case 1: return size;
    case 2: return (rng() % 8) * kBlock + (rng() % 2 == 0 ? 0 : kBlock - 1 - rng() % 3);
    case 3: return size + rng() % (3 * kBlock);
    case 4: return 40 * kBlock + rng() % kBlock;
    default: return rng() % (size + 1);
  }
}

auto Length(std::mt19937_64 &rng) -> std::size_t {
  switch (rng() % 5) {
    case 0: return 0;
    case 1: return 1 + rng() % 16;
    case 2: return kBlock;
    case 3: return 2 * kBlock + 1;
    default: return rng() % (3 * kBlock);
  }
}

void RunThread(telepath::PageCache &cache, std::vector<std::vector<std::byte>> &models, telepath::FileId first_file, std::uint64_t seed, std::size_t steps) {
  std::mt19937_64 rng(seed);
  for (std::size_t step = 0; step < steps; ++step) {
    const telepath::FileId local = static_cast<telepath::FileId>(rng() % kFilesPerThread);
    const telepath::FileId file = first_file + local;
    std::vector<std::byte> &model = models[local];
    const std::uint64_t kind = rng() % 100;
    if (kind < 40) {
      const std::uint64_t offset = Offset(rng, model.size());
      std::vector<std::byte> data(Length(rng));
      for (auto &byte : data) byte = static_cast<std::byte>(rng());
      if (!cache.Write(file, offset, data).has_value()) Fail(file, seed, step, "Write failed");
      if (!data.empty()) {
        if (model.size() < offset + data.size()) model.resize(offset + data.size());
        std::copy(data.begin(), data.end(), model.begin() + static_cast<std::ptrdiff_t>(offset));
      }
    } else if (kind < 75) {
      const std::uint64_t offset = Offset(rng, model.size());
      std::vector<std::byte> out(Length(rng), std::byte{0xEE});
      auto read = cache.Read(file, offset, out);
      if (!read.has_value()) Fail(file, seed, step, "Read failed");
      const std::size_t expected = offset >= model.size() ? 0 : std::min<std::size_t>(out.size(), model.size() - offset);
      if (read.value() != expected) Fail(file, seed, step, "Read returned the wrong length");
      if (!std::equal(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(expected), model.begin() + static_cast<std::ptrdiff_t>(std::min<std::uint64_t>(offset, model.size())))) Fail(file, seed, step, "Read returned the wrong bytes");
      if (std::any_of(out.begin() + static_cast<std::ptrdiff_t>(expected), out.end(), [](std::byte byte) { return byte != std::byte{0xEE}; })) Fail(file, seed, step, "Read wrote past the bytes it returned");
    } else if (kind < 87) {
      const std::uint64_t size = rng() % 2 == 0 ? rng() % (model.size() + 1) : model.size() + rng() % (4 * kBlock);
      if (auto truncated = cache.Truncate(file, size); !truncated.has_value()) {
        std::fprintf(stderr, "Truncate error %d: %s\n", static_cast<int>(truncated.error().code()), truncated.error().message());
        Fail(file, seed, step, "Truncate failed");
      }
      model.resize(size);
    } else if (kind < 92) {
      if (!cache.Sync(file).has_value()) Fail(file, seed, step, "Sync failed");
    } else {
      CheckFile(cache, file, model, seed, step);
    }
  }
}

void Run(const char *name, std::size_t pool_size, std::size_t readahead, std::uint64_t seed) {
  telepath::buffer_test_support::TestRootGuard root(name);
  std::vector<std::vector<std::vector<std::byte>>> models(kThreads, std::vector<std::vector<std::byte>>(kFilesPerThread));
  {
    auto cache = Open(root.path(), pool_size, readahead);
    std::vector<std::thread> threads;
    for (std::size_t thread = 0; thread < kThreads; ++thread) {
      threads.emplace_back([&, thread] { RunThread(*cache, models[thread], static_cast<telepath::FileId>(1 + thread * kFilesPerThread), seed * 31 + thread, 1500); });
    }
    for (auto &thread : threads) thread.join();
    assert(cache->Close().has_value());
  }
  auto reopened = Open(root.path(), pool_size, readahead);
  for (std::size_t thread = 0; thread < kThreads; ++thread) {
    for (telepath::FileId local = 0; local < kFilesPerThread; ++local) {
      const auto file = static_cast<telepath::FileId>(1 + thread * kFilesPerThread + local);
      CheckFile(*reopened, file, models[thread][local], seed, SIZE_MAX);
    }
  }
}

}  // namespace

int main() {
  std::uint64_t seed = 7;
  if (const char *value = std::getenv("TELEPATH_PAGE_CACHE_SEED")) seed = std::strtoull(value, nullptr, 10);
  Run("telepath_page_cache_model_tiny_pool", 4, 32, seed);
  Run("telepath_page_cache_model_no_readahead", 16, 0, seed + 1);
  Run("telepath_page_cache_model_roomy", 128, 8, seed + 2);
  return 0;
}
