#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#include "completion_dispatcher.h"
#include "telepath/io/disk_backend.h"

namespace {

class ScriptedCompletionBackend : public telepath::DiskBackend {
 public:
  telepath::Result<uint64_t> SubmitRead(const telepath::BufferTag &, std::byte *, std::size_t) override {
    return telepath::Unavailable("not used");
  }

  telepath::Result<uint64_t> SubmitWrite(const telepath::BufferTag &, const std::byte *, std::size_t) override {
    return telepath::Unavailable("not used");
  }

  telepath::Result<telepath::DiskCompletion> PollCompletion() override {
    std::unique_lock<std::mutex> lock(latch_);
    cv_.wait(lock, [&]() { return shutdown_ || !results_.empty(); });
    if (results_.empty()) return telepath::Unavailable("backend shutdown");

    auto result = std::move(results_.front());
    results_.pop_front();
    if (result.has_value()) {
      last_polled_request_id_ = result.value().request_id;
      has_polled_request_ = true;
      cv_.notify_all();
    }
    return result;
  }

  void Shutdown() override {
    std::lock_guard<std::mutex> guard(latch_);
    shutdown_ = true;
    cv_.notify_all();
  }

  telepath::DiskBackendCapabilities GetCapabilities() const override {
    return {telepath::DiskBackendKind::kPosix, false, false, 1, false};
  }

  void PushCompletion(uint64_t request_id, const telepath::Status &status) {
    std::lock_guard<std::mutex> guard(latch_);
    results_.push_back(telepath::DiskCompletion{request_id, telepath::DiskOperation::kWrite, {1, request_id}, status});
    cv_.notify_all();
  }

  void PushFailure(const telepath::Status &status) {
    std::lock_guard<std::mutex> guard(latch_);
    results_.push_back(std::unexpected(status.error()));
    cv_.notify_all();
  }

  bool WaitUntilPolled(uint64_t request_id, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(latch_);
    return cv_.wait_for(lock, timeout, [&]() {
      return has_polled_request_ && last_polled_request_id_ == request_id;
    });
  }

 private:
  std::mutex latch_;
  std::condition_variable cv_;
  std::deque<telepath::Result<telepath::DiskCompletion>> results_;
  uint64_t last_polled_request_id_{0};
  bool has_polled_request_{false};
  bool shutdown_{false};
};

void AssertWaitReturnsCompletionStatus() {
  ScriptedCompletionBackend backend;
  telepath::BufferManagerCompletionDispatcher dispatcher(&backend);
  assert(dispatcher.Start().has_value());

  dispatcher.Register(11);
  backend.PushCompletion(11, telepath::Ok());
  auto status = dispatcher.Wait(11);
  assert(status.has_value());

  dispatcher.Shutdown(telepath::Unavailable("test shutdown"));
}

void AssertOutOfOrderCompletionsReachCorrectWaiters() {
  ScriptedCompletionBackend backend;
  telepath::BufferManagerCompletionDispatcher dispatcher(&backend);
  assert(dispatcher.Start().has_value());

  dispatcher.Register(21);
  dispatcher.Register(22);
  backend.PushCompletion(22, telepath::Ok());
  backend.PushCompletion(21, telepath::Ok());

  auto first_status = dispatcher.Wait(21);
  auto second_status = dispatcher.Wait(22);
  assert(first_status.has_value());
  assert(second_status.has_value());

  dispatcher.Shutdown(telepath::Unavailable("test shutdown"));
}

void AssertBackendFailureUnblocksRegisteredWaiters() {
  ScriptedCompletionBackend backend;
  telepath::BufferManagerCompletionDispatcher dispatcher(&backend);
  assert(dispatcher.Start().has_value());

  dispatcher.Register(31);
  telepath::Status wait_status = telepath::Ok();
  std::thread waiter([&]() { wait_status = dispatcher.Wait(31); });

  backend.PushFailure(telepath::Unavailable("forced completion failure"));
  waiter.join();
  assert(wait_status.error().code() == telepath::ErrorCode::kUnavailable);

  dispatcher.Shutdown(telepath::Unavailable("test shutdown"));
}

void AssertEarlyCompletionIsDeliveredAfterRegistration() {
  ScriptedCompletionBackend backend;
  telepath::BufferManagerCompletionDispatcher dispatcher(&backend);
  assert(dispatcher.Start().has_value());

  dispatcher.Register(41);
  backend.PushCompletion(42, telepath::Ok());
  assert(backend.WaitUntilPolled(42, std::chrono::milliseconds(200)));

  dispatcher.Register(42);
  telepath::Status late_wait_status = dispatcher.Wait(42);
  assert(late_wait_status.has_value());

  backend.PushCompletion(41, telepath::Ok());
  auto original_wait_status = dispatcher.Wait(41);
  assert(original_wait_status.has_value());

  dispatcher.Shutdown(telepath::Unavailable("test shutdown"));
}

}  // namespace

int main() {
  AssertWaitReturnsCompletionStatus();
  AssertOutOfOrderCompletionsReachCorrectWaiters();
  AssertBackendFailureUnblocksRegisteredWaiters();
  AssertEarlyCompletionIsDeliveredAfterRegistration();
  return 0;
}
