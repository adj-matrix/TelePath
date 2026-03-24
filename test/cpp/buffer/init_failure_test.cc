#include <cassert>
#include <memory>

#include "telepath/buffer/buffer_manager.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

class NoOpDiskBackend : public telepath::DiskBackend {
 public:
  telepath::Result<uint64_t> SubmitRead(const telepath::BufferTag &,
                                        std::byte *,
                                        std::size_t) override {
    return telepath::Status::Unavailable("not expected");
  }

  telepath::Result<uint64_t> SubmitWrite(const telepath::BufferTag &,
                                         const std::byte *,
                                         std::size_t) override {
    return telepath::Status::Unavailable("not expected");
  }

  telepath::Result<telepath::DiskCompletion> PollCompletion() override {
    return telepath::Status::Unavailable("not expected");
  }

  void Shutdown() override {}
};

}  // namespace

int main() {
  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto disk_backend = std::make_unique<NoOpDiskBackend>();
  auto replacer = telepath::MakeClockReplacer(1);

  telepath::BufferManager manager(0, 4096, std::move(disk_backend),
                                  std::move(replacer), telemetry);

  auto read_result = manager.ReadBuffer(1, 0);
  assert(!read_result.ok());
  assert(read_result.status().code() == telepath::StatusCode::kInvalidArgument);

  const telepath::Status flush_all_status = manager.FlushAll();
  assert(!flush_all_status.ok());
  assert(flush_all_status.code() == telepath::StatusCode::kInvalidArgument);

  telepath::BufferHandle invalid_handle;
  const telepath::Status release_status =
      manager.ReleaseBuffer(std::move(invalid_handle));
  assert(!release_status.ok());
  assert(release_status.code() == telepath::StatusCode::kInvalidArgument);

  return 0;
}
