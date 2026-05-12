#include <array>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>

#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

class DirtyEvictionBackend : public telepath::DiskBackend {
 public:
  explicit DirtyEvictionBackend(std::size_t page_size) : page_size_(page_size) {}

  auto SubmitRead(
    const telepath::BufferTag &tag,
    std::byte *out,
    std::size_t size
  ) -> telepath::Result<uint64_t> override {
    if (out == nullptr || size != page_size_) return telepath::Status::InvalidArgument("invalid read request");

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
    if (data == nullptr || size != page_size_) return telepath::Status::InvalidArgument("invalid write request");

    std::lock_guard<std::mutex> guard(latch_);
    const uint64_t request_id = next_request_id_++;
    pending_.push_back({request_id, telepath::DiskOperation::kWrite, tag, nullptr, data, size});
    ++submitted_writes_;
    cv_.notify_all();
    return request_id;
  }

  auto PollCompletion() -> telepath::Result<telepath::DiskCompletion> override {
    telepath::DiskRequest request;
    {
      std::unique_lock<std::mutex> lock(latch_);
      cv_.wait(lock, [&]() { return shutdown_ || !pending_.empty(); });
      if (shutdown_ && pending_.empty()) return telepath::Status::Unavailable("backend shutting down");
      request = pending_.front();
      pending_.pop_front();
    }

    if (request.operation == telepath::DiskOperation::kRead) {
      CompleteRead(request);
      return telepath::DiskCompletion{request.request_id, request.operation, request.tag, telepath::Status::Ok()};
    }

    telepath::Status write_status = CompleteWrite(request);
    return telepath::DiskCompletion{request.request_id, request.operation, request.tag, write_status};
  }

  void Shutdown() override {
    std::lock_guard<std::mutex> guard(latch_);
    shutdown_ = true;
    cv_.notify_all();
  }

  auto GetCapabilities() const -> telepath::DiskBackendCapabilities override {
    return {telepath::DiskBackendKind::kPosix, false, false, 1, false};
  }

  void FailNextWriteFor(const telepath::BufferTag &tag) {
    std::lock_guard<std::mutex> guard(latch_);
    write_failures_remaining_[tag] = 1;
  }

  auto ReadPersistedPage(const telepath::BufferTag &tag) const -> std::array<std::byte, 4096> {
    std::array<std::byte, 4096> page{};
    std::lock_guard<std::mutex> guard(latch_);
    auto it = pages_.find(tag);
    if (it != pages_.end()) std::memcpy(page.data(), it->second.data(), page.size());
    return page;
  }

  std::size_t submitted_reads() const {
    std::lock_guard<std::mutex> guard(latch_);
    return submitted_reads_;
  }

  std::size_t submitted_writes() const {
    std::lock_guard<std::mutex> guard(latch_);
    return submitted_writes_;
  }

 private:
  void CompleteRead(const telepath::DiskRequest &request) {
    std::array<std::byte, 4096> page{};
    {
      std::lock_guard<std::mutex> guard(latch_);
      auto it = pages_.find(request.tag);
      if (it != pages_.end()) page = it->second;
    }
    std::memcpy(request.mutable_buffer, page.data(), page_size_);
  }

  auto CompleteWrite(const telepath::DiskRequest &request) -> telepath::Status {
    {
      std::lock_guard<std::mutex> guard(latch_);
      auto failure_it = write_failures_remaining_.find(request.tag);
      if (failure_it != write_failures_remaining_.end() && failure_it->second > 0) {
        --failure_it->second;
        return telepath::Status::IoError("forced dirty victim write failure");
      }
    }

    std::array<std::byte, 4096> page{};
    std::memcpy(page.data(), request.const_buffer, page_size_);
    {
      std::lock_guard<std::mutex> guard(latch_);
      pages_[request.tag] = page;
    }
    cv_.notify_all();
    return telepath::Status::Ok();
  }

  std::size_t page_size_{0};
  mutable std::mutex latch_;
  std::condition_variable cv_;
  std::deque<telepath::DiskRequest> pending_;
  std::unordered_map<telepath::BufferTag, std::array<std::byte, 4096>, telepath::BufferTagHash> pages_;
  std::unordered_map<telepath::BufferTag, std::size_t, telepath::BufferTagHash> write_failures_remaining_;
  uint64_t next_request_id_{1};
  std::size_t submitted_reads_{0};
  std::size_t submitted_writes_{0};
  bool shutdown_{false};
};

auto FindOnlyFrame(const telepath::BufferPoolSnapshot &snapshot) -> telepath::FrameSnapshot {
  assert(snapshot.frames.size() == 1);
  return snapshot.frames.front();
}

void ExpectDirtyVictimRollbackAndRetry() {
  constexpr telepath::BufferTag old_tag{70, 0};
  constexpr telepath::BufferTag new_tag{70, 1};

  auto backend = std::make_unique<DirtyEvictionBackend>(4096);
  auto *backend_ptr = backend.get();
  auto replacer = telepath::MakeClockReplacer(1);
  auto telemetry = telepath::MakeCounterTelemetrySink();
  telepath::BufferManager manager(1, 4096, std::move(backend), std::move(replacer), telemetry);

  auto old_result = manager.ReadBuffer(old_tag.file_id, old_tag.block_id);
  assert(old_result.ok());
  auto old_handle = std::move(old_result.value());
  old_handle.mutable_data()[0] = std::byte{0xA0};
  assert(manager.MarkBufferDirty(old_handle).ok());
  old_handle.Reset();

  backend_ptr->FailNextWriteFor(old_tag);
  auto failed_new_result = manager.ReadBuffer(new_tag.file_id, new_tag.block_id);
  assert(!failed_new_result.ok());
  assert(failed_new_result.status().code() == telepath::StatusCode::kIoError);
  assert(backend_ptr->ReadPersistedPage(old_tag)[0] == std::byte{0});

  auto rolled_back = FindOnlyFrame(manager.ExportSnapshot());
  assert(rolled_back.tag == old_tag);
  assert(rolled_back.state == telepath::BufferFrameState::kResident);
  assert(rolled_back.pin_count == 0);
  assert(rolled_back.is_valid);
  assert(rolled_back.is_dirty);
  assert(!rolled_back.flush_queued);
  assert(!rolled_back.flush_in_flight);

  const std::size_t reads_after_failure = backend_ptr->submitted_reads();
  auto warm_old_result = manager.ReadBuffer(old_tag.file_id, old_tag.block_id);
  assert(warm_old_result.ok());
  auto warm_old_handle = std::move(warm_old_result.value());
  assert(warm_old_handle.data()[0] == std::byte{0xA0});
  assert(backend_ptr->submitted_reads() == reads_after_failure);
  warm_old_handle.Reset();

  auto retry_new_result = manager.ReadBuffer(new_tag.file_id, new_tag.block_id);
  assert(retry_new_result.ok());
  auto retry_new_handle = std::move(retry_new_result.value());
  assert(retry_new_handle.data()[0] == std::byte{0});
  retry_new_handle.Reset();

  assert(backend_ptr->submitted_writes() == 2);
  assert(backend_ptr->ReadPersistedPage(old_tag)[0] == std::byte{0xA0});

  const auto telemetry_snapshot = telemetry->Snapshot();
  assert(telemetry_snapshot.flush_tasks_scheduled == 2);
  assert(telemetry_snapshot.flush_tasks_completed == 2);
  assert(telemetry_snapshot.flush_failures == 1);
  assert(telemetry_snapshot.eviction_failures == 1);
}

}  // namespace

int main() {
  ExpectDirtyVictimRollbackAndRetry();
  return 0;
}
