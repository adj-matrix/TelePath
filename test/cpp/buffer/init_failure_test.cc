#include <cassert>
#include <memory>

#include "telepath/buffer/buffer_manager.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

class NoOpDiskBackend : public telepath::DiskBackend {
 public:
  auto SubmitRead(
    const telepath::BufferTag &,
    std::byte *,
    std::size_t
  ) -> telepath::Result<uint64_t> override {
    return telepath::Status::Unavailable("not expected");
  }

  auto SubmitWrite(
    const telepath::BufferTag &,
    const std::byte *,
    std::size_t
  ) -> telepath::Result<uint64_t> override {
    return telepath::Status::Unavailable("not expected");
  }

  auto PollCompletion() -> telepath::Result<telepath::DiskCompletion> override {
    return telepath::Status::Unavailable("not expected");
  }

  void Shutdown() override {}

  auto GetCapabilities() const -> telepath::DiskBackendCapabilities override {
    return {
      telepath::DiskBackendKind::kPosix,
      false,
      false,
      1,
      false,
    };
  }
};

auto BuildInvalidManager() -> telepath::BufferManager {
  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto disk_backend = std::make_unique<NoOpDiskBackend>();
  auto replacer = telepath::MakeClockReplacer(1);
  return telepath::BufferManager(0, 4096, std::move(disk_backend), std::move(replacer), telemetry);
}

void ExpectInitializationFailurePaths(telepath::BufferManager *manager) {
  auto read_result = manager->ReadBuffer(1, 0);
  assert(!read_result.ok());
  assert(read_result.status().code() == telepath::StatusCode::kInvalidArgument);

  const auto flush_all_status = manager->FlushAll();
  assert(!flush_all_status.ok());
  assert(flush_all_status.code() == telepath::StatusCode::kInvalidArgument);

  telepath::BufferHandle invalid_handle;
  const auto release_status = manager->ReleaseBuffer(std::move(invalid_handle));
  assert(!release_status.ok());
  assert(release_status.code() == telepath::StatusCode::kInvalidArgument);
}

}  // namespace

int main() {
  auto manager = BuildInvalidManager();
  ExpectInitializationFailurePaths(&manager);
  return 0;
}
