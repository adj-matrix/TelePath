#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

#include "telepath/buffer/buffer_manager.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

// Accepts submissions, then reports a broken completion channel on the first
// PollCompletion() call.
class BrokenCompletionBackend : public telepath::DiskBackend {
 public:
  auto SubmitRead(const telepath::BufferTag &, std::byte *, std::size_t) -> telepath::Result<uint64_t> override {
    return Submit();
  }

  auto SubmitWrite(const telepath::BufferTag &, const std::byte *, std::size_t) -> telepath::Result<uint64_t> override {
    return Submit();
  }

  auto PollCompletion() -> telepath::Result<telepath::DiskCompletion> override {
    std::unique_lock<std::mutex> lock(latch_);
    cv_.wait(lock, [&]() { return submitted_ || shutdown_; });
    if (shutdown_ && !submitted_) return telepath::Unavailable("backend shutting down");
    return telepath::IoError("completion channel broken", 5);
  }

  void Shutdown() override {
    std::lock_guard<std::mutex> guard(latch_);
    shutdown_ = true;
    cv_.notify_all();
  }

  auto GetCapabilities() const -> telepath::DiskBackendCapabilities override {
    return {telepath::DiskBackendKind::kPosix, false, false, 1, false};
  }

 private:
  auto Submit() -> telepath::Result<uint64_t> {
    std::lock_guard<std::mutex> guard(latch_);
    submitted_ = true;
    cv_.notify_all();
    return next_request_id_++;
  }

  std::mutex latch_;
  std::condition_variable cv_;
  bool submitted_{false};
  bool shutdown_{false};
  uint64_t next_request_id_{1};
};

void ExpectBrokenCompletionChannelFailsInstance() {
  auto manager_result = telepath::BufferManager::Create(
    2, 4096, std::make_unique<BrokenCompletionBackend>(), telepath::MakeClockReplacer(2), telepath::MakeNoOpTelemetrySink());
  assert(manager_result.has_value());
  telepath::BufferManager &manager = *manager_result.value();
  assert(!manager.failure().has_value());

  auto first_read = manager.ReadBuffer(1, 0);
  assert(!first_read.has_value());

  const auto failure = manager.failure();
  assert(failure.has_value());
  assert(failure->code() == telepath::ErrorCode::kIoError);
  assert(failure->system_error() == 5);

  auto later_read = manager.ReadBuffer(1, 1);
  assert(!later_read.has_value());
  assert(later_read.error().code() == telepath::ErrorCode::kFailed);
  assert(later_read.error().error_class() == telepath::ErrorClass::kFatal);

  const auto flush_all_status = manager.FlushAll();
  assert(!flush_all_status.has_value());
  assert(flush_all_status.error().code() == telepath::ErrorCode::kFailed);
}

void ExpectOrderlyShutdownDoesNotFailInstance() {
  auto manager_result = telepath::BufferManager::Create(
    2, 4096, std::make_unique<BrokenCompletionBackend>(), telepath::MakeClockReplacer(2), telepath::MakeNoOpTelemetrySink());
  assert(manager_result.has_value());
  assert(!manager_result.value()->failure().has_value());
}

}  // namespace

int main() {
  ExpectBrokenCompletionChannelFailsInstance();
  ExpectOrderlyShutdownDoesNotFailInstance();
  return 0;
}
