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

#include "polling_disk_backend.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

// Every manager here routes foreground flushes through the flush workers,
// which is the behavior under test.
auto QueuedOptions(std::size_t pool_size = 0) -> telepath::BufferManagerOptions {
  telepath::BufferManagerOptions options;
  options.pool_size = pool_size;
  options.page_size = 4096;
  options.foreground_writeback = telepath::ForegroundWriteback::kQueued;
  return options;
}

class CoordinatedWriteBackend : public telepath::test_support::PollingDiskBackend {
 public:
  explicit CoordinatedWriteBackend(std::size_t page_size) : PollingDiskBackend(page_size, 4), page_size_(page_size) {}

  telepath::Result<uint64_t> SubmitRead(const telepath::BufferTag &tag, std::byte *out, std::size_t size) override {
    if (out == nullptr || size != page_size_) return telepath::InvalidArgument("invalid read request");
    std::lock_guard<std::mutex> guard(latch_);
    const uint64_t request_id = next_request_id_++;
    pending_.push_back({request_id, telepath::DiskOperation::kRead, tag, out, nullptr, size});
    cv_.notify_all();
    return request_id;
  }

  telepath::Result<uint64_t> SubmitWrite(
    const telepath::BufferTag &tag,
    const std::byte *data,
    std::size_t size) override {
    if (data == nullptr || size != page_size_) return telepath::InvalidArgument("invalid write request");
    std::lock_guard<std::mutex> guard(latch_);
    auto submit_failure_it = write_submit_failures_remaining_.find(tag);
    if (submit_failure_it != write_submit_failures_remaining_.end() && submit_failure_it->second > 0) {
      --submit_failure_it->second;
      return telepath::IoError("forced write submit failure");
    }
    const uint64_t request_id = next_request_id_++;
    pending_.push_back({request_id, telepath::DiskOperation::kWrite, tag, nullptr, data, size});
    submit_write_threads_.push_back(std::this_thread::get_id());
    submitted_write_tags_.push_back(tag);
    ++submitted_writes_;
    cv_.notify_all();
    return request_id;
  }

  telepath::Result<telepath::test_support::LegacyDiskCompletion> PollCompletion() override {
    telepath::test_support::LegacyDiskRequest request;
    {
      std::unique_lock<std::mutex> lock(latch_);
      cv_.wait(lock, [&]() { return shutdown_ || !pending_.empty(); });
      if (shutdown_ && pending_.empty()) return telepath::Unavailable("backend shutting down");
      request = pending_.front();
      pending_.pop_front();

      if (request.operation == telepath::DiskOperation::kWrite && block_first_write_completion_ && !first_write_blocked_) {
        first_write_blocked_ = true;
        cv_.notify_all();
        cv_.wait(lock, [&]() { return shutdown_ || allow_first_write_completion_; });
        if (shutdown_) return telepath::Unavailable("backend shutting down");
        block_first_write_completion_ = false;
      }

      if (request.operation == telepath::DiskOperation::kWrite) {
        auto blocked_it = blocked_write_tags_.find(request.tag);
        if (blocked_it != blocked_write_tags_.end()) {
          blocked_write_waiters_.insert(request.tag);
          cv_.notify_all();
          cv_.wait(lock, [&]() { return shutdown_ || blocked_write_tags_.find(request.tag) == blocked_write_tags_.end(); });
          blocked_write_waiters_.erase(request.tag);
          if (shutdown_) return telepath::Unavailable("backend shutting down");
        }
      }

      if (request.operation == telepath::DiskOperation::kWrite && block_all_write_completions_) {
        cv_.wait(lock, [&]() { return shutdown_ || allow_all_write_completions_; });
        if (shutdown_) return telepath::Unavailable("backend shutting down");
      }
    }

    if (request.operation == telepath::DiskOperation::kRead) {
      std::vector<std::byte> page(page_size_, std::byte{0});
      {
        std::lock_guard<std::mutex> guard(latch_);
        auto it = pages_.find(request.tag);
        if (it != pages_.end()) page = it->second;
      }
      std::memcpy(request.mutable_buffer, page.data(), page_size_);
    } else {
      {
        std::lock_guard<std::mutex> guard(latch_);
        auto failure_it = write_failures_remaining_.find(request.tag);
        if (failure_it != write_failures_remaining_.end() && failure_it->second > 0) {
          --failure_it->second;
          return telepath::test_support::LegacyDiskCompletion{request.request_id, request.operation, request.tag, telepath::IoError("forced write failure")};
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

    return telepath::test_support::LegacyDiskCompletion{request.request_id, request.operation, request.tag, telepath::Ok()};
  }

  void OnShutdown() override {
    std::lock_guard<std::mutex> guard(latch_);
    shutdown_ = true;
    cv_.notify_all();
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

  bool WaitForSubmittedWritesFor(std::size_t expected, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(latch_);
    return cv_.wait_for(lock, timeout, [&]() { return shutdown_ || submitted_writes_ >= expected; });
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
    cv_.wait(lock, [&]() { return shutdown_ || blocked_write_waiters_.find(tag) != blocked_write_waiters_.end(); });
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

  std::array<std::byte, 4096> ReadPersistedPage(const telepath::BufferTag &tag) const {
    std::array<std::byte, 4096> page{};
    std::lock_guard<std::mutex> guard(latch_);
    auto it = pages_.find(tag);
    if (it != pages_.end()) std::memcpy(page.data(), it->second.data(), page.size());
    return page;
  }

  bool WaitForPersistedByte(
    const telepath::BufferTag &tag,
    std::size_t index,
    std::byte value,
    std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(latch_);
    return cv_.wait_for(lock, timeout, [&]() {
      auto it = pages_.find(tag);
      return it != pages_.end() && index < it->second.size() && it->second[index] == value;
    });
  }

 private:
  std::size_t page_size_{0};
  mutable std::mutex latch_;
  std::condition_variable cv_;
  std::deque<telepath::test_support::LegacyDiskRequest> pending_;
  std::unordered_map<telepath::BufferTag, std::vector<std::byte>, telepath::BufferTagHash> pages_;
  std::unordered_map<telepath::BufferTag, std::size_t, telepath::BufferTagHash> write_submit_failures_remaining_;
  std::unordered_map<telepath::BufferTag, std::size_t, telepath::BufferTagHash> write_failures_remaining_;
  std::unordered_set<telepath::BufferTag, telepath::BufferTagHash> blocked_write_tags_;
  std::unordered_set<telepath::BufferTag, telepath::BufferTagHash> blocked_write_waiters_;
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

auto WriteHandle(
  telepath::BufferManager *manager,
  telepath::FileId file_id,
  telepath::BlockId block_id
) -> telepath::ExclusiveBufferHandle {
  auto result = manager->ReadBufferExclusive(file_id, block_id);
  assert(result.has_value());
  return std::move(result.value());
}

void SeedDirtyByte(
  telepath::BufferManager *manager,
  telepath::FileId file_id,
  telepath::BlockId block_id,
  std::byte value
) {
  auto handle = WriteHandle(manager, file_id, block_id);
  handle.mutable_data()[0] = value;
}

void ExpectForegroundFlushUsesFlushWorkerAndPersistsLatestDirtyVersion() {
  auto backend = std::make_unique<CoordinatedWriteBackend>(4096);
  auto *backend_ptr = backend.get();
  backend_ptr->BlockFirstWriteCompletion();

  auto replacer = telepath::MakeClockReplacer(2);
  auto telemetry = telepath::MakeCounterTelemetrySink();
  auto manager_owner = telepath::BufferManager::Create(QueuedOptions(2), std::move(backend), std::move(replacer), telemetry);
  assert(manager_owner.has_value());
  telepath::BufferManager &manager = *manager_owner.value();

  SeedDirtyByte(&manager, 5, 1, std::byte{0x11});

  std::thread::id flush_requester_thread_id;
  telepath::Status flush_status = telepath::Ok();
  std::thread flush_thread([&]() {
    flush_requester_thread_id = std::this_thread::get_id();
    flush_status = manager.FlushBuffer(5, 1);
  });

  backend_ptr->WaitForSubmittedWrites(1);
  backend_ptr->WaitForFirstWriteBlocked();

  auto updater = WriteHandle(&manager, 5, 1);
  updater.mutable_data()[0] = std::byte{0x22};
  updater.Reset();

  backend_ptr->AllowFirstWriteCompletion();
  backend_ptr->WaitForSubmittedWrites(2);

  flush_thread.join();
  assert(flush_status.has_value());

  const auto write_threads = backend_ptr->submit_write_threads();
  assert(write_threads.size() >= 2);
  assert(write_threads[0] != flush_requester_thread_id);
  assert(write_threads[1] != flush_requester_thread_id);
  assert(backend_ptr->ReadPersistedPage({5, 1})[0] == std::byte{0x22});
}

void ExpectEvictionUsesFlushWorkerForWriteback() {
  auto backend = std::make_unique<CoordinatedWriteBackend>(4096);
  auto *backend_ptr = backend.get();
  backend_ptr->BlockFirstWriteCompletion();

  auto replacer = telepath::MakeClockReplacer(1);
  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto manager_owner = telepath::BufferManager::Create(QueuedOptions(1), std::move(backend), std::move(replacer), telemetry);
  assert(manager_owner.has_value());
  telepath::BufferManager &manager = *manager_owner.value();

  SeedDirtyByte(&manager, 8, 0, std::byte{0x33});

  std::thread::id eviction_requester_thread_id;
  telepath::Status eviction_status = telepath::Ok();
  std::thread eviction_thread([&]() {
    eviction_requester_thread_id = std::this_thread::get_id();
    auto result = manager.ReadBuffer(8, 1);
    if (!result.has_value()) {
      eviction_status = std::unexpected(result.error());
      return;
    }
    auto handle = std::move(result.value());
    assert(handle.data()[0] == std::byte{0});
  });

  backend_ptr->WaitForSubmittedWrites(1);
  backend_ptr->WaitForFirstWriteBlocked();
  backend_ptr->AllowFirstWriteCompletion();
  eviction_thread.join();

  assert(eviction_status.has_value());
  const auto write_threads = backend_ptr->submit_write_threads();
  assert(!write_threads.empty());
  assert(write_threads.front() != eviction_requester_thread_id);
  assert(backend_ptr->ReadPersistedPage({8, 0})[0] == std::byte{0x33});
}

void ExpectForegroundBurstLimitLetsForegroundFlushesCutAhead() {
  auto backend = std::make_unique<CoordinatedWriteBackend>(4096);
  auto *backend_ptr = backend.get();
  backend_ptr->BlockAllWriteCompletions();

  telepath::BufferManagerOptions options = QueuedOptions();
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
  auto manager_owner = telepath::BufferManager::Create(options, std::move(backend), std::move(replacer), telemetry);
  assert(manager_owner.has_value());
  telepath::BufferManager &manager = *manager_owner.value();

  auto bg_first = WriteHandle(&manager, 19, 0);
  auto bg_second = WriteHandle(&manager, 19, 1);
  auto fg_first = WriteHandle(&manager, 19, 2);
  auto fg_second = WriteHandle(&manager, 19, 3);
  bg_first.mutable_data()[0] = std::byte{0x30};
  bg_second.mutable_data()[0] = std::byte{0x31};
  bg_first.Reset();
  bg_second.Reset();

  backend_ptr->WaitForSubmittedWrites(1);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  fg_first.mutable_data()[0] = std::byte{0x32};
  fg_second.mutable_data()[0] = std::byte{0x33};

  telepath::Status fg_first_status = telepath::Ok();
  telepath::Status fg_second_status = telepath::Ok();
  std::atomic<bool> fg_first_started{false};
  std::atomic<bool> fg_second_started{false};
  std::thread fg_first_flush([&]() {
    fg_first_started.store(true);
    fg_first_status = manager.FlushBuffer(fg_first);
  });
  std::thread fg_second_flush([&]() {
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
  assert(fg_first_status.has_value());
  assert(fg_second_status.has_value());
  assert(backend_ptr->WaitForSubmittedWritesFor(4, std::chrono::milliseconds(1000)));

  const auto submit_order = backend_ptr->submitted_write_tags();
  assert(submit_order.size() == 4);
  const telepath::BufferTag background_tail{19, 1};
  const telepath::BufferTag foreground_a{19, 2};
  const telepath::BufferTag foreground_b{19, 3};
  assert(submit_order[1] == foreground_a || submit_order[1] == foreground_b);

  bool background_tail_seen = false;
  bool foreground_a_seen = false;
  bool foreground_b_seen = false;
  for (std::size_t index = 1; index < submit_order.size(); ++index) {
    if (submit_order[index] == background_tail) background_tail_seen = true;
    if (submit_order[index] == foreground_a) foreground_a_seen = true;
    if (submit_order[index] == foreground_b) foreground_b_seen = true;
  }
  assert(background_tail_seen);
  assert(foreground_a_seen);
  assert(foreground_b_seen);
}

void ExpectForegroundFlushSubmitsAfterOlderBackgroundWrite() {
  auto backend = std::make_unique<CoordinatedWriteBackend>(4096);
  auto *backend_ptr = backend.get();
  backend_ptr->BlockAllWriteCompletions();

  telepath::BufferManagerOptions options = QueuedOptions();
  options.pool_size = 3;
  options.page_size = 4096;
  options.flush_worker_count = 1;
  options.flush_submit_batch_size = 1;
  options.enable_background_cleaner = true;
  options.dirty_page_high_watermark = 2;
  options.dirty_page_low_watermark = 0;
  auto replacer = telepath::MakeClockReplacer(3);
  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto manager_owner = telepath::BufferManager::Create(options, std::move(backend), std::move(replacer), telemetry);
  assert(manager_owner.has_value());
  telepath::BufferManager &manager = *manager_owner.value();

  auto first = WriteHandle(&manager, 20, 0);
  auto second = WriteHandle(&manager, 20, 1);
  auto resident = WriteHandle(&manager, 20, 2);
  first.mutable_data()[0] = std::byte{0x50};
  second.mutable_data()[0] = std::byte{0x51};
  resident.Reset();
  first.Reset();
  second.Reset();
  backend_ptr->WaitForSubmittedWrites(1);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto foreground_handle = WriteHandle(&manager, 20, 2);
  foreground_handle.mutable_data()[0] = std::byte{0x5A};

  telepath::Status foreground_flush_status = telepath::Ok();
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
  assert(foreground_flush_status.has_value());
  foreground_handle.Reset();
  assert(backend_ptr->WaitForSubmittedWritesFor(2, std::chrono::milliseconds(1000)));
  assert(backend_ptr->WaitForPersistedByte({20, 2}, 0, std::byte{0x5A}, std::chrono::milliseconds(1000)));

  const auto submit_order = backend_ptr->submitted_write_tags();
  const telepath::BufferTag foreground_tag{20, 2};
  assert(submit_order.size() >= 2);
  assert(submit_order[0] != foreground_tag);

  bool foreground_seen = false;
  for (std::size_t index = 1; index < submit_order.size(); ++index) {
    if (submit_order[index] != foreground_tag) continue;
    foreground_seen = true;
    break;
  }
  assert(foreground_seen);
}

void ExpectCleanerBacklogDoesNotPreventFlushAllProgress() {
  auto backend = std::make_unique<CoordinatedWriteBackend>(4096);
  auto *backend_ptr = backend.get();
  backend_ptr->BlockAllWriteCompletions();

  telepath::BufferManagerOptions options = QueuedOptions();
  options.pool_size = 4;
  options.page_size = 4096;
  options.enable_background_cleaner = true;
  options.dirty_page_high_watermark = 3;
  options.dirty_page_low_watermark = 1;
  auto replacer = telepath::MakeClockReplacer(4);
  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto manager_owner = telepath::BufferManager::Create(options, std::move(backend), std::move(replacer), telemetry);
  assert(manager_owner.has_value());
  telepath::BufferManager &manager = *manager_owner.value();

  for (telepath::BlockId block_id : {0UL, 1UL, 2UL}) {
    auto handle = WriteHandle(&manager, 21, block_id);
    handle.mutable_data()[0] = static_cast<std::byte>(0x78 + block_id);
    handle.Reset();
  }

  assert(backend_ptr->WaitForSubmittedWritesFor(2, std::chrono::milliseconds(500)));
  assert(!backend_ptr->WaitForSubmittedWritesFor(3, std::chrono::milliseconds(50)));

  backend_ptr->AllowAllWriteCompletions();
  assert(manager.FlushAll().has_value());
  assert(backend_ptr->submitted_writes() == 3);

  for (telepath::BlockId block_id : {0UL, 1UL, 2UL}) {
    const auto page = backend_ptr->ReadPersistedPage({21, block_id});
    assert(page[0] == static_cast<std::byte>(0x78 + block_id));
  }
}

void ExpectSerializedFlushWorkerCompletesIndependentFlushes() {
  auto backend = std::make_unique<CoordinatedWriteBackend>(4096);
  auto *backend_ptr = backend.get();
  backend_ptr->BlockAllWriteCompletions();

  telepath::BufferManagerOptions options = QueuedOptions();
  options.pool_size = 4;
  options.page_size = 4096;
  options.flush_worker_count = 1;
  options.flush_submit_batch_size = 1;
  auto replacer = telepath::MakeClockReplacer(4);
  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto manager_owner = telepath::BufferManager::Create(options, std::move(backend), std::move(replacer), telemetry);
  assert(manager_owner.has_value());
  telepath::BufferManager &manager = *manager_owner.value();

  SeedDirtyByte(&manager, 12, 0, std::byte{0x40});
  SeedDirtyByte(&manager, 12, 1, std::byte{0x41});

  telepath::Status first_status = telepath::Ok();
  telepath::Status second_status = telepath::Ok();
  std::thread first_flush([&]() { first_status = manager.FlushBuffer(12, 0); });
  std::thread second_flush([&]() { second_status = manager.FlushBuffer(12, 1); });

  backend_ptr->WaitForSubmittedWrites(1);
  assert(backend_ptr->submitted_writes() >= 1);

  backend_ptr->AllowAllWriteCompletions();
  first_flush.join();
  second_flush.join();
  assert(first_status.has_value());
  assert(second_status.has_value());
  assert(backend_ptr->ReadPersistedPage({12, 0})[0] == std::byte{0x40});
  assert(backend_ptr->ReadPersistedPage({12, 1})[0] == std::byte{0x41});
}

void ExpectFlushAllSubmitsSecondBatchAfterFirstBatchCompletes() {
  auto backend = std::make_unique<CoordinatedWriteBackend>(4096);
  auto *backend_ptr = backend.get();
  backend_ptr->BlockAllWriteCompletions();

  telepath::BufferManagerOptions options = QueuedOptions();
  options.pool_size = 4;
  options.page_size = 4096;
  options.flush_worker_count = 1;
  options.flush_submit_batch_size = 2;
  auto replacer = telepath::MakeClockReplacer(4);
  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto manager_owner = telepath::BufferManager::Create(options, std::move(backend), std::move(replacer), telemetry);
  assert(manager_owner.has_value());
  telepath::BufferManager &manager = *manager_owner.value();

  for (telepath::BlockId block_id : {13UL, 14UL}) {
    auto handle = WriteHandle(&manager, 13, block_id);
    handle.mutable_data()[0] = static_cast<std::byte>(0x44 + block_id - 13);
  }

  std::atomic<bool> flush_all_finished{false};
  telepath::Status flush_all_status = telepath::Ok();
  std::thread flush_all_thread([&]() {
    flush_all_status = manager.FlushAll();
    flush_all_finished.store(true);
  });

  assert(backend_ptr->WaitForSubmittedWritesFor(1, std::chrono::milliseconds(1000)));
  assert(!flush_all_finished.load());

  backend_ptr->AllowAllWriteCompletions();
  flush_all_thread.join();
  assert(flush_all_status.has_value());
  assert(backend_ptr->submitted_writes() == 2);
  assert(backend_ptr->ReadPersistedPage({13, 13})[0] == std::byte{0x44});
  assert(backend_ptr->ReadPersistedPage({13, 14})[0] == std::byte{0x45});
}

void ExpectFlushAllWaitsForAllBlockedWrites() {
  auto backend = std::make_unique<CoordinatedWriteBackend>(4096);
  auto *backend_ptr = backend.get();
  backend_ptr->BlockAllWriteCompletions();

  auto replacer = telepath::MakeClockReplacer(4);
  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto manager_owner = telepath::BufferManager::Create(QueuedOptions(4), std::move(backend), std::move(replacer), telemetry);
  assert(manager_owner.has_value());
  telepath::BufferManager &manager = *manager_owner.value();

  for (telepath::BlockId block_id : {0UL, 1UL}) {
    auto seed = WriteHandle(&manager, 14, block_id);
    seed.mutable_data()[0] = static_cast<std::byte>(0x60 + block_id);
  }

  std::atomic<bool> flush_all_finished{false};
  telepath::Status flush_all_status = telepath::Ok();
  std::thread flush_all_thread([&]() {
    flush_all_status = manager.FlushAll();
    flush_all_finished.store(true);
  });

  backend_ptr->WaitForSubmittedWrites(2);
  assert(!flush_all_finished.load());

  backend_ptr->AllowAllWriteCompletions();
  flush_all_thread.join();
  assert(flush_all_status.has_value());
  assert(backend_ptr->ReadPersistedPage({14, 0})[0] == std::byte{0x60});
  assert(backend_ptr->ReadPersistedPage({14, 1})[0] == std::byte{0x61});
}

void ExpectFailedFlushAllLeavesHealthyPagePersistedAndAllowsRetry() {
  auto backend = std::make_unique<CoordinatedWriteBackend>(4096);
  auto *backend_ptr = backend.get();
  auto replacer = telepath::MakeClockReplacer(2);
  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto manager_owner = telepath::BufferManager::Create(QueuedOptions(2), std::move(backend), std::move(replacer), telemetry);
  assert(manager_owner.has_value());
  telepath::BufferManager &manager = *manager_owner.value();

  for (telepath::BlockId block_id : {0UL, 1UL}) {
    auto seed = WriteHandle(&manager, 18, block_id);
    seed.mutable_data()[0] = static_cast<std::byte>(0x70 + block_id);
  }

  const telepath::BufferTag failed_tag{18, 1};
  const telepath::BufferTag blocked_tag{18, 0};
  backend_ptr->FailWriteOnce(failed_tag);
  backend_ptr->BlockWriteCompletionForTag(blocked_tag);

  std::atomic<bool> flush_all_finished{false};
  telepath::Status flush_all_status = telepath::Ok();
  std::thread flush_all_thread([&]() {
    flush_all_status = manager.FlushAll();
    flush_all_finished.store(true);
  });

  backend_ptr->WaitForSubmittedWrites(2);
  backend_ptr->WaitForWriteBlocked(blocked_tag);
  assert(!flush_all_finished.load());

  backend_ptr->AllowWriteCompletionForTag(blocked_tag);
  flush_all_thread.join();
  assert(!flush_all_status.has_value());
  assert(flush_all_status.error().code() == telepath::ErrorCode::kIoError);
  assert(backend_ptr->ReadPersistedPage(blocked_tag)[0] == std::byte{0x70});
  assert(backend_ptr->ReadPersistedPage(failed_tag)[0] == std::byte{0x00});

  const auto retry_status = manager.FlushBuffer(failed_tag.file_id, failed_tag.block_id);
  assert(retry_status.has_value());
  assert(backend_ptr->ReadPersistedPage(failed_tag)[0] == std::byte{0x71});
}

void ExpectFlushAllReportsSubmitFailureAndAllowsRetry() {
  auto backend = std::make_unique<CoordinatedWriteBackend>(4096);
  auto *backend_ptr = backend.get();

  telepath::BufferManagerOptions options = QueuedOptions();
  options.pool_size = 2;
  options.page_size = 4096;
  options.flush_worker_count = 1;
  options.flush_submit_batch_size = 2;
  auto replacer = telepath::MakeClockReplacer(2);
  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto manager_owner = telepath::BufferManager::Create(options, std::move(backend), std::move(replacer), telemetry);
  assert(manager_owner.has_value());
  telepath::BufferManager &manager = *manager_owner.value();

  for (telepath::BlockId block_id : {0UL, 1UL}) {
    auto handle = WriteHandle(&manager, 25, block_id);
    handle.mutable_data()[0] = static_cast<std::byte>(0xB0 + block_id);
  }

  const telepath::BufferTag submit_fail_tag{25, 0};
  const telepath::BufferTag success_tag{25, 1};
  backend_ptr->FailWriteSubmitOnce(submit_fail_tag);

  const auto flush_all_status = manager.FlushAll();
  assert(!flush_all_status.has_value());
  assert(flush_all_status.error().code() == telepath::ErrorCode::kIoError);
  assert(backend_ptr->submitted_writes() == 1);
  assert(backend_ptr->ReadPersistedPage(submit_fail_tag)[0] == std::byte{0x00});
  assert(backend_ptr->ReadPersistedPage(success_tag)[0] == std::byte{0xB1});

  const auto retry_status = manager.FlushBuffer(submit_fail_tag.file_id, submit_fail_tag.block_id);
  assert(retry_status.has_value());
  assert(backend_ptr->ReadPersistedPage(submit_fail_tag)[0] == std::byte{0xB0});
}

void ExpectBackgroundCleanerPersistsAtLeastOneDirtyPage() {
  auto backend = std::make_unique<CoordinatedWriteBackend>(4096);
  auto *backend_ptr = backend.get();
  backend_ptr->BlockAllWriteCompletions();

  telepath::BufferManagerOptions options = QueuedOptions();
  options.pool_size = 4;
  options.page_size = 4096;
  options.enable_background_cleaner = true;
  options.dirty_page_high_watermark = 2;
  options.dirty_page_low_watermark = 1;
  auto replacer = telepath::MakeClockReplacer(4);
  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto manager_owner = telepath::BufferManager::Create(options, std::move(backend), std::move(replacer), telemetry);
  assert(manager_owner.has_value());
  telepath::BufferManager &manager = *manager_owner.value();

  for (telepath::BlockId block_id : {0UL, 1UL}) {
    auto handle = WriteHandle(&manager, 22, block_id);
    handle.mutable_data()[0] = static_cast<std::byte>(0x80 + block_id);
    handle.Reset();
  }

  backend_ptr->WaitForSubmittedWrites(1);
  assert(backend_ptr->submitted_writes() >= 1);
  backend_ptr->AllowAllWriteCompletions();

  const bool first_persisted = backend_ptr->WaitForPersistedByte({22, 0}, 0, std::byte{0x80}, std::chrono::milliseconds(200));
  const bool second_persisted = first_persisted ? false : backend_ptr->WaitForPersistedByte({22, 1}, 0, std::byte{0x81}, std::chrono::milliseconds(200));
  assert(first_persisted || second_persisted);
}

void ExpectCleanerFlushFailureRequeuesAndRetries() {
  auto backend = std::make_unique<CoordinatedWriteBackend>(4096);
  auto *backend_ptr = backend.get();

  telepath::BufferManagerOptions options = QueuedOptions();
  options.pool_size = 1;
  options.page_size = 4096;
  options.enable_background_cleaner = true;
  options.dirty_page_high_watermark = 1;
  options.dirty_page_low_watermark = 0;
  auto replacer = telepath::MakeClockReplacer(1);
  auto telemetry = telepath::MakeCounterTelemetrySink();
  auto manager_owner = telepath::BufferManager::Create(options, std::move(backend), std::move(replacer), telemetry);
  assert(manager_owner.has_value());
  telepath::BufferManager &manager = *manager_owner.value();

  const telepath::BufferTag tag{27, 0};
  auto handle = WriteHandle(&manager, tag.file_id, tag.block_id);
  handle.mutable_data()[0] = std::byte{0xD0};
  backend_ptr->FailWriteOnce(tag);
  handle.Reset();

  assert(backend_ptr->WaitForSubmittedWritesFor(2, std::chrono::milliseconds(1000)));
  assert(backend_ptr->WaitForPersistedByte(tag, 0, std::byte{0xD0}, std::chrono::milliseconds(1000)));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  assert(backend_ptr->submitted_writes() == 2);

  const auto snapshot = telemetry->Snapshot();
  assert(snapshot.flush_tasks_scheduled == 2);
  assert(snapshot.flush_tasks_completed == 2);
  assert(snapshot.flush_failures == 1);
  assert(snapshot.cleaner_flushes_scheduled == 2);
  assert(snapshot.cleaner_flushes_finished == 2);
}

void ExpectSnapshotAggregatesQueuedAndInFlightFlushes() {
  auto backend = std::make_unique<CoordinatedWriteBackend>(4096);
  auto *backend_ptr = backend.get();
  backend_ptr->BlockAllWriteCompletions();

  telepath::BufferManagerOptions options = QueuedOptions();
  options.pool_size = 2;
  options.page_size = 4096;
  options.flush_worker_count = 1;
  options.flush_submit_batch_size = 1;
  auto replacer = telepath::MakeClockReplacer(2);
  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto manager_owner = telepath::BufferManager::Create(options, std::move(backend), std::move(replacer), telemetry);
  assert(manager_owner.has_value());
  telepath::BufferManager &manager = *manager_owner.value();

  SeedDirtyByte(&manager, 28, 0, std::byte{0xE0});
  SeedDirtyByte(&manager, 28, 1, std::byte{0xE1});

  telepath::Status flush_all_status = telepath::Ok();
  std::thread flush_all_thread([&]() {
    flush_all_status = manager.FlushAll();
  });

  backend_ptr->WaitForSubmittedWrites(1);

  bool saw_queued_and_in_flight = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto snapshot = manager.ExportSnapshot();
    if (snapshot.dirty_page_count == 2 &&
        snapshot.flush_queued_count == 1 &&
        snapshot.flush_in_flight_count == 1) {
      saw_queued_and_in_flight = true;
      break;
    }
    std::this_thread::yield();
  }

  assert(saw_queued_and_in_flight);
  backend_ptr->AllowAllWriteCompletions();
  flush_all_thread.join();
  assert(flush_all_status.has_value());

  const auto clean_snapshot = manager.ExportSnapshot();
  assert(clean_snapshot.dirty_page_count == 0);
  assert(clean_snapshot.flush_queued_count == 0);
  assert(clean_snapshot.flush_in_flight_count == 0);
}

void ExpectPinnedDirtyPageDoesNotFlushUntilReleased() {
  auto backend = std::make_unique<CoordinatedWriteBackend>(4096);
  auto *backend_ptr = backend.get();
  backend_ptr->BlockAllWriteCompletions();

  telepath::BufferManagerOptions options = QueuedOptions();
  options.pool_size = 2;
  options.page_size = 4096;
  options.enable_background_cleaner = true;
  options.dirty_page_high_watermark = 1;
  options.dirty_page_low_watermark = 0;
  auto replacer = telepath::MakeClockReplacer(2);
  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto manager_owner = telepath::BufferManager::Create(options, std::move(backend), std::move(replacer), telemetry);
  assert(manager_owner.has_value());
  telepath::BufferManager &manager = *manager_owner.value();

  auto handle = WriteHandle(&manager, 23, 0);
  handle.mutable_data()[0] = std::byte{0x90};
  assert(!backend_ptr->WaitForSubmittedWritesFor(1, std::chrono::milliseconds(50)));

  handle.Reset();
  assert(backend_ptr->WaitForSubmittedWritesFor(1, std::chrono::milliseconds(200)));
  backend_ptr->AllowAllWriteCompletions();
  assert(backend_ptr->WaitForPersistedByte({23, 0}, 0, std::byte{0x90}, std::chrono::milliseconds(200)));
}

void ExpectFlushAllWaitsForInFlightBackgroundFlush() {
  auto backend = std::make_unique<CoordinatedWriteBackend>(4096);
  auto *backend_ptr = backend.get();
  backend_ptr->BlockAllWriteCompletions();

  telepath::BufferManagerOptions options = QueuedOptions();
  options.pool_size = 1;
  options.page_size = 4096;
  options.enable_background_cleaner = true;
  options.dirty_page_high_watermark = 1;
  options.dirty_page_low_watermark = 0;
  auto replacer = telepath::MakeClockReplacer(1);
  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto manager_owner = telepath::BufferManager::Create(options, std::move(backend), std::move(replacer), telemetry);
  assert(manager_owner.has_value());
  telepath::BufferManager &manager = *manager_owner.value();

  auto handle = WriteHandle(&manager, 26, 0);
  handle.mutable_data()[0] = std::byte{0xC0};
  handle.Reset();

  backend_ptr->WaitForSubmittedWrites(1);

  std::atomic<bool> flush_all_finished{false};
  telepath::Status flush_all_status = telepath::Ok();
  std::thread flush_all_thread([&]() {
    flush_all_status = manager.FlushAll();
    flush_all_finished.store(true);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  assert(!flush_all_finished.load());
  assert(backend_ptr->submitted_writes() == 1);

  backend_ptr->AllowAllWriteCompletions();
  flush_all_thread.join();
  assert(flush_all_status.has_value());
  assert(backend_ptr->submitted_writes() == 1);
  assert(backend_ptr->WaitForPersistedByte({26, 0}, 0, std::byte{0xC0}, std::chrono::milliseconds(1000)));
}

void ExpectEvictionAfterCleanerFlushDoesNotDuplicateWriteback() {
  auto backend = std::make_unique<CoordinatedWriteBackend>(4096);
  auto *backend_ptr = backend.get();

  telepath::BufferManagerOptions options = QueuedOptions();
  options.pool_size = 1;
  options.page_size = 4096;
  options.enable_background_cleaner = true;
  options.dirty_page_high_watermark = 1;
  options.dirty_page_low_watermark = 0;
  auto replacer = telepath::MakeClockReplacer(1);
  auto telemetry = telepath::MakeNoOpTelemetrySink();
  auto manager_owner = telepath::BufferManager::Create(options, std::move(backend), std::move(replacer), telemetry);
  assert(manager_owner.has_value());
  telepath::BufferManager &manager = *manager_owner.value();

  auto first = WriteHandle(&manager, 24, 0);
  first.mutable_data()[0] = std::byte{0xA0};
  first.Reset();

  assert(backend_ptr->WaitForSubmittedWritesFor(1, std::chrono::milliseconds(200)));
  assert(backend_ptr->WaitForPersistedByte({24, 0}, 0, std::byte{0xA0}, std::chrono::milliseconds(200)));

  const std::size_t writes_before_eviction = backend_ptr->submitted_writes();
  assert(manager.FlushAll().has_value());
  assert(backend_ptr->submitted_writes() == writes_before_eviction);

  auto second = WriteHandle(&manager, 24, 1);
  assert(second.data()[0] == std::byte{0x00});
  second.Reset();
  assert(backend_ptr->submitted_writes() == writes_before_eviction);
}

}  // namespace

int main() {
  ExpectForegroundFlushUsesFlushWorkerAndPersistsLatestDirtyVersion();
  ExpectEvictionUsesFlushWorkerForWriteback();
  ExpectForegroundBurstLimitLetsForegroundFlushesCutAhead();
  ExpectForegroundFlushSubmitsAfterOlderBackgroundWrite();
  ExpectCleanerBacklogDoesNotPreventFlushAllProgress();
  ExpectSerializedFlushWorkerCompletesIndependentFlushes();
  ExpectFlushAllSubmitsSecondBatchAfterFirstBatchCompletes();
  ExpectFlushAllWaitsForAllBlockedWrites();
  ExpectFailedFlushAllLeavesHealthyPagePersistedAndAllowsRetry();
  ExpectFlushAllReportsSubmitFailureAndAllowsRetry();
  ExpectBackgroundCleanerPersistsAtLeastOneDirtyPage();
  ExpectCleanerFlushFailureRequeuesAndRetries();
  ExpectSnapshotAggregatesQueuedAndInFlightFlushes();
  ExpectPinnedDirtyPageDoesNotFlushUntilReleased();
  ExpectFlushAllWaitsForInFlightBackgroundFlush();
  ExpectEvictionAfterCleanerFlushDoesNotDuplicateWriteback();
  return 0;
}
