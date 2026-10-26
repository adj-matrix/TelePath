#ifndef TELEPATH_TEST_CPP_SUPPORT_FAULT_INJECTION_BACKEND_H_
#define TELEPATH_TEST_CPP_SUPPORT_FAULT_INJECTION_BACKEND_H_

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <span>
#include <utility>
#include <vector>

#include "telepath/io/disk_backend.h"

namespace telepath::test_support {

// Forwards to another backend and fails a random share of requests of the
// enabled kinds. The inner backend still performs a failed request, so a
// failed write may or may not have reached the file, as with a real device.
class FaultInjectionBackend : public DiskBackend {
 public:
  explicit FaultInjectionBackend(std::unique_ptr<DiskBackend> inner, std::uint64_t seed = 1) : inner_(std::move(inner)), rng_(seed) {}

  // Fails each read, write, or sync with probability `per_mille` / 1000.
  void SetFaultRate(DiskOperation operation, unsigned per_mille) { rates_[Index(operation)].store(per_mille); }
  void ClearFaults() {
    for (auto &rate : rates_) rate.store(0);
  }

  auto injected() const -> std::uint64_t { return injected_.load(); }
  auto submitted(DiskOperation operation) const -> std::uint64_t { return submitted_[Index(operation)].load(); }

  auto Submit(std::span<const DiskRequest> requests, DiskSubmitMode mode) -> Result<std::size_t> override {
    {
      std::lock_guard<std::mutex> guard(latch_);
      for (const DiskRequest &request : requests) {
        submitted_[Index(request.operation)].fetch_add(1);
        const unsigned rate = rates_[Index(request.operation)].load();
        if (rate != 0 && std::uniform_int_distribution<unsigned>(0, 999)(rng_) < rate) failing_.push_back(request.token);
      }
    }
    return inner_->Submit(requests, mode);
  }

  auto Reap(std::span<DiskCompletion> out, bool wait) -> Result<std::size_t> override {
    Result<std::size_t> reaped = inner_->Reap(out, wait);
    if (!reaped.has_value()) return reaped;
    std::lock_guard<std::mutex> guard(latch_);
    for (std::size_t i = 0; i < reaped.value(); ++i) {
      auto it = std::find(failing_.begin(), failing_.end(), out[i].token);
      if (it == failing_.end()) continue;
      failing_.erase(it);
      if (out[i].status.has_value()) out[i].status = IoError("injected fault", 5);
      injected_.fetch_add(1);
    }
    return reaped;
  }

  void Shutdown() override { inner_->Shutdown(); }
  auto GetCapabilities() const -> DiskBackendCapabilities override { return inner_->GetCapabilities(); }
  auto GetFileSize(FileId file_id) -> Result<std::uint64_t> override { return inner_->GetFileSize(file_id); }
  auto SetFileSize(FileId file_id, std::uint64_t size) -> Status override { return inner_->SetFileSize(file_id, size); }

 private:
  static auto Index(DiskOperation operation) -> std::size_t { return static_cast<std::size_t>(operation); }

  std::unique_ptr<DiskBackend> inner_;
  std::mutex latch_;
  std::mt19937_64 rng_;
  std::vector<std::uint64_t> failing_;
  std::atomic<unsigned> rates_[3]{};
  std::atomic<std::uint64_t> submitted_[3]{};
  std::atomic<std::uint64_t> injected_{0};
};

}  // namespace telepath::test_support

#endif  // TELEPATH_TEST_CPP_SUPPORT_FAULT_INJECTION_BACKEND_H_
