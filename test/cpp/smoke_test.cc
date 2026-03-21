#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/clock_replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

int main() {
  namespace fs = std::filesystem;
  using telepath::BlockId;
  using telepath::BufferManager;
  using telepath::FileId;
  using telepath::MakeCounterTelemetrySink;
  using telepath::PosixDiskBackend;
  using telepath::ClockReplacer;

  const fs::path root = fs::temp_directory_path() / "telepath_smoke_data";
  fs::create_directories(root);

  auto telemetry = MakeCounterTelemetrySink();
  auto disk_backend =
      std::make_unique<PosixDiskBackend>(root.string(), 4096);
  auto replacer = std::make_unique<ClockReplacer>(2);
  BufferManager manager(2, 4096, std::move(disk_backend), std::move(replacer),
                        telemetry);

  const FileId file_id = 1;
  const BlockId block_id = 0;

  auto first_result = manager.ReadBuffer(file_id, block_id);
  assert(first_result.ok());
  telepath::BufferHandle first_handle = std::move(first_result.value());
  std::byte *data = first_handle.mutable_data();
  data[0] = std::byte{0x2A};
  data[1] = std::byte{0x0B};
  assert(manager.MarkBufferDirty(first_handle).ok());
  assert(manager.FlushBuffer(first_handle).ok());
  first_handle.Reset();

  auto second_result = manager.ReadBuffer(file_id, block_id);
  assert(second_result.ok());
  telepath::BufferHandle second_handle = std::move(second_result.value());
  const std::byte *read_back = second_handle.data();
  assert(std::to_integer<int>(read_back[0]) == 0x2A);
  assert(std::to_integer<int>(read_back[1]) == 0x0B);
  second_handle.Reset();

  const telepath::TelemetrySnapshot snapshot = telemetry->Snapshot();
  assert(snapshot.buffer_misses >= 1);
  assert(snapshot.disk_reads >= 1);
  assert(snapshot.disk_writes >= 1);
  assert(snapshot.dirty_flushes >= 1);

  fs::remove_all(root);
  return 0;
}
