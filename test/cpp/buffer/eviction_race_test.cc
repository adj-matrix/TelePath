#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "buffer_test_support.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

// Holds flush-worker writes at SubmitWrite() so tests can act while a
// writeback is pending, then releases them with an optional injected failure.
class GatedWriteBackend : public telepath::DiskBackend {
 public:
  explicit GatedWriteBackend(const std::string &root) : inner_(root, telepath::BlockGeometry::Of<4096>()) {}

  auto SubmitRead(const telepath::BufferTag &tag, std::byte *out, std::size_t size) -> telepath::Result<uint64_t> override {
    return inner_.SubmitRead(tag, out, size);
  }

  auto SubmitWrite(const telepath::BufferTag &tag, const std::byte *data, std::size_t size) -> telepath::Result<uint64_t> override {
    {
      std::unique_lock<std::mutex> lock(latch_);
      if (gate_closed_) {
        ++blocked_writes_;
        cv_.notify_all();
        cv_.wait(lock, [&]() { return !gate_closed_; });
      }
      if (fail_writes_) return telepath::IoError("injected write failure");
    }
    return inner_.SubmitWrite(tag, data, size);
  }

  auto PollCompletion() -> telepath::Result<telepath::DiskCompletion> override { return inner_.PollCompletion(); }
  void Shutdown() override { inner_.Shutdown(); }
  auto GetCapabilities() const -> telepath::DiskBackendCapabilities override { return inner_.GetCapabilities(); }

  void CloseGate() {
    std::lock_guard<std::mutex> guard(latch_);
    gate_closed_ = true;
  }

  void OpenGate(bool fail_writes) {
    {
      std::lock_guard<std::mutex> guard(latch_);
      gate_closed_ = false;
      fail_writes_ = fail_writes;
    }
    cv_.notify_all();
  }

  void WaitForBlockedWrite() {
    std::unique_lock<std::mutex> lock(latch_);
    cv_.wait(lock, [&]() { return blocked_writes_ > 0; });
  }

 private:
  telepath::PosixDiskBackend inner_;
  std::mutex latch_;
  std::condition_variable cv_;
  bool gate_closed_{false};
  bool fail_writes_{false};
  std::size_t blocked_writes_{0};
};

// Fails the test instead of hanging ctest when a deadlock regresses.
class Watchdog {
 public:
  Watchdog(int seconds, const char *name) : thread_([this, seconds, name]() {
    std::unique_lock<std::mutex> lock(latch_);
    if (cv_.wait_for(lock, std::chrono::seconds(seconds), [&]() { return done_; })) return;
    std::fprintf(stderr, "%s: no progress after %ds, deadlock\n", name, seconds);
    std::abort();
  }) {}

  ~Watchdog() {
    {
      std::lock_guard<std::mutex> guard(latch_);
      done_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }

 private:
  std::mutex latch_;
  std::condition_variable cv_;
  bool done_{false};
  std::thread thread_;
};

void WriteDirtyPage(telepath::BufferManager *manager, telepath::BlockId block_id, std::byte value) {
  auto result = manager->ReadBufferExclusive(1, block_id);
  assert(result.has_value());
  auto handle = std::move(result.value());
  handle.mutable_data()[0] = value;
}

void TouchPage(telepath::BufferManager *manager, telepath::BlockId block_id) {
  auto result = manager->ReadBuffer(1, block_id);
  assert(result.has_value());
}

auto CountResidentCopies(const telepath::BufferManager &manager, telepath::BlockId block_id) -> int {
  int copies = 0;
  for (const auto &frame : manager.ExportSnapshot().frames) {
    if (frame.is_valid && frame.tag.file_id == 1 && frame.tag.block_id == block_id) ++copies;
  }
  return copies;
}

// While a dirty victim is being written back, its page must stay reachable
// under the old tag; a concurrent reader must not load stale bytes from disk.
void ExpectReaderOfDirtyVictimSeesLatestBytes() {
  telepath::buffer_test_support::TestRootGuard root("telepath_eviction_race_stale");
  auto backend = std::make_unique<GatedWriteBackend>(root.path().string());
  auto *gate = backend.get();
  auto manager_owner = telepath::BufferManager::Create(2, 4096, std::move(backend), telepath::MakeLruReplacer(2), telepath::MakeNoOpTelemetrySink());
  assert(manager_owner.has_value());
  telepath::BufferManager &manager = *manager_owner.value();

  WriteDirtyPage(&manager, 0, std::byte{0xAB});
  TouchPage(&manager, 1);

  gate->CloseGate();
  std::thread evictor([&]() { assert(manager.ReadBuffer(1, 2).has_value()); });
  gate->WaitForBlockedWrite();

  {
    auto result = manager.ReadBuffer(1, 0);
    assert(result.has_value());
    assert(result.value().data()[0] == std::byte{0xAB});
  }

  gate->OpenGate(false);
  evictor.join();

  assert(CountResidentCopies(manager, 0) <= 1);
  auto reread = manager.ReadBuffer(1, 0);
  assert(reread.has_value());
  assert(reread.value().data()[0] == std::byte{0xAB});
}

// A failed victim writeback must leave exactly one resident copy of the page,
// still dirty, even if another thread read that page during the writeback.
void ExpectFailedVictimWritebackKeepsSingleResidentCopy() {
  telepath::buffer_test_support::TestRootGuard root("telepath_eviction_race_duplicate");
  auto backend = std::make_unique<GatedWriteBackend>(root.path().string());
  auto *gate = backend.get();
  auto telemetry = telepath::MakeCounterTelemetrySink();
  auto manager_owner = telepath::BufferManager::Create(3, 4096, std::move(backend), telepath::MakeLruReplacer(3), telemetry);
  assert(manager_owner.has_value());
  telepath::BufferManager &manager = *manager_owner.value();

  WriteDirtyPage(&manager, 0, std::byte{0xCD});
  TouchPage(&manager, 1);
  TouchPage(&manager, 3);

  gate->CloseGate();
  std::thread evictor([&]() {
    auto result = manager.ReadBuffer(1, 2);
    assert(!result.has_value());
    assert(result.error().code() == telepath::ErrorCode::kIoError);
  });
  gate->WaitForBlockedWrite();
  TouchPage(&manager, 0);
  gate->OpenGate(true);
  evictor.join();

  assert(CountResidentCopies(manager, 0) == 1);
  for (const auto &frame : manager.ExportSnapshot().frames) {
    if (frame.is_valid && frame.tag.block_id == 0) {
      assert(frame.is_dirty);
      assert(frame.pin_count == 0);
    }
  }
  assert(telemetry->Snapshot().eviction_failures == 1);

  auto result = manager.ReadBuffer(1, 0);
  assert(result.has_value());
  assert(result.value().data()[0] == std::byte{0xCD});
}

// A victim skipped because its flush is in flight must return to the replacer;
// otherwise the frame silently leaves the evictable set for good.
void ExpectBusyVictimReturnsToReplacer(const char *name, const std::function<std::unique_ptr<telepath::Replacer>()> &make_replacer) {
  telepath::buffer_test_support::TestRootGuard root(std::string("telepath_eviction_race_leak_") + name);
  auto backend = std::make_unique<GatedWriteBackend>(root.path().string());
  auto *gate = backend.get();
  auto manager_owner = telepath::BufferManager::Create(2, 4096, std::move(backend), make_replacer(), telepath::MakeNoOpTelemetrySink());
  assert(manager_owner.has_value());
  telepath::BufferManager &manager = *manager_owner.value();

  WriteDirtyPage(&manager, 0, std::byte{0x11});
  TouchPage(&manager, 1);

  gate->CloseGate();
  std::thread flusher([&]() { assert(manager.FlushAll().has_value()); });
  gate->WaitForBlockedWrite();
  TouchPage(&manager, 2);
  gate->OpenGate(false);
  flusher.join();

  auto pinned = manager.ReadBuffer(1, 3);
  assert(pinned.has_value());
  auto other = manager.ReadBuffer(1, 4);
  if (!other.has_value()) std::fprintf(stderr, "%s replacer lost an evictable frame: %s\n", name, other.error().message());
  assert(other.has_value());
}

// FlushAll() must not publish a queued flush and then block behind a writer
// that is itself waiting for that flush in FlushBuffer(). The background
// cleaner shares FlushAll()'s opportunistic scheduling path.
void ExpectFlushAllDoesNotDeadlockWithWriterFlush() {
  telepath::buffer_test_support::TestRootGuard root("telepath_eviction_race_flush_all");
  auto manager_owner = telepath::BufferManager::Create(
    4, 4096,
    std::make_unique<telepath::PosixDiskBackend>(root.path().string(), telepath::BlockGeometry::Of<4096>(), false, 64, false),
    telepath::MakeLruReplacer(4), telepath::MakeNoOpTelemetrySink());
  assert(manager_owner.has_value());
  telepath::BufferManager &manager = *manager_owner.value();
  Watchdog watchdog(20, "flush_all_vs_writer");

  std::atomic<bool> stop{false};
  std::thread flusher([&]() {
    while (!stop.load()) assert(manager.FlushAll().has_value());
  });
  for (int i = 0; i < 300; ++i) {
    auto result = manager.ReadBufferExclusive(1, 0);
    assert(result.has_value());
    auto handle = std::move(result.value());
    handle.mutable_data()[0] = std::byte(i & 0xff);
    std::this_thread::sleep_for(std::chrono::microseconds(500));
    assert(manager.FlushBuffer(handle).has_value());
  }
  stop = true;
  flusher.join();
}

}  // namespace

int main() {
  ExpectReaderOfDirtyVictimSeesLatestBytes();
  ExpectFailedVictimWritebackKeepsSingleResidentCopy();
  ExpectBusyVictimReturnsToReplacer("lru", []() { return telepath::MakeLruReplacer(2); });
  ExpectBusyVictimReturnsToReplacer("lru_k", []() { return telepath::MakeLruKReplacer(2, 2); });
  ExpectBusyVictimReturnsToReplacer("two_queue", []() { return telepath::MakeTwoQueueReplacer(2); });
  ExpectBusyVictimReturnsToReplacer("clock", []() { return telepath::MakeClockReplacer(2); });
  ExpectFlushAllDoesNotDeadlockWithWriterFlush();
  return 0;
}
