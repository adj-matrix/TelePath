// Cross-process mode runs the background cleaner in one process at a time:
// the leader cleans pages every process dirtied, the others do not clean, and
// another process takes over when the leader detaches. (A leader that dies
// fences the whole segment instead; see crash_recovery_test.)
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "buffer_test_support.h"
#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/posix_disk_backend.h"
#include "telepath/options/buffer_manager_options.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

extern char **environ;

namespace {

constexpr std::size_t kPageSize = 4096;
constexpr std::size_t kPoolSize = 32;
constexpr std::size_t kHighWatermark = 4;
constexpr std::size_t kLowWatermark = 1;
constexpr telepath::BlockId kDirtyPages = 12;

struct Process {
  std::shared_ptr<telepath::TelemetrySink> telemetry = telepath::MakeCounterTelemetrySink();
  std::unique_ptr<telepath::BufferManager> manager;
};

auto Open(const std::string &segment, telepath::SharedMemoryMode mode, const std::string &root) -> Process {
  telepath::BufferManagerOptions options;
  options.pool_size = kPoolSize;
  options.page_size = kPageSize;
  options.enable_background_cleaner = true;
  options.dirty_page_high_watermark = kHighWatermark;
  options.dirty_page_low_watermark = kLowWatermark;
  options.shared_memory.name = segment;
  options.shared_memory.mode = mode;
  Process process;
  auto created = telepath::BufferManager::Create(options, std::make_unique<telepath::PosixDiskBackend>(root, telepath::BlockGeometry::Of<kPageSize>()), telepath::MakeClockSweepReplacer(kPoolSize), process.telemetry);
  if (created.has_value()) process.manager = std::move(created.value());
  return process;
}

auto CleanerFlushes(const Process &process) -> std::uint64_t { return process.telemetry->Snapshot().cleaner_flushes_scheduled; }

// Dirties pages [first, first + count) and releases them.
void Dirty(telepath::BufferManager *manager, telepath::BlockId first, telepath::BlockId count) {
  for (telepath::BlockId block = first; block < first + count; ++block) {
    auto handle = manager->ReadBufferExclusive(1, block);
    assert(handle.has_value());
    handle->mutable_data()[0] = std::byte{1};
  }
}

// Waits until no page is dirty or being written back.
bool Settled(telepath::BufferManager *manager, std::chrono::seconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto snapshot = manager->ExportSnapshot();
    if (snapshot.dirty_page_count < kHighWatermark && snapshot.flush_queued_count == 0 && snapshot.flush_in_flight_count == 0) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

auto Name(const char *test) -> std::string { return std::string("/telepath_") + test + "_" + std::to_string(getpid()); }

auto Spawn(const std::vector<std::string> &args) -> pid_t {
  std::vector<char *> argv{const_cast<char *>("/proc/self/exe")};
  for (const std::string &arg : args) argv.push_back(const_cast<char *>(arg.c_str()));
  argv.push_back(nullptr);
  pid_t pid = 0;
  assert(posix_spawn(&pid, "/proc/self/exe", nullptr, nullptr, argv.data(), environ) == 0);
  return pid;
}

auto Wait(pid_t pid) -> int {
  int status = 0;
  assert(waitpid(pid, &status, 0) == pid);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

// Child of ExpectOnlyTheLeaderCleans: dirties pages past the watermark and
// relies on the parent, which leads, to clean them.
auto FollowerChild(const std::string &segment, const std::string &root) -> int {
  Process process = Open(segment, telepath::SharedMemoryMode::kAttach, root);
  if (process.manager == nullptr) return 2;
  Dirty(process.manager.get(), 0, kDirtyPages);
  if (!Settled(process.manager.get(), std::chrono::seconds(10))) return 3;
  return CleanerFlushes(process) == 0 ? 0 : 4;
}

void ExpectOnlyTheLeaderCleans() {
  const std::string segment = Name("cleaner_leader");
  telepath::buffer_test_support::TestRootGuard root("telepath_cleaner_leader");
  Process leader = Open(segment, telepath::SharedMemoryMode::kCreate, root.path().string());
  assert(leader.manager != nullptr);
  const int status = Wait(Spawn({"follower-child", segment, root.path().string()}));
  if (status != 0) std::fprintf(stderr, "follower child ended with %d\n", status);
  assert(status == 0);
  assert(CleanerFlushes(leader) > 0);
}

// Child of ExpectAnotherProcessTakesOver: creates the segment, so it leads,
// cleans until `leave` appears, and then detaches.
auto LeaderChild(const std::string &segment, const std::string &root, const std::string &ready, const std::string &leave) -> int {
  Process process = Open(segment, telepath::SharedMemoryMode::kCreate, root);
  if (process.manager == nullptr) return 2;
  close(open(ready.c_str(), O_CREAT | O_WRONLY, 0600));
  while (access(leave.c_str(), F_OK) != 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  return 0;
}

void ExpectAnotherProcessTakesOver() {
  const std::string segment = Name("cleaner_failover");
  telepath::buffer_test_support::TestRootGuard root("telepath_cleaner_failover");
  const std::string ready = (root.path() / "ready").string();
  const std::string leave = (root.path() / "leave").string();
  const pid_t child = Spawn({"leader-child", segment, root.path().string(), ready, leave});
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (access(ready.c_str(), F_OK) != 0) {
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  Process follower = Open(segment, telepath::SharedMemoryMode::kAttach, root.path().string());
  assert(follower.manager != nullptr);
  Dirty(follower.manager.get(), 0, kDirtyPages);
  assert(Settled(follower.manager.get(), std::chrono::seconds(10)));
  assert(CleanerFlushes(follower) == 0);

  close(open(leave.c_str(), O_CREAT | O_WRONLY, 0600));
  assert(Wait(child) == 0);
  Dirty(follower.manager.get(), kDirtyPages, kDirtyPages);
  assert(Settled(follower.manager.get(), std::chrono::seconds(10)));
  assert(CleanerFlushes(follower) > 0);
  assert(!follower.manager->failure().has_value());
}

}  // namespace

int main(int argc, char **argv) {
  if (argc == 4 && std::strcmp(argv[1], "follower-child") == 0) return FollowerChild(argv[2], argv[3]);
  if (argc == 6 && std::strcmp(argv[1], "leader-child") == 0) return LeaderChild(argv[2], argv[3], argv[4], argv[5]);

  ExpectOnlyTheLeaderCleans();
  ExpectAnotherProcessTakesOver();
  return 0;
}
