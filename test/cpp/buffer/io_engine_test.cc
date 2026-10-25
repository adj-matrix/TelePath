#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include "io_engine.h"
#include "telepath/io/disk_backend.h"

namespace {

// Completions are scripted by the test. With `inline_completion`, a submitted
// request is ready for non-blocking reaps as soon as Submit() returns, and a
// blocking Reap() never delivers it.
class ScriptedBackend : public telepath::DiskBackend {
 public:
  explicit ScriptedBackend(bool inline_completion = false) : inline_completion_(inline_completion) {}

  auto Submit(std::span<const telepath::DiskRequest> requests, telepath::DiskSubmitMode) -> telepath::Result<std::size_t> override {
    std::lock_guard<std::mutex> guard(latch_);
    if (reject_submits_) return telepath::IoError("submit rejected");
    for (const auto &request : requests) {
      submitted_.push_back(request.token);
      if (inline_completion_) inline_ready_.push_back({request.token, telepath::Ok()});
    }
    cv_.notify_all();
    return requests.size();
  }

  auto Reap(std::span<telepath::DiskCompletion> out, bool wait) -> telepath::Result<std::size_t> override {
    std::unique_lock<std::mutex> lock(latch_);
    if (!wait) {
      std::size_t count = 0;
      while (count < out.size() && !inline_ready_.empty()) {
        out[count++] = inline_ready_.front();
        inline_ready_.pop_front();
      }
      return count;
    }
    ++blocking_reaps_;
    cv_.wait(lock, [&]() { return shutdown_ || failure_.has_value() || !scripted_.empty(); });
    if (failure_.has_value()) return std::unexpected(*failure_);
    if (scripted_.empty()) return telepath::Unavailable("backend shutdown");
    out[0] = scripted_.front();
    scripted_.pop_front();
    return std::size_t{1};
  }

  void Shutdown() override {
    std::lock_guard<std::mutex> guard(latch_);
    shutdown_ = true;
    ++shutdown_calls_;
    cv_.notify_all();
  }

  auto GetCapabilities() const -> telepath::DiskBackendCapabilities override { return {}; }

  void PushCompletion(uint64_t token, const telepath::Status &status) {
    std::lock_guard<std::mutex> guard(latch_);
    scripted_.push_back({token, status});
    cv_.notify_all();
  }

  void Fail(const telepath::Error &error) {
    std::lock_guard<std::mutex> guard(latch_);
    failure_ = error;
    cv_.notify_all();
  }

  void RejectSubmits() {
    std::lock_guard<std::mutex> guard(latch_);
    reject_submits_ = true;
  }

  void WaitForSubmitted(std::size_t count) {
    std::unique_lock<std::mutex> lock(latch_);
    cv_.wait(lock, [&]() { return submitted_.size() >= count; });
  }

  int blocking_reaps() {
    std::lock_guard<std::mutex> guard(latch_);
    return blocking_reaps_;
  }

  int shutdown_calls() {
    std::lock_guard<std::mutex> guard(latch_);
    return shutdown_calls_;
  }

 private:
  bool inline_completion_;
  std::mutex latch_;
  std::condition_variable cv_;
  std::vector<uint64_t> submitted_;
  std::deque<telepath::DiskCompletion> inline_ready_;
  std::deque<telepath::DiskCompletion> scripted_;
  std::optional<telepath::Error> failure_;
  bool reject_submits_{false};
  bool shutdown_{false};
  int blocking_reaps_{0};
  int shutdown_calls_{0};
};

auto TokenOf(telepath::IoWaiter &waiter) -> uint64_t {
  return reinterpret_cast<uint64_t>(static_cast<telepath::IoCompletion *>(&waiter));
}

auto RequestFor(telepath::IoWaiter &waiter) -> telepath::DiskRequest {
  telepath::DiskRequest request;
  request.token = TokenOf(waiter);
  return request;
}

void SubmitOne(telepath::IoEngine &engine, telepath::IoWaiter &waiter) {
  const telepath::DiskRequest request = RequestFor(waiter);
  engine.Submit(std::span<const telepath::DiskRequest>(&request, 1), telepath::DiskSubmitMode::kAsync);
}

void AssertWaitReturnsCompletionStatus() {
  ScriptedBackend backend;
  telepath::IoEngine engine(&backend);
  assert(engine.Start().has_value());

  telepath::IoWaiter ok_waiter;
  telepath::IoWaiter failed_waiter;
  SubmitOne(engine, ok_waiter);
  SubmitOne(engine, failed_waiter);
  backend.PushCompletion(TokenOf(failed_waiter), telepath::IoError("disk failed", 5));
  backend.PushCompletion(TokenOf(ok_waiter), telepath::Ok());

  assert(engine.Wait(ok_waiter).has_value());
  telepath::Status failed = engine.Wait(failed_waiter);
  assert(!failed.has_value());
  assert(failed.error().code() == telepath::ErrorCode::kIoError);
  assert(failed.error().system_error() == 5);
  engine.Shutdown(telepath::Unavailable("test shutdown"));
}

void AssertWaiterCountsDownSeveralRequests() {
  ScriptedBackend backend;
  telepath::IoEngine engine(&backend);
  assert(engine.Start().has_value());

  telepath::IoWaiter waiter(0);
  waiter.Add(3);
  std::array<telepath::DiskRequest, 3> requests{RequestFor(waiter), RequestFor(waiter), RequestFor(waiter)};
  engine.Submit(requests, telepath::DiskSubmitMode::kAsync);
  std::thread completer([&]() {
    backend.PushCompletion(TokenOf(waiter), telepath::Ok());
    backend.PushCompletion(TokenOf(waiter), telepath::IoError("second failed"));
    backend.PushCompletion(TokenOf(waiter), telepath::Ok());
  });
  telepath::Status status = engine.Wait(waiter);
  completer.join();
  assert(waiter.done());
  assert(!status.has_value());
  assert(status.error().code() == telepath::ErrorCode::kIoError);
  engine.Shutdown(telepath::Unavailable("test shutdown"));
}

void AssertWaiterReapsInlineCompletionItself() {
  ScriptedBackend backend(true);
  telepath::IoEngine engine(&backend);
  assert(engine.Start().has_value());

  // The blocking reaper never sees inline completions, so only the waiting
  // thread can deliver this one.
  telepath::IoWaiter waiter;
  SubmitOne(engine, waiter);
  assert(engine.Wait(waiter).has_value());
  engine.Shutdown(telepath::Unavailable("test shutdown"));
}

void AssertRejectedSubmitCompletesWithError() {
  ScriptedBackend backend;
  backend.RejectSubmits();
  telepath::IoEngine engine(&backend);
  assert(engine.Start().has_value());

  telepath::IoWaiter waiter;
  SubmitOne(engine, waiter);
  assert(waiter.done());
  telepath::Status status = engine.Wait(waiter);
  assert(!status.has_value());
  assert(status.error().code() == telepath::ErrorCode::kIoError);
  engine.Shutdown(telepath::Unavailable("test shutdown"));
}

void AssertBackendFailureIsFatalAndUnblocksWaiters() {
  ScriptedBackend backend;
  telepath::IoEngine engine(&backend);
  std::optional<telepath::Error> fatal;
  assert(engine.Start([&](const telepath::Error &error) { fatal = error; }).has_value());

  telepath::IoWaiter waiter;
  SubmitOne(engine, waiter);
  telepath::Status status = telepath::Ok();
  std::thread blocked([&]() { status = engine.Wait(waiter); });
  backend.WaitForSubmitted(1);
  backend.Fail(telepath::Error(telepath::ErrorCode::kIoError, "completion channel broken", 5));
  blocked.join();

  assert(!status.has_value());
  assert(status.error().code() == telepath::ErrorCode::kIoError);
  assert(fatal.has_value() && fatal->system_error() == 5);
  engine.Shutdown(telepath::Unavailable("test shutdown"));
}

void AssertIdleReaperDoesNotSpin() {
  ScriptedBackend backend;
  {
    telepath::IoEngine engine(&backend);
    assert(engine.Start().has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(backend.blocking_reaps() <= 1);
    engine.Shutdown(telepath::Unavailable("test shutdown"));
    engine.Shutdown(telepath::Unavailable("second shutdown"));
  }
  assert(backend.shutdown_calls() == 1);
}

void AssertWaitAfterShutdownFails() {
  ScriptedBackend backend;
  telepath::IoEngine engine(&backend);
  assert(engine.Start().has_value());
  engine.Shutdown(telepath::Unavailable("engine stopped for test"));

  telepath::IoWaiter waiter;
  SubmitOne(engine, waiter);
  telepath::Status status = engine.Wait(waiter);
  assert(!status.has_value());
  assert(status.error().code() == telepath::ErrorCode::kUnavailable);
}

}  // namespace

int main() {
  AssertWaitReturnsCompletionStatus();
  AssertWaiterCountsDownSeveralRequests();
  AssertWaiterReapsInlineCompletionItself();
  AssertRejectedSubmitCompletesWithError();
  AssertBackendFailureIsFatalAndUnblocksWaiters();
  AssertIdleReaperDoesNotSpin();
  AssertWaitAfterShutdownFails();
  return 0;
}
