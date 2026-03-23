#include <cassert>
#include <filesystem>
#include <memory>

#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

int main() {
  namespace fs = std::filesystem;

  const fs::path root =
      fs::temp_directory_path() / "telepath_resource_exhaustion_test_data";
  fs::remove_all(root);
  fs::create_directories(root);

  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto disk_backend =
      std::make_unique<telepath::PosixDiskBackend>(root.string(), 4096);
  auto replacer = telepath::MakeClockReplacer(1);
  telepath::BufferManager manager(1, 4096, std::move(disk_backend),
                                  std::move(replacer), telemetry);

  auto first = manager.ReadBuffer(5, 0);
  assert(first.ok());
  telepath::BufferHandle pinned = std::move(first.value());

  auto second = manager.ReadBuffer(5, 1);
  assert(!second.ok());
  assert(second.status().code() == telepath::StatusCode::kResourceExhausted);

  pinned.Reset();
  auto retry = manager.ReadBuffer(5, 1);
  assert(retry.ok());

  fs::remove_all(root);
  return 0;
}
