// Lifecycle and latch boundaries: Close() racing with threads that keep using
// the manager, and the limit of 255 shared holders of one page.
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "buffer_test_support.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

namespace {

constexpr std::size_t kPageSize = 4096;

auto Open(const std::filesystem::path &root, std::size_t pool_size) -> std::unique_ptr<telepath::BufferManager> {
  telepath::BufferManagerOptions options;
  options.pool_size = pool_size;
  options.page_size = kPageSize;
  options.enable_background_cleaner = true;
  auto created = telepath::BufferManager::Create(options, std::make_unique<telepath::PosixDiskBackend>(root.string(), telepath::BlockGeometry::Of<kPageSize>()), telepath::MakeClockSweepReplacer(pool_size), telepath::MakeNoOpTelemetrySink());
  assert(created.has_value());
  return std::move(created.value());
}

bool Expected(const telepath::Error &error) {
  return error.code() == telepath::ErrorCode::kUnavailable || error.code() == telepath::ErrorCode::kResourceExhausted || error.code() == telepath::ErrorCode::kIoError;
}

// Close() may refuse while pages are pinned, but it never hangs or crashes
// while other threads keep calling in, and once it succeeds every new call
// fails with kUnavailable.
void ExpectCloseRacingWithUsersNeverHangs() {
  for (int round = 0; round < 20; ++round) {
    telepath::buffer_test_support::TestRootGuard root("telepath_lifecycle_close_race");
    auto manager = Open(root.path(), 8);
    std::atomic<bool> closed{false};
    std::vector<std::thread> users;
    for (int user = 0; user < 6; ++user) {
      users.emplace_back([&, user] {
        std::mt19937 rng(round * 10 + user);
        int after_close = 0;
        while (after_close < 50) {
          const bool was_closed = closed.load();
          const telepath::BlockId block = rng() % 32;
          switch (rng() % 5) {
            case 0: {
              auto handle = manager->ReadBufferExclusive(1, block);
              if (handle.has_value()) handle->mutable_data()[0] = std::byte{1};
              else assert(Expected(handle.error()));
              if (was_closed) assert(!handle.has_value() && handle.error().code() == telepath::ErrorCode::kUnavailable);
              break;
            }
            case 1: {
              auto status = manager->PrefetchBuffer(1, block, 4);
              if (was_closed) assert(!status.has_value() && status.error().code() == telepath::ErrorCode::kUnavailable);
              break;
            }
            case 2: {
              auto status = manager->FlushBuffer(1, block);
              if (!status.has_value()) assert(Expected(status.error()));
              if (was_closed) assert(!status.has_value());
              break;
            }
            default: {
              auto handle = manager->ReadBuffer(1, block);
              if (!handle.has_value()) assert(Expected(handle.error()));
              if (was_closed) assert(!handle.has_value() && handle.error().code() == telepath::ErrorCode::kUnavailable);
            }
          }
          if (was_closed) ++after_close;
        }
      });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (true) {
      telepath::Status status = manager->Close();
      if (status.has_value()) break;
      assert(status.error().code() == telepath::ErrorCode::kInvalidArgument);
      assert(std::chrono::steady_clock::now() < deadline);
    }
    closed.store(true);
    for (auto &user : users) user.join();
    assert(manager->Close().has_value());
  }
}

// A page admits 255 shared holders; the next reader waits until one leaves,
// and the count never wraps into the exclusive bit.
void ExpectSharedHolderLimit() {
  telepath::buffer_test_support::TestRootGuard root("telepath_lifecycle_shared_limit");
  auto manager = Open(root.path(), 4);
  std::vector<telepath::SharedBufferHandle> holders;
  for (int holder = 0; holder < 255; ++holder) {
    auto handle = manager->ReadBuffer(1, 0);
    assert(handle.has_value());
    holders.push_back(std::move(handle.value()));
  }
  std::atomic<bool> admitted{false};
  std::thread waiter([&] {
    auto handle = manager->ReadBuffer(1, 0);
    assert(handle.has_value());
    admitted.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  assert(!admitted.load());
  auto snapshot = manager->ExportSnapshot();
  std::uint32_t pins = 0;
  for (const auto &frame : snapshot.frames) pins += frame.pin_count;
  assert(pins == 256);

  holders.pop_back();
  waiter.join();
  assert(admitted.load());
  holders.clear();
  auto writer = manager->ReadBufferExclusive(1, 0);
  assert(writer.has_value());
}

}  // namespace

int main() {
  ExpectCloseRacingWithUsersNeverHangs();
  ExpectSharedHolderLimit();
  return 0;
}
