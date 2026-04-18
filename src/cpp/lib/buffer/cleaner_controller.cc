#include "cleaner_controller.h"

namespace telepath {

BufferManagerCleanerController::BufferManagerCleanerController(
  std::size_t pool_size, std::atomic<std::size_t> *dirty_page_count,
  std::size_t dirty_page_high_watermark,
  std::size_t dirty_page_low_watermark
) : pool_size_(pool_size),
    dirty_page_count_(dirty_page_count),
    dirty_page_high_watermark_(dirty_page_high_watermark),
    dirty_page_low_watermark_(dirty_page_low_watermark),
    candidate_enqueued_(pool_size, false),
    candidate_generations_(pool_size, 0) {}

BufferManagerCleanerController::~BufferManagerCleanerController() { Shutdown(); }

auto BufferManagerCleanerController::Start(SeedCallback on_seed, ScheduleCallback on_schedule) -> Status {
  std::lock_guard<std::mutex> guard(latch_);
  if (started_) return Status::Ok();

  on_seed_ = std::move(on_seed);
  on_schedule_ = std::move(on_schedule);
  try {
    thread_ = std::thread(&BufferManagerCleanerController::Run, this);
  } catch (...) {
    return Status::Unavailable("failed to start background cleaner");
  }

  started_ = true;
  return Status::Ok();
}

void BufferManagerCleanerController::Shutdown() {
  {
    std::lock_guard<std::mutex> guard(latch_);
    if (!started_) return;
    shutdown_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  started_ = false;
}

void BufferManagerCleanerController::Notify() { cv_.notify_one(); }

void BufferManagerCleanerController::ResetCandidate(FrameId frame_id) {
  if (frame_id >= pool_size_) return;

  std::lock_guard<std::mutex> guard(latch_);
  candidate_enqueued_[frame_id] = false;
  ++candidate_generations_[frame_id];
}

void BufferManagerCleanerController::EnqueueCandidate(FrameId frame_id) {
  if (frame_id >= pool_size_) return;

  std::lock_guard<std::mutex> guard(latch_);
  if (candidate_enqueued_[frame_id]) return;

  candidate_enqueued_[frame_id] = true;
  candidate_queue_.emplace_back(frame_id, candidate_generations_[frame_id]);
}

void BufferManagerCleanerController::OnFlushScheduled() {
  std::lock_guard<std::mutex> guard(latch_);
  ++inflight_flushes_;
}

void BufferManagerCleanerController::OnFlushFinished() {
  {
    std::lock_guard<std::mutex> guard(latch_);
    if (inflight_flushes_ > 0) --inflight_flushes_;
  }
  cv_.notify_one();
}

auto BufferManagerCleanerController::DirtyPageCount() const -> std::size_t {
  if (dirty_page_count_ == nullptr) return 0;
  return dirty_page_count_->load(std::memory_order_acquire);
}

auto BufferManagerCleanerController::TakeCandidate(FrameId *frame_id) -> bool {
  while (!candidate_queue_.empty()) {
    const auto [candidate_frame_id, candidate_generation] = candidate_queue_.front();
    candidate_queue_.pop_front();
    if (candidate_frame_id >= pool_size_) continue;
    if (!candidate_enqueued_[candidate_frame_id]) continue;
    if (candidate_generations_[candidate_frame_id] != candidate_generation) continue;

    candidate_enqueued_[candidate_frame_id] = false;
    *frame_id = candidate_frame_id;
    return true;
  }
  return false;
}

void BufferManagerCleanerController::Run() {
  while (true) {
    std::unique_lock<std::mutex> lock(latch_);
    cv_.wait(lock, [&]() {
      return shutdown_ || DirtyPageCount() >= dirty_page_high_watermark_;
    });
    if (shutdown_) return;

    while (!shutdown_) {
      const std::size_t dirty_snapshot = DirtyPageCount();
      if (dirty_snapshot < dirty_page_high_watermark_) break;

      const std::size_t flush_budget = dirty_snapshot > dirty_page_low_watermark_ ? dirty_snapshot - dirty_page_low_watermark_ : 0;
      if (flush_budget == 0) break;

      if (inflight_flushes_ >= flush_budget) {
        cv_.wait(lock, [&]() {
          const std::size_t dirty_snapshot = DirtyPageCount();
          const std::size_t current_flush_budget = dirty_snapshot > dirty_page_low_watermark_ ? dirty_snapshot - dirty_page_low_watermark_ : 0;
          return shutdown_ || dirty_snapshot < dirty_page_high_watermark_ || inflight_flushes_ < current_flush_budget;
        });
        continue;
      }

      if (candidate_queue_.empty()) {
        lock.unlock();
        if (on_seed_) on_seed_();
        lock.lock();
        if (candidate_queue_.empty()) break;
      }

      FrameId frame_id = kInvalidFrameId;
      if (!TakeCandidate(&frame_id)) continue;

      lock.unlock();
      if (on_schedule_) on_schedule_(frame_id);
      lock.lock();
    }
  }
}

}  // namespace telepath
