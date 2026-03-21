#include <cassert>
#include <filesystem>
#include <memory>

#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

int main() {
  namespace fs = std::filesystem;

  const fs::path root = fs::temp_directory_path() / "telepath_handle_test_data";
  fs::remove_all(root);
  fs::create_directories(root);

  auto telemetry = telepath::MakeCounterTelemetrySink();
  auto disk_backend =
      std::make_unique<telepath::PosixDiskBackend>(root.string(), 4096);
  auto replacer = telepath::MakeClockReplacer(2);
  telepath::BufferManager manager(2, 4096, std::move(disk_backend),
                                  std::move(replacer), telemetry);

  auto result = manager.ReadBuffer(7, 1);
  assert(result.ok());
  telepath::BufferHandle handle = std::move(result.value());

  assert(handle.valid());
  assert(handle.writable());
  handle.mutable_data()[0] = std::byte{0x4D};
  assert(manager.MarkBufferDirty(handle).ok());

  assert(manager.ReleaseBuffer(std::move(handle)).ok());
  assert(!handle.valid());
  assert(manager.MarkBufferDirty(handle).code() ==
         telepath::StatusCode::kInvalidArgument);
  assert(manager.ReleaseBuffer(std::move(handle)).code() ==
         telepath::StatusCode::kInvalidArgument);

  fs::remove_all(root);
  return 0;
}
