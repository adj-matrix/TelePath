#include "completion_dispatcher.h"

#include "telepath/io/disk_backend.h"

namespace telepath {

BufferManagerCompletionDispatcher::BufferManagerCompletionDispatcher(DiskBackend *disk_backend) : disk_backend_(disk_backend) {}

BufferManagerCompletionDispatcher::~BufferManagerCompletionDispatcher() {
  Shutdown(Status::Unavailable("completion dispatcher destroyed"));
}

auto BufferManagerCompletionDispatcher::Start() -> Status {
  try {
    thread_ = std::thread(&BufferManagerCompletionDispatcher::Run, this);
  } catch (...) {
    return Status::Unavailable("failed to start completion dispatcher");
  }
  return Status::Ok();
}

void BufferManagerCompletionDispatcher::Shutdown(const Status &status) {
  {
    std::lock_guard<std::mutex> guard(latch_);
    if (stopped_) return;
    stopped_ = true;
    shutdown_ = true;
    shutdown_status_ = status;
  }
  cv_.notify_all();
  if (disk_backend_ != nullptr) disk_backend_->Shutdown();
  if (thread_.joinable()) thread_.join();
}

void BufferManagerCompletionDispatcher::Register(uint64_t request_id) {
  std::lock_guard<std::mutex> guard(latch_);
  request_states_.try_emplace(request_id);
  ++outstanding_requests_;
  cv_.notify_all();
}

auto BufferManagerCompletionDispatcher::Wait(uint64_t request_id) -> Status {
  std::unique_lock<std::mutex> lock(latch_);
  if (request_states_.find(request_id) == request_states_.end()) return Status::InvalidArgument("disk request was not registered");

  cv_.wait(lock, [&]() {
    auto state_it = request_states_.find(request_id);
    return shutdown_ || (state_it != request_states_.end() && state_it->second.completed);
  });

  auto state_it = request_states_.find(request_id);
  if (state_it == request_states_.end()) return Status::Internal("registered disk request disappeared");

  if (!state_it->second.completed) {
    request_states_.erase(state_it);
    return shutdown_status_;
  }

  Status status = state_it->second.status;
  request_states_.erase(state_it);
  return status;
}

void BufferManagerCompletionDispatcher::Run() {
  while (true) {
    {
      std::unique_lock<std::mutex> lock(latch_);
      cv_.wait(lock, [this]() { return shutdown_ || outstanding_requests_ > 0; });
      if (shutdown_) return;
    }

    Result<DiskCompletion> completion_result = disk_backend_->PollCompletion();
    std::lock_guard<std::mutex> guard(latch_);
    if (!completion_result.ok()) {
      shutdown_ = true;
      shutdown_status_ = completion_result.status();
      cv_.notify_all();
      return;
    }

    DiskCompletion completion = completion_result.value();
    auto &state = request_states_[completion.request_id];
    state.completed = true;
    state.status = completion.status;
    if (outstanding_requests_ > 0) --outstanding_requests_;
    cv_.notify_all();
  }
}

}  // namespace telepath
