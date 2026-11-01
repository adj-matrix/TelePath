#ifndef TELEPATH_LIB_BUFFER_CLEANER_CONTROLLER_H_
#define TELEPATH_LIB_BUFFER_CLEANER_CONTROLLER_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "telepath/common/status.h"
#include "telepath/common/types.h"

namespace telepath {

class BufferManagerCleanerController {
 public:
  using SeedCallback = std::function<void()>;
  using ScheduleCallback = std::function<void(FrameId)>;
  // Whether this process may clean now; asked before each round of cleaning.
  using LeadCallback = std::function<bool()>;

  // With a `lead` callback the controller cleans only while it returns true,
  // and with a nonzero `poll_interval` it also rechecks the dirty-page count
  // and its leadership that often, since another process can dirty pages or
  // give up leadership without notifying it.
  BufferManagerCleanerController(
    std::size_t pool_size,
    std::atomic<std::size_t> *dirty_page_count,
    std::size_t dirty_page_high_watermark,
    std::size_t dirty_page_low_watermark,
    LeadCallback lead = {},
    std::chrono::milliseconds poll_interval = std::chrono::milliseconds(0));
  ~BufferManagerCleanerController();

  auto Start(SeedCallback on_seed, ScheduleCallback on_schedule) -> Status;
  void Shutdown();

  void Notify();
  void ResetCandidate(FrameId frame_id);
  void EnqueueCandidate(FrameId frame_id);
  void OnFlushScheduled();
  void OnFlushFinished();

 private:
  auto DirtyPageCount() const -> std::size_t;
  auto TakeCandidate(FrameId *frame_id) -> bool;
  void Run();

  std::size_t pool_size_{0};
  std::atomic<std::size_t> *dirty_page_count_{nullptr};
  std::size_t dirty_page_high_watermark_{0};
  std::size_t dirty_page_low_watermark_{0};
  LeadCallback lead_{};
  std::chrono::milliseconds poll_interval_{0};
  SeedCallback on_seed_{};
  ScheduleCallback on_schedule_{};
  std::mutex latch_;
  std::condition_variable cv_;
  std::size_t inflight_flushes_{0};
  std::deque<std::pair<FrameId, uint64_t>> candidate_queue_;
  std::vector<bool> candidate_enqueued_;
  std::vector<uint64_t> candidate_generations_;
  std::thread thread_;
  bool shutdown_{false};
  bool started_{false};
};

}  // namespace telepath

#endif  // TELEPATH_LIB_BUFFER_CLEANER_CONTROLLER_H_
