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
    submitted_tags_.push_back(tag);
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
    cv_.notify_all();
    return request_id;
  }

  auto PollCompletion() -> telepath::Result<telepath::DiskCompletion> override {
    auto request = WaitForPendingRequest();
    if (!request.ok()) return request.status();

    auto completion = CompleteRequest(request.value());
    if (!completion.ok()) return completion.status();
    return completion.value();
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

  auto submitted_tags() const -> std::vector<telepath::BufferTag> {
    std::lock_guard<std::mutex> guard(latch_);
    return submitted_tags_;
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
      std::fill_n(request.mutable_buffer, request.size, std::byte{0});
      request.mutable_buffer[0] = static_cast<std::byte>(request.tag.block_id + 1);
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
  std::vector<telepath::BufferTag> submitted_tags_;
  uint64_t next_request_id_{1};
  bool shutdown_{false};
  bool completions_allowed_{false};
};

struct ParallelMissRun {
  std::array<std::byte, 2> observed{};
  std::array<bool, 2> ok{false, false};
};

void RunWorker(
  telepath::BufferManager *manager,
  std::mutex *start_latch,
  std::condition_variable *start_cv,
  bool *start,
  ParallelMissRun *run,
  int worker
) {
  std::unique_lock<std::mutex> lock(*start_latch);
  start_cv->wait(lock, [&]() { return *start; });
  lock.unlock();

  auto result = manager->ReadBuffer(51, static_cast<telepath::BlockId>(worker));
  assert(result.ok());

  auto handle = std::move(result.value());
  run->observed[static_cast<std::size_t>(worker)] = handle.data()[0];
  run->ok[static_cast<std::size_t>(worker)] = true;
}

void ExpectSubmittedMissesTargetDifferentPages(
  const std::vector<telepath::BufferTag> &submitted_tags
) {
  assert(submitted_tags.size() == 2);
  assert(submitted_tags[0] != submitted_tags[1]);
}

void ExpectParallelReadsReturnDifferentPayloads(const ParallelMissRun &run) {
  assert(run.ok[0]);
  assert(run.ok[1]);
  assert(run.observed[0] == std::byte{0x01});
  assert(run.observed[1] == std::byte{0x02});
}

void ExpectTelemetryShowsTwoMisses(
  const telepath::TelemetrySnapshot &snapshot
) {
  assert(snapshot.buffer_misses == 2);
  assert(snapshot.buffer_hits == 0);
}

}  // namespace

int main() {
  auto backend = std::make_unique<ParallelMissBackend>(4096);
  auto *backend_ptr = backend.get();
  auto replacer = telepath::MakeClockReplacer(4);
  auto telemetry = telepath::MakeCounterTelemetrySink();
  telepath::BufferManager manager(4, 4096, std::move(backend), std::move(replacer), telemetry);

  std::mutex start_latch;
  std::condition_variable start_cv;
  bool start = false;
  ParallelMissRun run;
  std::vector<std::thread> workers;
  workers.reserve(2);

  backend_ptr->BlockCompletions();
  for (int worker = 0; worker < 2; ++worker) {
    workers.emplace_back([&manager, &start_latch, &start_cv, &start, &run, worker]() {
      RunWorker(&manager, &start_latch, &start_cv, &start, &run, worker);
    });
  }

  {
    std::lock_guard<std::mutex> guard(start_latch);
    start = true;
  }
  start_cv.notify_all();

  backend_ptr->WaitForSubmittedReads(2);
  ExpectSubmittedMissesTargetDifferentPages(backend_ptr->submitted_tags());

  backend_ptr->AllowCompletions();
  for (auto &worker : workers) worker.join();

  ExpectParallelReadsReturnDifferentPayloads(run);
  ExpectTelemetryShowsTwoMisses(telemetry->Snapshot());
  return 0;
}
