#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#include "telepath/io/io_uring_disk_backend.h"

namespace {

bool RequireIoUringSuccess() {
  const char *value = std::getenv("TELEPATH_REQUIRE_IO_URING_SUCCESS");
  return value != nullptr && std::string(value) == "1";
}

}  // namespace

int main() {
  namespace fs = std::filesystem;

  const fs::path root =
      fs::temp_directory_path() / "telepath_io_uring_shutdown_wait_test";
  fs::remove_all(root);
  fs::create_directories(root);

  telepath::IoUringDiskBackend backend(root.string(), 4096, 32);
  if (!backend.initialization_status().ok()) {
    assert(!RequireIoUringSuccess());
    assert(backend.initialization_status().code() ==
           telepath::StatusCode::kUnavailable);
    fs::remove_all(root);
    return 0;
  }

  std::atomic<bool> completed{false};
  telepath::Status poll_status = telepath::Status::Ok();

  std::thread waiter([&]() {
    auto result = backend.PollCompletion();
    assert(!result.ok());
    poll_status = result.status();
    completed.store(true);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  backend.Shutdown();

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(1);
  while (!completed.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  waiter.join();
  assert(completed.load());
  assert(poll_status.code() == telepath::StatusCode::kUnavailable);

  fs::remove_all(root);
  return 0;
}
