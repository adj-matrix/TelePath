#ifndef TELEPATH_LIB_BUFFER_IO_ENGINE_H_
#define TELEPATH_LIB_BUFFER_IO_ENGINE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <thread>

#include "telepath/common/status.h"
#include "telepath/io/disk_backend.h"

namespace telepath {

// Receives the outcome of one submitted DiskRequest whose token points to it.
// Complete() runs exactly once, on whichever thread reaped the completion, and
// must not block.
class IoCompletion {
 public:
  virtual void Complete(const Status &status) = 0;

 protected:
  ~IoCompletion() = default;
};

// Counts outstanding requests down to zero and keeps the first error.
class IoWaiter final : public IoCompletion {
 public:
  explicit IoWaiter(std::uint32_t pending = 1) : pending_(pending) {}

  void Add(std::uint32_t count) { pending_.fetch_add(count, std::memory_order_relaxed); }
  void Complete(const Status &status) override;
  bool done() const { return pending_.load(std::memory_order_acquire) == 0; }
  // First error among the completed requests; meaningful once done().
  auto status() const -> Status { return error_; }

 private:
  friend class IoEngine;

  std::atomic<std::uint32_t> pending_;
  std::atomic<bool> error_claimed_{false};
  Status error_{};
};

// Owns completion delivery for one DiskBackend. A waiting thread reaps ready
// completions itself, so a request that completes during Submit() costs no
// thread switch. A background reaper thread delivers everything else; it only
// waits on the backend while asynchronous requests are outstanding or a waiter
// is asleep, so it does not compete with waiters for their own completions.
class IoEngine {
 public:
  using FatalCallback = std::move_only_function<void(const Error &)>;

  explicit IoEngine(DiskBackend *backend);
  ~IoEngine();

  auto Start(FatalCallback on_fatal = {}) -> Status;
  // Shuts the backend down, delivers the completions of requests already in
  // flight, and stops the reaper. Later waits and submissions fail with
  // `reason`.
  void Shutdown(const Status &reason);

  auto max_request_blocks() const -> std::size_t { return max_request_blocks_; }
  // Submits requests whose tokens point to IoCompletion objects. A request the
  // backend rejects is completed here with the backend's error and the requests
  // after it are still submitted, so every completion runs exactly once.
  void Submit(std::span<const DiskRequest> requests, DiskSubmitMode mode);
  // Blocks until `waiter` has no outstanding request and returns its status.
  auto Wait(IoWaiter &waiter) -> Status;

 private:
  void SubmitTagged(std::span<const DiskRequest> requests, DiskSubmitMode mode);
  bool ReapReady();
  void AddReaperDemand(std::uint32_t count);
  void Deliver(const DiskCompletion &completion);
  void Dispatch(std::span<const DiskCompletion> completions);
  void Stop(const Status &status);
  auto StopStatus() -> Status;
  void Run();

  DiskBackend *backend_;
  std::size_t max_request_blocks_{1};
  FatalCallback on_fatal_;
  // Outstanding asynchronous requests plus sleeping waiters; the reaper's
  // futex word.
  std::atomic<std::uint32_t> demand_{0};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> stopped_{false};
  std::mutex stop_latch_;
  Status stop_status_{Unavailable("io engine stopped")};
  std::thread reaper_;
};

}  // namespace telepath

#endif  // TELEPATH_LIB_BUFFER_IO_ENGINE_H_
