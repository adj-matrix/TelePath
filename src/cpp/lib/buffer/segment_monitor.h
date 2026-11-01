#ifndef TELEPATH_LIB_BUFFER_SEGMENT_MONITOR_H_
#define TELEPATH_LIB_BUFFER_SEGMENT_MONITOR_H_

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace telepath {

// Runs `check` every `interval` on its own thread until `check` returns false
// or Stop() is called; watches a cross-process segment for dead processes.
class SegmentMonitor {
 public:
  SegmentMonitor(std::chrono::milliseconds interval, std::function<bool()> check)
    : interval_(interval), check_(std::move(check)), thread_([this] { Run(); }) {}

  SegmentMonitor(const SegmentMonitor &) = delete;
  SegmentMonitor &operator=(const SegmentMonitor &) = delete;
  ~SegmentMonitor() { Stop(); }

  void Stop() {
    {
      std::lock_guard<std::mutex> guard(latch_);
      stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }

 private:
  void Run() {
    std::unique_lock<std::mutex> lock(latch_);
    while (!cv_.wait_for(lock, interval_, [this] { return stopping_; })) {
      lock.unlock();
      const bool keep_going = check_();
      lock.lock();
      if (!keep_going) return;
    }
  }

  std::chrono::milliseconds interval_;
  std::function<bool()> check_;
  std::mutex latch_;
  std::condition_variable cv_;
  bool stopping_{false};
  std::thread thread_;
};

}  // namespace telepath

#endif  // TELEPATH_LIB_BUFFER_SEGMENT_MONITOR_H_
