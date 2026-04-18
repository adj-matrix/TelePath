#ifndef TELEPATH_LIB_BUFFER_FLUSH_SCHEDULER_H_
#define TELEPATH_LIB_BUFFER_FLUSH_SCHEDULER_H_

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "telepath/common/status.h"
#include "telepath/common/types.h"

namespace telepath {

class DiskBackend;
class BufferManagerCompletionDispatcher;

struct BufferManagerFlushTask {
  FrameId frame_id{kInvalidFrameId};
  BufferTag tag{};
  uint64_t generation{0};
  bool clear_dirty_on_success{false};
  bool cleaner_owned{false};
  std::vector<std::byte> snapshot;

  auto Wait() -> Status;
  void Complete(const Status &status);

 private:
  std::mutex latch;
  std::condition_variable cv;
  bool completed{false};
  Status status{};
};

class BufferManagerFlushScheduler {
 public:
  using TaskPtr = std::shared_ptr<BufferManagerFlushTask>;
  using TaskStartCallback = std::function<void(const TaskPtr &)>;
  using TaskCompleteCallback = std::function<void(const TaskPtr &, const Status &)>;

  BufferManagerFlushScheduler(
    DiskBackend *disk_backend,
    BufferManagerCompletionDispatcher *dispatcher,
    std::size_t page_size,
    std::size_t worker_count,
    std::size_t submit_batch_size,
    std::size_t foreground_burst_limit);
  ~BufferManagerFlushScheduler();

  auto Start(
    TaskStartCallback on_task_start,
    TaskCompleteCallback on_task_complete
  ) -> Status;
  void Shutdown();

  auto Enqueue(const TaskPtr &task) -> Status;
  auto Wait(const TaskPtr &task) -> Status;

 private:
  struct Batch {
    std::vector<TaskPtr> tasks;
    bool should_stop{false};
  };

  struct SubmittedTask {
    TaskPtr task;
    uint64_t request_id{0};
  };

  auto TakeNextBatch() -> Batch;
  bool ShouldServeBackground(std::size_t consecutive_foreground_flushes) const;
  void CompleteTask(const TaskPtr &task, const Status &status);
  void Run();

  DiskBackend *disk_backend_{nullptr};
  BufferManagerCompletionDispatcher *dispatcher_{nullptr};
  std::size_t page_size_{0};
  std::size_t worker_count_{0};
  std::size_t submit_batch_size_{0};
  std::size_t foreground_burst_limit_{0};
  TaskStartCallback on_task_start_{};
  TaskCompleteCallback on_task_complete_{};
  std::mutex latch_;
  std::condition_variable cv_;
  std::deque<TaskPtr> foreground_queue_;
  std::deque<TaskPtr> background_queue_;
  std::vector<std::thread> workers_;
  bool shutdown_{false};
  bool started_{false};
};

}  // namespace telepath

#endif  // TELEPATH_LIB_BUFFER_FLUSH_SCHEDULER_H_
