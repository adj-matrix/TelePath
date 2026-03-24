#include <atomic>
#include <cassert>
#include <chrono>
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

class ReorderingDiskBackend : public telepath::DiskBackend {
 public:
  explicit ReorderingDiskBackend(std::size_t page_size)
      : page_size_(page_size), worker_(&ReorderingDiskBackend::WorkerLoop, this) {}

  ~ReorderingDiskBackend() override {
    Shutdown();
    worker_.join();
  }

  telepath::Result<uint64_t> SubmitRead(const telepath::BufferTag &tag,
                                        std::byte *out,
                                        std::size_t size) override {
    if (out == nullptr || size != page_size_) {
      return telepath::Status::InvalidArgument("invalid read request");
    }
    std::lock_guard<std::mutex> guard(latch_);
    const uint64_t request_id = next_request_id_++;
    pending_.push_back({request_id, telepath::DiskOperation::kRead, tag, out,
                        nullptr, size});
    request_cv_.notify_one();
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
    completed_.push_back({request_id, telepath::DiskOperation::kWrite, tag,
                          telepath::Status::Ok()});
    completion_cv_.notify_one();
    return request_id;
  }

  telepath::Result<telepath::DiskCompletion> PollCompletion() override {
    std::unique_lock<std::mutex> lock(latch_);
    completion_cv_.wait(lock, [this]() {
      return shutdown_ || !completed_.empty();
    });
    if (completed_.empty()) {
      return telepath::Status::Unavailable("backend is shutting down");
    }
    telepath::DiskCompletion completion = completed_.front();
    completed_.pop_front();
    return completion;
  }

  void Shutdown() override {
    {
      std::lock_guard<std::mutex> guard(latch_);
      shutdown_ = true;
    }
    request_cv_.notify_all();
    completion_cv_.notify_all();
  }

 private:
  void CompleteRead(const telepath::DiskRequest &request) {
    for (std::size_t i = 0; i < request.size; ++i) {
      request.mutable_buffer[i] = std::byte{0};
    }
    request.mutable_buffer[0] =
        static_cast<std::byte>((request.tag.block_id % 251) + 1);
  }

  void WorkerLoop() {
    while (true) {
      telepath::DiskRequest first;
      telepath::DiskRequest second;
      bool have_first = false;
      bool have_second = false;

      {
        std::unique_lock<std::mutex> lock(latch_);
        request_cv_.wait(lock, [this]() {
          return shutdown_ || !pending_.empty();
        });
        if (shutdown_ && pending_.empty()) {
          return;
        }

        first = pending_.front();
        pending_.pop_front();
        have_first = true;

        if (pending_.empty() && !shutdown_) {
          request_cv_.wait_for(lock, std::chrono::milliseconds(1), [this]() {
            return shutdown_ || !pending_.empty();
          });
        }

        if (!pending_.empty()) {
          second = pending_.front();
          pending_.pop_front();
          have_second = true;
        }
      }

      if (have_second) {
        CompleteRead(second);
        {
          std::lock_guard<std::mutex> guard(latch_);
          completed_.push_back({second.request_id, second.operation, second.tag,
                                telepath::Status::Ok()});
        }
        completion_cv_.notify_one();
      }

      if (have_first) {
        CompleteRead(first);
        {
          std::lock_guard<std::mutex> guard(latch_);
          completed_.push_back(
              {first.request_id, first.operation, first.tag, telepath::Status::Ok()});
        }
        completion_cv_.notify_one();
      }
    }
  }

  std::size_t page_size_{0};
  std::mutex latch_;
  std::condition_variable request_cv_;
  std::condition_variable completion_cv_;
  std::deque<telepath::DiskRequest> pending_;
  std::deque<telepath::DiskCompletion> completed_;
  uint64_t next_request_id_{1};
  bool shutdown_{false};
  std::thread worker_;
};

}  // namespace

int main() {
  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto backend = std::make_unique<ReorderingDiskBackend>(4096);
  auto replacer = telepath::MakeClockReplacer(8);
  telepath::BufferManager manager(8, 4096, std::move(backend),
                                  std::move(replacer), telemetry);

  std::atomic<bool> failed{false};
  std::atomic<int> ready{0};
  std::atomic<bool> start{false};
  std::vector<std::thread> workers;
  workers.reserve(8);

  for (int worker = 0; worker < 8; ++worker) {
    workers.emplace_back([&manager, &failed, &ready, &start, worker]() {
      ready.fetch_add(1);
      while (!start.load()) {
      }

      for (int round = 0; round < 4; ++round) {
        const telepath::BlockId block_id =
            static_cast<telepath::BlockId>(worker + round * 8);
        auto result = manager.ReadBuffer(9, block_id);
        if (!result.ok()) {
          failed.store(true);
          return;
        }
        telepath::BufferHandle handle = std::move(result.value());
        const std::byte expected =
            static_cast<std::byte>((block_id % 251) + 1);
        if (handle.data()[0] != expected) {
          failed.store(true);
          return;
        }
      }
    });
  }

  while (ready.load() != 8) {
  }
  start.store(true);

  for (auto &worker : workers) {
    worker.join();
  }

  assert(!failed.load());
  return 0;
}
