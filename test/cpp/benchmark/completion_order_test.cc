#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

constexpr int kWorkerCount = 8;
constexpr int kRoundCount = 4;
constexpr std::size_t kPageSize = 4096;

class ReorderingDiskBackend : public telepath::DiskBackend {
 public:
  explicit ReorderingDiskBackend(std::size_t page_size)
    : page_size_(page_size),
      worker_(&ReorderingDiskBackend::WorkerLoop, this) {}

  ~ReorderingDiskBackend() override {
    Shutdown();
    if (worker_.joinable()) worker_.join();
  }

  auto SubmitRead(
    const telepath::BufferTag &tag,
    std::byte *out,
    std::size_t size
  ) -> telepath::Result<uint64_t> override {
    auto validate_status = ValidateReadRequest(out, size);
    if (!validate_status.ok()) return validate_status;
    return EnqueueReadRequest(tag, out, size);
  }

  auto SubmitWrite(
    const telepath::BufferTag &tag,
    const std::byte *data,
    std::size_t size
  ) -> telepath::Result<uint64_t> override {
    auto validate_status = ValidateWriteRequest(data, size);
    if (!validate_status.ok()) return validate_status;
    return CompleteWriteRequest(tag, size);
  }

  auto PollCompletion() -> telepath::Result<telepath::DiskCompletion> override {
    std::unique_lock<std::mutex> lock(latch_);
    completion_cv_.wait(lock, [this]() {
      return !completed_.empty() || CanReturnUnavailableAfterShutdown();
    });
    if (completed_.empty()) return telepath::Status::Unavailable("backend is shutting down");

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

  auto GetCapabilities() const -> telepath::DiskBackendCapabilities override {
    return {
      telepath::DiskBackendKind::kPosix,
      false,
      false,
      1,
      false,
    };
  }

 private:
  auto ValidateReadRequest(
    std::byte *out,
    std::size_t size
  ) const -> telepath::Status {
    if (out == nullptr) return telepath::Status::InvalidArgument("read buffer must not be null");
    if (size != page_size_) return telepath::Status::InvalidArgument("read size does not match page size");
    return telepath::Status::Ok();
  }

  auto ValidateWriteRequest(
    const std::byte *data,
    std::size_t size
  ) const -> telepath::Status {
    if (data == nullptr) return telepath::Status::InvalidArgument("write buffer must not be null");
    if (size != page_size_) return telepath::Status::InvalidArgument("write size does not match page size");
    return telepath::Status::Ok();
  }

  auto NextRequestIdLocked() -> uint64_t { return next_request_id_++; }

  auto EnqueueReadRequest(
    const telepath::BufferTag &tag,
    std::byte *out,
    std::size_t size
  ) -> telepath::Result<uint64_t> {
    std::lock_guard<std::mutex> guard(latch_);
    if (shutdown_) return telepath::Status::Unavailable("backend is shutting down");

    const uint64_t request_id = NextRequestIdLocked();
    pending_.push_back({
      request_id,
      telepath::DiskOperation::kRead,
      tag,
      out,
      nullptr,
      size,
    });
    request_cv_.notify_one();
    return request_id;
  }

  auto CompleteWriteRequest(
    const telepath::BufferTag &tag,
    std::size_t size
  ) -> telepath::Result<uint64_t> {
    (void)size;

    std::lock_guard<std::mutex> guard(latch_);
    if (shutdown_) return telepath::Status::Unavailable("backend is shutting down");

    const uint64_t request_id = NextRequestIdLocked();
    completed_.push_back({
      request_id,
      telepath::DiskOperation::kWrite,
      tag,
      telepath::Status::Ok(),
    });
    completion_cv_.notify_one();
    return request_id;
  }

  auto TakePendingRequestLocked() -> telepath::DiskRequest {
    telepath::DiskRequest request = pending_.front();
    pending_.pop_front();
    return request;
  }

  auto WaitForFirstPendingRequest() -> std::optional<telepath::DiskRequest> {
    std::unique_lock<std::mutex> lock(latch_);
    request_cv_.wait(lock, [this]() {
      return shutdown_ || !pending_.empty();
    });
    if (shutdown_ && pending_.empty()) return std::nullopt;

    worker_active_ = true;
    return TakePendingRequestLocked();
  }

  auto WaitForSecondPendingRequest() -> std::optional<telepath::DiskRequest> {
    std::unique_lock<std::mutex> lock(latch_);
    if (pending_.empty() && !shutdown_) {
      request_cv_.wait_for(lock, std::chrono::milliseconds(1), [this]() {
        return shutdown_ || !pending_.empty();
      });
    }
    if (pending_.empty()) return std::nullopt;
    return TakePendingRequestLocked();
  }

  void FillReadBuffer(const telepath::DiskRequest &request) const {
    for (std::size_t i = 0; i < request.size; ++i) {
      request.mutable_buffer[i] = std::byte{0};
    }
    request.mutable_buffer[0] = static_cast<std::byte>((request.tag.block_id % 251) + 1);
  }

  void PublishCompletion(const telepath::DiskRequest &request) {
    completed_.push_back({
      request.request_id,
      request.operation,
      request.tag,
      telepath::Status::Ok(),
    });
  }

  void PublishReadBatch(
    const telepath::DiskRequest &first,
    const std::optional<telepath::DiskRequest> &second
  ) {
    FillReadBuffer(first);
    if (second.has_value()) FillReadBuffer(*second);

    {
      std::lock_guard<std::mutex> guard(latch_);
      if (second.has_value()) PublishCompletion(*second);
      PublishCompletion(first);
      worker_active_ = false;
    }
    completion_cv_.notify_all();
  }

  bool CanReturnUnavailableAfterShutdown() const {
    return shutdown_ && pending_.empty() && !worker_active_;
  }

  void WorkerLoop() {
    while (true) {
      auto first = WaitForFirstPendingRequest();
      if (!first.has_value()) return;

      auto second = WaitForSecondPendingRequest();
      PublishReadBatch(*first, second);
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
  bool worker_active_{false};
  std::thread worker_;
};

bool ReadExpectedBlock(
  telepath::BufferManager *manager,
  telepath::BlockId block_id
) {
  auto result = manager->ReadBuffer(9, block_id);
  if (!result.ok()) return false;

  telepath::BufferHandle handle = std::move(result.value());
  const std::byte expected = static_cast<std::byte>((block_id % 251) + 1);
  return handle.data()[0] == expected;
}

void RunWorker(
  telepath::BufferManager *manager,
  std::atomic<bool> *failed,
  std::atomic<int> *ready,
  std::atomic<bool> *start,
  int worker
) {
  ready->fetch_add(1);
  while (!start->load()) {
  }

  for (int round = 0; round < kRoundCount; ++round) {
    const auto block_id = static_cast<telepath::BlockId>(worker + round * kWorkerCount);
    if (ReadExpectedBlock(manager, block_id)) continue;
    failed->store(true);
    return;
  }
}

void WaitForWorkersToBeReady(const std::atomic<int> &ready) {
  while (ready.load() != kWorkerCount) {
  }
}

}  // namespace

int main() {
  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto backend = std::make_unique<ReorderingDiskBackend>(kPageSize);
  auto replacer = telepath::MakeClockReplacer(kWorkerCount);
  telepath::BufferManager manager(kWorkerCount, kPageSize, std::move(backend), std::move(replacer), telemetry);

  std::atomic<bool> failed{false};
  std::atomic<int> ready{0};
  std::atomic<bool> start{false};
  std::vector<std::thread> workers;
  workers.reserve(kWorkerCount);

  for (int worker = 0; worker < kWorkerCount; ++worker) {
    workers.emplace_back(
      [&manager, &failed, &ready, &start, worker]() {
      RunWorker(&manager, &failed, &ready, &start, worker);
    });
  }

  WaitForWorkersToBeReady(ready);
  start.store(true);

  for (auto &worker : workers) worker.join();

  assert(!failed.load());
  return 0;
}
