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

constexpr int kWorkerCount = 6;

class CoordinatedReadBackend : public telepath::DiskBackend {
 public:
  explicit CoordinatedReadBackend(std::size_t page_size) : page_size_(page_size) {}

  auto SubmitRead(
    const telepath::BufferTag &tag,
    std::byte *out,
    std::size_t size
  ) -> telepath::Result<uint64_t> override {
    auto validate_status = ValidateReadRequest(out, size);
    if (!validate_status.ok()) return validate_status;

    std::lock_guard<std::mutex> guard(latch_);
    const uint64_t request_id = next_request_id_++;
    pending_.push_back({request_id, telepath::DiskOperation::kRead, tag, out, nullptr, size});
    ++submitted_reads_;
    cv_.notify_all();
    return request_id;
  }

  auto SubmitWrite(
    const telepath::BufferTag &tag,
    const std::byte *data,
    std::size_t size
  ) -> telepath::Result<uint64_t> override {
    auto validate_status = ValidateWriteRequest(data, size);
    if (!validate_status.ok()) return validate_status;

    std::lock_guard<std::mutex> guard(latch_);
    const uint64_t request_id = next_request_id_++;
    pending_.push_back({request_id, telepath::DiskOperation::kWrite, tag, nullptr, data, size});
    cv_.notify_one();
    return request_id;
  }

  auto PollCompletion() -> telepath::Result<telepath::DiskCompletion> override {
    auto request = WaitForPendingRequest();
    if (!request.ok()) return request.status();
    return CompleteRequest(request.value());
  }

  void Shutdown() override {
    std::lock_guard<std::mutex> guard(latch_);
    shutdown_ = true;
    cv_.notify_all();
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
  auto ValidateReadRequest(
    std::byte *out,
    std::size_t size
  ) const -> telepath::Status {
    if (out == nullptr) return telepath::Status::InvalidArgument("invalid read request");
    if (size != page_size_) return telepath::Status::InvalidArgument("invalid read request");
    return telepath::Status::Ok();
  }

  auto ValidateWriteRequest(
    const std::byte *data,
    std::size_t size
  ) const -> telepath::Status {
    if (data == nullptr) return telepath::Status::InvalidArgument("invalid write request");
    if (size != page_size_) return telepath::Status::InvalidArgument("invalid write request");
    return telepath::Status::Ok();
  }

  auto WaitForPendingRequest() -> telepath::Result<telepath::DiskRequest> {
    std::unique_lock<std::mutex> lock(latch_);
    cv_.wait(lock, [&]() { return shutdown_ || !pending_.empty(); });
    if (shutdown_ && pending_.empty()) return telepath::Status::Unavailable("backend shutting down");

    auto request = pending_.front();
    pending_.pop_front();
    cv_.wait(lock, [&]() { return shutdown_ || completions_allowed_; });
    if (shutdown_) return telepath::Status::Unavailable("backend shutting down");
    return request;
  }

  auto CompleteRequest(
    const telepath::DiskRequest &request
  ) -> telepath::Result<telepath::DiskCompletion> {
    if (request.operation == telepath::DiskOperation::kRead) {
      if (fail_reads_.load()) {
        return telepath::DiskCompletion{
          request.request_id,
          request.operation,
          request.tag,
          telepath::Status::IoError("forced concurrent read failure"),
        };
      }
      for (std::size_t i = 0; i < request.size; ++i) {
        request.mutable_buffer[i] = std::byte{0};
      }
      request.mutable_buffer[0] = std::byte{0x6D};
    }

    return telepath::DiskCompletion{
      request.request_id,
      request.operation,
      request.tag,
      telepath::Status::Ok(),
    };
  }

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

struct WorkerStartGate {
  std::mutex latch;
  std::condition_variable cv;
  bool start{false};
};

void StartWorkers(WorkerStartGate *gate) {
  {
    std::lock_guard<std::mutex> guard(gate->latch);
    gate->start = true;
  }
  gate->cv.notify_all();
}

void ResetWorkers(WorkerStartGate *gate) {
  std::lock_guard<std::mutex> guard(gate->latch);
  gate->start = false;
}

void WaitForWorkerStart(
  WorkerStartGate *gate
) {
  std::unique_lock<std::mutex> lock(gate->latch);
  gate->cv.wait(lock, [&]() { return gate->start; });
}

void RunFailingWorker(
  telepath::BufferManager *manager,
  WorkerStartGate *gate,
  std::atomic<int> *started_reads,
  std::atomic<int> *failed_reads
) {
  WaitForWorkerStart(gate);
  ++(*started_reads);

  auto result = manager->ReadBuffer(41, 9);
  if (!result.ok() && result.status().code() == telepath::StatusCode::kIoError) ++(*failed_reads);
}

void RunRecoveryWorker(
  telepath::BufferManager *manager,
  WorkerStartGate *gate,
  std::atomic<int> *started_reads,
  std::atomic<int> *successful_reads
) {
  WaitForWorkerStart(gate);
  ++(*started_reads);

  auto result = manager->ReadBuffer(41, 9);
  if (!result.ok()) return;

  auto handle = std::move(result.value());
  if (handle.data()[0] == std::byte{0x6D}) ++(*successful_reads);
}

void WaitForAllWorkers(const std::atomic<int> &started_reads) {
  while (started_reads.load() != kWorkerCount) {
    std::this_thread::yield();
  }
}

void ExpectWarmReadHitsRecoveredPage(
  telepath::BufferManager *manager,
  CoordinatedReadBackend *backend,
  int reads_after_recovery
) {
  auto warm_result = manager->ReadBuffer(41, 9);
  assert(warm_result.ok());
  auto warm_handle = std::move(warm_result.value());
  assert(warm_handle.data()[0] == std::byte{0x6D});
  assert(backend->submitted_reads() == reads_after_recovery);
}

void ExpectTelemetryShowsFailureThenRecovery(
  const telepath::TelemetrySnapshot &snapshot
) {
  assert(snapshot.buffer_misses >= 2);
  assert(snapshot.buffer_hits >= 1);
}

}  // namespace

int main() {
  auto backend = std::make_unique<CoordinatedReadBackend>(4096);
  auto *backend_ptr = backend.get();
  auto replacer = telepath::MakeClockReplacer(4);
  auto telemetry = telepath::MakeCounterTelemetrySink();
  telepath::BufferManager manager(4, 4096, std::move(backend), std::move(replacer), telemetry);

  WorkerStartGate gate;

  backend_ptr->BlockCompletions();
  std::atomic<int> started_fail_reads{0};
  std::atomic<int> failed_reads{0};
  std::vector<std::thread> failing_workers;
  failing_workers.reserve(kWorkerCount);
  for (int i = 0; i < kWorkerCount; ++i) {
    failing_workers.emplace_back([&]() {
      RunFailingWorker(&manager, &gate, &started_fail_reads, &failed_reads);
    });
  }

  StartWorkers(&gate);
  WaitForAllWorkers(started_fail_reads);
  backend_ptr->WaitForSubmittedReads(1);
  backend_ptr->AllowCompletions();
  for (auto &worker : failing_workers) worker.join();

  assert(failed_reads.load() == kWorkerCount);
  const int reads_after_failure = backend_ptr->submitted_reads();
  assert(reads_after_failure >= 1);

  backend_ptr->set_fail_reads(false);
  backend_ptr->BlockCompletions();
  ResetWorkers(&gate);

  std::atomic<int> started_recovery_reads{0};
  std::atomic<int> successful_reads{0};
  std::vector<std::thread> recovery_workers;
  recovery_workers.reserve(kWorkerCount);
  for (int i = 0; i < kWorkerCount; ++i) {
    recovery_workers.emplace_back([&]() {
      RunRecoveryWorker(&manager, &gate, &started_recovery_reads, &successful_reads);
    });
  }

  StartWorkers(&gate);
  WaitForAllWorkers(started_recovery_reads);
  backend_ptr->WaitForSubmittedReads(2);
  backend_ptr->AllowCompletions();
  for (auto &worker : recovery_workers) worker.join();

  assert(successful_reads.load() == kWorkerCount);
  const int reads_after_recovery = backend_ptr->submitted_reads();
  assert(reads_after_recovery >= reads_after_failure + 1);

  ExpectWarmReadHitsRecoveredPage(&manager, backend_ptr, reads_after_recovery);
  ExpectTelemetryShowsFailureThenRecovery(telemetry->Snapshot());
  return 0;
}
