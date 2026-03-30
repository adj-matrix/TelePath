#include <array>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

class CoordinatedWriteBackend : public telepath::DiskBackend {
 public:
  explicit CoordinatedWriteBackend(std::size_t page_size)
      : page_size_(page_size) {}

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
    pending_.push_back(
        {request_id, telepath::DiskOperation::kWrite, tag, nullptr, data, size});
    submit_write_threads_.push_back(std::this_thread::get_id());
    ++submitted_writes_;
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

      if (request.operation == telepath::DiskOperation::kWrite &&
          block_first_write_completion_ && !first_write_blocked_) {
        first_write_blocked_ = true;
        cv_.notify_all();
        cv_.wait(lock, [&]() {
          return shutdown_ || allow_first_write_completion_;
        });
        if (shutdown_) {
          return telepath::Status::Unavailable("backend shutting down");
        }
        block_first_write_completion_ = false;
      }
    }

    if (request.operation == telepath::DiskOperation::kRead) {
      std::vector<std::byte> page(page_size_, std::byte{0});
      {
        std::lock_guard<std::mutex> guard(latch_);
        auto it = pages_.find(request.tag);
        if (it != pages_.end()) {
          page = it->second;
        }
      }
      std::memcpy(request.mutable_buffer, page.data(), page_size_);
    } else {
      std::vector<std::byte> page(page_size_);
      std::memcpy(page.data(), request.const_buffer, page_size_);
      std::lock_guard<std::mutex> guard(latch_);
      pages_[request.tag] = std::move(page);
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

  void BlockFirstWriteCompletion() {
    std::lock_guard<std::mutex> guard(latch_);
    block_first_write_completion_ = true;
    allow_first_write_completion_ = false;
    first_write_blocked_ = false;
  }

  void AllowFirstWriteCompletion() {
    std::lock_guard<std::mutex> guard(latch_);
    allow_first_write_completion_ = true;
    cv_.notify_all();
  }

  void WaitForSubmittedWrites(std::size_t expected) {
    std::unique_lock<std::mutex> lock(latch_);
    cv_.wait(lock, [&]() { return shutdown_ || submitted_writes_ >= expected; });
  }

  void WaitForFirstWriteBlocked() {
    std::unique_lock<std::mutex> lock(latch_);
    cv_.wait(lock, [&]() { return shutdown_ || first_write_blocked_; });
  }

  std::vector<std::thread::id> submit_write_threads() const {
    std::lock_guard<std::mutex> guard(latch_);
    return submit_write_threads_;
  }

  std::array<std::byte, 4096> ReadPersistedPage(
      const telepath::BufferTag &tag) const {
    std::array<std::byte, 4096> page{};
    std::lock_guard<std::mutex> guard(latch_);
    auto it = pages_.find(tag);
    if (it != pages_.end()) {
      std::memcpy(page.data(), it->second.data(), page.size());
    }
    return page;
  }

 private:
  std::size_t page_size_{0};
  mutable std::mutex latch_;
  std::condition_variable cv_;
  std::deque<telepath::DiskRequest> pending_;
  std::unordered_map<telepath::BufferTag, std::vector<std::byte>,
                     telepath::BufferTagHash>
      pages_;
  std::vector<std::thread::id> submit_write_threads_;
  uint64_t next_request_id_{1};
  std::size_t submitted_writes_{0};
  bool shutdown_{false};
  bool block_first_write_completion_{false};
  bool allow_first_write_completion_{false};
  bool first_write_blocked_{false};
};

}  // namespace

int main() {
  {
    auto backend = std::make_unique<CoordinatedWriteBackend>(4096);
    auto *backend_ptr = backend.get();
    backend_ptr->BlockFirstWriteCompletion();

    auto replacer = telepath::MakeClockReplacer(2);
    auto telemetry = telepath::MakeCounterTelemetrySink();
    telepath::BufferManager manager(2, 4096, std::move(backend),
                                    std::move(replacer), telemetry);

    {
      auto seed_result = manager.ReadBuffer(5, 1);
      assert(seed_result.ok());
      auto seed = std::move(seed_result.value());
      seed.mutable_data()[0] = std::byte{0x11};
      assert(manager.MarkBufferDirty(seed).ok());
    }

    auto flush_result = manager.ReadBuffer(5, 1);
    assert(flush_result.ok());
    telepath::BufferHandle flusher = std::move(flush_result.value());

    auto update_result = manager.ReadBuffer(5, 1);
    assert(update_result.ok());
    telepath::BufferHandle updater = std::move(update_result.value());

    std::thread::id flush_requester_thread_id;
    telepath::Status flush_status = telepath::Status::Ok();
    std::thread flush_thread([&]() {
      flush_requester_thread_id = std::this_thread::get_id();
      flush_status = manager.FlushBuffer(flusher);
    });

    backend_ptr->WaitForSubmittedWrites(1);
    backend_ptr->WaitForFirstWriteBlocked();

    updater.mutable_data()[0] = std::byte{0x22};
    assert(manager.MarkBufferDirty(updater).ok());
    updater.Reset();

    backend_ptr->AllowFirstWriteCompletion();
    backend_ptr->WaitForSubmittedWrites(2);

    flush_thread.join();
    assert(flush_status.ok());

    const auto write_threads = backend_ptr->submit_write_threads();
    assert(write_threads.size() >= 2);
    assert(write_threads[0] != flush_requester_thread_id);
    assert(write_threads[1] != flush_requester_thread_id);

    const auto page = backend_ptr->ReadPersistedPage({5, 1});
    assert(page[0] == std::byte{0x22});
  }

  {
    auto backend = std::make_unique<CoordinatedWriteBackend>(4096);
    auto *backend_ptr = backend.get();
    backend_ptr->BlockFirstWriteCompletion();

    auto replacer = telepath::MakeClockReplacer(1);
    auto telemetry = telepath::MakeNoOpTelemetrySink();
    telepath::BufferManager manager(1, 4096, std::move(backend),
                                    std::move(replacer), telemetry);

    {
      auto seed_result = manager.ReadBuffer(8, 0);
      assert(seed_result.ok());
      auto seed = std::move(seed_result.value());
      seed.mutable_data()[0] = std::byte{0x33};
      assert(manager.MarkBufferDirty(seed).ok());
    }

    std::thread::id eviction_requester_thread_id;
    telepath::Status eviction_status = telepath::Status::Ok();
    std::thread eviction_thread([&]() {
      eviction_requester_thread_id = std::this_thread::get_id();
      auto result = manager.ReadBuffer(8, 1);
      if (!result.ok()) {
        eviction_status = result.status();
        return;
      }
      auto handle = std::move(result.value());
      assert(handle.data()[0] == std::byte{0});
    });

    backend_ptr->WaitForSubmittedWrites(1);
    backend_ptr->WaitForFirstWriteBlocked();
    backend_ptr->AllowFirstWriteCompletion();
    eviction_thread.join();

    assert(eviction_status.ok());
    const auto write_threads = backend_ptr->submit_write_threads();
    assert(!write_threads.empty());
    assert(write_threads.front() != eviction_requester_thread_id);

    const auto page = backend_ptr->ReadPersistedPage({8, 0});
    assert(page[0] == std::byte{0x33});
  }

  return 0;
}
