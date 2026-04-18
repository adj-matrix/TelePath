#include "flush_scheduler.h"

#include <utility>

#include "completion_dispatcher.h"
#include "telepath/io/disk_backend.h"

namespace telepath {

auto BufferManagerFlushTask::Wait() -> Status {
  std::unique_lock<std::mutex> lock(latch);
  cv.wait(lock, [&]() { return completed; });
  return status;
}

void BufferManagerFlushTask::Complete(const Status &flush_status) {
  {
    std::lock_guard<std::mutex> guard(latch);
    completed = true;
    status = flush_status;
  }
  cv.notify_all();
}

BufferManagerFlushScheduler::BufferManagerFlushScheduler(
  DiskBackend *disk_backend,
  BufferManagerCompletionDispatcher *dispatcher,
  std::size_t page_size,
  std::size_t worker_count,
  std::size_t submit_batch_size,
  std::size_t foreground_burst_limit
) : disk_backend_(disk_backend),
    dispatcher_(dispatcher),
    page_size_(page_size),
    worker_count_(worker_count),
    submit_batch_size_(submit_batch_size),
    foreground_burst_limit_(foreground_burst_limit) {}

BufferManagerFlushScheduler::~BufferManagerFlushScheduler() { Shutdown(); }

auto BufferManagerFlushScheduler::Start(TaskStartCallback on_task_start, TaskCompleteCallback on_task_complete) -> Status {
  {
    std::lock_guard<std::mutex> guard(latch_);
    if (started_) return Status::Ok();
    on_task_start_ = std::move(on_task_start);
    on_task_complete_ = std::move(on_task_complete);
  }
  try {
    workers_.reserve(worker_count_);
    for (std::size_t worker = 0; worker < worker_count_; ++worker) {
      workers_.emplace_back(&BufferManagerFlushScheduler::Run, this);
    }
  } catch (...) {
    {
      std::lock_guard<std::mutex> guard(latch_);
      shutdown_ = true;
    }
    cv_.notify_all();
    for (auto &worker : workers_) {
      if (worker.joinable()) worker.join();
    }
    workers_.clear();
    return Status::Unavailable("failed to start flush scheduler");
  }

  {
    std::lock_guard<std::mutex> guard(latch_);
    started_ = true;
  }
  return Status::Ok();
}

void BufferManagerFlushScheduler::Shutdown() {
  {
    std::lock_guard<std::mutex> guard(latch_);
    if (!started_) return;
    shutdown_ = true;
  }
  cv_.notify_all();
  for (auto &worker : workers_) {
    if (worker.joinable()) worker.join();
  }
  workers_.clear();
  {
    std::lock_guard<std::mutex> guard(latch_);
    started_ = false;
  }
}

auto BufferManagerFlushScheduler::Enqueue(const TaskPtr &task) -> Status {
  if (task == nullptr) return Status::InvalidArgument("flush task must not be null");

  std::lock_guard<std::mutex> guard(latch_);
  if (shutdown_) return Status::Unavailable("flush scheduler is shutting down");

  if (task->cleaner_owned) background_queue_.push_back(task);
  else foreground_queue_.push_back(task);

  cv_.notify_one();
  return Status::Ok();
}

auto BufferManagerFlushScheduler::Wait(const TaskPtr &task) -> Status {
  if (task == nullptr) return Status::InvalidArgument("flush task must not be null");
  return task->Wait();
}

auto BufferManagerFlushScheduler::TakeNextBatch() -> Batch {
  std::unique_lock<std::mutex> lock(latch_);
  cv_.wait(lock, [&]() {
    return shutdown_ || !foreground_queue_.empty() || !background_queue_.empty();
  });
  if (foreground_queue_.empty() && background_queue_.empty()) return Batch{{}, shutdown_};

  static thread_local std::size_t consecutive_foreground_flushes = 0;
  Batch batch;
  batch.tasks.reserve(submit_batch_size_);
  while (batch.tasks.size() < submit_batch_size_) {
    if (ShouldServeBackground(consecutive_foreground_flushes)) {
      batch.tasks.push_back(background_queue_.front());
      background_queue_.pop_front();
      consecutive_foreground_flushes = 0;
      continue;
    }
    if (!foreground_queue_.empty()) {
      batch.tasks.push_back(foreground_queue_.front());
      foreground_queue_.pop_front();
      ++consecutive_foreground_flushes;
      continue;
    }
    if (background_queue_.empty()) break;
    batch.tasks.push_back(background_queue_.front());
    background_queue_.pop_front();
    consecutive_foreground_flushes = 0;
  }
  return batch;
}

auto BufferManagerFlushScheduler::ShouldServeBackground(std::size_t consecutive_foreground_flushes) const -> bool {
  return !background_queue_.empty() && consecutive_foreground_flushes >= foreground_burst_limit_;
}

void BufferManagerFlushScheduler::CompleteTask(const TaskPtr &task, const Status &status) {
  if (on_task_complete_) on_task_complete_(task, status);
  task->Complete(status);
}

void BufferManagerFlushScheduler::Run() {
  while (true) {
    Batch batch = TakeNextBatch();
    if (batch.tasks.empty()) {
      if (batch.should_stop) return;
      continue;
    }

    std::vector<SubmittedTask> submitted_tasks;
    submitted_tasks.reserve(batch.tasks.size());
    for (const auto &task : batch.tasks) {
      if (on_task_start_) on_task_start_(task);

      Result<uint64_t> submit_result = disk_backend_->SubmitWrite(task->tag, task->snapshot.data(), page_size_);
      if (!submit_result.ok()) {
        CompleteTask(task, submit_result.status());
        continue;
      }

      dispatcher_->Register(submit_result.value());
      submitted_tasks.push_back({task, submit_result.value()});
    }

    for (const auto &submitted_task : submitted_tasks) {
      CompleteTask(submitted_task.task, dispatcher_->Wait(submitted_task.request_id));
    }
  }
}

}  // namespace telepath
