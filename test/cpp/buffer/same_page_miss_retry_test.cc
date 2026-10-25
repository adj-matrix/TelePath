#include <array>
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <utility>

#include "disk_io_helpers.h"
#include "buffer_test_support.h"
#include "miss_coordinator.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/buffer/buffer_manager_test_peer.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

auto BuildPage(std::byte marker) -> std::array<std::byte, 4096> {
  std::array<std::byte, 4096> page{};
  page[0] = marker;
  return page;
}

void ReadBufferExclusive(
  const std::filesystem::path &root,
  const telepath::BufferTag &tag,
  std::byte marker
) {
  telepath::PosixDiskBackend backend(root.string(), telepath::BlockGeometry::Of<4096>());
  auto page = BuildPage(marker);
  assert(telepath::test_support::WriteBlock(backend, tag, page).has_value());
}

auto BuildManager(const std::filesystem::path &root) -> std::unique_ptr<telepath::BufferManager> {
  auto backend = std::make_unique<telepath::PosixDiskBackend>(root.string(), telepath::BlockGeometry::Of<4096>());
  auto replacer = telepath::MakeClockReplacer(1);
  auto telemetry = telepath::MakeCounterTelemetrySink();
  return telepath::BufferManager::Create(1, 4096, std::move(backend), std::move(replacer), telemetry).value();
}

void ExpectLateJoinedMissRetriesAfterFrameWasEvicted(
  telepath::BufferManager *manager
) {
  const telepath::BufferTag hot_tag{61, 0};
  const telepath::BufferTag cold_tag{61, 1};

  auto hot_result = manager->ReadBuffer(hot_tag.file_id, hot_tag.block_id);
  assert(hot_result.has_value());
  {
    auto hot_handle = std::move(hot_result.value());
    assert(hot_handle.frame_id() == 0);
    assert(hot_handle.data()[0] == std::byte{0xA1});
  }

  auto completed_stale_miss = std::make_shared<telepath::BufferManagerMissState>();
  {
    std::lock_guard<std::mutex> guard(completed_stale_miss->latch);
    completed_stale_miss->completed = true;
    completed_stale_miss->status = telepath::Ok();
    completed_stale_miss->frame_id = 0;
  }

  auto cold_result = manager->ReadBuffer(cold_tag.file_id, cold_tag.block_id);
  assert(cold_result.has_value());
  {
    auto cold_handle = std::move(cold_result.value());
    assert(cold_handle.frame_id() == 0);
    assert(cold_handle.data()[0] == std::byte{0xB2});
  }

  auto retry_result = telepath::BufferManagerTestPeer::AwaitJoinedMiss(
    *manager,
    hot_tag,
    completed_stale_miss);
  assert(retry_result.has_value());
  auto retry_handle = std::move(retry_result.value());
  assert(retry_handle.frame_id() == 0);
  assert(retry_handle.tag() == hot_tag);
  assert(retry_handle.data()[0] == std::byte{0xA1});
}

}  // namespace

int main() {
  telepath::buffer_test_support::TestRootGuard root_guard("telepath_same_page_miss_retry_data");
  ReadBufferExclusive(root_guard.path(), telepath::BufferTag{61, 0}, std::byte{0xA1});
  ReadBufferExclusive(root_guard.path(), telepath::BufferTag{61, 1}, std::byte{0xB2});

  auto manager_owner = BuildManager(root_guard.path());
  telepath::BufferManager &manager = *manager_owner;
  ExpectLateJoinedMissRetriesAfterFrameWasEvicted(&manager);
  return 0;
}
