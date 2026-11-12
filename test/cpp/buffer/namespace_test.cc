// Namespaces: policy and accounting domains inside one cache. A page keeps
// its identity (file, block) whichever namespace reads it, and belongs to the
// namespace that loaded it; each namespace has its own replacer, quota, and
// pre-write hook.
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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
#include "telepath/buffer/buffer_manager.h"
#include "telepath/buffer/read_buffers_operation.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/options/buffer_manager_options.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

constexpr std::size_t kPageSize = 4096;
constexpr std::size_t kPoolSize = 8;
constexpr telepath::FileId kFile = 1;

// Forwards to a PosixDiskBackend and counts reads.
class CountingBackend : public telepath::DiskBackend {
 public:
  explicit CountingBackend(const std::string &root) : inner_(root, telepath::BlockGeometry::Of<kPageSize>()) {}
  auto Submit(std::span<const telepath::DiskRequest> requests, telepath::DiskSubmitMode mode) -> telepath::Result<std::size_t> override {
    for (const auto &request : requests) {
      if (request.operation == telepath::DiskOperation::kRead) reads_.fetch_add(request.block_count);
    }
    return inner_.Submit(requests, mode);
  }
  auto Reap(std::span<telepath::DiskCompletion> out, bool wait) -> telepath::Result<std::size_t> override { return inner_.Reap(out, wait); }
  void Shutdown() override { inner_.Shutdown(); }
  auto GetCapabilities() const -> telepath::DiskBackendCapabilities override { return inner_.GetCapabilities(); }
  auto reads() const -> std::uint64_t { return reads_.load(); }

 private:
  telepath::PosixDiskBackend inner_;
  std::atomic<std::uint64_t> reads_{0};
};

struct Fixture {
  explicit Fixture(const char *name, std::size_t pool_size = kPoolSize, telepath::BufferManagerOptions options = {}) : root(name) {
    auto owned = std::make_unique<CountingBackend>(root.path().string());
    backend = owned.get();
    options.pool_size = pool_size;
    options.page_size = kPageSize;
    auto created = telepath::BufferManager::Create(options, std::move(owned), telepath::MakeClockSweepReplacer(pool_size), telepath::MakeNoOpTelemetrySink());
    assert(created.has_value());
    manager = std::move(created.value());
  }

  auto Namespace(const char *name, std::size_t min_frames = 0, std::size_t max_frames = 0, telepath::PreWriteHook hook = {}) -> telepath::BufferNamespace {
    telepath::NamespaceOptions options;
    options.name = name;
    options.replacer = telepath::MakeLruReplacer(manager->pool_size());
    options.min_frames = min_frames;
    options.max_frames = max_frames;
    options.pre_write_hook = std::move(hook);
    auto created = manager->CreateNamespace(std::move(options));
    assert(created.has_value());
    return created.value();
  }

  // Resident frames per namespace.
  auto Occupancy() -> std::map<telepath::NamespaceId, std::size_t> {
    std::map<telepath::NamespaceId, std::size_t> owned;
    for (const auto &frame : manager->ExportSnapshot().frames) {
      if (frame.is_valid) ++owned[frame.namespace_id];
    }
    return owned;
  }

  auto OwnerOf(telepath::BlockId block) -> telepath::NamespaceId {
    for (const auto &frame : manager->ExportSnapshot().frames) {
      if (frame.is_valid && frame.tag == telepath::BufferTag{kFile, block}) return frame.namespace_id;
    }
    std::abort();
  }

  telepath::buffer_test_support::TestRootGuard root;
  CountingBackend *backend{nullptr};
  std::unique_ptr<telepath::BufferManager> manager;
};

void Touch(telepath::BufferNamespace &space, telepath::BlockId block) {
  auto handle = space.ReadBuffer(kFile, block);
  assert(handle.has_value());
}

void ExpectInvalidNamespacesAreRejected() {
  Fixture fixture("telepath_namespace_invalid");
  auto expect = [&](telepath::NamespaceOptions options, telepath::ErrorCode code) {
    auto created = fixture.manager->CreateNamespace(std::move(options));
    assert(!created.has_value() && created.error().code() == code);
  };
  auto valid = [&](const char *name) {
    telepath::NamespaceOptions options;
    options.name = name;
    options.replacer = telepath::MakeClockReplacer(kPoolSize);
    return options;
  };
  expect(valid(""), telepath::ErrorCode::kInvalidArgument);
  expect(valid("default"), telepath::ErrorCode::kInvalidArgument);
  telepath::NamespaceOptions no_replacer = valid("no_replacer");
  no_replacer.replacer = nullptr;
  expect(std::move(no_replacer), telepath::ErrorCode::kInvalidArgument);
  telepath::NamespaceOptions inverted = valid("inverted");
  inverted.min_frames = 4;
  inverted.max_frames = 2;
  expect(std::move(inverted), telepath::ErrorCode::kInvalidArgument);
  telepath::NamespaceOptions greedy = valid("greedy");
  greedy.min_frames = kPoolSize + 1;
  expect(std::move(greedy), telepath::ErrorCode::kInvalidArgument);

  auto first = fixture.Namespace("first", 5);
  assert(first.id() != telepath::kDefaultNamespace);
  expect(valid("first"), telepath::ErrorCode::kInvalidArgument);
  // Guarantees together may not exceed the pool.
  telepath::NamespaceOptions over = valid("over");
  over.min_frames = kPoolSize - 4;
  expect(std::move(over), telepath::ErrorCode::kInvalidArgument);

  std::vector<telepath::BufferNamespace> spaces;
  for (std::size_t index = 0;; ++index) {
    auto created = fixture.manager->CreateNamespace(valid(("n" + std::to_string(index)).c_str()));
    if (!created.has_value()) {
      assert(created.error().code() == telepath::ErrorCode::kResourceExhausted);
      break;
    }
    spaces.push_back(created.value());
  }
  assert(spaces.size() + 2 == telepath::kMaxNamespaces);
}

void ExpectCrossProcessNamespacesAreNotSupportedYet() {
  telepath::BufferManagerOptions options;
  options.shared_memory.name = "/telepath_namespace_shared_" + std::to_string(getpid());
  Fixture fixture("telepath_namespace_shared", kPoolSize, options);
  telepath::NamespaceOptions space;
  space.name = "db";
  space.replacer = telepath::MakeClockSweepReplacer(kPoolSize);
  auto created = fixture.manager->CreateNamespace(std::move(space));
  assert(!created.has_value() && created.error().code() == telepath::ErrorCode::kUnavailable);
}

// A page belongs to the namespace that loaded it; another namespace reading it
// hits the same copy without a disk read and without taking it over.
void ExpectPagesAreSharedAndOwnedByTheLoader() {
  Fixture fixture("telepath_namespace_shared_pages");
  auto db = fixture.Namespace("db");
  auto train = fixture.Namespace("train");
  {
    auto handle = db.ReadBufferExclusive(kFile, 0);
    assert(handle.has_value());
    handle->mutable_data()[0] = std::byte{0x5A};
  }
  Touch(train, 1);
  const std::uint64_t reads = fixture.backend->reads();
  auto seen = train.ReadBuffer(kFile, 0);
  assert(seen.has_value() && seen->data()[0] == std::byte{0x5A});
  assert(fixture.backend->reads() == reads);
  assert(fixture.OwnerOf(0) == db.id() && fixture.OwnerOf(1) == train.id());
  seen->Reset();
  assert(fixture.manager->ReadBuffer(kFile, 2).has_value());
  assert(fixture.OwnerOf(2) == telepath::kDefaultNamespace);
}

// A guaranteed minimum keeps a namespace's pages out of reach of another
// namespace's scan.
void ExpectMinimumProtectsAgainstScans() {
  Fixture fixture("telepath_namespace_minimum");
  auto db = fixture.Namespace("db", 5);
  auto scan = fixture.Namespace("scan");
  for (telepath::BlockId block = 0; block < 5; ++block) Touch(db, block);
  for (telepath::BlockId block = 100; block < 300; ++block) Touch(scan, block);
  const std::uint64_t reads = fixture.backend->reads();
  for (telepath::BlockId block = 0; block < 5; ++block) Touch(db, block);
  assert(fixture.backend->reads() == reads);
  assert(fixture.Occupancy()[db.id()] == 5);
}

// A namespace at its maximum evicts its own pages, even with frames free.
void ExpectMaximumCapsUsage() {
  Fixture fixture("telepath_namespace_maximum");
  auto scan = fixture.Namespace("scan", 0, 2);
  for (telepath::BlockId block = 0; block < 50; ++block) {
    Touch(scan, block);
    assert(fixture.Occupancy()[scan.id()] <= 2);
  }
  assert(fixture.Occupancy()[scan.id()] == 2);
  auto free_frames = kPoolSize - 2;
  for (telepath::BlockId block = 100; block < 100 + free_frames; ++block) assert(fixture.manager->ReadBuffer(kFile, block).has_value());
}

// When every other frame is guaranteed to another namespace and its own are
// pinned, a namespace runs out of frames instead of stealing.
void ExpectGuaranteesAreNotStolen() {
  Fixture fixture("telepath_namespace_exhausted");
  auto db = fixture.Namespace("db", kPoolSize - 1);
  auto scan = fixture.Namespace("scan");
  for (telepath::BlockId block = 0; block < kPoolSize - 1; ++block) Touch(db, block);
  auto held = scan.ReadBuffer(kFile, 100);
  assert(held.has_value());
  auto refused = scan.ReadBuffer(kFile, 101);
  assert(!refused.has_value() && refused.error().code() == telepath::ErrorCode::kResourceExhausted);
  held->Reset();
  assert(scan.ReadBuffer(kFile, 101).has_value());
  assert(fixture.Occupancy()[db.id()] == kPoolSize - 1);
}

// A namespace at its maximum whose own frames are all pinned cannot use the
// free frames, so it runs out at once instead of waiting for them.
void ExpectCappedNamespaceDoesNotWaitForFreeFrames() {
  Fixture fixture("telepath_namespace_capped_wait");
  auto capped = fixture.Namespace("capped", 0, 2);
  telepath::ReadBuffersOperation operation;
  std::atomic<bool> done{false};
  std::thread watchdog([&] {
    for (int tick = 0; tick < 3000 && !done.load(); ++tick) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!done.load()) {
      std::fprintf(stderr, "a capped namespace waited for frames it may not use\n");
      std::abort();
    }
  });
  assert(capped.StartReadBuffers(operation, kFile, 0, 4).has_value());
  auto waited = fixture.manager->WaitReadBuffers(operation);
  done.store(true);
  watchdog.join();
  assert(!waited.has_value() && waited.error().code() == telepath::ErrorCode::kResourceExhausted);
  assert(fixture.Occupancy()[capped.id()] <= 2);
}

// Each namespace's pages are written back through its own pre-write hook; the
// default namespace keeps BufferManagerOptions::pre_write_hook.
void ExpectPerNamespacePreWriteHook() {
  std::mutex latch;
  std::vector<std::string> calls;
  telepath::BufferManagerOptions options;
  options.pre_write_hook = [&](const telepath::BufferTag &, std::span<const std::byte>) -> telepath::Status {
    std::lock_guard<std::mutex> guard(latch);
    calls.push_back("default");
    return telepath::Ok();
  };
  Fixture fixture("telepath_namespace_hook", kPoolSize, options);
  auto db = fixture.Namespace("db", 0, 0, [&](const telepath::BufferTag &, std::span<const std::byte>) -> telepath::Status {
    std::lock_guard<std::mutex> guard(latch);
    calls.push_back("db");
    return telepath::Ok();
  });
  auto plain = fixture.Namespace("plain");
  for (auto *space : {&db, &plain}) {
    auto handle = space->ReadBufferExclusive(kFile, space == &db ? 1 : 2);
    assert(handle.has_value());
    handle->mutable_data()[0] = std::byte{1};
  }
  {
    auto handle = fixture.manager->ReadBufferExclusive(kFile, 3);
    assert(handle.has_value());
    handle->mutable_data()[0] = std::byte{1};
  }
  assert(fixture.manager->FlushBuffer(kFile, 1).has_value());
  assert(fixture.manager->FlushBuffer(kFile, 2).has_value());
  assert(fixture.manager->FlushBuffer(kFile, 3).has_value());
  assert((calls == std::vector<std::string>{"db", "default"}));
}

// Threads of three namespaces with quotas rewrite pages concurrently: every
// page keeps its last value, quotas hold, and the namespaces' frames add up.
void ExpectConcurrentNamespacesKeepPagesAndQuotas() {
  Fixture fixture("telepath_namespace_concurrent", 16);
  std::array<telepath::BufferNamespace, 3> spaces{fixture.Namespace("a", 4, 8), fixture.Namespace("b", 2), fixture.Namespace("c", 0, 4)};
  constexpr telepath::BlockId kBlocksPerSpace = 24;
  std::vector<std::atomic<std::uint64_t>> expected(3 * kBlocksPerSpace);
  std::vector<std::thread> threads;
  for (int thread = 0; thread < 6; ++thread) {
    threads.emplace_back([&, thread] {
      std::mt19937 rng(thread);
      for (int op = 0; op < 3000; ++op) {
        const std::size_t space = rng() % 3;
        const telepath::BlockId block = space * kBlocksPerSpace + rng() % kBlocksPerSpace;
        auto handle = spaces[space].ReadBufferExclusive(kFile, block);
        if (!handle.has_value()) {
          assert(handle.error().code() == telepath::ErrorCode::kResourceExhausted);
          continue;
        }
        std::uint64_t value = 0;
        std::memcpy(&value, handle->data().data(), sizeof(value));
        assert(value == expected[block].load());
        ++value;
        std::memcpy(handle->mutable_data().data(), &value, sizeof(value));
        expected[block].store(value);
      }
    });
  }
  for (auto &thread : threads) thread.join();
  auto owned = fixture.Occupancy();
  // Quotas are soft: threads that read a count below the cap at the same
  // moment may all reserve, so a cap can be overshot by one frame per
  // concurrent thread beyond the first, and no further.
  constexpr std::size_t kOvershoot = 6 - 1;
  assert(owned[spaces[0].id()] <= 8 + kOvershoot && owned[spaces[2].id()] <= 4 + kOvershoot);
  std::size_t total = 0;
  for (const auto &[space, frames] : owned) total += frames;
  assert(total <= 16);
  for (telepath::BlockId block = 0; block < 3 * kBlocksPerSpace; ++block) {
    auto handle = fixture.manager->ReadBuffer(kFile, block);
    assert(handle.has_value());
    std::uint64_t value = 0;
    std::memcpy(&value, handle->data().data(), sizeof(value));
    assert(value == expected[block].load());
  }
}

}  // namespace

int main() {
  ExpectInvalidNamespacesAreRejected();
  ExpectCrossProcessNamespacesAreNotSupportedYet();
  ExpectPagesAreSharedAndOwnedByTheLoader();
  ExpectMinimumProtectsAgainstScans();
  ExpectMaximumCapsUsage();
  ExpectGuaranteesAreNotStolen();
  ExpectCappedNamespaceDoesNotWaitForFreeFrames();
  ExpectPerNamespacePreWriteHook();
  ExpectConcurrentNamespacesKeepPagesAndQuotas();
  return 0;
}
