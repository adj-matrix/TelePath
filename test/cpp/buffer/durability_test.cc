#include <array>
#include <cassert>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
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

// Forwards to a PosixDiskBackend, counts sync requests per file, and can fail
// them.
class SyncRecordingBackend : public telepath::DiskBackend {
 public:
  explicit SyncRecordingBackend(const std::string &root) : inner_(root, telepath::BlockGeometry::Of<kPageSize>()) {}

  auto Submit(std::span<const telepath::DiskRequest> requests, telepath::DiskSubmitMode mode) -> telepath::Result<std::size_t> override {
    {
      std::lock_guard<std::mutex> guard(latch_);
      for (const auto &request : requests) {
        if (request.operation != telepath::DiskOperation::kSync) continue;
        ++syncs_[request.tag.file_id];
        if (fail_syncs_) failing_tokens_.push_back(request.token);
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
        out[i].status = telepath::IoError("injected sync failure", 5);
        failing_tokens_.erase(it);
        break;
      }
    }
    return reaped;
  }

  void Shutdown() override { inner_.Shutdown(); }
  auto GetCapabilities() const -> telepath::DiskBackendCapabilities override { return inner_.GetCapabilities(); }

  auto syncs() -> std::map<telepath::FileId, int> {
    std::lock_guard<std::mutex> guard(latch_);
    return syncs_;
  }

  void set_fail_syncs(bool value) {
    std::lock_guard<std::mutex> guard(latch_);
    fail_syncs_ = value;
  }

 private:
  telepath::PosixDiskBackend inner_;
  std::mutex latch_;
  std::map<telepath::FileId, int> syncs_;
  std::vector<uint64_t> failing_tokens_;
  bool fail_syncs_{false};
};

struct Fixture {
  explicit Fixture(const char *name, telepath::BufferManagerOptions options = {}) : root(name) {
    auto owned = std::make_unique<SyncRecordingBackend>(root.path().string());
    backend = owned.get();
    options.pool_size = 8;
    options.page_size = kPageSize;
    auto created = telepath::BufferManager::Create(options, std::move(owned), telepath::MakeClockSweepReplacer(8), telepath::MakeNoOpTelemetrySink());
    assert(created.has_value());
    manager = std::move(created.value());
  }

  telepath::buffer_test_support::TestRootGuard root;
  SyncRecordingBackend *backend{nullptr};
  std::unique_ptr<telepath::BufferManager> manager;
};

void WriteByte(telepath::BufferManager *manager, telepath::FileId file, telepath::BlockId block, std::byte value) {
  auto handle = manager->ReadBufferExclusive(file, block);
  assert(handle.has_value());
  handle->mutable_data()[0] = value;
}

auto PersistedByte(const std::filesystem::path &root, telepath::FileId file, telepath::BlockId block) -> std::byte {
  telepath::PosixDiskBackend verifier(root.string(), telepath::BlockGeometry::Of<kPageSize>());
  std::array<std::byte, kPageSize> page{};
  assert(telepath::test_support::ReadBlock(verifier, {file, block}, page).has_value());
  return page[0];
}

void ExpectFlushAllSyncsEachWrittenFileOnce() {
  Fixture fixture("telepath_durability_flush_all");
  WriteByte(fixture.manager.get(), 1, 0, std::byte{0x11});
  WriteByte(fixture.manager.get(), 1, 1, std::byte{0x12});
  WriteByte(fixture.manager.get(), 2, 0, std::byte{0x21});

  assert(fixture.manager->FlushBuffer(1, 0).has_value());
  assert(fixture.backend->syncs().empty());

  assert(fixture.manager->FlushAll().has_value());
  auto syncs = fixture.backend->syncs();
  assert(syncs.size() == 2 && syncs[1] == 1 && syncs[2] == 1);
  assert(PersistedByte(fixture.root.path(), 1, 1) == std::byte{0x12});

  assert(fixture.manager->FlushAll().has_value());
  assert(fixture.backend->syncs() == syncs);
}

void ExpectFailedSyncIsRetried() {
  Fixture fixture("telepath_durability_sync_retry");
  WriteByte(fixture.manager.get(), 3, 0, std::byte{0x31});
  fixture.backend->set_fail_syncs(true);
  auto status = fixture.manager->FlushAll();
  assert(!status.has_value() && status.error().code() == telepath::ErrorCode::kIoError);

  fixture.backend->set_fail_syncs(false);
  assert(fixture.manager->FlushAll().has_value());
  assert(fixture.backend->syncs()[3] == 2);
}

void ExpectCloseFlushesAndRejectsLaterCalls() {
  Fixture fixture("telepath_durability_close");
  WriteByte(fixture.manager.get(), 4, 0, std::byte{0x41});
  {
    auto held = fixture.manager->ReadBuffer(4, 0);
    assert(held.has_value());
    auto refused = fixture.manager->Close();
    assert(!refused.has_value() && refused.error().code() == telepath::ErrorCode::kInvalidArgument);
  }
  assert(fixture.manager->Close().has_value());
  assert(fixture.backend->syncs()[4] == 1);
  assert(PersistedByte(fixture.root.path(), 4, 0) == std::byte{0x41});

  auto after = fixture.manager->ReadBuffer(4, 0);
  assert(!after.has_value() && after.error().code() == telepath::ErrorCode::kUnavailable);
  assert(fixture.manager->Close().has_value());
}

void ExpectPreWriteHookSeesSnapshotAndCanFail() {
  std::vector<std::pair<telepath::BufferTag, std::byte>> seen;
  bool fail = true;
  telepath::BufferManagerOptions options;
  options.pre_write_hook = [&](const telepath::BufferTag &tag, std::span<const std::byte> page) -> telepath::Status {
    assert(page.size() == kPageSize);
    seen.emplace_back(tag, page[0]);
    if (fail) return telepath::Internal("wal not flushed");
    return telepath::Ok();
  };
  Fixture fixture("telepath_durability_hook", options);
  WriteByte(fixture.manager.get(), 5, 2, std::byte{0x52});

  auto refused = fixture.manager->FlushBuffer(5, 2);
  assert(!refused.has_value() && refused.error().code() == telepath::ErrorCode::kInternal);
  assert(fixture.manager->ExportSnapshot().dirty_page_count == 1);
  assert(PersistedByte(fixture.root.path(), 5, 2) == std::byte{0});

  fail = false;
  assert(fixture.manager->FlushBuffer(5, 2).has_value());
  assert(seen.size() == 2);
  assert(seen[1].first == (telepath::BufferTag{5, 2}) && seen[1].second == std::byte{0x52});
  assert(PersistedByte(fixture.root.path(), 5, 2) == std::byte{0x52});
}

void ExpectObservationIsReserved() {
  telepath::buffer_test_support::TestRootGuard root("telepath_durability_observation");
  telepath::BufferManagerOptions options;
  options.pool_size = 4;
  options.page_size = kPageSize;
  options.observation.enabled = true;
  auto created = telepath::BufferManager::Create(options, std::make_unique<telepath::PosixDiskBackend>(root.path().string(), telepath::BlockGeometry::Of<kPageSize>()), telepath::MakeClockReplacer(4), nullptr);
  assert(!created.has_value() && created.error().code() == telepath::ErrorCode::kUnavailable);
}

void ExpectZeroAndLockKeepsResidentContents() {
  Fixture fixture("telepath_durability_zero_and_lock");
  {
    auto fresh = fixture.manager->ReadBufferExclusive(6, 0, nullptr, telepath::ReadBufferMode::kZeroAndLock);
    assert(fresh.has_value());
    assert(fresh->data()[0] == std::byte{0});
    fresh->mutable_data()[0] = std::byte{0x66};
  }
  auto again = fixture.manager->ReadBufferExclusive(6, 0, nullptr, telepath::ReadBufferMode::kZeroAndLock);
  assert(again.has_value());
  assert(again->data()[0] == std::byte{0x66});
}

void ExpectFileFlushAndSyncTouchOnlyThatFile() {
  Fixture fixture("telepath_durability_file_flush");
  WriteByte(fixture.manager.get(), 7, 0, std::byte{0x71});
  WriteByte(fixture.manager.get(), 8, 0, std::byte{0x81});
  assert(fixture.manager->FlushFileBuffers(7).has_value());
  assert(fixture.manager->ExportSnapshot().dirty_page_count == 1);
  assert(fixture.backend->syncs().empty());
  assert(PersistedByte(fixture.root.path(), 7, 0) == std::byte{0x71});
  assert(PersistedByte(fixture.root.path(), 8, 0) == std::byte{0});

  assert(fixture.manager->SyncFile(7).has_value());
  auto syncs = fixture.backend->syncs();
  assert(syncs.size() == 1 && syncs[7] == 1);
  assert(fixture.manager->FlushAll().has_value());
  syncs = fixture.backend->syncs();
  assert(syncs[7] == 1 && syncs[8] == 1);
}

void ExpectDropFileBuffersDiscardsDirtyPages() {
  Fixture fixture("telepath_durability_drop");
  for (telepath::BlockId block = 0; block < 4; ++block) WriteByte(fixture.manager.get(), 9, block, static_cast<std::byte>(0x90 + block));
  WriteByte(fixture.manager.get(), 10, 3, std::byte{0xA3});
  assert(fixture.manager->ExportSnapshot().dirty_page_count == 5);

  {
    auto pinned = fixture.manager->ReadBuffer(9, 3);
    assert(pinned.has_value());
    auto refused = fixture.manager->DropFileBuffers(9, 2);
    assert(!refused.has_value() && refused.error().code() == telepath::ErrorCode::kInvalidArgument);
  }
  assert(fixture.manager->DropFileBuffers(9, 2).has_value());
  assert(fixture.manager->ExportSnapshot().dirty_page_count == 3);

  auto dropped = fixture.manager->ReadBuffer(9, 2);
  assert(dropped.has_value() && dropped->data()[0] == std::byte{0});
  dropped->Reset();
  auto kept = fixture.manager->ReadBuffer(9, 1);
  assert(kept.has_value() && kept->data()[0] == std::byte{0x91});
  kept->Reset();
  auto other = fixture.manager->ReadBuffer(10, 3);
  assert(other.has_value() && other->data()[0] == std::byte{0xA3});
}

}  // namespace

int main() {
  ExpectZeroAndLockKeepsResidentContents();
  ExpectFileFlushAndSyncTouchOnlyThatFile();
  ExpectDropFileBuffersDiscardsDirtyPages();
  ExpectFlushAllSyncsEachWrittenFileOnce();
  ExpectFailedSyncIsRetried();
  ExpectCloseFlushesAndRejectsLaterCalls();
  ExpectPreWriteHookSeesSnapshotAndCanFail();
  ExpectObservationIsReserved();
  return 0;
}
