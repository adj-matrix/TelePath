#include <array>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "buffer_test_support.h"
#include "disk_io_helpers.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/options/buffer_manager_options.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

constexpr std::size_t kPageSize = 4096;
constexpr telepath::FileId kFile = 1;

// Forwards to a PosixDiskBackend and can fail writes.
class FailingWriteBackend : public telepath::DiskBackend {
 public:
  explicit FailingWriteBackend(const std::string &root) : inner_(root, telepath::BlockGeometry::Of<kPageSize>()) {}

  auto Submit(std::span<const telepath::DiskRequest> requests, telepath::DiskSubmitMode mode) -> telepath::Result<std::size_t> override {
    {
      std::lock_guard<std::mutex> guard(latch_);
      for (const auto &request : requests) {
        if (request.operation == telepath::DiskOperation::kWrite && fail_writes_) failing_tokens_.push_back(request.token);
      }
    }
    return inner_.Submit(requests, mode);
  }

  auto Reap(std::span<telepath::DiskCompletion> out, bool wait) -> telepath::Result<std::size_t> override {
    auto reaped = inner_.Reap(out, wait);
    if (!reaped.has_value()) return reaped;
    std::lock_guard<std::mutex> guard(latch_);
    for (std::size_t i = 0; i < reaped.value(); ++i) {
      for (auto it = failing_tokens_.begin(); it != failing_tokens_.end(); ++it) {
        if (*it != out[i].token) continue;
        out[i].status = telepath::IoError("injected write failure", 5);
        failing_tokens_.erase(it);
        break;
      }
    }
    return reaped;
  }

  void Shutdown() override { inner_.Shutdown(); }
  auto GetCapabilities() const -> telepath::DiskBackendCapabilities override { return inner_.GetCapabilities(); }

  void set_fail_writes(bool value) {
    std::lock_guard<std::mutex> guard(latch_);
    fail_writes_ = value;
  }

 private:
  telepath::PosixDiskBackend inner_;
  std::mutex latch_;
  std::vector<std::uint64_t> failing_tokens_;
  bool fail_writes_{false};
};

struct Fixture {
  explicit Fixture(const char *name, telepath::BufferManagerOptions options = {}) : root(name) {
    auto owned = std::make_unique<FailingWriteBackend>(root.path().string());
    backend = owned.get();
    options.pool_size = 8;
    options.page_size = kPageSize;
    auto created = telepath::BufferManager::Create(options, std::move(owned), telepath::MakeClockSweepReplacer(8), telepath::MakeNoOpTelemetrySink());
    assert(created.has_value());
    manager = std::move(created.value());
  }

  void Log(telepath::BlockId block, telepath::Lsn lsn, std::byte value = std::byte{1}) {
    auto handle = manager->ReadBufferExclusive(kFile, block);
    assert(handle.has_value());
    handle->mutable_data()[0] = value;
    handle->MarkDirty(lsn);
  }

  auto RecLsn(telepath::BlockId block) -> telepath::Lsn {
    for (const auto &frame : manager->ExportSnapshot().frames) {
      if (frame.is_valid && frame.tag == telepath::BufferTag{kFile, block}) return frame.rec_lsn;
    }
    return telepath::kInvalidLsn;
  }

  auto Persisted(telepath::BlockId block) -> std::byte {
    telepath::PosixDiskBackend verifier(root.path().string(), telepath::BlockGeometry::Of<kPageSize>());
    std::array<std::byte, kPageSize> page{};
    assert(telepath::test_support::ReadBlock(verifier, {kFile, block}, page).has_value());
    return page[0];
  }

  telepath::buffer_test_support::TestRootGuard root;
  FailingWriteBackend *backend{nullptr};
  std::unique_ptr<telepath::BufferManager> manager;
};

void ExpectEarliestLsnSinceWritebackIsTracked() {
  Fixture fixture("telepath_rec_lsn_tracking");
  assert(!fixture.manager->MinRecLsn().has_value());

  fixture.Log(0, 10);
  fixture.Log(1, 20);
  fixture.Log(0, 15);
  assert(fixture.RecLsn(0) == 10 && fixture.RecLsn(1) == 20);
  assert(fixture.manager->MinRecLsn() == telepath::Lsn{10});

  // A change without an LSN dirties the page but is not tracked.
  {
    auto untracked = fixture.manager->ReadBufferExclusive(kFile, 2);
    assert(untracked.has_value());
    untracked->mutable_data()[0] = std::byte{2};
  }
  assert(fixture.RecLsn(2) == telepath::kInvalidLsn);

  assert(fixture.manager->FlushBuffer(kFile, 0).has_value());
  assert(fixture.RecLsn(0) == telepath::kInvalidLsn);
  assert(fixture.manager->MinRecLsn() == telepath::Lsn{20});

  fixture.Log(0, 30);
  assert(fixture.RecLsn(0) == 30);
}

void ExpectCheckpointWritesOnlyEarlierPages() {
  Fixture fixture("telepath_rec_lsn_checkpoint");
  fixture.Log(0, 10, std::byte{0x10});
  fixture.Log(1, 20, std::byte{0x20});
  fixture.Log(2, 30, std::byte{0x30});

  assert(fixture.manager->CheckpointBuffers(25).has_value());
  assert(fixture.Persisted(0) == std::byte{0x10} && fixture.Persisted(1) == std::byte{0x20});
  assert(fixture.Persisted(2) == std::byte{0});
  assert(fixture.manager->MinRecLsn() == telepath::Lsn{30});

  assert(fixture.manager->CheckpointBuffers(31).has_value());
  assert(fixture.Persisted(2) == std::byte{0x30});
  assert(!fixture.manager->MinRecLsn().has_value());
  assert(fixture.manager->ExportSnapshot().dirty_page_count == 0);
}

// A change made while the page's writeback is in flight becomes the page's
// recLSN once that writeback completes; until then the snapshot's recLSN holds.
void ExpectChangeDuringWritebackStartsNextRecLsn() {
  std::mutex latch;
  std::condition_variable cv;
  int hook_entries = 0;
  int hook_releases = 0;
  telepath::BufferManagerOptions options;
  options.pre_write_hook = [&](const telepath::BufferTag &, std::span<const std::byte>) -> telepath::Status {
    std::unique_lock<std::mutex> lock(latch);
    const int entry = ++hook_entries;
    cv.notify_all();
    cv.wait(lock, [&] { return hook_releases >= entry; });
    return telepath::Ok();
  };
  Fixture fixture("telepath_rec_lsn_during_writeback", options);
  const auto wait_for_entry = [&](int entry) {
    std::unique_lock<std::mutex> lock(latch);
    cv.wait(lock, [&] { return hook_entries >= entry; });
  };
  const auto release = [&] {
    {
      std::lock_guard<std::mutex> lock(latch);
      ++hook_releases;
    }
    cv.notify_all();
  };
  fixture.Log(0, 10);

  // FlushBuffer() writes until the page is clean: the first round carries
  // recLSN 10, and the change at 40 made during it needs a second round.
  std::thread flusher([&] { assert(fixture.manager->FlushBuffer(kFile, 0).has_value()); });
  wait_for_entry(1);
  fixture.Log(0, 40);
  assert(fixture.manager->MinRecLsn() == telepath::Lsn{10});
  release();

  wait_for_entry(2);
  assert(fixture.manager->MinRecLsn() == telepath::Lsn{40});
  release();
  flusher.join();

  assert(fixture.RecLsn(0) == telepath::kInvalidLsn);
  assert(!fixture.manager->MinRecLsn().has_value());
}

void ExpectFailedWritebackKeepsRecLsn() {
  bool fail_hook = false;
  telepath::BufferManagerOptions options;
  options.pre_write_hook = [&](const telepath::BufferTag &, std::span<const std::byte>) -> telepath::Status {
    if (fail_hook) return telepath::Internal("wal not flushed");
    return telepath::Ok();
  };
  Fixture fixture("telepath_rec_lsn_failure", options);
  fixture.Log(0, 10);

  fail_hook = true;
  assert(!fixture.manager->FlushBuffer(kFile, 0).has_value());
  assert(fixture.RecLsn(0) == 10);
  fail_hook = false;

  fixture.backend->set_fail_writes(true);
  auto failed = fixture.manager->CheckpointBuffers(11);
  assert(!failed.has_value() && failed.error().code() == telepath::ErrorCode::kIoError);
  assert(fixture.RecLsn(0) == 10);
  assert(fixture.manager->MinRecLsn() == telepath::Lsn{10});

  fixture.backend->set_fail_writes(false);
  assert(fixture.manager->CheckpointBuffers(11).has_value());
  assert(!fixture.manager->MinRecLsn().has_value());
}

void ExpectDroppedPagesForgetRecLsn() {
  Fixture fixture("telepath_rec_lsn_drop");
  fixture.Log(3, 10);
  fixture.Log(4, 20);
  assert(fixture.manager->DropFileBuffers(kFile, 4).has_value());
  assert(fixture.manager->MinRecLsn() == telepath::Lsn{10});
  assert(fixture.manager->DropFileBuffers(kFile, 0).has_value());
  assert(!fixture.manager->MinRecLsn().has_value());
}

}  // namespace

int main() {
  ExpectEarliestLsnSinceWritebackIsTracked();
  ExpectCheckpointWritesOnlyEarlierPages();
  ExpectChangeDuringWritebackStartsNextRecLsn();
  ExpectFailedWritebackKeepsRecLsn();
  ExpectDroppedPagesForgetRecLsn();
  return 0;
}
