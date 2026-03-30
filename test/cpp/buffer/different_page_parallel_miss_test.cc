#include <algorithm>
#include <array>
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

class ParallelMissBackend : public telepath::DiskBackend {
 public:
  explicit ParallelMissBackend(std::size_t page_size) : page_size_(page_size) {}

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
    submitted_tags_.push_back(tag);
    cv_.notify_all();
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
    cv_.notify_all();
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
      std::fill_n(request.mutable_buffer, request.size, std::byte{0});
      request.mutable_buffer[0] =
          static_cast<std::byte>(request.tag.block_id + 1);
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

  void BlockCompletions() {
    std::lock_guard<std::mutex> guard(latch_);
    completions_allowed_ = false;
  }

  void AllowCompletions() {
    std::lock_guard<std::mutex> guard(latch_);
    completions_allowed_ = true;
    cv_.notify_all();
  }

  void WaitForSubmittedReads(std::size_t expected_count) {
    std::unique_lock<std::mutex> lock(latch_);
    cv_.wait(lock, [&]() {
      return shutdown_ || submitted_tags_.size() >= expected_count;
    });
  }

  std::vector<telepath::BufferTag> submitted_tags() const {
    std::lock_guard<std::mutex> guard(latch_);
    return submitted_tags_;
  }

 private:
  std::size_t page_size_{0};
  mutable std::mutex latch_;
  std::condition_variable cv_;
  std::deque<telepath::DiskRequest> pending_;
  std::vector<telepath::BufferTag> submitted_tags_;
  uint64_t next_request_id_{1};
  bool shutdown_{false};
  bool completions_allowed_{false};
};

}  // namespace

int main() {
  auto backend = std::make_unique<ParallelMissBackend>(4096);
  auto *backend_ptr = backend.get();
  auto replacer = telepath::MakeClockReplacer(4);
  auto telemetry = telepath::MakeCounterTelemetrySink();
  telepath::BufferManager manager(4, 4096, std::move(backend),
                                  std::move(replacer), telemetry);

  std::mutex start_latch;
  std::condition_variable start_cv;
  bool start = false;

  backend_ptr->BlockCompletions();

  std::array<std::byte, 2> observed{};
  std::array<bool, 2> ok{false, false};
  std::vector<std::thread> workers;
  workers.reserve(2);

  for (int i = 0; i < 2; ++i) {
    workers.emplace_back([&, i]() {
      std::unique_lock<std::mutex> lock(start_latch);
      start_cv.wait(lock, [&]() { return start; });
      lock.unlock();

      auto result = manager.ReadBuffer(51, static_cast<telepath::BlockId>(i));
      assert(result.ok());
      telepath::BufferHandle handle = std::move(result.value());
      observed[static_cast<std::size_t>(i)] = handle.data()[0];
      ok[static_cast<std::size_t>(i)] = true;
    });
  }

  {
    std::lock_guard<std::mutex> guard(start_latch);
    start = true;
  }
  start_cv.notify_all();

  backend_ptr->WaitForSubmittedReads(2);
  const auto submitted_tags = backend_ptr->submitted_tags();
  assert(submitted_tags.size() == 2);
  assert(submitted_tags[0] != submitted_tags[1]);

  backend_ptr->AllowCompletions();

  for (auto &worker : workers) {
    worker.join();
  }

  assert(ok[0]);
  assert(ok[1]);
  assert(observed[0] == std::byte{0x01});
  assert(observed[1] == std::byte{0x02});

  const telepath::TelemetrySnapshot snapshot = telemetry->Snapshot();
  assert(snapshot.buffer_misses == 2);
  assert(snapshot.buffer_hits == 0);
  return 0;
}
