#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>

#include "buffer_test_support.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

auto BuildManager(const std::filesystem::path &root) -> std::unique_ptr<telepath::BufferManager> {
  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto disk_backend = std::make_unique<telepath::PosixDiskBackend>(root.string(), telepath::BlockGeometry::Of<4096>());
  auto replacer = telepath::MakeClockReplacer(2);
  return telepath::BufferManager::Create(2, 4096, std::move(disk_backend), std::move(replacer), telemetry).value();
}

auto AcquireWriter(telepath::BufferManager *manager) -> telepath::ExclusiveBufferHandle {
  auto writer_result = manager->ReadBufferExclusive(21, 0);
  assert(writer_result.has_value());
  return std::move(writer_result.value());
}

void ExpectFlushWaitsForWriterRelease(
  telepath::BufferManager *manager,
  telepath::ExclusiveBufferHandle writer
) {
  writer.mutable_data()[0] = std::byte{0xA5};

  std::atomic<bool> flush_started{false};
  std::atomic<bool> flush_finished{false};
  telepath::Status flush_status = telepath::Ok();
  std::thread flush_thread([&]() {
    flush_started.store(true);
    auto flusher = manager->ReadBuffer(21, 0);
    assert(flusher.has_value());
    flush_status = manager->FlushBuffer(*flusher);
    flush_finished.store(true);
  });

  while (!flush_started.load()) {
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  assert(!flush_finished.load());

  writer.Reset();
  flush_thread.join();
  assert(flush_status.has_value());
  assert(flush_finished.load());
}

void ExpectPersistedPageContainsFlushedByte(
  const std::filesystem::path &root
) {
  auto verifier = std::make_unique<telepath::PosixDiskBackend>(root.string(), telepath::BlockGeometry::Of<4096>());
  std::array<std::byte, 4096> page{};
  auto request = verifier->SubmitRead(telepath::BufferTag{21, 0}, page.data(), page.size());
  assert(request.has_value());

  auto completion = verifier->PollCompletion();
  assert(completion.has_value());
  assert(completion.value().status.has_value());
  assert(page[0] == std::byte{0xA5});
}

}  // namespace

int main() {
  telepath::buffer_test_support::TestRootGuard root_guard("telepath_flush_consistency_test_data");
  auto manager_owner = BuildManager(root_guard.path());
  telepath::BufferManager &manager = *manager_owner;
  ExpectFlushWaitsForWriterRelease(&manager, AcquireWriter(&manager));
  ExpectPersistedPageContainsFlushedByte(root_guard.path());
  return 0;
}
