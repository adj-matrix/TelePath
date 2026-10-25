#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include "disk_io_helpers.h"
#include "io_test_support.h"
#include "telepath/io/io_uring_disk_backend.h"

namespace {

bool HandleUnavailableIoUringBackend(const telepath::Result<std::unique_ptr<telepath::IoUringDiskBackend>> &backend_result) {
  if (backend_result.has_value()) return false;

  assert(!telepath::io_test_support::RequireIoUringSuccess());
  assert(backend_result.error().code() == telepath::ErrorCode::kUnavailable);
  return true;
}

void WaitForCompletionExit(
    telepath::IoUringDiskBackend *backend,
    std::atomic<bool> *completed,
    telepath::Status *poll_status) {
  auto result = telepath::test_support::ReapOne(*backend);
  assert(!result.has_value());
  *poll_status = std::unexpected(result.error());
  completed->store(true);
}

void AssertWaiterUnblocksOnShutdown(telepath::IoUringDiskBackend *backend) {
  std::atomic<bool> completed{false};
  telepath::Status poll_status = telepath::Ok();

  std::thread waiter([&]() { WaitForCompletionExit(backend, &completed, &poll_status); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  backend->Shutdown();

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!completed.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  waiter.join();
  assert(completed.load());
  assert(poll_status.error().code() == telepath::ErrorCode::kUnavailable);
}

}  // namespace

int main() {
  telepath::io_test_support::TestRootGuard root_guard("telepath_io_uring_shutdown_wait_test");
  auto backend_result = telepath::IoUringDiskBackend::Create(root_guard.path().string(), telepath::BlockGeometry::Of<4096>(), 32);
  if (HandleUnavailableIoUringBackend(backend_result)) return 0;
  telepath::IoUringDiskBackend &backend = *backend_result.value();
  AssertWaiterUnblocksOnShutdown(&backend);
  return 0;
}
