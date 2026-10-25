#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

#include "polling_disk_backend.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

class FailingDiskBackend : public telepath::test_support::PollingDiskBackend {
 public:
  explicit FailingDiskBackend(std::size_t page_size) : page_size_(page_size) {}

  auto SubmitRead(
    const telepath::BufferTag &tag,
    std::byte *out,
    std::size_t size
  ) -> telepath::Result<uint64_t> override {
    auto validate_status = ValidateReadRequest(out, size);
    if (!validate_status.has_value()) return std::unexpected(validate_status.error());

    std::lock_guard<std::mutex> guard(latch_);
    const uint64_t request_id = next_request_id_++;
    pending_.push_back({request_id, telepath::DiskOperation::kRead, tag, out, nullptr, size});
    return request_id;
  }

  auto SubmitWrite(
    const telepath::BufferTag &tag,
    const std::byte *data,
    std::size_t size
  ) -> telepath::Result<uint64_t> override {
    auto validate_status = ValidateWriteRequest(data, size);
    if (!validate_status.has_value()) return std::unexpected(validate_status.error());

    std::lock_guard<std::mutex> guard(latch_);
    const uint64_t request_id = next_request_id_++;
    pending_.push_back({request_id, telepath::DiskOperation::kWrite, tag, nullptr, data, size});
    return request_id;
  }

  auto PollCompletion() -> telepath::Result<telepath::test_support::LegacyDiskCompletion> override {
    auto request = TakePendingRequest();
    if (!request.has_value()) return std::unexpected(request.error());
    return CompleteRequest(request.value());
  }

  void OnShutdown() override {}


  void set_fail_reads(bool value) { fail_reads_ = value; }
  void set_fail_writes(bool value) { fail_writes_ = value; }

 private:
  auto ValidateReadRequest(
    std::byte *out,
    std::size_t size
  ) const -> telepath::Status {
    if (out == nullptr) return telepath::InvalidArgument("invalid read request");
    if (size != page_size_) return telepath::InvalidArgument("invalid read request");
    return telepath::Ok();
  }

  auto ValidateWriteRequest(
    const std::byte *data,
    std::size_t size
  ) const -> telepath::Status {
    if (data == nullptr) return telepath::InvalidArgument("invalid write request");
    if (size != page_size_) return telepath::InvalidArgument("invalid write request");
    return telepath::Ok();
  }

  auto TakePendingRequest() -> telepath::Result<telepath::test_support::LegacyDiskRequest> {
    std::lock_guard<std::mutex> guard(latch_);
    if (pending_.empty()) return telepath::Unavailable("no pending request");

    auto request = pending_.front();
    pending_.pop_front();
    return request;
  }

  auto CompleteRequest(
    const telepath::test_support::LegacyDiskRequest &request
  ) -> telepath::Result<telepath::test_support::LegacyDiskCompletion> {
    if (request.operation == telepath::DiskOperation::kRead && fail_reads_) {
      return telepath::test_support::LegacyDiskCompletion{
        request.request_id,
        request.operation,
        request.tag,
        telepath::IoError("forced read failure"),
      };
    }
    if (request.operation == telepath::DiskOperation::kWrite && fail_writes_) {
      return telepath::test_support::LegacyDiskCompletion{
        request.request_id,
        request.operation,
        request.tag,
        telepath::IoError("forced write failure"),
      };
    }

    if (request.operation == telepath::DiskOperation::kRead) {
      for (std::size_t i = 0; i < request.size; ++i) {
        request.mutable_buffer[i] = std::byte{0};
      }
    }
    return telepath::test_support::LegacyDiskCompletion{
      request.request_id,
      request.operation,
      request.tag,
      telepath::Ok(),
    };
  }

  std::size_t page_size_{0};
  std::mutex latch_;
  std::deque<telepath::test_support::LegacyDiskRequest> pending_;
  uint64_t next_request_id_{1};
  bool fail_reads_{false};
  bool fail_writes_{false};
};

void ExpectReadFailurePropagates() {
  auto backend = std::make_unique<FailingDiskBackend>(4096);
  backend->set_fail_reads(true);
  auto replacer = telepath::MakeClockReplacer(2);
  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto manager_owner = telepath::BufferManager::Create(2, 4096, std::move(backend), std::move(replacer), telemetry);
  assert(manager_owner.has_value());
  telepath::BufferManager &manager = *manager_owner.value();

  auto result = manager.ReadBuffer(1, 0);
  assert(!result.has_value());
  assert(result.error().code() == telepath::ErrorCode::kIoError);
}

void ExpectWriteFailurePropagates() {
  auto backend = std::make_unique<FailingDiskBackend>(4096);
  auto *backend_ptr = backend.get();
  auto replacer = telepath::MakeClockReplacer(2);
  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto manager_owner = telepath::BufferManager::Create(2, 4096, std::move(backend), std::move(replacer), telemetry);
  assert(manager_owner.has_value());
  telepath::BufferManager &manager = *manager_owner.value();

  auto result = manager.ReadBufferExclusive(1, 1);
  assert(result.has_value());

  auto handle = std::move(result.value());
  handle.mutable_data()[0] = std::byte{0x5A};

  backend_ptr->set_fail_writes(true);
  const auto flush_status = manager.FlushBuffer(handle);
  assert(!flush_status.has_value());
  assert(flush_status.error().code() == telepath::ErrorCode::kIoError);
}

}  // namespace

int main() {
  ExpectReadFailurePropagates();
  ExpectWriteFailurePropagates();
  return 0;
}
