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

auto BuildManager(const std::filesystem::path &root) -> telepath::BufferManager {
  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto disk_backend = std::make_unique<telepath::PosixDiskBackend>(root.string(), 4096);
  auto replacer = telepath::MakeClockReplacer(2);
  return telepath::BufferManager(2, 4096, std::move(disk_backend), std::move(replacer), telemetry);
}

void SeedDirtyPage(telepath::BufferManager *manager) {
  auto seed_result = manager->ReadBuffer(31, 0);
  assert(seed_result.ok());

  auto seed = std::move(seed_result.value());
  seed.mutable_data()[0] = std::byte{0x6C};
  seed.mutable_data()[1] = std::byte{0x7D};
  assert(manager->MarkBufferDirty(seed).ok());
}

struct FlushReadHandles {
  telepath::BufferHandle reader;
  telepath::BufferHandle flusher;
};

auto AcquireFlushReadHandles(telepath::BufferManager *manager) -> FlushReadHandles {
  auto reader_result = manager->ReadBuffer(31, 0);
  auto flush_result = manager->ReadBuffer(31, 0);
  assert(reader_result.ok());
  assert(flush_result.ok());
  return {
    std::move(reader_result.value()),
    std::move(flush_result.value()),
  };
}

void ExpectFlushCompletesWhileReadHandleStaysPinned(
  telepath::BufferManager *manager,
  FlushReadHandles handles
) {
  assert(handles.reader.data()[0] == std::byte{0x6C});

  std::atomic<bool> flush_finished{false};
  telepath::Status flush_status = telepath::Status::Ok();
  std::thread flush_thread([&]() {
    flush_status = manager->FlushBuffer(handles.flusher);
    flush_finished.store(true);
  });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  while (!flush_finished.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(flush_finished.load());
  flush_thread.join();
  assert(flush_status.ok());
}

void ExpectPersistedPageMatches(const std::filesystem::path &root) {
  auto verifier = std::make_unique<telepath::PosixDiskBackend>(root.string(), 4096);
  std::array<std::byte, 4096> page{};
  auto request = verifier->SubmitRead(telepath::BufferTag{31, 0}, page.data(), page.size());
  assert(request.ok());

  auto completion = verifier->PollCompletion();
  assert(completion.ok());
  assert(completion.value().status.ok());
  assert(page[0] == std::byte{0x6C});
  assert(page[1] == std::byte{0x7D});
}

}  // namespace

int main() {
  telepath::buffer_test_support::TestRootGuard root_guard("telepath_read_lock_flush_test_data");
  auto manager = BuildManager(root_guard.path());
  SeedDirtyPage(&manager);
  auto handles = AcquireFlushReadHandles(&manager);
  ExpectFlushCompletesWhileReadHandleStaysPinned(&manager, std::move(handles));
  ExpectPersistedPageMatches(root_guard.path());
  return 0;
}
