#include "io_engine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>
#include <vector>

#include "frame_control.h"

namespace telepath {

namespace {

constexpr std::size_t kReapBatch = 32;
constexpr std::chrono::milliseconds kStopCheckInterval{10};
constexpr uint64_t kAsyncTokenTag = 1;

}  // namespace

void IoWaiter::Complete(const Status &status) {
  if (!status.has_value() && !error_claimed_.exchange(true, std::memory_order_relaxed)) error_ = status;
  if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) frame_control::FutexWakeAll(pending_);
}

IoEngine::IoEngine(DiskBackend *backend) : backend_(backend) {
  if (backend_ != nullptr) max_request_blocks_ = std::clamp<std::size_t>(backend_->GetCapabilities().max_request_blocks, 1, kMaxDiskRequestBlocks);
}

IoEngine::~IoEngine() { Shutdown(Unavailable("io engine destroyed")); }

auto IoEngine::Start(FatalCallback on_fatal) -> Status {
  on_fatal_ = std::move(on_fatal);
  try {
    reaper_ = std::thread(&IoEngine::Run, this);
  } catch (...) {
    return Unavailable("failed to start io engine");
  }
  return Ok();
}

void IoEngine::Shutdown(const Status &reason) {
  {
    std::lock_guard<std::mutex> guard(stop_latch_);
    if (stopping_.exchange(true)) return;
    stop_status_ = reason;
  }
  if (backend_ != nullptr) backend_->Shutdown();
  demand_.fetch_add(1, std::memory_order_acq_rel);
  frame_control::FutexWakeAll(demand_);
  if (reaper_.joinable()) reaper_.join();
  stopped_.store(true, std::memory_order_release);
}

void IoEngine::Submit(std::span<const DiskRequest> requests, DiskSubmitMode mode) {
  if (mode == DiskSubmitMode::kWait) {
    SubmitTagged(requests, mode);
    return;
  }
  // Nobody waits on asynchronous requests right away, so the reaper must run
  // until they complete; their tokens carry kAsyncTokenTag to account for that.
  std::vector<DiskRequest> tagged(requests.begin(), requests.end());
  for (DiskRequest &request : tagged) request.token |= kAsyncTokenTag;
  AddReaperDemand(static_cast<std::uint32_t>(tagged.size()));
  SubmitTagged(tagged, mode);
}

void IoEngine::SubmitTagged(std::span<const DiskRequest> requests, DiskSubmitMode mode) {
  std::size_t offset = 0;
  while (offset < requests.size()) {
    if (stopped_.load(std::memory_order_acquire)) {
      const Status stopped = StopStatus();
      for (; offset < requests.size(); ++offset) Deliver(DiskCompletion{requests[offset].token, stopped});
      return;
    }
    Result<std::size_t> accepted = backend_->Submit(requests.subspan(offset), mode);
    if (accepted.has_value() && accepted.value() > 0) {
      offset += accepted.value();
      continue;
    }
    // The error describes the first request of the call; the rest are retried.
    Deliver(DiskCompletion{requests[offset].token, accepted.has_value() ? Internal("disk backend accepted no request") : PropagateError(accepted)});
    ++offset;
  }
}

auto IoEngine::Wait(IoWaiter &waiter) -> Status {
  bool sleeping = false;
  while (true) {
    const std::uint32_t pending = waiter.pending_.load(std::memory_order_acquire);
    if (pending == 0) break;
    if (ReapReady()) continue;
    if (stopped_.load(std::memory_order_acquire)) {
      if (sleeping) demand_.fetch_sub(1, std::memory_order_acq_rel);
      return StopStatus();
    }
    if (!sleeping) {
      // Nothing is ready: let the reaper deliver the completion.
      sleeping = true;
      AddReaperDemand(1);
      continue;
    }
    frame_control::FutexWaitFor(waiter.pending_, pending, kStopCheckInterval);
  }
  if (sleeping) demand_.fetch_sub(1, std::memory_order_acq_rel);
  return waiter.status();
}

bool IoEngine::ReapReady() {
  std::array<DiskCompletion, kReapBatch> completions;
  Result<std::size_t> reaped = backend_->Reap(completions, false);
  if (!reaped.has_value() || reaped.value() == 0) return false;
  Dispatch(std::span<const DiskCompletion>(completions.data(), reaped.value()));
  return true;
}

void IoEngine::AddReaperDemand(std::uint32_t count) {
  if (demand_.fetch_add(count, std::memory_order_acq_rel) == 0) frame_control::FutexWakeAll(demand_);
}

void IoEngine::Deliver(const DiskCompletion &completion) {
  const bool async = (completion.token & kAsyncTokenTag) != 0;
  reinterpret_cast<IoCompletion *>(completion.token & ~kAsyncTokenTag)->Complete(completion.status);
  if (async) demand_.fetch_sub(1, std::memory_order_acq_rel);
}

void IoEngine::Dispatch(std::span<const DiskCompletion> completions) {
  for (const DiskCompletion &completion : completions) Deliver(completion);
}

void IoEngine::Stop(const Status &status) {
  {
    std::lock_guard<std::mutex> guard(stop_latch_);
    if (!stopping_.load()) stop_status_ = status;
  }
  stopped_.store(true, std::memory_order_release);
}

auto IoEngine::StopStatus() -> Status {
  std::lock_guard<std::mutex> guard(stop_latch_);
  return stop_status_;
}

void IoEngine::Run() {
  std::array<DiskCompletion, kReapBatch> completions;
  while (true) {
    const bool stopping = stopping_.load(std::memory_order_acquire);
    if (!stopping) {
      const std::uint32_t demand = demand_.load(std::memory_order_acquire);
      if (demand == 0) {
        frame_control::FutexWait(demand_, 0);
        continue;
      }
    }
    Result<std::size_t> reaped = backend_->Reap(completions, true);
    if (!reaped.has_value()) {
      if (!stopping_.load() && on_fatal_) on_fatal_(reaped.error());
      Stop(PropagateError(reaped));
      return;
    }
    Dispatch(std::span<const DiskCompletion>(completions.data(), reaped.value()));
  }
}

}  // namespace telepath
