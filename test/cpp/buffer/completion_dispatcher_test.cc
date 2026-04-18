#include <cassert>
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
    return telepath::Status::Unavailable("not used");
  }

  telepath::Result<uint64_t> SubmitWrite(const telepath::BufferTag &, const std::byte *, std::size_t) override {
    return telepath::Status::Unavailable("not used");
  }

  telepath::Result<telepath::DiskCompletion> PollCompletion() override {
    std::unique_lock<std::mutex> lock(latch_);
    cv_.wait(lock, [&]() { return shutdown_ || !results_.empty(); });
    if (results_.empty()) return telepath::Status::Unavailable("backend shutdown");

    auto result = std::move(results_.front());
    results_.pop_front();
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
    results_.push_back(status);
    cv_.notify_all();
  }

 private:
  std::mutex latch_;
  std::condition_variable cv_;
  std::deque<telepath::Result<telepath::DiskCompletion>> results_;
  bool shutdown_{false};
};

void AssertWaitReturnsCompletionStatus() {
  ScriptedCompletionBackend backend;
  telepath::BufferManagerCompletionDispatcher dispatcher(&backend);
  assert(dispatcher.Start().ok());

  dispatcher.Register(11);
  backend.PushCompletion(11, telepath::Status::Ok());
  auto status = dispatcher.Wait(11);
  assert(status.ok());

  dispatcher.Shutdown(telepath::Status::Unavailable("test shutdown"));
}

void AssertOutOfOrderCompletionsReachCorrectWaiters() {
  ScriptedCompletionBackend backend;
  telepath::BufferManagerCompletionDispatcher dispatcher(&backend);
  assert(dispatcher.Start().ok());

  dispatcher.Register(21);
  dispatcher.Register(22);
  backend.PushCompletion(22, telepath::Status::Ok());
  backend.PushCompletion(21, telepath::Status::Ok());

  auto first_status = dispatcher.Wait(21);
  auto second_status = dispatcher.Wait(22);
  assert(first_status.ok());
  assert(second_status.ok());

  dispatcher.Shutdown(telepath::Status::Unavailable("test shutdown"));
}

void AssertBackendFailureUnblocksRegisteredWaiters() {
  ScriptedCompletionBackend backend;
  telepath::BufferManagerCompletionDispatcher dispatcher(&backend);
  assert(dispatcher.Start().ok());

  dispatcher.Register(31);
  telepath::Status wait_status = telepath::Status::Ok();
  std::thread waiter([&]() { wait_status = dispatcher.Wait(31); });

  backend.PushFailure(telepath::Status::Unavailable("forced completion failure"));
  waiter.join();
  assert(wait_status.code() == telepath::StatusCode::kUnavailable);

  dispatcher.Shutdown(telepath::Status::Unavailable("test shutdown"));
}

}  // namespace

int main() {
  AssertWaitReturnsCompletionStatus();
  AssertOutOfOrderCompletionsReachCorrectWaiters();
  AssertBackendFailureUnblocksRegisteredWaiters();
  return 0;
}
