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

struct FlushConsistencyHandles {
  telepath::BufferHandle writer;
  telepath::BufferHandle flusher;
};

auto AcquireFlushConsistencyHandles(telepath::BufferManager *manager) -> FlushConsistencyHandles {
  auto writer_result = manager->ReadBuffer(21, 0);
  auto flush_result = manager->ReadBuffer(21, 0);
  assert(writer_result.ok());
  assert(flush_result.ok());
  return {
    std::move(writer_result.value()),
    std::move(flush_result.value()),
  };
}

void ExpectFlushWaitsForWriterRelease(
  telepath::BufferManager *manager,
  FlushConsistencyHandles handles
) {
  handles.writer.mutable_data()[0] = std::byte{0xA5};
  assert(manager->MarkBufferDirty(handles.writer).ok());

  std::atomic<bool> flush_started{false};
  std::atomic<bool> flush_finished{false};
  telepath::Status flush_status = telepath::Status::Ok();
  std::thread flush_thread([&]() {
    flush_started.store(true);
    flush_status = manager->FlushBuffer(handles.flusher);
    flush_finished.store(true);
  });

  while (!flush_started.load()) {
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  assert(!flush_finished.load());

  handles.writer.Reset();
  flush_thread.join();
  assert(flush_status.ok());
  assert(flush_finished.load());
}

void ExpectPersistedPageContainsFlushedByte(
  const std::filesystem::path &root
) {
  auto verifier = std::make_unique<telepath::PosixDiskBackend>(root.string(), 4096);
  std::array<std::byte, 4096> page{};
  auto request = verifier->SubmitRead(telepath::BufferTag{21, 0}, page.data(), page.size());
  assert(request.ok());

  auto completion = verifier->PollCompletion();
  assert(completion.ok());
  assert(completion.value().status.ok());
  assert(page[0] == std::byte{0xA5});
}

}  // namespace

int main() {
  telepath::buffer_test_support::TestRootGuard root_guard("telepath_flush_consistency_test_data");
  auto manager = BuildManager(root_guard.path());
  auto handles = AcquireFlushConsistencyHandles(&manager);
  ExpectFlushWaitsForWriterRelease(&manager, std::move(handles));
  ExpectPersistedPageContainsFlushedByte(root_guard.path());
  return 0;
}
