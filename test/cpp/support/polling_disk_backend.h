#ifndef TELEPATH_TEST_CPP_SUPPORT_POLLING_DISK_BACKEND_H_
#define TELEPATH_TEST_CPP_SUPPORT_POLLING_DISK_BACKEND_H_

#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <unordered_map>

#include "telepath/io/disk_backend.h"

namespace telepath::test_support {

// One single-block request as seen by a PollingDiskBackend fake.
struct LegacyDiskRequest {
  uint64_t request_id{0};
  DiskOperation operation{DiskOperation::kRead};
  BufferTag tag{};
  std::byte *mutable_buffer{nullptr};
  const std::byte *const_buffer{nullptr};
  std::size_t size{0};
};

struct LegacyDiskCompletion {
  uint64_t request_id{0};
  DiskOperation operation{DiskOperation::kRead};
  BufferTag tag{};
  Status status{};
};

// Adapts a scripted fake written as "submit one block, poll one completion" to
// the batch DiskBackend interface. Non-blocking reaps never return anything,
// so every completion comes from a blocking Reap() (the engine's reaper
// thread) and the fake fully controls completion timing. PollCompletion() is
// only called while a request is outstanding. Sync requests succeed at once.
class PollingDiskBackend : public DiskBackend {
 public:
  explicit PollingDiskBackend(std::size_t block_size = 4096, std::size_t queue_depth = 1) : block_size_(block_size), queue_depth_(queue_depth) {}

  // A request the fake rejects after an accepted prefix is reported when the
  // caller resubmits it, so each rejection reaches the right request.
  auto Submit(std::span<const DiskRequest> requests, DiskSubmitMode) -> Result<std::size_t> final {
    std::size_t accepted = 0;
    for (const DiskRequest &request : requests) {
      Result<uint64_t> id = SubmitLegacy(request);
      if (!id.has_value()) {
        if (accepted == 0) return std::unexpected(id.error());
        std::lock_guard<std::mutex> guard(token_latch_);
        deferred_errors_.insert_or_assign(request.token, id.error());
        return accepted;
      }
      ++accepted;
    }
    return accepted;
  }

  auto Reap(std::span<DiskCompletion> out, bool wait) -> Result<std::size_t> final {
    if (!wait || out.empty()) return std::size_t{0};
    {
      std::unique_lock<std::mutex> lock(outstanding_latch_);
      outstanding_cv_.wait(lock, [this]() { return outstanding_ > 0 || shutdown_; });
      if (outstanding_ == 0) return Unavailable("backend shutting down");
      if (!synced_.empty()) {
        out[0] = synced_.front();
        synced_.pop_front();
        --outstanding_;
        return std::size_t{1};
      }
    }
    Result<LegacyDiskCompletion> completion = PollCompletion();
    if (!completion.has_value()) return std::unexpected(completion.error());
    {
      std::lock_guard<std::mutex> guard(outstanding_latch_);
      --outstanding_;
    }
    std::lock_guard<std::mutex> guard(token_latch_);
    auto it = tokens_.find(completion->request_id);
    if (it == tokens_.end()) return Internal("completion for an unknown request");
    out[0] = DiskCompletion{it->second, completion->status};
    tokens_.erase(it);
    return std::size_t{1};
  }

  auto SubmitLegacy(const DiskRequest &request) -> Result<uint64_t> {
    if (request.operation == DiskOperation::kSync) {
      {
        std::lock_guard<std::mutex> guard(outstanding_latch_);
        synced_.push_back(DiskCompletion{request.token, Ok()});
        ++outstanding_;
      }
      outstanding_cv_.notify_all();
      return uint64_t{0};
    }
    std::lock_guard<std::mutex> guard(token_latch_);
    if (auto deferred = deferred_errors_.find(request.token); deferred != deferred_errors_.end()) {
      Error error = deferred->second;
      deferred_errors_.erase(deferred);
      return std::unexpected(error);
    }
    Result<uint64_t> id = request.operation == DiskOperation::kRead
      ? SubmitRead(request.tag, request.buffers[0], block_size_)
      : SubmitWrite(request.tag, request.buffers[0], block_size_);
    if (!id.has_value()) return id;
    tokens_[id.value()] = request.token;
    {
      std::lock_guard<std::mutex> outstanding_guard(outstanding_latch_);
      ++outstanding_;
    }
    outstanding_cv_.notify_all();
    return id;
  }

  void Shutdown() final {
    {
      std::lock_guard<std::mutex> guard(outstanding_latch_);
      shutdown_ = true;
    }
    outstanding_cv_.notify_all();
    OnShutdown();
  }

  auto GetCapabilities() const -> DiskBackendCapabilities final {
    DiskBackendCapabilities capabilities;
    capabilities.queue_depth = queue_depth_;
    return capabilities;
  }

 protected:
  virtual auto SubmitRead(const BufferTag &tag, std::byte *out, std::size_t size) -> Result<uint64_t> = 0;
  virtual auto SubmitWrite(const BufferTag &tag, const std::byte *data, std::size_t size) -> Result<uint64_t> = 0;
  // Blocks until a request completes; returns kUnavailable after Shutdown().
  virtual auto PollCompletion() -> Result<LegacyDiskCompletion> = 0;
  // Unblocks PollCompletion() for shutdown.
  virtual void OnShutdown() {}

 private:
  std::size_t block_size_;
  std::size_t queue_depth_;
  std::mutex token_latch_;
  std::mutex outstanding_latch_;
  std::condition_variable outstanding_cv_;
  std::size_t outstanding_{0};
  std::deque<DiskCompletion> synced_;
  bool shutdown_{false};
  std::unordered_map<uint64_t, uint64_t> tokens_;
  std::unordered_map<uint64_t, Error> deferred_errors_;
};

}  // namespace telepath::test_support

#endif  // TELEPATH_TEST_CPP_SUPPORT_POLLING_DISK_BACKEND_H_
