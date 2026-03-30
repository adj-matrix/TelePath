#include <atomic>
#include <array>
#include <cassert>
#include <chrono>
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
    auto submit_failure_it = write_submit_failures_remaining_.find(tag);
    if (submit_failure_it != write_submit_failures_remaining_.end() &&
        submit_failure_it->second > 0) {
      --submit_failure_it->second;
      return telepath::Status::IoError("forced write submit failure");
    }
    const uint64_t request_id = next_request_id_++;
    pending_.push_back(
        {request_id, telepath::DiskOperation::kWrite, tag, nullptr, data, size});
    submit_write_threads_.push_back(std::this_thread::get_id());
    submitted_write_tags_.push_back(tag);
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
      {
        std::lock_guard<std::mutex> guard(latch_);
        pages_[request.tag] = std::move(page);
      }
      cv_.notify_all();
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

  bool WaitForSubmittedWritesFor(
      std::size_t expected, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(latch_);
    return cv_.wait_for(lock, timeout, [&]() {
      return shutdown_ || submitted_writes_ >= expected;
    });
  }

  void WaitForFirstWriteBlocked() {
    std::unique_lock<std::mutex> lock(latch_);
    cv_.wait(lock, [&]() { return shutdown_ || first_write_blocked_; });
  }

  void FailWriteOnce(const telepath::BufferTag &tag) {
    std::lock_guard<std::mutex> guard(latch_);
    write_failures_remaining_[tag] = 1;
  }

  void FailWriteSubmitOnce(const telepath::BufferTag &tag) {
    std::lock_guard<std::mutex> guard(latch_);
    write_submit_failures_remaining_[tag] = 1;
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

  std::vector<telepath::BufferTag> submitted_write_tags() const {
    std::lock_guard<std::mutex> guard(latch_);
    return submitted_write_tags_;
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

  bool WaitForPersistedByte(const telepath::BufferTag &tag, std::size_t index,
                            std::byte value,
                            std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(latch_);
    return cv_.wait_for(lock, timeout, [&]() {
      auto it = pages_.find(tag);
      return it != pages_.end() && index < it->second.size() &&
             it->second[index] == value;
    });
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
      write_submit_failures_remaining_;
  std::unordered_map<telepath::BufferTag, std::size_t, telepath::BufferTagHash>
      write_failures_remaining_;
  std::unordered_set<telepath::BufferTag, telepath::BufferTagHash>
      blocked_write_tags_;
  std::unordered_set<telepath::BufferTag, telepath::BufferTagHash>
      blocked_write_waiters_;
  std::vector<std::thread::id> submit_write_threads_;
  std::vector<telepath::BufferTag> submitted_write_tags_;
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
    options.flush_submit_batch_size = 1;
    options.flush_foreground_burst_limit = 1;
    options.enable_background_cleaner = true;
    options.dirty_page_high_watermark = 2;
    options.dirty_page_low_watermark = 0;
    auto replacer = telepath::MakeClockReplacer(4);
    auto telemetry = telepath::MakeNoOpTelemetrySink();
    telepath::BufferManager manager(options, std::move(backend),
                                    std::move(replacer), telemetry);

    auto bg_first_result = manager.ReadBuffer(19, 0);
    auto bg_second_result = manager.ReadBuffer(19, 1);
    auto fg_first_result = manager.ReadBuffer(19, 2);
    auto fg_second_result = manager.ReadBuffer(19, 3);
    assert(bg_first_result.ok());
    assert(bg_second_result.ok());
    assert(fg_first_result.ok());
    assert(fg_second_result.ok());
    auto bg_first = std::move(bg_first_result.value());
    auto bg_second = std::move(bg_second_result.value());
    auto fg_first = std::move(fg_first_result.value());
    auto fg_second = std::move(fg_second_result.value());

    bg_first.mutable_data()[0] = std::byte{0x30};
    bg_second.mutable_data()[0] = std::byte{0x31};
    assert(manager.MarkBufferDirty(bg_first).ok());
    assert(manager.MarkBufferDirty(bg_second).ok());
    bg_first.Reset();
    bg_second.Reset();

    backend_ptr->WaitForSubmittedWrites(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    fg_first.mutable_data()[0] = std::byte{0x32};
    fg_second.mutable_data()[0] = std::byte{0x33};
    assert(manager.MarkBufferDirty(fg_first).ok());
    assert(manager.MarkBufferDirty(fg_second).ok());

    telepath::Status fg_first_status = telepath::Status::Ok();
    telepath::Status fg_second_status = telepath::Status::Ok();
    std::atomic<bool> fg_first_started{false};
    std::atomic<bool> fg_second_started{false};
    std::thread fg_first_flush(
        [&]() {
          fg_first_started.store(true);
          fg_first_status = manager.FlushBuffer(fg_first);
        });
    std::thread fg_second_flush(
        [&]() {
          fg_second_started.store(true);
          fg_second_status = manager.FlushBuffer(fg_second);
        });

    while (!fg_first_started.load() || !fg_second_started.load()) {
      std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    backend_ptr->AllowAllWriteCompletions();
    fg_first_flush.join();
    fg_second_flush.join();
    assert(fg_first_status.ok());
    assert(fg_second_status.ok());

    const auto submit_order = backend_ptr->submitted_write_tags();
    assert(submit_order.size() == 4);
    assert((submit_order[1] == telepath::BufferTag{19, 2}));
    assert((submit_order[2] == telepath::BufferTag{19, 1}));
    assert((submit_order[3] == telepath::BufferTag{19, 3}));
  }

  {
    auto backend = std::make_unique<CoordinatedWriteBackend>(4096);
    auto *backend_ptr = backend.get();
    backend_ptr->BlockAllWriteCompletions();

    telepath::BufferManagerOptions options;
    options.pool_size = 3;
    options.page_size = 4096;
    options.flush_worker_count = 1;
    options.flush_submit_batch_size = 1;
    options.enable_background_cleaner = true;
    options.dirty_page_high_watermark = 2;
    options.dirty_page_low_watermark = 0;
    auto replacer = telepath::MakeClockReplacer(3);
    auto telemetry = telepath::MakeNoOpTelemetrySink();
    telepath::BufferManager manager(options, std::move(backend),
                                    std::move(replacer), telemetry);

    auto first_result = manager.ReadBuffer(20, 0);
    auto second_result = manager.ReadBuffer(20, 1);
    auto resident_result = manager.ReadBuffer(20, 2);
    assert(first_result.ok());
    assert(second_result.ok());
    assert(resident_result.ok());
    auto first = std::move(first_result.value());
    auto second = std::move(second_result.value());
    auto resident = std::move(resident_result.value());
    first.mutable_data()[0] = std::byte{0x50};
    second.mutable_data()[0] = std::byte{0x51};
    assert(manager.MarkBufferDirty(first).ok());
    assert(manager.MarkBufferDirty(second).ok());
    resident.Reset();
    first.Reset();
    second.Reset();
    backend_ptr->WaitForSubmittedWrites(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto foreground_result = manager.ReadBuffer(20, 2);
    assert(foreground_result.ok());
    auto foreground_handle = std::move(foreground_result.value());
    foreground_handle.mutable_data()[0] = std::byte{0x5A};
    assert(manager.MarkBufferDirty(foreground_handle).ok());

    telepath::Status foreground_flush_status = telepath::Status::Ok();
    std::atomic<bool> foreground_flush_started{false};
    std::thread foreground_flush_thread([&]() {
      foreground_flush_started.store(true);
      foreground_flush_status = manager.FlushBuffer(foreground_handle);
    });

    while (!foreground_flush_started.load()) {
      std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    backend_ptr->AllowAllWriteCompletions();
    foreground_flush_thread.join();
    assert(foreground_flush_status.ok());
    foreground_handle.Reset();
    assert(backend_ptr->WaitForSubmittedWritesFor(
        3, std::chrono::milliseconds(1000)));
    assert(backend_ptr->WaitForPersistedByte(
        {20, 2}, 0, std::byte{0x5A}, std::chrono::milliseconds(1000)));

    const auto submit_order = backend_ptr->submitted_write_tags();
    assert(submit_order.size() == 3);
    assert((submit_order[0] != telepath::BufferTag{20, 2}));
    assert((submit_order[1] == telepath::BufferTag{20, 2} ||
            submit_order[2] == telepath::BufferTag{20, 2}));
  }

  {
    auto backend = std::make_unique<CoordinatedWriteBackend>(4096);
    auto *backend_ptr = backend.get();
    backend_ptr->BlockAllWriteCompletions();

    telepath::BufferManagerOptions options;
    options.pool_size = 4;
    options.page_size = 4096;
    options.enable_background_cleaner = true;
    options.dirty_page_high_watermark = 3;
    options.dirty_page_low_watermark = 1;
    auto replacer = telepath::MakeClockReplacer(4);
    auto telemetry = telepath::MakeNoOpTelemetrySink();
    telepath::BufferManager manager(options, std::move(backend),
                                    std::move(replacer), telemetry);

    for (telepath::BlockId block_id : {0UL, 1UL, 2UL}) {
      auto result = manager.ReadBuffer(21, block_id);
      assert(result.ok());
      auto handle = std::move(result.value());
      handle.mutable_data()[0] = static_cast<std::byte>(0x78 + block_id);
      assert(manager.MarkBufferDirty(handle).ok());
      handle.Reset();
    }

    assert(backend_ptr->WaitForSubmittedWritesFor(
        2, std::chrono::milliseconds(500)));
    assert(!backend_ptr->WaitForSubmittedWritesFor(
        3, std::chrono::milliseconds(50)));

    backend_ptr->AllowAllWriteCompletions();
    assert(manager.FlushAll().ok());
    assert(backend_ptr->submitted_writes() == 3);

    for (telepath::BlockId block_id : {0UL, 1UL, 2UL}) {
      const auto page = backend_ptr->ReadPersistedPage({21, block_id});
      assert(page[0] == static_cast<std::byte>(0x78 + block_id));
    }
  }

  {
    auto backend = std::make_unique<CoordinatedWriteBackend>(4096);
    auto *backend_ptr = backend.get();
    backend_ptr->BlockAllWriteCompletions();

    telepath::BufferManagerOptions options;
    options.pool_size = 4;
    options.page_size = 4096;
    options.flush_worker_count = 1;
    options.flush_submit_batch_size = 1;
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
    assert(backend_ptr->submitted_writes() >= 1);

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

    telepath::BufferManagerOptions options;
    options.pool_size = 4;
    options.page_size = 4096;
    options.flush_worker_count = 1;
    options.flush_submit_batch_size = 2;
    auto replacer = telepath::MakeClockReplacer(4);
    auto telemetry = telepath::MakeNoOpTelemetrySink();
    telepath::BufferManager manager(options, std::move(backend),
                                    std::move(replacer), telemetry);

    for (telepath::BlockId block_id : {13UL, 14UL}) {
      auto result = manager.ReadBuffer(13, block_id);
      assert(result.ok());
      auto handle = std::move(result.value());
      handle.mutable_data()[0] = static_cast<std::byte>(0x44 + block_id - 13);
      assert(manager.MarkBufferDirty(handle).ok());
    }

    std::atomic<bool> flush_all_finished{false};
    telepath::Status flush_all_status = telepath::Status::Ok();
    std::thread flush_all_thread([&]() {
      flush_all_status = manager.FlushAll();
      flush_all_finished.store(true);
    });

    assert(backend_ptr->WaitForSubmittedWritesFor(
        1, std::chrono::milliseconds(1000)));
    assert(!flush_all_finished.load());

    backend_ptr->AllowAllWriteCompletions();
    flush_all_thread.join();
    assert(flush_all_status.ok());
    assert(backend_ptr->submitted_writes() == 2);

    const auto first_page = backend_ptr->ReadPersistedPage({13, 13});
    const auto second_page = backend_ptr->ReadPersistedPage({13, 14});
    assert(first_page[0] == std::byte{0x44});
    assert(second_page[0] == std::byte{0x45});
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

  {
    auto backend = std::make_unique<CoordinatedWriteBackend>(4096);
    auto *backend_ptr = backend.get();

    telepath::BufferManagerOptions options;
    options.pool_size = 2;
    options.page_size = 4096;
    options.flush_worker_count = 1;
    options.flush_submit_batch_size = 2;
    auto replacer = telepath::MakeClockReplacer(2);
    auto telemetry = telepath::MakeNoOpTelemetrySink();
    telepath::BufferManager manager(options, std::move(backend),
                                    std::move(replacer), telemetry);

    for (telepath::BlockId block_id : {0UL, 1UL}) {
      auto result = manager.ReadBuffer(25, block_id);
      assert(result.ok());
      auto handle = std::move(result.value());
      handle.mutable_data()[0] = static_cast<std::byte>(0xB0 + block_id);
      assert(manager.MarkBufferDirty(handle).ok());
    }

    const telepath::BufferTag submit_fail_tag{25, 0};
    const telepath::BufferTag success_tag{25, 1};
    backend_ptr->FailWriteSubmitOnce(submit_fail_tag);

    const telepath::Status flush_all_status = manager.FlushAll();
    assert(!flush_all_status.ok());
    assert(flush_all_status.code() == telepath::StatusCode::kIoError);
    assert(backend_ptr->submitted_writes() == 1);

    const auto failed_page = backend_ptr->ReadPersistedPage(submit_fail_tag);
    const auto success_page = backend_ptr->ReadPersistedPage(success_tag);
    assert(failed_page[0] == std::byte{0x00});
    assert(success_page[0] == std::byte{0xB1});

    auto retry_result = manager.ReadBuffer(submit_fail_tag.file_id,
                                           submit_fail_tag.block_id);
    assert(retry_result.ok());
    auto retry_handle = std::move(retry_result.value());
    const telepath::Status retry_status = manager.FlushBuffer(retry_handle);
    assert(retry_status.ok());

    const auto retried_page = backend_ptr->ReadPersistedPage(submit_fail_tag);
    assert(retried_page[0] == std::byte{0xB0});
  }

  {
    auto backend = std::make_unique<CoordinatedWriteBackend>(4096);
    auto *backend_ptr = backend.get();
    backend_ptr->BlockAllWriteCompletions();

    telepath::BufferManagerOptions options;
    options.pool_size = 4;
    options.page_size = 4096;
    options.enable_background_cleaner = true;
    options.dirty_page_high_watermark = 2;
    options.dirty_page_low_watermark = 1;
    auto replacer = telepath::MakeClockReplacer(4);
    auto telemetry = telepath::MakeNoOpTelemetrySink();
    telepath::BufferManager manager(options, std::move(backend),
                                    std::move(replacer), telemetry);

    for (telepath::BlockId block_id : {0UL, 1UL}) {
      auto result = manager.ReadBuffer(22, block_id);
      assert(result.ok());
      auto handle = std::move(result.value());
      handle.mutable_data()[0] = static_cast<std::byte>(0x80 + block_id);
      assert(manager.MarkBufferDirty(handle).ok());
      handle.Reset();
    }

    backend_ptr->WaitForSubmittedWrites(1);
    assert(backend_ptr->submitted_writes() >= 1);
    backend_ptr->AllowAllWriteCompletions();
    const bool first_persisted = backend_ptr->WaitForPersistedByte(
        {22, 0}, 0, std::byte{0x80}, std::chrono::milliseconds(200));
    const bool second_persisted = first_persisted
                                      ? false
                                      : backend_ptr->WaitForPersistedByte(
                                            {22, 1}, 0, std::byte{0x81},
                                            std::chrono::milliseconds(200));
    assert(first_persisted || second_persisted);
  }

  {
    auto backend = std::make_unique<CoordinatedWriteBackend>(4096);
    auto *backend_ptr = backend.get();
    backend_ptr->BlockAllWriteCompletions();

    telepath::BufferManagerOptions options;
    options.pool_size = 2;
    options.page_size = 4096;
    options.enable_background_cleaner = true;
    options.dirty_page_high_watermark = 1;
    options.dirty_page_low_watermark = 0;
    auto replacer = telepath::MakeClockReplacer(2);
    auto telemetry = telepath::MakeNoOpTelemetrySink();
    telepath::BufferManager manager(options, std::move(backend),
                                    std::move(replacer), telemetry);

    auto result = manager.ReadBuffer(23, 0);
    assert(result.ok());
    auto handle = std::move(result.value());
    handle.mutable_data()[0] = std::byte{0x90};
    assert(manager.MarkBufferDirty(handle).ok());

    assert(!backend_ptr->WaitForSubmittedWritesFor(
        1, std::chrono::milliseconds(50)));

    handle.Reset();
    assert(backend_ptr->WaitForSubmittedWritesFor(
        1, std::chrono::milliseconds(200)));
    backend_ptr->AllowAllWriteCompletions();
    assert(backend_ptr->WaitForPersistedByte(
        {23, 0}, 0, std::byte{0x90}, std::chrono::milliseconds(200)));
  }

  {
    auto backend = std::make_unique<CoordinatedWriteBackend>(4096);
    auto *backend_ptr = backend.get();
    backend_ptr->BlockAllWriteCompletions();

    telepath::BufferManagerOptions options;
    options.pool_size = 1;
    options.page_size = 4096;
    options.enable_background_cleaner = true;
    options.dirty_page_high_watermark = 1;
    options.dirty_page_low_watermark = 0;
    auto replacer = telepath::MakeClockReplacer(1);
    auto telemetry = telepath::MakeNoOpTelemetrySink();
    telepath::BufferManager manager(options, std::move(backend),
                                    std::move(replacer), telemetry);

    auto result = manager.ReadBuffer(26, 0);
    assert(result.ok());
    auto handle = std::move(result.value());
    handle.mutable_data()[0] = std::byte{0xC0};
    assert(manager.MarkBufferDirty(handle).ok());
    handle.Reset();

    backend_ptr->WaitForSubmittedWrites(1);

    std::atomic<bool> flush_all_finished{false};
    telepath::Status flush_all_status = telepath::Status::Ok();
    std::thread flush_all_thread([&]() {
      flush_all_status = manager.FlushAll();
      flush_all_finished.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(!flush_all_finished.load());
    assert(backend_ptr->submitted_writes() == 1);

    backend_ptr->AllowAllWriteCompletions();
    flush_all_thread.join();
    assert(flush_all_status.ok());
    assert(backend_ptr->submitted_writes() == 1);
    assert(backend_ptr->WaitForPersistedByte(
        {26, 0}, 0, std::byte{0xC0}, std::chrono::milliseconds(1000)));
  }

  {
    auto backend = std::make_unique<CoordinatedWriteBackend>(4096);
    auto *backend_ptr = backend.get();

    telepath::BufferManagerOptions options;
    options.pool_size = 1;
    options.page_size = 4096;
    options.enable_background_cleaner = true;
    options.dirty_page_high_watermark = 1;
    options.dirty_page_low_watermark = 0;
    auto replacer = telepath::MakeClockReplacer(1);
    auto telemetry = telepath::MakeNoOpTelemetrySink();
    telepath::BufferManager manager(options, std::move(backend),
                                    std::move(replacer), telemetry);

    auto first_result = manager.ReadBuffer(24, 0);
    assert(first_result.ok());
    auto first = std::move(first_result.value());
    first.mutable_data()[0] = std::byte{0xA0};
    assert(manager.MarkBufferDirty(first).ok());
    first.Reset();

    assert(backend_ptr->WaitForSubmittedWritesFor(
        1, std::chrono::milliseconds(200)));
    assert(backend_ptr->WaitForPersistedByte(
        {24, 0}, 0, std::byte{0xA0}, std::chrono::milliseconds(200)));

    const std::size_t writes_before_eviction = backend_ptr->submitted_writes();
    assert(manager.FlushAll().ok());
    assert(backend_ptr->submitted_writes() == writes_before_eviction);

    telepath::Result<telepath::BufferHandle> second_result = manager.ReadBuffer(24, 1);
    assert(second_result.ok());
    auto second = std::move(second_result.value());
    assert(second.data()[0] == std::byte{0x00});
    second.Reset();

    assert(backend_ptr->submitted_writes() == writes_before_eviction);
  }

  return 0;
}
