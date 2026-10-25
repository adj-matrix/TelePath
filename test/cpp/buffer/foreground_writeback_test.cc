#include <array>
#include <cassert>
#include <cstddef>
#include <memory>
#include <mutex>
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

// Forwards to a PosixDiskBackend, records the thread that submits each write,
// and can fail writes.
class WriterRecordingBackend : public telepath::DiskBackend {
 public:
  explicit WriterRecordingBackend(const std::string &root) : inner_(root, telepath::BlockGeometry::Of<kPageSize>()) {}

  auto Submit(std::span<const telepath::DiskRequest> requests, telepath::DiskSubmitMode mode) -> telepath::Result<std::size_t> override {
    {
      std::lock_guard<std::mutex> guard(latch_);
      for (const auto &request : requests) {
        if (request.operation != telepath::DiskOperation::kWrite) continue;
        writers_.push_back(std::this_thread::get_id());
        if (fail_writes_) return telepath::IoError("injected write failure", 5);
      }
    }
    return inner_.Submit(requests, mode);
  }

  auto Reap(std::span<telepath::DiskCompletion> out, bool wait) -> telepath::Result<std::size_t> override { return inner_.Reap(out, wait); }
  void Shutdown() override { inner_.Shutdown(); }
  auto GetCapabilities() const -> telepath::DiskBackendCapabilities override { return inner_.GetCapabilities(); }

  auto writers() -> std::vector<std::thread::id> {
    std::lock_guard<std::mutex> guard(latch_);
    return writers_;
  }

  void set_fail_writes(bool value) {
    std::lock_guard<std::mutex> guard(latch_);
    fail_writes_ = value;
  }

 private:
  telepath::PosixDiskBackend inner_;
  std::mutex latch_;
  std::vector<std::thread::id> writers_;
  bool fail_writes_{false};
};

struct Fixture {
  Fixture(const char *name, std::size_t pool_size, telepath::ForegroundWriteback mode) : root(name) {
    auto owned = std::make_unique<WriterRecordingBackend>(root.path().string());
    backend = owned.get();
    telepath::BufferManagerOptions options;
    options.pool_size = pool_size;
    options.page_size = kPageSize;
    options.foreground_writeback = mode;
    auto created = telepath::BufferManager::Create(options, std::move(owned), telepath::MakeClockReplacer(pool_size), telepath::MakeNoOpTelemetrySink());
    assert(created.has_value());
    manager = std::move(created.value());
  }

  telepath::buffer_test_support::TestRootGuard root;
  WriterRecordingBackend *backend{nullptr};
  std::unique_ptr<telepath::BufferManager> manager;
};

void WriteByte(telepath::BufferManager *manager, telepath::BlockId block, std::byte value) {
  auto handle = manager->ReadBufferExclusive(6, block);
  assert(handle.has_value());
  handle->mutable_data()[0] = value;
}

auto PersistedByte(const std::filesystem::path &root, telepath::BlockId block) -> std::byte {
  telepath::PosixDiskBackend verifier(root.string(), telepath::BlockGeometry::Of<kPageSize>());
  std::array<std::byte, kPageSize> page{};
  assert(telepath::test_support::ReadBlock(verifier, {6, block}, page).has_value());
  return page[0];
}

void ExpectDirectFlushWritesOnCallingThread() {
  Fixture fixture("telepath_foreground_direct_flush", 4, telepath::ForegroundWriteback::kDirect);
  WriteByte(fixture.manager.get(), 0, std::byte{0x61});
  assert(fixture.manager->FlushBuffer(6, 0).has_value());
  const auto writers = fixture.backend->writers();
  assert(writers.size() == 1 && writers[0] == std::this_thread::get_id());
  assert(fixture.manager->ExportSnapshot().dirty_page_count == 0);
  assert(PersistedByte(fixture.root.path(), 0) == std::byte{0x61});
}

void ExpectDirectEvictionWritesVictimOnEvictingThread() {
  Fixture fixture("telepath_foreground_direct_eviction", 1, telepath::ForegroundWriteback::kDirect);
  WriteByte(fixture.manager.get(), 0, std::byte{0x62});
  assert(fixture.manager->ReadBuffer(6, 1).has_value());
  const auto writers = fixture.backend->writers();
  assert(writers.size() == 1 && writers[0] == std::this_thread::get_id());
  assert(PersistedByte(fixture.root.path(), 0) == std::byte{0x62});
}

void ExpectQueuedFlushWritesOnFlushWorker() {
  Fixture fixture("telepath_foreground_queued_flush", 4, telepath::ForegroundWriteback::kQueued);
  WriteByte(fixture.manager.get(), 0, std::byte{0x63});
  assert(fixture.manager->FlushBuffer(6, 0).has_value());
  const auto writers = fixture.backend->writers();
  assert(writers.size() == 1 && writers[0] != std::this_thread::get_id());
}

void ExpectDirectWriteFailureKeepsPageDirty() {
  Fixture fixture("telepath_foreground_direct_failure", 4, telepath::ForegroundWriteback::kDirect);
  WriteByte(fixture.manager.get(), 0, std::byte{0x64});
  fixture.backend->set_fail_writes(true);
  auto failed = fixture.manager->FlushBuffer(6, 0);
  assert(!failed.has_value() && failed.error().code() == telepath::ErrorCode::kIoError);
  assert(fixture.manager->ExportSnapshot().dirty_page_count == 1);

  fixture.backend->set_fail_writes(false);
  assert(fixture.manager->FlushBuffer(6, 0).has_value());
  assert(fixture.manager->ExportSnapshot().dirty_page_count == 0);
  assert(PersistedByte(fixture.root.path(), 0) == std::byte{0x64});
}

}  // namespace

int main() {
  ExpectDirectFlushWritesOnCallingThread();
  ExpectDirectEvictionWritesVictimOnEvictingThread();
  ExpectQueuedFlushWritesOnFlushWorker();
  ExpectDirectWriteFailureKeepsPageDirty();
  return 0;
}
