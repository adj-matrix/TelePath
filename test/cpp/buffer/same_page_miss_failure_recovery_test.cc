#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

class CoordinatedReadBackend : public telepath::DiskBackend {
 public:
  explicit CoordinatedReadBackend(std::size_t page_size) : page_size_(page_size) {}

  telepath::Result<uint64_t> SubmitRead(const telepath::BufferTag &tag,
                                        std::byte *out,
                                        std::size_t size) override {
    if (out == nullptr || size != page_size_) {
      return telepath::Status::InvalidArgument("invalid read request");
    }
    std::lock_guard<std::mutex> guard(latch_);
    const uint64_t request_id = next_request_id_++;
    pending_.push_back({request_id, telepath::DiskOperation::kRead, tag,
                        out, nullptr, size});
    ++submitted_reads_;
    cv_.notify_all();
    cv_.notify_one();
    return request_id;
  }

  telepath::Result<uint64_t> SubmitWrite(const telepath::BufferTag &tag,
                                         const std::byte *data,
                                         std::size_t size) override {
    if (data == nullptr || size != page_size_) {
      return telepath::Status::InvalidArgument("invalid write request");
    }
    std::lock_guard<std::mutex> guard(latch_);
    const uint64_t request_id = next_request_id_++;
    pending_.push_back({request_id, telepath::DiskOperation::kWrite, tag,
                        nullptr, data, size});
    cv_.notify_one();
    return request_id;
  }

  telepath::Result<telepath::DiskCompletion> PollCompletion() override {
    telepath::DiskRequest request;
    {
      std::unique_lock<std::mutex> lock(latch_);
      cv_.wait(lock, [&]() { return shutdown_ || !pending_.empty(); });
      if (shutdown_ && pending_.empty()) {
        return telepath::Status::Unavailable("backend shutting down");
      }
      request = pending_.front();
      pending_.pop_front();
      cv_.wait(lock, [&]() { return shutdown_ || completions_allowed_; });
      if (shutdown_) {
        return telepath::Status::Unavailable("backend shutting down");
      }
    }

    if (request.operation == telepath::DiskOperation::kRead) {
      if (fail_reads_.load()) {
        return telepath::DiskCompletion{
            request.request_id, request.operation, request.tag,
            telepath::Status::IoError("forced concurrent read failure")};
      }
      for (std::size_t i = 0; i < request.size; ++i) {
        request.mutable_buffer[i] = std::byte{0};
      }
      request.mutable_buffer[0] = std::byte{0x6D};
      return telepath::DiskCompletion{
          request.request_id, request.operation, request.tag,
          telepath::Status::Ok()};
    }

    return telepath::DiskCompletion{request.request_id, request.operation,
                                    request.tag, telepath::Status::Ok()};
  }

  void Shutdown() override {
    std::lock_guard<std::mutex> guard(latch_);
    shutdown_ = true;
    cv_.notify_all();
  }

  telepath::DiskBackendCapabilities GetCapabilities() const override {
    return {telepath::DiskBackendKind::kPosix, false, false, 1, false};
  }

  void set_fail_reads(bool value) { fail_reads_.store(value); }
  int submitted_reads() const { return submitted_reads_.load(); }

  void BlockCompletions() {
    std::lock_guard<std::mutex> guard(latch_);
    completions_allowed_ = false;
  }

  void AllowCompletions() {
    std::lock_guard<std::mutex> guard(latch_);
    completions_allowed_ = true;
    cv_.notify_all();
  }

  void WaitForSubmittedReads(int expected_count) {
    std::unique_lock<std::mutex> lock(latch_);
    cv_.wait(lock, [&]() {
      return shutdown_ || submitted_reads_.load() >= expected_count;
    });
  }

 private:
  std::size_t page_size_{0};
  mutable std::mutex latch_;
  std::condition_variable cv_;
  std::deque<telepath::DiskRequest> pending_;
  uint64_t next_request_id_{1};
  std::atomic<bool> fail_reads_{true};
  std::atomic<int> submitted_reads_{0};
  bool shutdown_{false};
  bool completions_allowed_{false};
};

}  // namespace

int main() {
  auto backend = std::make_unique<CoordinatedReadBackend>(4096);
  auto *backend_ptr = backend.get();
  auto replacer = telepath::MakeClockReplacer(4);
  auto telemetry = telepath::MakeCounterTelemetrySink();
  telepath::BufferManager manager(4, 4096, std::move(backend),
                                  std::move(replacer), telemetry);

  std::mutex start_latch;
  std::condition_variable start_cv;
  bool start = false;

  backend_ptr->BlockCompletions();
  std::atomic<int> started_fail_reads{0};
  std::atomic<int> failed_reads{0};
  std::vector<std::thread> failing_workers;
  failing_workers.reserve(6);
  for (int i = 0; i < 6; ++i) {
    failing_workers.emplace_back([&]() {
      std::unique_lock<std::mutex> lock(start_latch);
      start_cv.wait(lock, [&]() { return start; });
      lock.unlock();
      ++started_fail_reads;
      auto result = manager.ReadBuffer(41, 9);
      if (!result.ok() &&
          result.status().code() == telepath::StatusCode::kIoError) {
        ++failed_reads;
      }
    });
  }
  {
    std::lock_guard<std::mutex> guard(start_latch);
    start = true;
  }
  start_cv.notify_all();
  while (started_fail_reads.load() != 6) {
    std::this_thread::yield();
  }
  backend_ptr->WaitForSubmittedReads(1);
  backend_ptr->AllowCompletions();
  for (auto &worker : failing_workers) {
    worker.join();
  }

  assert(failed_reads.load() == 6);
  const int reads_after_failure = backend_ptr->submitted_reads();
  assert(reads_after_failure >= 1);

  backend_ptr->set_fail_reads(false);
  backend_ptr->BlockCompletions();
  {
    std::lock_guard<std::mutex> guard(start_latch);
    start = false;
  }

  std::atomic<int> started_recovery_reads{0};
  std::atomic<int> successful_reads{0};
  std::vector<std::thread> recovery_workers;
  recovery_workers.reserve(6);
  for (int i = 0; i < 6; ++i) {
    recovery_workers.emplace_back([&]() {
      std::unique_lock<std::mutex> lock(start_latch);
      start_cv.wait(lock, [&]() { return start; });
      lock.unlock();
      ++started_recovery_reads;
      auto result = manager.ReadBuffer(41, 9);
      if (!result.ok()) {
        return;
      }
      telepath::BufferHandle handle = std::move(result.value());
      if (handle.data()[0] == std::byte{0x6D}) {
        ++successful_reads;
      }
    });
  }
  {
    std::lock_guard<std::mutex> guard(start_latch);
    start = true;
  }
  start_cv.notify_all();
  while (started_recovery_reads.load() != 6) {
    std::this_thread::yield();
  }
  backend_ptr->WaitForSubmittedReads(2);
  backend_ptr->AllowCompletions();
  for (auto &worker : recovery_workers) {
    worker.join();
  }

  assert(successful_reads.load() == 6);
  const int reads_after_recovery = backend_ptr->submitted_reads();
  assert(reads_after_recovery >= reads_after_failure + 1);

  auto warm_result = manager.ReadBuffer(41, 9);
  assert(warm_result.ok());
  telepath::BufferHandle warm_handle = std::move(warm_result.value());
  assert(warm_handle.data()[0] == std::byte{0x6D});
  assert(backend_ptr->submitted_reads() == reads_after_recovery);

  const telepath::TelemetrySnapshot snapshot = telemetry->Snapshot();
  assert(snapshot.buffer_misses >= 2);
  assert(snapshot.buffer_hits >= 1);
  return 0;
}
