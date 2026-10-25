#include <array>
#include <cassert>
#include <memory>

#include "disk_io_helpers.h"
#include "buffer_test_support.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

auto BuildManager(const std::filesystem::path &root) -> std::unique_ptr<telepath::BufferManager> {
  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto disk_backend = std::make_unique<telepath::PosixDiskBackend>(root.string(), telepath::BlockGeometry::Of<4096>());
  auto replacer = telepath::MakeLruKReplacer(3, 2);
  return telepath::BufferManager::Create(3, 4096, std::move(disk_backend), std::move(replacer), telemetry).value();
}

void SeedDirtyPages(telepath::BufferManager *manager) {
  for (telepath::BlockId block_id = 0; block_id < 3; ++block_id) {
    auto result = manager->ReadBufferExclusive(8, block_id);
    assert(result.has_value());

    auto handle = std::move(result.value());
    handle.mutable_data()[0] = static_cast<std::byte>(0x40 + block_id);
    handle.mutable_data()[1] = static_cast<std::byte>(0x50 + block_id);
  }
}

void ExpectPersistedPageMatches(
  telepath::PosixDiskBackend *backend,
  telepath::BlockId block_id
) {
  std::array<std::byte, 4096> page{};
  assert(telepath::test_support::ReadBlock(*backend, telepath::BufferTag{8, block_id}, page).has_value());
  assert(page[0] == static_cast<std::byte>(0x40 + block_id));
  assert(page[1] == static_cast<std::byte>(0x50 + block_id));
}

void ExpectFlushAllPersistsDirtyPages(const std::filesystem::path &root) {
  {
    auto manager_owner = BuildManager(root);
    telepath::BufferManager &manager = *manager_owner;
    SeedDirtyPages(&manager);
    assert(manager.FlushAll().has_value());
  }

  auto verifier = std::make_unique<telepath::PosixDiskBackend>(root.string(), telepath::BlockGeometry::Of<4096>());
  for (telepath::BlockId block_id = 0; block_id < 3; ++block_id) {
    ExpectPersistedPageMatches(verifier.get(), block_id);
  }
}

}  // namespace

int main() {
  telepath::buffer_test_support::TestRootGuard root_guard("telepath_flush_all_persistence_test_data");
  ExpectFlushAllPersistsDirtyPages(root_guard.path());
  return 0;
}
