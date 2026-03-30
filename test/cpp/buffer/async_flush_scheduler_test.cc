#include <atomic>
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
#include <unordered_set>
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

      if (request.operation == telepath::DiskOperation::kWrite) {
        auto blocked_it = blocked_write_tags_.find(request.tag);
        if (blocked_it != blocked_write_tags_.end()) {
          blocked_write_waiters_.insert(request.tag);
          cv_.notify_all();
          cv_.wait(lock, [&]() {
            return shutdown_ ||
                   blocked_write_tags_.find(request.tag) ==
                       blocked_write_tags_.end();
          });
          blocked_write_waiters_.erase(request.tag);
          if (shutdown_) {
            return telepath::Status::Unavailable("backend shutting down");
          }
        }
      }

      if (request.operation == telepath::DiskOperation::kWrite &&
          block_all_write_completions_) {
        cv_.wait(lock, [&]() {
          return shutdown_ || allow_all_write_completions_;
        });
        if (shutdown_) {
          return telepath::Status::Unavailable("backend shutting down");
        }
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
      {
        std::lock_guard<std::mutex> guard(latch_);
        auto failure_it = write_failures_remaining_.find(request.tag);
        if (failure_it != write_failures_remaining_.end() &&
            failure_it->second > 0) {
          --failure_it->second;
          return telepath::DiskCompletion{
              request.request_id, request.operation, request.tag,
              telepath::Status::IoError("forced write failure")};
        }
      }

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
    return {telepath::DiskBackendKind::kPosix, true, true, 4, false};
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

  void BlockAllWriteCompletions() {
    std::lock_guard<std::mutex> guard(latch_);
    block_all_write_completions_ = true;
    allow_all_write_completions_ = false;
  }

  void AllowAllWriteCompletions() {
    std::lock_guard<std::mutex> guard(latch_);
    allow_all_write_completions_ = true;
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

  void FailWriteOnce(const telepath::BufferTag &tag) {
    std::lock_guard<std::mutex> guard(latch_);
    write_failures_remaining_[tag] = 1;
  }

  void BlockWriteCompletionForTag(const telepath::BufferTag &tag) {
    std::lock_guard<std::mutex> guard(latch_);
    blocked_write_tags_.insert(tag);
  }

  void AllowWriteCompletionForTag(const telepath::BufferTag &tag) {
    std::lock_guard<std::mutex> guard(latch_);
    blocked_write_tags_.erase(tag);
    cv_.notify_all();
  }

  void WaitForWriteBlocked(const telepath::BufferTag &tag) {
    std::unique_lock<std::mutex> lock(latch_);
    cv_.wait(lock, [&]() {
      return shutdown_ || blocked_write_waiters_.find(tag) !=
                              blocked_write_waiters_.end();
    });
  }

  std::vector<std::thread::id> submit_write_threads() const {
    std::lock_guard<std::mutex> guard(latch_);
    return submit_write_threads_;
  }

  std::size_t submitted_writes() const {
    std::lock_guard<std::mutex> guard(latch_);
    return submitted_writes_;
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
  std::unordered_map<telepath::BufferTag, std::size_t, telepath::BufferTagHash>
      write_failures_remaining_;
  std::unordered_set<telepath::BufferTag, telepath::BufferTagHash>
      blocked_write_tags_;
  std::unordered_set<telepath::BufferTag, telepath::BufferTagHash>
      blocked_write_waiters_;
  std::vector<std::thread::id> submit_write_threads_;
  uint64_t next_request_id_{1};
  std::size_t submitted_writes_{0};
  bool shutdown_{false};
  bool block_first_write_completion_{false};
  bool allow_first_write_completion_{false};
  bool first_write_blocked_{false};
  bool block_all_write_completions_{false};
  bool allow_all_write_completions_{false};
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

  {
    auto backend = std::make_unique<CoordinatedWriteBackend>(4096);
    auto *backend_ptr = backend.get();
    backend_ptr->BlockAllWriteCompletions();

    telepath::BufferManagerOptions options;
    options.pool_size = 4;
    options.page_size = 4096;
    options.flush_worker_count = 1;
    auto replacer = telepath::MakeClockReplacer(4);
    auto telemetry = telepath::MakeNoOpTelemetrySink();
    telepath::BufferManager manager(options, std::move(backend),
                                    std::move(replacer), telemetry);

    {
      auto first_seed_result = manager.ReadBuffer(12, 0);
      auto second_seed_result = manager.ReadBuffer(12, 1);
      assert(first_seed_result.ok());
      assert(second_seed_result.ok());
      auto first_seed = std::move(first_seed_result.value());
      auto second_seed = std::move(second_seed_result.value());
      first_seed.mutable_data()[0] = std::byte{0x40};
      second_seed.mutable_data()[0] = std::byte{0x41};
      assert(manager.MarkBufferDirty(first_seed).ok());
      assert(manager.MarkBufferDirty(second_seed).ok());
    }

    auto first_flush_result = manager.ReadBuffer(12, 0);
    auto second_flush_result = manager.ReadBuffer(12, 1);
    assert(first_flush_result.ok());
    assert(second_flush_result.ok());
    telepath::BufferHandle first = std::move(first_flush_result.value());
    telepath::BufferHandle second = std::move(second_flush_result.value());

    telepath::Status first_status = telepath::Status::Ok();
    telepath::Status second_status = telepath::Status::Ok();
    std::thread first_flush([&]() { first_status = manager.FlushBuffer(first); });
    std::thread second_flush(
        [&]() { second_status = manager.FlushBuffer(second); });

    backend_ptr->WaitForSubmittedWrites(1);
    assert(backend_ptr->submitted_writes() == 1);

    backend_ptr->AllowAllWriteCompletions();
    first_flush.join();
    second_flush.join();
    assert(first_status.ok());
    assert(second_status.ok());

    const auto first_page = backend_ptr->ReadPersistedPage({12, 0});
    const auto second_page = backend_ptr->ReadPersistedPage({12, 1});
    assert(first_page[0] == std::byte{0x40});
    assert(second_page[0] == std::byte{0x41});
  }

  {
    auto backend = std::make_unique<CoordinatedWriteBackend>(4096);
    auto *backend_ptr = backend.get();
    backend_ptr->BlockAllWriteCompletions();

    auto replacer = telepath::MakeClockReplacer(4);
    auto telemetry = telepath::MakeNoOpTelemetrySink();
    telepath::BufferManager manager(4, 4096, std::move(backend),
                                    std::move(replacer), telemetry);

    for (telepath::BlockId block_id : {0UL, 1UL}) {
      auto seed_result = manager.ReadBuffer(14, block_id);
      assert(seed_result.ok());
      auto seed = std::move(seed_result.value());
      seed.mutable_data()[0] = static_cast<std::byte>(0x60 + block_id);
      assert(manager.MarkBufferDirty(seed).ok());
    }

    std::atomic<bool> flush_all_finished{false};
    telepath::Status flush_all_status = telepath::Status::Ok();
    std::thread flush_all_thread([&]() {
      flush_all_status = manager.FlushAll();
      flush_all_finished.store(true);
    });

    backend_ptr->WaitForSubmittedWrites(2);
    assert(!flush_all_finished.load());

    backend_ptr->AllowAllWriteCompletions();
    flush_all_thread.join();
    assert(flush_all_status.ok());

    const auto first_page = backend_ptr->ReadPersistedPage({14, 0});
    const auto second_page = backend_ptr->ReadPersistedPage({14, 1});
    assert(first_page[0] == std::byte{0x60});
    assert(second_page[0] == std::byte{0x61});
  }

  {
    auto backend = std::make_unique<CoordinatedWriteBackend>(4096);
    auto *backend_ptr = backend.get();
    auto replacer = telepath::MakeClockReplacer(2);
    auto telemetry = telepath::MakeNoOpTelemetrySink();
    telepath::BufferManager manager(2, 4096, std::move(backend),
                                    std::move(replacer), telemetry);

    for (telepath::BlockId block_id : {0UL, 1UL}) {
      auto seed_result = manager.ReadBuffer(18, block_id);
      assert(seed_result.ok());
      auto seed = std::move(seed_result.value());
      seed.mutable_data()[0] = static_cast<std::byte>(0x70 + block_id);
      assert(manager.MarkBufferDirty(seed).ok());
    }

    const telepath::BufferTag failed_tag{18, 1};
    const telepath::BufferTag blocked_tag{18, 0};
    backend_ptr->FailWriteOnce(failed_tag);
    backend_ptr->BlockWriteCompletionForTag(blocked_tag);

    std::atomic<bool> flush_all_finished{false};
    telepath::Status flush_all_status = telepath::Status::Ok();
    std::thread flush_all_thread([&]() {
      flush_all_status = manager.FlushAll();
      flush_all_finished.store(true);
    });

    backend_ptr->WaitForSubmittedWrites(2);
    backend_ptr->WaitForWriteBlocked(blocked_tag);
    assert(!flush_all_finished.load());

    backend_ptr->AllowWriteCompletionForTag(blocked_tag);
    flush_all_thread.join();
    assert(!flush_all_status.ok());
    assert(flush_all_status.code() == telepath::StatusCode::kIoError);

    const auto blocked_page = backend_ptr->ReadPersistedPage(blocked_tag);
    assert(blocked_page[0] == std::byte{0x70});

    const auto failed_before_retry = backend_ptr->ReadPersistedPage(failed_tag);
    assert(failed_before_retry[0] == std::byte{0x00});

    auto retry_result = manager.ReadBuffer(failed_tag.file_id, failed_tag.block_id);
    assert(retry_result.ok());
    auto retry_handle = std::move(retry_result.value());
    const telepath::Status retry_status = manager.FlushBuffer(retry_handle);
    assert(retry_status.ok());

    const auto failed_after_retry = backend_ptr->ReadPersistedPage(failed_tag);
    assert(failed_after_retry[0] == std::byte{0x71});
  }

  return 0;
}
