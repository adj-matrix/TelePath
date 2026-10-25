#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <thread>
#include <vector>

#include "buffer_test_support.h"
#include "disk_io_helpers.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/buffer/read_buffers_operation.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

constexpr telepath::FileId kFile = 4;
constexpr std::size_t kPageSize = 4096;

// Forwards to a PosixDiskBackend, records every read request, and fails the
// reads that cover `failing_block`.
class RecordingBackend : public telepath::DiskBackend {
 public:
  explicit RecordingBackend(const std::string &root) : inner_(root, telepath::BlockGeometry::Of<kPageSize>()) {}

  auto Submit(std::span<const telepath::DiskRequest> requests, telepath::DiskSubmitMode mode) -> telepath::Result<std::size_t> override {
    {
      std::lock_guard<std::mutex> guard(latch_);
      for (const auto &request : requests) {
        if (request.operation != telepath::DiskOperation::kRead) continue;
        reads_.push_back({request.tag.block_id, request.block_count});
        if (failing_block_ >= request.tag.block_id && failing_block_ < request.tag.block_id + request.block_count) failing_tokens_.insert(request.token);
      }
    }
    return inner_.Submit(requests, mode);
  }

  auto Reap(std::span<telepath::DiskCompletion> out, bool wait) -> telepath::Result<std::size_t> override {
    auto reaped = inner_.Reap(out, wait);
    if (!reaped.has_value()) return reaped;
    std::lock_guard<std::mutex> guard(latch_);
    for (std::size_t i = 0; i < reaped.value(); ++i) {
      if (failing_tokens_.erase(out[i].token) == 1) out[i].status = telepath::IoError("injected read failure", 5);
    }
    return reaped;
  }

  void Shutdown() override { inner_.Shutdown(); }
  auto GetCapabilities() const -> telepath::DiskBackendCapabilities override { return inner_.GetCapabilities(); }

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

  void FailBlock(telepath::BlockId block) {
    std::lock_guard<std::mutex> guard(latch_);
    failing_block_ = block;
  }

 private:
  telepath::PosixDiskBackend inner_;
  std::mutex latch_;
  std::vector<Read> reads_;
  std::set<uint64_t> failing_tokens_;
  telepath::BlockId failing_block_{~telepath::BlockId{0}};
};

void SeedBlocks(const std::filesystem::path &root, telepath::BlockId count) {
  telepath::PosixDiskBackend backend(root.string(), telepath::BlockGeometry::Of<kPageSize>());
  std::array<std::byte, kPageSize> page{};
  for (telepath::BlockId block = 0; block < count; ++block) {
    page[0] = static_cast<std::byte>(0x10 + block);
    page[kPageSize - 1] = static_cast<std::byte>(0x80 + block);
    assert(telepath::test_support::WriteBlock(backend, {kFile, block}, page).has_value());
  }
}

struct Fixture {
  explicit Fixture(const char *name, std::size_t pool_size = 32, telepath::BlockId seeded = 16) : root(name) {
    SeedBlocks(root.path(), seeded);
    auto owned = std::make_unique<RecordingBackend>(root.path().string());
    backend = owned.get();
    telemetry = telepath::MakeCounterTelemetrySink();
    auto created = telepath::BufferManager::Create(pool_size, kPageSize, std::move(owned), telepath::MakeClockSweepReplacer(pool_size), telemetry);
    assert(created.has_value());
    manager = std::move(created.value());
  }

  telepath::buffer_test_support::TestRootGuard root;
  RecordingBackend *backend{nullptr};
  std::shared_ptr<telepath::TelemetrySink> telemetry;
  std::unique_ptr<telepath::BufferManager> manager;
};

void ExpectPage(std::span<const std::byte> page, telepath::BlockId block) {
  assert(page[0] == static_cast<std::byte>(0x10 + block));
  assert(page[kPageSize - 1] == static_cast<std::byte>(0x80 + block));
}

auto ResidentUnpinned(const telepath::BufferManager &manager, telepath::BlockId first, telepath::BlockId last) -> std::size_t {
  std::size_t count = 0;
  for (const auto &frame : manager.ExportSnapshot().frames) {
    if (frame.is_valid && frame.pin_count == 0 && frame.tag.file_id == kFile && frame.tag.block_id >= first && frame.tag.block_id < last) ++count;
  }
  return count;
}

void WaitForResident(const telepath::BufferManager &manager, telepath::BlockId first, telepath::BlockId last) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (ResidentUnpinned(manager, first, last) < last - first) {
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

auto TotalPins(const telepath::BufferManager &manager) -> std::size_t {
  std::size_t pins = 0;
  for (const auto &frame : manager.ExportSnapshot().frames) pins += frame.pin_count;
  return pins;
}

void ExpectPrefetchLoadsUnpinnedPagesInOneRequest() {
  Fixture fixture("telepath_async_read_prefetch");
  assert(fixture.manager->PrefetchBuffer(kFile, 0, 8).has_value());
  WaitForResident(*fixture.manager, 0, 8);

  const auto reads = fixture.backend->reads();
  assert(reads.size() == 1);
  assert(reads[0].first == 0 && reads[0].count == 8);
  for (telepath::BlockId block = 0; block < 8; ++block) {
    auto handle = fixture.manager->ReadBuffer(kFile, block);
    assert(handle.has_value());
    ExpectPage(handle->data(), block);
  }
  assert(fixture.backend->reads().size() == 1);
  assert(fixture.telemetry->Snapshot().buffer_hits == 8);
}

void ExpectPrefetchSkipsResidentBlocks() {
  Fixture fixture("telepath_async_read_prefetch_skip");
  assert(fixture.manager->ReadBuffer(kFile, 2).has_value());
  fixture.backend->ClearReads();

  assert(fixture.manager->PrefetchBuffer(kFile, 0, 5).has_value());
  WaitForResident(*fixture.manager, 0, 5);
  const auto reads = fixture.backend->reads();
  assert(reads.size() == 2);
  assert(reads[0].first == 0 && reads[0].count == 2);
  assert(reads[1].first == 3 && reads[1].count == 2);

  assert(fixture.manager->PrefetchBuffer(kFile, 0, 5).has_value());
  assert(fixture.backend->reads().size() == 2);
}

void ExpectPrefetchStopsWhenPoolIsPinned() {
  Fixture fixture("telepath_async_read_prefetch_full", 4);
  std::vector<telepath::SharedBufferHandle> pinned;
  for (telepath::BlockId block = 0; block < 4; ++block) {
    auto handle = fixture.manager->ReadBuffer(kFile, block);
    assert(handle.has_value());
    pinned.push_back(std::move(handle.value()));
  }
  fixture.backend->ClearReads();
  assert(fixture.manager->PrefetchBuffer(kFile, 8, 4).has_value());
  assert(fixture.backend->reads().empty());
  pinned.clear();
  assert(TotalPins(*fixture.manager) == 0);
}

void ExpectReadBuffersReturnsHandlesInOrder() {
  Fixture fixture("telepath_async_read_read_buffers");
  assert(fixture.manager->ReadBuffer(kFile, 3).has_value());
  fixture.backend->ClearReads();

  telepath::ReadBuffersOperation operation;
  assert(fixture.manager->StartReadBuffers(operation, kFile, 0, 8).has_value());
  assert(operation.in_progress());
  assert(fixture.manager->WaitReadBuffers(operation).has_value());
  assert(!operation.in_progress());

  auto handles = operation.handles();
  assert(handles.size() == 8);
  for (telepath::BlockId block = 0; block < 8; ++block) {
    assert(handles[block].valid());
    assert(handles[block].tag().block_id == block);
    ExpectPage(handles[block].data(), block);
  }
  const auto reads = fixture.backend->reads();
  assert(reads.size() == 2);
  assert(reads[0].first == 0 && reads[0].count == 3);
  assert(reads[1].first == 4 && reads[1].count == 4);

  for (auto &handle : handles) handle.Reset();
  assert(TotalPins(*fixture.manager) == 0);
}

void ExpectAbandonedOperationDropsPins() {
  Fixture fixture("telepath_async_read_abandoned");
  {
    telepath::ReadBuffersOperation operation;
    assert(fixture.manager->StartReadBuffers(operation, kFile, 0, 6).has_value());
  }
  assert(TotalPins(*fixture.manager) == 0);
  for (telepath::BlockId block = 0; block < 6; ++block) {
    auto handle = fixture.manager->ReadBuffer(kFile, block);
    assert(handle.has_value());
    ExpectPage(handle->data(), block);
  }
}

void ExpectFailedReadReleasesEveryPin() {
  Fixture fixture("telepath_async_read_failure");
  fixture.backend->FailBlock(2);
  telepath::ReadBuffersOperation operation;
  assert(fixture.manager->StartReadBuffers(operation, kFile, 0, 6).has_value());
  telepath::Status status = fixture.manager->WaitReadBuffers(operation);
  assert(!status.has_value());
  assert(status.error().code() == telepath::ErrorCode::kIoError);
  assert(operation.handles().empty());
  assert(!operation.in_progress());
  assert(TotalPins(*fixture.manager) == 0);
  assert(!fixture.manager->failure().has_value());

  fixture.backend->FailBlock(~telepath::BlockId{0});
  assert(fixture.manager->StartReadBuffers(operation, kFile, 0, 6).has_value());
  assert(fixture.manager->WaitReadBuffers(operation).has_value());
  for (telepath::BlockId block = 0; block < 6; ++block) ExpectPage(operation.handles()[block].data(), block);
}

void ExpectOperationMisuseIsRejected() {
  Fixture fixture("telepath_async_read_misuse");
  telepath::ReadBuffersOperation operation;
  auto status = fixture.manager->WaitReadBuffers(operation);
  assert(!status.has_value() && status.error().code() == telepath::ErrorCode::kInvalidArgument);
  assert(fixture.manager->StartReadBuffers(operation, kFile, 0, 2).has_value());
  status = fixture.manager->StartReadBuffers(operation, kFile, 4, 2);
  assert(!status.has_value() && status.error().code() == telepath::ErrorCode::kInvalidArgument);
  assert(fixture.manager->WaitReadBuffers(operation).has_value());
}

void ExpectConcurrentReadersShareInFlightLoads() {
  Fixture fixture("telepath_async_read_concurrent", 64, 32);
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&, t]() {
      while (!go.load()) std::this_thread::yield();
      if (t % 2 == 0) {
        telepath::ReadBuffersOperation operation;
        assert(fixture.manager->StartReadBuffers(operation, kFile, 0, 32).has_value());
        assert(fixture.manager->WaitReadBuffers(operation).has_value());
        for (telepath::BlockId block = 0; block < 32; ++block) ExpectPage(operation.handles()[block].data(), block);
        return;
      }
      assert(fixture.manager->PrefetchBuffer(kFile, 0, 32).has_value());
      for (telepath::BlockId block = 0; block < 32; ++block) {
        auto handle = fixture.manager->ReadBuffer(kFile, block);
        assert(handle.has_value());
        ExpectPage(handle->data(), block);
      }
    });
  }
  go.store(true);
  for (auto &thread : threads) thread.join();
  assert(TotalPins(*fixture.manager) == 0);
  std::size_t blocks_read = 0;
  for (const auto &read : fixture.backend->reads()) blocks_read += read.count;
  assert(blocks_read == 32);
}

}  // namespace

int main() {
  ExpectPrefetchLoadsUnpinnedPagesInOneRequest();
  ExpectPrefetchSkipsResidentBlocks();
  ExpectPrefetchStopsWhenPoolIsPinned();
  ExpectReadBuffersReturnsHandlesInOrder();
  ExpectAbandonedOperationDropsPins();
  ExpectFailedReadReleasesEveryPin();
  ExpectOperationMisuseIsRejected();
  ExpectConcurrentReadersShareInFlightLoads();
  return 0;
}
