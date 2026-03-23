#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <thread>

#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

int main() {
  namespace fs = std::filesystem;

  const fs::path root =
      fs::temp_directory_path() / "telepath_flush_consistency_test_data";
  fs::remove_all(root);
  fs::create_directories(root);

  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto disk_backend =
      std::make_unique<telepath::PosixDiskBackend>(root.string(), 4096);
  auto replacer = telepath::MakeClockReplacer(2);
  telepath::BufferManager manager(2, 4096, std::move(disk_backend),
                                  std::move(replacer), telemetry);

  auto writer_result = manager.ReadBuffer(21, 0);
  assert(writer_result.ok());
  telepath::BufferHandle writer = std::move(writer_result.value());

  auto flush_result = manager.ReadBuffer(21, 0);
  assert(flush_result.ok());
  telepath::BufferHandle flusher = std::move(flush_result.value());

  writer.mutable_data()[0] = std::byte{0xA5};
  assert(manager.MarkBufferDirty(writer).ok());

  std::atomic<bool> flush_started{false};
  std::atomic<bool> flush_finished{false};
  telepath::Status flush_status = telepath::Status::Ok();
  std::thread flush_thread([&]() {
    flush_started.store(true);
    flush_status = manager.FlushBuffer(flusher);
    flush_finished.store(true);
  });

  while (!flush_started.load()) {
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  assert(!flush_finished.load());

  writer.Reset();
  flush_thread.join();
  assert(flush_status.ok());
  assert(flush_finished.load());

  auto verifier =
      std::make_unique<telepath::PosixDiskBackend>(root.string(), 4096);
  std::array<std::byte, 4096> page{};
  auto request = verifier->SubmitRead(telepath::BufferTag{21, 0}, page.data(),
                                      page.size());
  assert(request.ok());
  auto completion = verifier->PollCompletion();
  assert(completion.ok());
  assert(completion.value().status.ok());
  assert(page[0] == std::byte{0xA5});

  fs::remove_all(root);
  return 0;
}
