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
      fs::temp_directory_path() / "telepath_read_lock_flush_test_data";
  fs::remove_all(root);
  fs::create_directories(root);

  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto disk_backend =
      std::make_unique<telepath::PosixDiskBackend>(root.string(), 4096);
  auto replacer = telepath::MakeClockReplacer(2);
  telepath::BufferManager manager(2, 4096, std::move(disk_backend),
                                  std::move(replacer), telemetry);

  {
    auto seed_result = manager.ReadBuffer(31, 0);
    assert(seed_result.ok());
    telepath::BufferHandle seed = std::move(seed_result.value());
    seed.mutable_data()[0] = std::byte{0x6C};
    seed.mutable_data()[1] = std::byte{0x7D};
    assert(manager.MarkBufferDirty(seed).ok());
  }

  auto reader_result = manager.ReadBuffer(31, 0);
  assert(reader_result.ok());
  telepath::BufferHandle reader = std::move(reader_result.value());
  const std::byte *reader_data = reader.data();
  assert(reader_data[0] == std::byte{0x6C});

  auto flush_result = manager.ReadBuffer(31, 0);
  assert(flush_result.ok());
  telepath::BufferHandle flusher = std::move(flush_result.value());

  std::atomic<bool> flush_finished{false};
  telepath::Status flush_status = telepath::Status::Ok();
  std::thread flush_thread([&]() {
    flush_status = manager.FlushBuffer(flusher);
    flush_finished.store(true);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  assert(flush_finished.load());
  flush_thread.join();
  assert(flush_status.ok());

  auto verifier =
      std::make_unique<telepath::PosixDiskBackend>(root.string(), 4096);
  std::array<std::byte, 4096> page{};
  auto request = verifier->SubmitRead(telepath::BufferTag{31, 0}, page.data(),
                                      page.size());
  assert(request.ok());
  auto completion = verifier->PollCompletion();
  assert(completion.ok());
  assert(completion.value().status.ok());
  assert(page[0] == std::byte{0x6C});
  assert(page[1] == std::byte{0x7D});

  fs::remove_all(root);
  return 0;
}
