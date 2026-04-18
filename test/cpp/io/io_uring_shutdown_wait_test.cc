#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include "io_test_support.h"
#include "telepath/io/io_uring_disk_backend.h"

namespace {

bool HandleUnavailableIoUringBackend(const telepath::IoUringDiskBackend &backend) {
  if (backend.initialization_status().ok()) return false;

  assert(!telepath::io_test_support::RequireIoUringSuccess());
  assert(backend.initialization_status().code() == telepath::StatusCode::kUnavailable);
  return true;
}

void WaitForCompletionExit(
    telepath::IoUringDiskBackend *backend,
    std::atomic<bool> *completed,
    telepath::Status *poll_status) {
  auto result = backend->PollCompletion();
  assert(!result.ok());
  *poll_status = result.status();
  completed->store(true);
}

void AssertWaiterUnblocksOnShutdown(telepath::IoUringDiskBackend *backend) {
  std::atomic<bool> completed{false};
  telepath::Status poll_status = telepath::Status::Ok();

  std::thread waiter([&]() { WaitForCompletionExit(backend, &completed, &poll_status); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  backend->Shutdown();

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!completed.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  waiter.join();
  assert(completed.load());
  assert(poll_status.code() == telepath::StatusCode::kUnavailable);
}

}  // namespace

int main() {
  telepath::io_test_support::TestRootGuard root_guard("telepath_io_uring_shutdown_wait_test");
  telepath::IoUringDiskBackend backend(root_guard.path().string(), 4096, 32);
  if (HandleUnavailableIoUringBackend(backend)) return 0;
  AssertWaiterUnblocksOnShutdown(&backend);
  return 0;
}
