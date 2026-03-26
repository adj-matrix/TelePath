#include <atomic>
#include <cassert>
#include <cstddef>
#include <condition_variable>
#include <memory>
#include <mutex>

#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

struct IdleBackendState {
  std::atomic<int> poll_calls{0};
  std::atomic<int> shutdown_calls{0};
};

class IdleAwareDiskBackend : public telepath::DiskBackend {
 public:
  explicit IdleAwareDiskBackend(std::shared_ptr<IdleBackendState> state)
      : state_(std::move(state)) {}

  telepath::Result<uint64_t> SubmitRead(const telepath::BufferTag &,
                                        std::byte *,
                                        std::size_t) override {
    return telepath::Status::Unavailable("not expected");
  }

  telepath::Result<uint64_t> SubmitWrite(const telepath::BufferTag &,
                                         const std::byte *,
                                         std::size_t) override {
    return telepath::Status::Unavailable("not expected");
  }

  telepath::Result<telepath::DiskCompletion> PollCompletion() override {
    state_->poll_calls.fetch_add(1);
    std::unique_lock<std::mutex> lock(latch_);
    cv_.wait(lock, [this]() { return shutdown_; });
    return telepath::Status::Unavailable("backend shutdown");
  }

  void Shutdown() override {
    {
      std::lock_guard<std::mutex> guard(latch_);
      shutdown_ = true;
    }
    state_->shutdown_calls.fetch_add(1);
    cv_.notify_all();
  }

  telepath::DiskBackendCapabilities GetCapabilities() const override {
    return {telepath::DiskBackendKind::kPosix, false, false, 1, false};
  }

 private:
  std::shared_ptr<IdleBackendState> state_;
  std::mutex latch_;
  std::condition_variable cv_;
  bool shutdown_{false};
};

}  // namespace

int main() {
  auto state = std::make_shared<IdleBackendState>();
  {
    auto backend = std::make_unique<IdleAwareDiskBackend>(state);
    auto replacer = telepath::MakeClockReplacer(2);
    auto telemetry = telepath::MakeNoOpTelemetrySink();
    telepath::BufferManager manager(2, 4096, std::move(backend),
                                    std::move(replacer), telemetry);
  }

  assert(state->poll_calls.load() == 0);
  assert(state->shutdown_calls.load() == 1);
  return 0;
}
